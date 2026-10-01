#include "rime/action/dispatcher.hpp"
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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
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
          std::unordered_set<std::string>{"windows.window.read", "windows.window.write"}),
      trace);
  const auto window_executor = std::make_shared<rime::win32::WindowExecutor>(service);
  for (const char* type : {"window.move", "window.focus", "window.close", "window.hide",
                           "window.show", "window.minimize", "window.maximize",
                           "window.restore"}) {
    assert(kernel.register_executor(type, window_executor).ok());
  }

  std::atomic<std::uint64_t> next_action_id{0};
  // Every mutation in this slice submits to one bounded queue, so the trace
  // records acceptance, execution and refusal through the same pipeline.
  rime::action::Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  rime::win32::WindowModuleBinding binding{&service, &kernel, &dispatcher, &next_action_id};

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

  // Segment 2b: an exhausted deadline never reaches the executor -- the
  // kernel rejects with Timeout before dispatch. deadlineMs:0 pins
  // deadline_unix_ms at build time so `deadline <= now` holds on every
  // dispatch; a 1ms budget raced the wall clock (the queue hop plus the
  // executor can finish inside one millisecond and let the action resolve).
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.deadlineOutcome = null;\n"
        "windows.move(" + id_text +
            ", 'left', { deadlineMs: 0 })\n"
            "  .then(() => { globalThis.deadlineOutcome = 'unexpected resolution'; },\n"
            "        e => { globalThis.deadlineOutcome = (e && e.code) || String(e); });",
        "slice-deadline.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.deadlineOutcome !== 'timeout')\n"
        "  throw new Error('deadline must reject with timeout, got: ' + globalThis.deadlineOutcome);",
        "slice-deadline-check.mjs");

  // Segment 2c: a malformed deadlineMs throws TypeError synchronously
  // instead of producing a promise.
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.badDeadlineIsTypeError = false;\n"
        "try { windows.move(" + id_text + ", 'left', { deadlineMs: -1 }); }\n"
        "catch (e) { globalThis.badDeadlineIsTypeError = (e instanceof TypeError); }",
        "slice-bad-deadline.mjs");
  check(runtime,
        "if (!globalThis.badDeadlineIsTypeError)\n"
        "  throw new Error('negative deadlineMs must throw TypeError');",
        "slice-bad-deadline-check.mjs");

  // Segment 3: focus and the 'active' target, scoped to OUR window only.
  // No global input injection (keybd_event) and no foreign window is ever
  // moved: the 'active' move runs only when our window owns the foreground,
  // otherwise the segment is skipped with a diagnostic.
  const bool focused = service.focus(id).ok();

  // JS focus: resolves, or rejects only with the foreground-lock denial
  // (SetForegroundWindow may be refused while another window owns the
  // foreground; that is not a binding failure).
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.focusHandle = null;\n"
        "globalThis.focusFailure = null;\n"
        "windows.focus(" + id_text +
            ")\n"
            "  .then(w => { globalThis.focusHandle = w; },\n"
            "        e => { globalThis.focusFailure = String(e); });",
        "slice-focus.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.focusFailure &&\n"
        "    !globalThis.focusFailure.includes('foreground lock'))\n"
        "  throw new Error('focus failed unexpectedly: ' + globalThis.focusFailure);\n"
        "if (globalThis.focusHandle && globalThis.focusHandle.id !== " + id_text + ")\n"
        "  throw new Error('focus resolved the wrong window');",
        "slice-focus-check.mjs");

  // JS active read: observation only, safe regardless of who owns the
  // foreground -- resolves null or a snapshot with a numeric id.
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.activeInfo = 'unset';\n"
        "globalThis.activeFailure = null;\n"
        "windows.active()\n"
        "  .then(w => { globalThis.activeInfo = w; },\n"
        "        e => { globalThis.activeFailure = String(e); });",
        "slice-active-read.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.activeFailure) throw new Error(globalThis.activeFailure);\n"
        "if (globalThis.activeInfo !== null && typeof globalThis.activeInfo.id !== 'number')\n"
        "  throw new Error('active() must resolve null or a snapshot');",
        "slice-active-read-check.mjs");

  // Re-check ownership right before deciding: the JS focus attempt above may
  // have succeeded after the first native request was refused. Deciding on a
  // stale read could move a foreign window.
  std::optional<WindowInfo> foreground;
  assert(service.active(foreground).ok());
  const bool ours_foreground = foreground.has_value() && foreground->id == id;

  bool active_move_ran = false;
  if (ours_foreground) {
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
          "if (globalThis.moved.id !== " + id_text +
              ") throw new Error('active resolved to the wrong window');",
          "slice-active-check.mjs");
    WindowInfo active_moved;
    assert(service.info(id, active_moved).ok());
    assert(active_moved.rect == primary_left_half());
    active_move_ran = true;
  } else if (!foreground.has_value()) {
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
  } else {
    // SKIP: a foreign window owns the foreground. Moving 'active' would
    // move it and restoring by hand races with user input -- never touch it.
    std::fprintf(stderr,
                 "SKIP: slice active-move (foreign window owns the foreground; "
                 "focus requested ok=%d)\n",
                 focused ? 1 : 0);
  }

  // Segment 4: WinTitle queries and the extended snapshot fields. An exact
  // title query resolves only our window; a bogus title resolves nothing.
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.queryHits = null;\n"
        "globalThis.readFailure = null;\n"
        "windows.list({ title: 'Rime Vertical Slice Window', matchMode: 'exact' })\n"
        "  .then(w => { globalThis.queryHits = w; },\n"
        "        e => { globalThis.readFailure = String(e); });",
        "slice-query.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.readFailure) throw new Error(globalThis.readFailure);\n"
        "if (!Array.isArray(globalThis.queryHits) || globalThis.queryHits.length !== 1)\n"
        "  throw new Error('exact query must resolve exactly one window');\n"
        "const hit = globalThis.queryHits[0];\n"
        "if (hit.id !== " + id_text + ") throw new Error('query resolved the wrong window');\n"
        "if (hit.className !== 'Static') throw new Error('snapshot className missing');\n"
        "if (!hit.processName) throw new Error('snapshot processName missing');\n"
        "if (hit.state !== 'normal') throw new Error('snapshot state must be normal');",
        "slice-query-check.mjs");
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.emptyHits = ['unset'];\n"
        "windows.list({ title: 'No Such Window Anywhere In This Test' })\n"
        "  .then(w => { globalThis.emptyHits = w; });",
        "slice-query-empty.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (!Array.isArray(globalThis.emptyHits) || globalThis.emptyHits.length !== 0)\n"
        "  throw new Error('a bogus title must resolve no windows');",
        "slice-query-empty-check.mjs");

  // Segment 5: state mutations through the kernel round-trip the snapshot.
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.stateFailure = null;\n"
        "globalThis.hiddenHandle = null;\n"
        "windows.hide(" + id_text +
            ")\n"
            "  .then(w => { globalThis.hiddenHandle = w; },\n"
            "        e => { globalThis.stateFailure = String(e); });",
        "slice-hide.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.stateFailure) throw new Error(globalThis.stateFailure);\n"
        "if (globalThis.hiddenHandle.state !== 'hidden') throw new Error('hide must report hidden');\n"
        "if (globalThis.hiddenHandle.visible !== false) throw new Error('hide must clear visible');",
        "slice-hide-check.mjs");
  WindowInfo hidden_after_js;
  assert(service.info(id, hidden_after_js).ok());
  assert(!hidden_after_js.visible);
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.shownHandle = null;\n"
        "windows.show(" + id_text +
            ")\n"
            "  .then(w => { globalThis.shownHandle = w; },\n"
            "        e => { globalThis.stateFailure = String(e); });",
        "slice-show.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.stateFailure) throw new Error(globalThis.stateFailure);\n"
        "if (globalThis.shownHandle.state !== 'normal') throw new Error('show must report normal');\n"
        "if (globalThis.shownHandle.visible !== true) throw new Error('show must set visible');",
        "slice-show-check.mjs");
  WindowInfo shown_after_js;
  assert(service.info(id, shown_after_js).ok());
  assert(shown_after_js.visible);

  // Segment 5b: the info read round-trips the extended snapshot fields.
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.infoSnap = null;\n"
        "globalThis.infoFailure = null;\n"
        "windows.info(" + id_text +
            ")\n"
            "  .then(s => { globalThis.infoSnap = s; },\n"
            "        e => { globalThis.infoFailure = String(e); });",
        "slice-info.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.infoFailure) throw new Error(globalThis.infoFailure);\n"
        "if (globalThis.infoSnap.id !== " + id_text +
            ") throw new Error('info resolved the wrong id');\n"
        "if (globalThis.infoSnap.className !== 'Static') throw new Error('info className missing');\n"
        "if (globalThis.infoSnap.state !== 'normal') throw new Error('info state must be normal');",
        "slice-info-check.mjs");

  // Segment 5c: minimize -> maximize -> restore round-trip the state machine
  // through JS, each step verified both in the returned snapshot and natively.
  for (const auto& step : std::vector<std::pair<const char*, const char*>>{
           {"minimize", "minimized"}, {"maximize", "maximized"}, {"restore", "normal"}}) {
    const std::string call = step.first;
    const std::string expected_state = step.second;
    check(runtime,
          "import { windows } from 'rime:window';\n"
          "globalThis.stateFailure = null;\n"
          "globalThis.stateSnap = null;\n"
          "windows." + call + "(" + id_text +
              ")\n"
              "  .then(w => { globalThis.stateSnap = w; },\n"
              "        e => { globalThis.stateFailure = String(e); });",
          "slice-" + call + ".mjs");
    assert(runtime.settle(5000ms).ok());
    check(runtime,
          "if (globalThis.stateFailure) throw new Error(globalThis.stateFailure);\n"
          "if (globalThis.stateSnap.state !== '" + expected_state + "')\n"
          "  throw new Error('" + call + " must report " + expected_state + "');",
          "slice-" + call + "-check.mjs");
    WindowInfo native_state;
    assert(service.info(id, native_state).ok());
    if (native_state.state != expected_state) {
      std::fprintf(stderr, "native state after %s was %s, expected %s\n", call.c_str(),
                   native_state.state.c_str(), expected_state.c_str());
      std::abort();
    }
  }

  // Segment 6: close destroys the window through the executor (the result
  // snapshot is taken before WM_CLOSE); later operations on the id reject.
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.closedSnap = null;\n"
        "globalThis.closeFailure = null;\n"
        "windows.close(" + id_text +
            ")\n"
            "  .then(s => { globalThis.closedSnap = s; },\n"
            "        e => { globalThis.closeFailure = String(e); });",
        "slice-close.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.closeFailure) throw new Error(globalThis.closeFailure);\n"
        "if (!globalThis.closedSnap || globalThis.closedSnap.id !== " + id_text +
            ")\n"
            "  throw new Error('close must resolve the pre-close snapshot');",
        "slice-close-check.mjs");
  WindowInfo gone;
  assert(!service.info(id, gone).ok());
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

  // Trace, grouped by action type (no fragile global totals). Sources:
  // - window.focus/hide/show/minimize/maximize/restore/close: one Started +
  //   one Finished each (segments 3/5/5b/5c/6);
  // - window.move: segment 1 (move), segment 2 (bad placement), segment 2b
  //   (exhausted deadline), segment 6 (stale id), plus the optional active
  //   move. The deadline action always records Finished but records Started
  //   only when it survives kernel pre-dispatch, so Started is base/base+1
  //   and Finished is Started/Started+1.
  std::map<std::string, std::size_t> started_count;
  std::map<std::string, std::size_t> finished_count;
  for (const auto& entry : trace->snapshot()) {
    if (entry.kind == rime::core::TraceKind::ActionStarted) ++started_count[entry.subject];
    if (entry.kind == rime::core::TraceKind::ActionFinished) ++finished_count[entry.subject];
  }
  for (const auto& [subject, count] : started_count) {
    const auto finished = finished_count.find(subject);
    assert(finished != finished_count.end());
    if (subject == "window.move") continue;  // bounded below, not exactly paired
    assert(finished->second == count);
  }
  const std::size_t move_base = active_move_ran ? 4u : 3u;  // seg 1+2+stale [+active]
  const std::size_t move_started = started_count["window.move"];
  const std::size_t move_finished = finished_count["window.move"];
  // +1 Started when the deadline action survived kernel pre-dispatch.
  assert(move_started == move_base || move_started == move_base + 1);
  // +1 Finished when the deadline action died in pre-dispatch (no Started).
  assert(move_finished == move_started || move_finished == move_started + 1);
  for (const char* type : {"window.focus", "window.hide", "window.show", "window.minimize",
                           "window.maximize", "window.restore", "window.close"}) {
    assert(started_count[type] == 1);
  }
  // The exhausted deadline left a Finished entry naming the timeout.
  bool saw_deadline_timeout = false;
  for (const auto& entry : trace->snapshot()) {
    if (entry.kind == rime::core::TraceKind::ActionFinished &&
        entry.subject == "window.move" &&
        entry.detail.find("deadline exceeded") != std::string::npos) {
      saw_deadline_timeout = true;
    }
  }
  assert(saw_deadline_timeout);

  assert(runtime.stop().ok());

  // Capability gate: an empty policy rejects reads and writes with the
  // capability name. The JS lane is process-wide, so the denial runtime only
  // starts after the first runtime released it.
  {
    rime::action::Kernel denied_kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
        std::unordered_set<std::string>{}));
    rime::action::Dispatcher denied_dispatcher(denied_kernel,
                                               rime::action::default_dispatch_policy());
    rime::win32::WindowModuleBinding denied_binding{&service, &denied_kernel, &denied_dispatcher,
                                                    &next_action_id};
    rime::js::Runtime denied_runtime;
    assert(rime::win32::register_window_module(denied_runtime, &denied_binding).ok());
    assert(denied_runtime.start().ok());
    check(denied_runtime,
          "import { windows } from 'rime:window';\n"
          "globalThis.readDenied = null;\n"
          "globalThis.writeDenied = null;\n"
          "windows.list().then(() => {},\n"
          "                    e => { globalThis.readDenied = String(e); });\n"
          "windows.move(" + id_text +
              ", 'left').then(() => {},\n"
              "               e => { globalThis.writeDenied = String(e); });",
          "slice-deny.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    check(denied_runtime,
          "if (!globalThis.readDenied ||\n"
          "    !globalThis.readDenied.includes('windows.window.read'))\n"
          "  throw new Error('read must name the capability: ' + globalThis.readDenied);\n"
          "if (!globalThis.writeDenied ||\n"
          "    !globalThis.writeDenied.includes('windows.window.write'))\n"
          "  throw new Error('write must name the capability: ' + globalThis.writeDenied);",
          "slice-deny-check.mjs");
    assert(denied_runtime.stop().ok());
  }

  assert(service.stop().ok());
  return 0;
}
