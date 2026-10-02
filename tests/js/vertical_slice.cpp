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
               // A child edit gives WinGetControls/WinGetText something to
               // enumerate and read through the JS surface.
               const HWND child_edit = CreateWindowExW(
                   0, L"EDIT", L"Rime Slice Control", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10,
                   10, 300, 24, created, nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(child_edit != nullptr);
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
        "if (!hit.processPath || hit.processPath.length <= hit.processName.length)\n"
        "  throw new Error('snapshot processPath must be the full image path');\n"
        "if (!hit.processPath.endsWith(hit.processName))\n"
        "  throw new Error('processPath must end with processName');\n"
        "if (globalThis.queryHits[globalThis.queryHits.length - 1].id !== hit.id)\n"
        "  throw new Error('WinGetIDLast must resolve the last match');\n"
        "if (hit.state !== 'normal') throw new Error('snapshot state must be normal');\n"
        "if (typeof hit.clientRect.left !== 'number' ||\n"
        "    typeof hit.clientRect.bottom !== 'number')\n"
        "  throw new Error('snapshot clientRect missing');\n"
        "if (typeof hit.style !== 'number' || hit.style <= 0)\n"
        "  throw new Error('snapshot style must be a positive integer');\n"
        "if (typeof hit.exStyle !== 'number' || hit.exStyle < 0)\n"
        "  throw new Error('snapshot exStyle must be an unsigned integer');\n"
        "if (hit.enabled !== true) throw new Error('snapshot enabled must be true');\n"
        "if (hit.alwaysOnTop !== false) throw new Error('snapshot alwaysOnTop must be false');\n"
        "if (hit.minMax !== 0) throw new Error('snapshot minMax must be 0 for a normal window');\n"
        "if (hit.transparent !== -1) throw new Error('snapshot transparent must be -1 when unset');\n"
        "if (hit.transColor !== '') throw new Error('snapshot transColor must be empty when unset');",
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

  // Segment 4b: WinExist/WinActive probes resolve plain booleans through the
  // same read capability: existence stops at the first match (no snapshot is
  // built), isActive only ever inspects the foreground window.
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.probe = null;\n"
        "globalThis.probeFailure = null;\n"
        "Promise.all([\n"
        "  windows.exists({ title: 'Rime Vertical Slice Window', matchMode: 'exact' }),\n"
        "  windows.exists({ title: 'No Such Window Anywhere In This Test' }),\n"
        "  windows.isActive({ title: 'Rime Vertical Slice Window' }),\n"
        "]).then(r => { globalThis.probe = r; },\n"
        "        e => { globalThis.probeFailure = String(e); });",
        "slice-probe.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.probeFailure) throw new Error(globalThis.probeFailure);\n"
        "if (!Array.isArray(globalThis.probe) || globalThis.probe.length !== 3)\n"
        "  throw new Error('probes must resolve three results');\n"
        "if (globalThis.probe[0] !== true)\n"
        "  throw new Error('exists must find our window');\n"
        "if (globalThis.probe[1] !== false)\n"
        "  throw new Error('exists must miss a bogus title');\n"
        "if (typeof globalThis.probe[2] !== 'boolean')\n"
        "  throw new Error('isActive must resolve a boolean');",
        "slice-probe-check.mjs");

  // Segment 4c: WinGetControls/WinGetControlsHwnd and WinGetText reach the
  // child control from JS (stable control ids, AHK ClassNN, CRLF-delimited
  // text).
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.controlList = null;\n"
        "globalThis.windowText = null;\n"
        "globalThis.controlFailure = null;\n"
        "Promise.all([ windows.controls(" + id_text + "), windows.text(" + id_text + ") ])\n"
        "  .then(r => { globalThis.controlList = r[0]; globalThis.windowText = r[1]; },\n"
        "        e => { globalThis.controlFailure = String(e); });",
        "slice-controls.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.controlFailure) throw new Error(globalThis.controlFailure);\n"
        "if (!Array.isArray(globalThis.controlList) || globalThis.controlList.length !== 1)\n"
        "  throw new Error('expected exactly one control');\n"
        "const control = globalThis.controlList[0];\n"
        "if (control.classNN !== 'Edit1' || control.className !== 'Edit')\n"
        "  throw new Error('ClassNN must number the edit as Edit1');\n"
        "if (typeof control.id !== 'number' || control.id === " + id_text + ")\n"
        "  throw new Error('control id must be a stable, distinct id');\n"
        "if (globalThis.windowText !== 'Rime Slice Control\\r\\n')\n"
        "  throw new Error('unexpected control text: ' + JSON.stringify(globalThis.windowText));",
        "slice-controls-check.mjs");

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

  // Segment 5d: the global window settings (SetTitleMatchMode /
  // DetectHiddenWindows / DetectHiddenText) are synchronous state on
  // rime:window -- reads and writes never enter the action queue. WinTitle
  // matching is case-sensitive in every mode (AHK rule), a per-query
  // matchMode overrides the global mode, and bad patterns throw sync
  // TypeErrors before any queue hop. A second, never-shown window proves
  // DetectHiddenWindows without disturbing the hide/show counts above.
  HWND hidden_helper = nullptr;
  assert(service.ui()
             .call([&] {
               hidden_helper =
                   CreateWindowExW(0, L"STATIC", L"Rime Hidden Slice Window", WS_OVERLAPPED, 110,
                                   80, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr),
                                   nullptr);
               assert(hidden_helper != nullptr);
             })
             .ok());
  check(runtime,
        "import { settings, windows } from 'rime:window';\n"
        "globalThis.s5d = {};\n"
        "(async () => {\n"
        "  const out = {};\n"
        "  out.defaults = [settings.window.titleMatchMode,\n"
        "                  settings.window.titleMatchModeSpeed,\n"
        "                  settings.window.detectHiddenWindows,\n"
        "                  settings.window.detectHiddenText].join(',');\n"
        "  out.lower = await windows.exists({ title: 'rime vertical slice window' });\n"
        "  out.exact = await windows.exists({ title: 'Rime Vertical Slice Window', matchMode: 'exact' });\n"
        "  out.prefix = await windows.exists({ title: 'Rime Vertical Slice', matchMode: 'startswith' });\n"
        "  out.prefixMiss = await windows.exists({ title: 'Vertical Slice Window', matchMode: 'startswith' });\n"
        "  out.regex = await windows.exists({ title: '^Rime Vertical Slice Window$', matchMode: 'regex' });\n"
        "  out.regexI = await windows.exists({ title: 'i)^rime vertical slice window$', matchMode: 'regex' });\n"
        "  try { windows.exists({ title: 'Nope(', matchMode: 'regex' }); out.bad = 'no-throw'; }\n"
        "  catch (e) { out.bad = (e instanceof TypeError ? 'TypeError' : typeof e) + ':' + e.message; }\n"
        "  try { windows.exists({ title: 'x)foo', matchMode: 'regex' }); out.opt = 'no-throw'; }\n"
        "  catch (e) { out.opt = (e instanceof TypeError ? 'TypeError' : typeof e) + ':' + e.message; }\n"
        "  settings.window.titleMatchMode = '3';\n"
        "  out.exactGlobal = await windows.exists({ title: 'Rime Vertical Slice' });\n"
        "  out.modeBack = settings.window.setTitleMatchMode('2');\n"
        "  out.containsGlobal = await windows.exists({ title: 'Rime Vertical Slice' });\n"
        "  out.speedBack = settings.window.setTitleMatchMode('Slow');\n"
        "  out.speedNow = settings.window.titleMatchModeSpeed;\n"
        "  settings.window.titleMatchModeSpeed = 'Fast';\n"
        "  out.hiddenOff = await windows.exists({ title: 'Rime Hidden Slice Window' });\n"
        "  out.hiddenBack = settings.window.setDetectHiddenWindows(true);\n"
        "  out.hiddenOn = await windows.exists({ title: 'Rime Hidden Slice Window' });\n"
        "  out.hiddenListed = (await windows.list()).some(w => w.title === 'Rime Hidden Slice Window');\n"
        "  out.hiddenSame = settings.window.setDetectHiddenWindows(true);\n"
        "  settings.window.detectHiddenWindows = false;\n"
        "  out.hiddenRestored = await windows.exists({ title: 'Rime Hidden Slice Window' });\n"
        "  return out;\n"
        "})().then(v => { globalThis.s5d.value = v; },\n"
        "         e => { globalThis.s5d.error = String(e); });",
        "slice-settings.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.s5d.error) throw new Error(globalThis.s5d.error);\n"
        "const v = globalThis.s5d.value;\n"
        "if (!v) throw new Error('segment 5d produced no value');\n"
        "if (v.defaults !== '2,Fast,false,false')\n"
        "  throw new Error('wrong settings defaults: ' + v.defaults);\n"
        "if (v.lower !== false)\n"
        "  throw new Error('title match must be case-sensitive');\n"
        "if (!v.exact || !v.prefix || v.prefixMiss)\n"
        "  throw new Error('exact/startswith match failed');\n"
        "if (!v.regex || !v.regexI)\n"
        "  throw new Error('regex match failed');\n"
        "if (v.bad !== 'TypeError:invalid regular expression pattern')\n"
        "  throw new Error('bad pattern must throw sync TypeError: ' + v.bad);\n"
        "if (v.opt !== 'TypeError:unsupported regex option: x')\n"
        "  throw new Error('bad option must throw sync TypeError: ' + v.opt);\n"
        "if (v.exactGlobal !== false)\n"
        "  throw new Error('global exact mode must reject substrings');\n"
        "if (v.modeBack !== '3' || !v.containsGlobal)\n"
        "  throw new Error('mode restore failed: ' + v.modeBack);\n"
        "if (v.speedBack !== 'Fast' || v.speedNow !== 'Slow')\n"
        "  throw new Error('speed knob failed');\n"
        "if (v.hiddenOff !== false || v.hiddenBack !== false || !v.hiddenOn || !v.hiddenListed)\n"
        "  throw new Error('detectHiddenWindows failed');\n"
        "if (v.hiddenSame !== true)\n"
        "  throw new Error('no-change set must return the current value');\n"
        "if (v.hiddenRestored !== false)\n"
        "  throw new Error('detectHiddenWindows restore failed');",
        "slice-settings-check.mjs");

  // DetectHiddenText: a hidden child control contributes only while the
  // setting is on; controls() counts hidden children either way.
  HWND hidden_text_child = nullptr;
  assert(service.ui()
             .call([&] {
               hidden_text_child =
                   CreateWindowExW(0, L"STATIC", L"Rime Hidden Slice Text", WS_CHILD, 10, 40, 300,
                                   24, created, nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(hidden_text_child != nullptr);
             })
             .ok());
  check(runtime,
        "import { settings, windows } from 'rime:window';\n"
        "globalThis.s5t = {};\n"
        "(async () => {\n"
        "  const out = {};\n"
        "  out.textOff = await windows.text(" + id_text + ");\n"
        "  out.controls = (await windows.controls(" + id_text + ")).map(c => c.classNN).join(',');\n"
        "  out.hiddenBack = settings.window.setDetectHiddenText(true);\n"
        "  out.textOn = await windows.text(" + id_text + ");\n"
        "  settings.window.detectHiddenText = false;\n"
        "  out.textRestored = await windows.text(" + id_text + ");\n"
        "  return out;\n"
        "})().then(v => { globalThis.s5t.value = v; },\n"
        "         e => { globalThis.s5t.error = String(e); });",
        "slice-text-settings.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.s5t.error) throw new Error(globalThis.s5t.error);\n"
        "const v = globalThis.s5t.value;\n"
        "if (!v) throw new Error('segment 5d-text produced no value');\n"
        "if (v.textOff.includes('Rime Hidden Slice Text'))\n"
        "  throw new Error('hidden text must stay excluded by default');\n"
        "if (!v.controls.split(',').includes('Edit1') ||\n"
        "    !v.controls.split(',').includes('Static1'))\n"
        "  throw new Error('controls must count hidden children: ' + v.controls);\n"
        "if (v.hiddenBack !== false)\n"
        "  throw new Error('detectHiddenText must start false');\n"
        "if (!v.textOn.includes('Rime Hidden Slice Text'))\n"
        "  throw new Error('hidden text must appear when enabled');\n"
        "if (v.textRestored.includes('Rime Hidden Slice Text'))\n"
        "  throw new Error('detectHiddenText restore failed');",
        "slice-text-settings-check.mjs");

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

  // A cancellation armed before the queued action executes settles its
  // promise as cancelled and never reaches the executor: queued work is
  // cancellable, not fire-and-forget.
  check(runtime,
        "import { runtime } from 'rime:runtime';\n"
        "import { windows } from 'rime:window';\n"
        "const cid = runtime.cancellation();\n"
        "runtime.cancel(cid);\n"
        "globalThis.prematureFailure = null;\n"
        "globalThis.prematureErrorCode = null;\n"
        "windows.move(" + id_text +
            ", 'left', { cancellationId: cid })\n"
            "  .then(() => { globalThis.prematureFailure = 'unexpected resolution'; },\n"
            "        e => { globalThis.prematureFailure = String(e);\n"
            "               globalThis.prematureErrorCode = e.code; });",
        "slice-premature.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (!globalThis.prematureFailure.includes('cancelled'))\n"
        "  throw new Error('wrong cancellation rejection: ' + globalThis.prematureFailure);\n"
        "if (globalThis.prematureErrorCode !== 'cancelled')\n"
        "  throw new Error('cancellation rejection must carry cancelled: ' +\n"
        "                  globalThis.prematureErrorCode);",
        "slice-premature-check.mjs");

  // Trace, grouped by action type (no fragile global totals). Sources:
  // - window.focus/hide/show/minimize/maximize/restore/close: one Started +
  //   one Finished each (segments 3/5/5b/5c/6);
  // - window.move: segment 1 (move), segment 2 (bad placement), segment 2b
  //   (exhausted deadline), segment 6 (stale id), plus the optional active
  //   move. The deadline action always records Finished but records Started
  //   only when it survives kernel pre-dispatch, so Started is base/base+1
  //   and Finished is Started/Started+1.
  // - window.settings: segment 5d's settings writes record StateChanged
  //   once per actual change (2 mode, 2 speed, 2 detectHiddenWindows,
  //   2 detectHiddenText); the no-change double set stays silent.
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

  // Segment 5d's settings writes are the only StateChanged entries: one per
  // actual change, all on window.settings, detail "key before -> after".
  std::map<std::string, std::size_t> settings_changes;
  for (const auto& entry : trace->snapshot()) {
    if (entry.kind != rime::core::TraceKind::StateChanged) continue;
    assert(entry.subject == "window.settings");
    const auto split = entry.detail.find(' ');
    assert(split != std::string::npos);
    ++settings_changes[entry.detail.substr(0, split)];
  }
  assert(settings_changes["titleMatchMode"] == 2);       // 2 -> 3 -> 2
  assert(settings_changes["titleMatchModeSpeed"] == 2);  // Fast -> Slow -> Fast
  assert(settings_changes["detectHiddenWindows"] == 2);  // false -> true -> false
  assert(settings_changes["detectHiddenText"] == 2);     // false -> true -> false
  assert(settings_changes.size() == 4);

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
    // The settings surface enforces capabilities synchronously: reads name
    // windows.window.read, writes name windows.window.write, both as
    // TypeErrors instead of queue hops.
    check(denied_runtime,
          "import { settings } from 'rime:window';\n"
          "globalThis.settingsReadDenied = null;\n"
          "globalThis.settingsWriteDenied = null;\n"
          "try { globalThis.settingsReadDenied = 'got:' + settings.window.titleMatchMode; }\n"
          "catch (e) { globalThis.settingsReadDenied = String(e); }\n"
          "try { settings.window.titleMatchMode = '3';\n"
          "      globalThis.settingsWriteDenied = 'unexpected success'; }\n"
          "catch (e) { globalThis.settingsWriteDenied = String(e); }",
          "slice-deny-settings.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    check(denied_runtime,
          "if (!globalThis.settingsReadDenied ||\n"
          "    !globalThis.settingsReadDenied.includes('windows.window.read'))\n"
          "  throw new Error('settings read must name the capability: ' +\n"
          "                  globalThis.settingsReadDenied);\n"
          "if (!globalThis.settingsWriteDenied ||\n"
          "    !globalThis.settingsWriteDenied.includes('windows.window.write'))\n"
          "  throw new Error('settings write must name the capability: ' +\n"
          "                  globalThis.settingsWriteDenied);",
          "slice-deny-settings-check.mjs");

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

    // After close the same mutation is refused with invalid_state: teardown
    // order is observable from JS instead of hanging the promise.
    denied_dispatcher.close();
    check(denied_runtime,
          "import { windows } from 'rime:window';\n"
          "globalThis.closedFailure = null;\n"
          "globalThis.closedErrorCode = null;\n"
          "windows.move(" + id_text +
              ", 'left')\n"
              "  .then(() => { globalThis.closedFailure = 'unexpected resolution'; },\n"
              "        e => { globalThis.closedFailure = String(e);\n"
              "               globalThis.closedErrorCode = e.code; });",
          "slice-closed.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    check(denied_runtime,
          "if (!globalThis.closedFailure.includes('dispatcher closed'))\n"
          "  throw new Error('wrong closed rejection: ' + globalThis.closedFailure);\n"
          "if (globalThis.closedErrorCode !== 'invalid_state')\n"
          "  throw new Error('closed rejection must carry invalid_state: ' +\n"
          "                  globalThis.closedErrorCode);",
          "slice-closed-check.mjs");
    assert(denied_runtime.stop().ok());
  }

  // Queue-policy rejection: a capacity-0 dispatcher refuses before enqueue,
  // so the promise settles with queue_full instead of bypassing the queue.
  {
    rime::action::Kernel full_kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
        std::unordered_set<std::string>{"windows.window.read", "windows.window.write"}));
    rime::action::Dispatcher full_dispatcher(full_kernel, 0);
    rime::win32::WindowModuleBinding full_binding{&service, &full_kernel, &full_dispatcher,
                                                   &next_action_id};
    rime::js::Runtime full_runtime;
    assert(rime::win32::register_window_module(full_runtime, &full_binding).ok());
    assert(full_runtime.start().ok());
    check(full_runtime,
          "import { windows } from 'rime:window';\n"
          "globalThis.fullFailure = null;\n"
          "globalThis.fullErrorCode = null;\n"
          "windows.move(" + id_text +
              ", 'left')\n"
              "  .then(() => { globalThis.fullFailure = 'unexpected resolution'; },\n"
              "        e => { globalThis.fullFailure = String(e);\n"
              "               globalThis.fullErrorCode = e.code; });",
          "slice-queue-full.mjs");
    assert(full_runtime.settle(5000ms).ok());
    check(full_runtime,
          "if (!globalThis.fullFailure.includes('action queue is full'))\n"
          "  throw new Error('wrong queue_full rejection: ' + globalThis.fullFailure);\n"
          "if (globalThis.fullErrorCode !== 'queue_full')\n"
          "  throw new Error('queue_full rejection must carry queue_full: ' +\n"
          "                  globalThis.fullErrorCode);",
          "slice-queue-full-check.mjs");
    assert(full_runtime.stop().ok());
  }

  assert(service.ui()
             .call([&] {
               if (hidden_text_child) DestroyWindow(hidden_text_child);
               if (hidden_helper) DestroyWindow(hidden_helper);
             })
             .ok());

  assert(service.stop().ok());
  return 0;
}
