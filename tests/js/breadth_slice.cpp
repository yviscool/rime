#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/clipboard.hpp"
#include "rime/win32/clipboard_executor.hpp"
#include "rime/win32/js_clipboard.hpp"
#include "rime/win32/js_process.hpp"
#include "rime/win32/process.hpp"
#include "rime/win32/process_executor.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;
using rime::win32::ClipboardService;
using rime::win32::ProcessService;

// Evaluates an assertion script; failures abort with the engine's message.
void check(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js check failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

std::string narrow(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                          nullptr, nullptr);
  assert(size > 0);
  std::string text(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size,
                      nullptr, nullptr);
  return text;
}

std::string self_path() {
  std::wstring path(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  assert(size > 0 && size < path.size());
  path.resize(size);
  return narrow(path);
}

std::size_t trace_count(const std::shared_ptr<rime::core::InMemoryTrace>& trace,
                        const std::string& subject, const rime::core::TraceKind kind) {
  std::size_t count = 0;
  for (const auto& entry : trace->snapshot()) {
    if (entry.subject == subject && entry.kind == kind) ++count;
  }
  return count;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--child") {
    Sleep(3000);
    return 0;
  }

  ProcessService process_service;
  ClipboardService clipboard_service;

  std::string original_clipboard;
  assert(clipboard_service.read_text(original_clipboard).ok());

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(
      std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{
          "process.launch", "process.terminate", "process.inspect", "windows.clipboard.read",
          "windows.clipboard.write"}),
      trace);
  const auto process_executor = std::make_shared<rime::win32::ProcessExecutor>(process_service);
  assert(kernel.register_executor("process.launch", process_executor).ok());
  assert(kernel.register_executor("process.terminate", process_executor).ok());
  assert(kernel
             .register_executor("clipboard.write",
                                std::make_shared<rime::win32::ClipboardExecutor>(clipboard_service))
             .ok());

  std::atomic<std::uint64_t> next_action_id{0};
  // Both modules share one bounded queue, so process and clipboard mutations
  // interleave through the same inspectable pipeline.
  rime::action::Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  rime::win32::ProcessModuleBinding process_binding{&process_service, &kernel, &dispatcher,
                                                    &next_action_id};
  rime::win32::ClipboardModuleBinding clipboard_binding{&clipboard_service, &kernel, &dispatcher,
                                                        &next_action_id};

  rime::js::Runtime runtime;
  assert(rime::win32::register_process_module(runtime, &process_binding).ok());
  assert(rime::win32::register_clipboard_module(runtime, &clipboard_binding).ok());
  assert(runtime.start().ok());

  const auto self_pid = static_cast<std::int64_t>(GetCurrentProcessId());
  const std::string executable = self_path();

  // Segment 1: rime:process resolves; list and info include this process.
  check(runtime,
        "import { process } from 'rime:process';\n"
        "globalThis.failure = null;\n"
        "process.list().then(list => { globalThis.list = list; },\n"
        "                    e => { globalThis.failure = String(e); });",
        "breadth-list.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (!Array.isArray(globalThis.list) || globalThis.list.length === 0)\n"
        "  throw new Error('process list must be a non-empty array');\n"
        "const self = globalThis.list.find(p => p.pid === " +
            std::to_string(self_pid) +
            ");\n"
            "if (!self) throw new Error('self missing from process list');\n"
            "if (typeof self.name !== 'string' || !self.name) throw new Error('bad process name');\n"
            "if (typeof self.parentPid !== 'number') throw new Error('bad parentPid');\n"
            "if (typeof self.exePath !== 'string') throw new Error('bad exePath');",
        "breadth-list-check.mjs");

  check(runtime,
        "import { process } from 'rime:process';\n"
        "globalThis.failure = null;\n"
        "process.info(" + std::to_string(self_pid) +
            ").then(p => { globalThis.mine = p; },\n"
            "        e => { globalThis.failure = String(e); });",
        "breadth-info.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "import { process } from 'rime:process';\n"
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (globalThis.mine.pid !== " + std::to_string(self_pid) +
            ") throw new Error('info resolved the wrong process');\n"
        "if (!globalThis.mine.exePath) throw new Error('own image path must resolve');\n"
            "globalThis.badPid = null;\n"
            "try { process.info(0); } catch (e) { globalThis.badPid = e instanceof TypeError; }\n"
            "if (!globalThis.badPid) throw new Error('info(0) must throw TypeError');",
        "breadth-info-check.mjs");

  // Segment 2: launch and terminate flow through the kernel.
  // The executable is embedded as a JSON string so backslashes survive.
  const std::string command_json =
      rime::core::json::stringify(rime::core::json::Value::string(executable));
  check(runtime,
        "import { process } from 'rime:process';\n"
        "globalThis.failure = null;\n"
        "globalThis.launched = null;\n"
        "process.launch({command: " +
            command_json +
            ", args: '--child'})\n"
            "  .then(r => { globalThis.launched = r; },\n"
            "        e => { globalThis.failure = String(e); });",
        "breadth-launch.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "import { process } from 'rime:process';\n"
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (!globalThis.launched || !(globalThis.launched.pid > 0))\n"
        "  throw new Error('launch must resolve a pid');\n"
        "globalThis.badLaunch = null;\n"
        "try { process.launch({}); } catch (e) { globalThis.badLaunch = e instanceof TypeError; }\n"
        "if (!globalThis.badLaunch) throw new Error('launch({}) must throw TypeError');",
        "breadth-launch-check.mjs");

  check(runtime,
        "import { process } from 'rime:process';\n"
        "globalThis.failure = null;\n"
        "globalThis.childInfo = null;\n"
        "process.info(globalThis.launched.pid)\n"
        "  .then(p => { globalThis.childInfo = p; },\n"
        "        e => { globalThis.failure = String(e); });",
        "breadth-child-info.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (globalThis.childInfo.pid !== globalThis.launched.pid)\n"
        "  throw new Error('child info mismatch');",
        "breadth-child-info-check.mjs");

  check(runtime,
        "import { process } from 'rime:process';\n"
        "globalThis.failure = null;\n"
        "globalThis.terminated = null;\n"
        "process.terminate(globalThis.launched.pid)\n"
        "  .then(r => { globalThis.terminated = r; },\n"
        "        e => { globalThis.failure = String(e); });",
        "breadth-terminate.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (globalThis.terminated.pid !== globalThis.launched.pid)\n"
        "  throw new Error('terminate must resolve the pid');",
        "breadth-terminate-check.mjs");

  check(runtime,
        "import { process } from 'rime:process';\n"
        "globalThis.goneErr = null;\n"
        "process.info(globalThis.launched.pid)\n"
        "  .then(() => { globalThis.goneErr = 'unexpected resolution'; },\n"
        "        e => { globalThis.goneErr = String(e); });",
        "breadth-gone.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (!globalThis.goneErr.includes('no longer exists'))\n"
        "  throw new Error('wrong rejection after terminate: ' + globalThis.goneErr);",
        "breadth-gone-check.mjs");

  // Segment 3: clipboard write flows through the kernel; read is a query.
  const std::string clip_text = "breadth-clip-\xE6\xB5\x8B\xE8\xAF\x95";
  check(runtime,
        "import { clipboard } from 'rime:clipboard';\n"
        "globalThis.failure = null;\n"
        "globalThis.written = null;\n"
        "clipboard.write('" +
            clip_text + "')\n"
            "  .then(r => { globalThis.written = r; },\n"
            "        e => { globalThis.failure = String(e); });",
        "breadth-clip-write.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (!globalThis.written || globalThis.written.text !== '" + clip_text +
            "') throw new Error('write must echo the text');",
        "breadth-clip-write-check.mjs");

  std::string native_read;
  assert(clipboard_service.read_text(native_read).ok());
  assert(native_read == clip_text);

  check(runtime,
        "import { clipboard } from 'rime:clipboard';\n"
        "globalThis.failure = null;\n"
        "globalThis.clipText = null;\n"
        "clipboard.read().then(r => { globalThis.clipText = r.text; },\n"
        "                     e => { globalThis.failure = String(e); });",
        "breadth-clip-read.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "import { clipboard } from 'rime:clipboard';\n"
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (globalThis.clipText !== '" + clip_text +
            "') throw new Error('clipboard read mismatch');\n"
            "globalThis.badWrite = null;\n"
            "try { clipboard.write(42); } catch (e) { globalThis.badWrite = e instanceof TypeError; }\n"
            "if (!globalThis.badWrite) throw new Error('write(42) must throw TypeError');",
        "breadth-clip-read-check.mjs");

  // Every mutation reached the kernel and produced a trace pair.
  assert(trace_count(trace, "process.launch", rime::core::TraceKind::ActionStarted) == 1);
  assert(trace_count(trace, "process.launch", rime::core::TraceKind::ActionFinished) == 1);
  assert(trace_count(trace, "process.terminate", rime::core::TraceKind::ActionStarted) == 1);
  assert(trace_count(trace, "process.terminate", rime::core::TraceKind::ActionFinished) == 1);
  assert(trace_count(trace, "clipboard.write", rime::core::TraceKind::ActionStarted) == 1);
  assert(trace_count(trace, "clipboard.write", rime::core::TraceKind::ActionFinished) == 1);

  assert(runtime.stop().ok());
  assert(clipboard_service.write_text(original_clipboard).ok());
  return 0;
}
