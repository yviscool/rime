#include "rime/action/kernel.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/js_window.hpp"
#include "rime/win32/window.hpp"
#include "rime/win32/window_executor.hpp"

#include <windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::win32::Rect;
using rime::win32::WindowInfo;
using rime::win32::WindowService;

constexpr wchar_t kSliceWindowTitle[] = L"Rime Vertical Slice Window";

std::optional<WindowInfo> find_by_title(const std::vector<WindowInfo>& windows) {
  for (const auto& window : windows) {
    if (window.title.find("Rime Vertical Slice Window") != std::string::npos) return window;
  }
  return std::nullopt;
}

// Evaluates an assertion script; failures abort with the engine's message.
void check(rime::js::Runtime& runtime, const std::string& source,
           const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js check failed (%s): %s\n", filename.c_str(),
                 error.message.c_str());
    std::abort();
  }
}

Rect primary_left_half() {
  RECT work{};
  assert(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE);
  return {work.left, work.top, work.left + (work.right - work.left) / 2, work.bottom};
}

}  // namespace

int main() {
  WindowService service;
  assert(service.start().ok());

  HWND created = nullptr;
  assert(service.ui()
             .call([&] {
               created = CreateWindowExW(0, L"STATIC", kSliceWindowTitle,
                                         WS_OVERLAPPED | WS_VISIBLE, 90, 60, 700, 500,
                                         nullptr, nullptr, GetModuleHandleW(nullptr),
                                         nullptr);
               assert(created != nullptr);
             })
             .ok());

  std::vector<WindowInfo> windows;
  assert(service.list(windows).ok());
  const auto found = find_by_title(windows);
  assert(found.has_value());
  const std::uint64_t id = found->id;

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{"window.write"}),
      trace);
  assert(kernel
             .register_executor("window.move",
                                std::make_shared<rime::win32::WindowExecutor>(service))
             .ok());

  std::atomic<std::uint64_t> next_action_id{0};
  rime::win32::WindowModuleBinding binding{&service, &kernel, &next_action_id};

  rime::js::Runtime runtime;
  assert(rime::win32::register_window_module(runtime, &binding).ok());
  assert(runtime.start().ok());

  const auto id_text = std::to_string(id);

  // Segment 1: rime:window resolves the native module, moves our window
  // through the kernel and settles with the moved handle.
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.moved = null;\n"
        "globalThis.failure = null;\n"
        "windows.move(" + id_text +
            ", 'left')\n"
        "  .then(w => { globalThis.moved = w; },\n"
        "        e => { globalThis.failure = String(e); });",
        "slice-move.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (!globalThis.moved) throw new Error('move resolved without a handle');\n"
        "if (globalThis.moved.id !== " + id_text + ") throw new Error('moved wrong window');\n"
        "if (typeof globalThis.moved.rect.left !== 'number') throw new Error('bad handle rect');",
        "slice-move-check.mjs");
  WindowInfo moved;
  assert(service.info(id, moved).ok());
  assert(moved.rect == primary_left_half());

  // Segment 2: an unknown placement rejects with the executor's reason.
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.failure = null;\n"
        "windows.move(" + id_text +
            ", 'diagonal')\n"
        "  .then(() => { globalThis.failure = 'unexpected resolution'; },\n"
        "        e => { globalThis.failure = String(e); });",
        "slice-bad-placement.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (!globalThis.failure.includes('unknown window placement'))\n"
        "  throw new Error('wrong rejection: ' + globalThis.failure);",
        "slice-bad-placement-check.mjs");

  // Segment 3: the 'active' target resolves the foreground window. Try to
  // take the foreground (ALT modifier lifts the lock); when another window
  // keeps it, that window is the resolved target and is restored after.
  keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY, 0);
  (void)service.focus(id);
  keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
  std::optional<WindowInfo> foreground;
  assert(service.active(foreground).ok());

  if (foreground.has_value()) {
    const std::uint64_t expected = foreground->id;
    const bool ours_foreground = expected == id;
    check(runtime,
          "import { windows } from 'rime:window';\n"
          "globalThis.moved = null;\n"
          "globalThis.failure = null;\n"
          "windows.move('active', 'left')\n"
          "  .then(w => { globalThis.moved = w; },\n"
          "        e => { globalThis.failure = String(e); });",
          "slice-active.mjs");
    assert(runtime.settle(5000ms).ok());
    check(runtime,
          "if (globalThis.failure) throw new Error(globalThis.failure);\n"
          "if (globalThis.moved.id !== " + std::to_string(expected) +
              ") throw new Error('active resolved to the wrong window');",
          "slice-active-check.mjs");
    if (ours_foreground) {
      WindowInfo active_moved;
      assert(service.info(id, active_moved).ok());
      assert(active_moved.rect == primary_left_half());
    } else {
      // A foreign window was moved; put its geometry back.
      assert(service.move_rect(expected, foreground->rect).ok());
    }
  } else {
    check(runtime,
          "import { windows } from 'rime:window';\n"
          "globalThis.failure = null;\n"
          "windows.move('active', 'left')\n"
          "  .then(() => { globalThis.failure = 'unexpected resolution'; },\n"
          "        e => { globalThis.failure = String(e); });",
          "slice-active-none.mjs");
    assert(runtime.settle(5000ms).ok());
    check(runtime,
          "if (!globalThis.failure.includes('no active window'))\n"
          "  throw new Error('wrong rejection: ' + globalThis.failure);",
          "slice-active-none-check.mjs");
  }

  // Segment 4: destroying the window turns later moves into rejections.
  assert(service.ui().call([&] { DestroyWindow(created); }).ok());
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.failure = null;\n"
        "windows.move(" + id_text +
            ", 'left')\n"
        "  .then(() => { globalThis.failure = 'unexpected resolution'; },\n"
        "        e => { globalThis.failure = String(e); });",
        "slice-stale.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (!globalThis.failure.includes('window no longer exists'))\n"
        "  throw new Error('wrong rejection: ' + globalThis.failure);",
        "slice-stale-check.mjs");

  // Every action reached the kernel and produced a trace pair.
  std::size_t started = 0;
  std::size_t finished = 0;
  for (const auto& entry : trace->snapshot()) {
    if (entry.subject != "window.move") continue;
    if (entry.kind == rime::core::TraceKind::ActionStarted) ++started;
    if (entry.kind == rime::core::TraceKind::ActionFinished) ++finished;
  }
  assert(started == 4);
  assert(finished == 4);

  assert(runtime.stop().ok());
  assert(service.stop().ok());
  return 0;
}
