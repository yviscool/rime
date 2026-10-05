// Realism: L6
// Vertical slice for runtime.reload() (AHK Reload): the embedder tears the
// script side of the host down and rebuilds it around a re-read file, no
// process restart. The limit and file-change cases drive run_bundle_file -
// the exact entry rime_js_bundle and rime_host use - with a real on-disk
// replacement, a cross-thread clipboard handshake and a captured stderr
// diagnostic (production wiring plus a strong condition). The Host/Runtime
// cases below are driven from C++ instead of script because the refusal
// path (reload after a pending exit) is unreachable from script once the
// abort interrupt is armed; they still exercise the real components (L4).
// rime_test_support is linked as an object (root CMakeLists): its static
// initializer keeps assert/abort failures on stderr instead of a dialog.

#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/bootstrap.hpp"
#include "rime/win32/clipboard.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace {

using namespace std::chrono_literals;

std::string g_fixtures;

std::string fixture(const char* name) {
  return (std::filesystem::path(g_fixtures) / name).string();
}

std::string read_text_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  assert(input);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void write_text_file(const std::string& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  assert(output);
  output << content;
  output.close();
  assert(!output.fail());
}

// Exit outranks reload at the request boundary: while an exit is pending the
// reload request is refused with a stable reason, the flag stays clear, and
// neither a later reload nor a later exit can rewrite the pending outcome.
// Driven from C++ because after runtime.exit() arms the abort interrupt no
// script turn survives long enough to call runtime.reload() again.
void test_host_refuses_reload_after_exit() {
  rime::js::Host host;
  host.set_reload_count(7);
  assert(host.reload_count() == 7);

  assert(host.request_exit(4).ok());
  const rime::core::Error refusal = host.request_reload();
  assert(!refusal.ok());
  assert(refusal.code == rime::core::Error::Code::InvalidState);
  assert(refusal.message == "exit already requested: reload is ignored");
  assert(!host.reload_requested());
  assert(host.exit_requested());
  assert(host.exit_code() == 4);

  assert(host.request_exit(9).ok());
  assert(host.exit_code() == 4);

  // Without a pending exit the same request is accepted and idempotent.
  rime::js::Host fresh_host;
  assert(!fresh_host.reload_requested());
  assert(fresh_host.request_reload().ok());
  assert(fresh_host.reload_requested());
  assert(fresh_host.request_reload().ok());
  assert(fresh_host.reload_requested());
}

// The runtime side of the flag: published by the JS thread through the
// reload notifier, observable through the accessors, and settle must return
// early on it instead of waiting out its own budget - the 30s timer below
// would otherwise make settle time out and report a hang.
void test_runtime_reload_state() {
  rime::js::Runtime runtime;
  assert(runtime.set_reload_count(3).ok());
  assert(runtime.start().ok());
  // Configuration is pre-start only: the count is read by the JS thread when
  // the host is built, so changing it later must be refused.
  assert(!runtime.set_reload_count(4).ok());
  assert(!runtime.reload_requested());

  auto task = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "runtime.delay(30000, null);\n"
      "runtime.reload();",
      "reload-flag.mjs");
  assert(!task.get().ok());  // unwound by the abort, like runtime.exit
  assert(runtime.reload_requested());
  // Early by construction: a 30s delay is still armed, so an ok() answer can
  // only come from the reload short-circuit, never from quiescence.
  assert(runtime.settle(500ms).ok());
  assert(!runtime.exit_requested());
  assert(runtime.stop().ok());
  assert(runtime.stop().ok());  // repeatable
  assert(runtime.state() == rime::js::RuntimeState::Stopped);
}

// A script that reloads on every pass would spin the host forever on one
// file: the ceiling stops it with a diagnostic that names the limit and the
// file, and the entry point reports failure instead of looping. The stderr
// text is captured so the assertion covers the message, not just the code.
void test_reload_limit_stops_the_loop() {
  std::ostringstream captured;
  std::streambuf* previous = std::cerr.rdbuf(captured.rdbuf());
  const int code =
      rime::win32::run_bundle_file(fixture("reload-loop.mjs"), rime::win32::demo_capabilities());
  std::cerr.rdbuf(previous);
  const std::string output = captured.str();
  assert(code == 1);
  assert(output.find("reload limit exceeded (8 reloads)") != std::string::npos);
  assert(output.find("reload-loop.mjs") != std::string::npos);
}

// The core Reload semantic: while pass 0 is provably running (it published
// "ready"), the harness replaces the file on disk and publishes "go"; pass 0
// then reloads and the host must re-read the file, so pass 1 runs the new
// content and exits 5. Every other interleaving has its own exit code (11 =
// V1 ran again, 12 = V2 ran before any reload, 13 = handshake never
// completed, 14 = handshake action failed), so a wrong order cannot pass.
void test_reload_re_reads_a_changed_file() {
  const std::string first_pass = read_text_file(fixture("reload-wait-for-change.mjs"));
  const std::string second_pass = read_text_file(fixture("reload-after-change.mjs"));
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("rime-reload-" +
                                     std::to_string(std::chrono::steady_clock::now()
                                                        .time_since_epoch()
                                                        .count()));
  std::filesystem::create_directories(dir);
  const std::string script_path = (dir / "script.mjs").string();
  write_text_file(script_path, first_pass);

  rime::win32::ClipboardService clipboard;
  std::string clipboard_before;
  const bool had_clipboard_text = clipboard.read_text(clipboard_before).ok();
  assert(clipboard.write_text("").ok());

  // Coordinate on the main thread's run_bundle_file (the production entry
  // point) from a helper thread: both waits are bounded condition polls, and
  // "go" is only published after the replacement write completed.
  bool handshake_ok = false;
  std::thread coordinator([&] {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    std::string text;
    for (;;) {
      if (clipboard.read_text(text).ok() && text == "rime-reload-ready") break;
      if (std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(25ms);
    }
    write_text_file(script_path, second_pass);
    if (!clipboard.write_text("rime-reload-go").ok()) return;
    handshake_ok = true;
  });

  const int code = rime::win32::run_bundle_file(script_path,
                                                rime::win32::production_capabilities());
  coordinator.join();

  if (had_clipboard_text) assert(clipboard.write_text(clipboard_before).ok());
  std::error_code ignored;
  std::filesystem::remove_all(dir, ignored);

  // Name the failing interleaving instead of only tripping the assert:
  // 11 = V1 ran again (replacement too late), 12 = V2 ran before any reload
  // (replacement too early), 13 = no "go", 14 = handshake action failed.
  if (!handshake_ok || code != 5) {
    std::cerr << "reload file-change: handshake=" << (handshake_ok ? 1 : 0) << " code=" << code
              << '\n';
  }
  assert(handshake_ok);
  assert(code == 5);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: rime_reload_slice <fixtures-dir>\n";
    return 2;
  }
  g_fixtures = argv[1];

  test_host_refuses_reload_after_exit();
  test_runtime_reload_state();
  test_reload_limit_stops_the_loop();
  test_reload_re_reads_a_changed_file();
  return 0;
}
