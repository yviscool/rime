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
                           "window.restore", "window.zorder", "window.kill", "window.redraw",
                           "window.group.add", "window.group.activate",
                           "window.group.deactivate", "window.group.close",
                           "window.minimizeall", "window.minimizeall.undo",
                           "window.set.title", "window.set.enabled",
                           "window.set.alwaysontop", "window.set.style",
                           "window.set.exstyle", "window.set.transparent",
                           "window.set.transcolor", "window.set.region"}) {
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
        "if (!Array.isArray(globalThis.controlList))\n"
        "  throw new Error('controls must resolve to an array');\n"
        // An IME that attaches when the slice edit gains focus adds its own
        // notification child, so look the edit up instead of counting.
        "const control = globalThis.controlList.find(c => c.classNN === 'Edit1');\n"
        "if (!control)\n"
        "  throw new Error('expected Edit1 among controls: ' +\n"
        "                  JSON.stringify(globalThis.controlList));\n"
        "if (control.className !== 'Edit')\n"
        "  throw new Error('Edit1 must be an Edit');\n"
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

  // WinWait family (WinWait/WinWaitActive/WinWaitClose/WinWaitNotActive via
  // `until`): the loop polls on the scheduler and never blocks a thread; an
  // already-satisfied condition settles on the first check (snapshot for
  // exists, null for closed/notActive — notActive is satisfied by a query
  // nothing matches). deadlineMs is the whole wait budget: the 150ms wait
  // rejects with code timeout only after the deadline, and a cancellation
  // bound to the loop rejects with code cancelled (CancelById races the
  // first poll; both paths report the same code). wait is a read: it adds
  // no Action Trace (trace groups below).
  check(runtime,
        "import { runtime } from 'rime:runtime';\n"
        "import { windows } from 'rime:window';\n"
        "globalThis.waitOut = {};\n"
        "globalThis.waitDeadlineStart = Date.now();\n"
        "const title = 'Rime Vertical Slice Window';\n"
        "const never = 'No Such Window Anywhere In This Test';\n"
        "windows.wait({ title, until: 'exists' })\n"
        "  .then(w => { globalThis.waitOut.exists = w; },\n"
        "        e => { globalThis.waitOut.existsErr = e.code; });\n"
        "windows.wait({ title: never, until: 'closed' })\n"
        "  .then(w => { globalThis.waitOut.closed = w; },\n"
        "        e => { globalThis.waitOut.closedErr = e.code; });\n"
        "windows.wait({ title: never, until: 'notActive' })\n"
        "  .then(w => { globalThis.waitOut.notActive = w; },\n"
        "        e => { globalThis.waitOut.notActiveErr = e.code; });\n"
        "windows.wait({ title: never, until: 'exists', deadlineMs: 150 })\n"
        "  .then(() => { globalThis.waitOut.timeout = 'resolved'; },\n"
        "        e => { globalThis.waitOut.timeout = e.code; });\n"
        "const cid = runtime.cancellation();\n"
        "windows.wait({ title: never, until: 'exists', cancellationId: cid, deadlineMs: 30000 })\n"
        "  .then(() => { globalThis.waitOut.cancel = 'resolved'; },\n"
        "        e => { globalThis.waitOut.cancel = e.code; });\n"
        "runtime.cancel(cid);",
        "slice-wait.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "const out = globalThis.waitOut;\n"
        "if (out.existsErr || !out.exists)\n"
        "  throw new Error('wait exists rejected: ' + out.existsErr);\n"
        "if (out.exists.id !== " + id_text + ")\n"
        "  throw new Error('wait exists resolved the wrong window');\n"
        "if (out.closedErr || out.closed !== null)\n"
        "  throw new Error('wait closed must resolve null, got: ' +\n"
        "                  (out.closedErr || JSON.stringify(out.closed)));\n"
        "if (out.notActiveErr || out.notActive !== null)\n"
        "  throw new Error('wait notActive must resolve null, got: ' +\n"
        "                  (out.notActiveErr || JSON.stringify(out.notActive)));\n"
        "if (out.timeout !== 'timeout')\n"
        "  throw new Error('wait deadline must reject with timeout, got: ' + out.timeout);\n"
        "if (Date.now() - globalThis.waitDeadlineStart < 140)\n"
        "  throw new Error('wait timeout returned before its deadline');\n"
        "if (out.cancel !== 'cancelled')\n"
        "  throw new Error('wait cancel must reject with cancelled, got: ' + out.cancel);",
        "slice-wait-check.mjs");

  // Window groups (GroupAdd/GroupActivate/GroupDeactivate/GroupClose): two
  // disposable victims back the group. Names and modes are validated
  // synchronously by the module; the executor enforces the payload
  // contract; the service owns the registry on the UI lane. Dedup, the
  // create-if-missing activate and the missing-group InvalidContract are
  // exact; close-all counts exactly because the pair lives only in this
  // group. Focus-dependent outcomes stay out (foreground lock). These are
  // writes: they record per-type trace pairs, which only the grouped trace
  // section observes.
  HWND group_slice_a = nullptr;
  HWND group_slice_b = nullptr;
  assert(service.ui()
             .call([&] {
               group_slice_a = CreateWindowExW(0, L"STATIC", L"Rime Group Slice A",
                                               WS_OVERLAPPED | WS_VISIBLE, 160, 160, 320, 240,
                                               nullptr, nullptr, GetModuleHandleW(nullptr),
                                               nullptr);
               group_slice_b = CreateWindowExW(0, L"STATIC", L"Rime Group Slice B",
                                               WS_OVERLAPPED | WS_VISIBLE, 200, 200, 320, 240,
                                               nullptr, nullptr, GetModuleHandleW(nullptr),
                                               nullptr);
               assert(group_slice_a != nullptr);
               assert(group_slice_b != nullptr);
             })
             .ok());
  check(runtime,
        "import { groups } from 'rime:window';\n"
        "globalThis.groupOut = {};\n"
        "const spec = { title: 'Rime Group Slice', matchMode: 'startswith' };\n"
        // The duplicate probe waits for the first add to finish: two identical
        // adds in flight would let the dispatcher's same-args key merge
        // supersede the older one, which is timing-dependent.
        "groups.add('slice_group', spec)\n"
        "  .then(r => { globalThis.groupOut.add = r.count; },\n"
        "        e => { globalThis.groupOut.addErr = String(e); })\n"
        "  .then(() => groups.add('slice_group', spec))\n"
        "  .then(r => { globalThis.groupOut.dup = r.count; },\n"
        "        e => { globalThis.groupOut.dupErr = String(e); });\n"
        "groups.activate('slice_missing')\n"
        "  .then(w => { globalThis.groupOut.missing = w; },\n"
        "        e => { globalThis.groupOut.missingErr = String(e); });\n"
        "groups.deactivate('slice_never')\n"
        "  .then(w => { globalThis.groupOut.noGroup = w; },\n"
        "        e => { globalThis.groupOut.noGroupCode = e.code; });\n"
        "groups.close('slice_never', 'all')\n"
        "  .then(r => { globalThis.groupOut.noClose = r; },\n"
        "        e => { globalThis.groupOut.noCloseCode = e.code; });",
        "slice-group-add.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "const g = globalThis.groupOut;\n"
        "if (g.addErr) throw new Error('group add rejected: ' + g.addErr);\n"
        "if (g.add !== 1) throw new Error('first group add must count 1, got: ' + g.add);\n"
        "if (g.dupErr) throw new Error('duplicate group add rejected: ' + g.dupErr);\n"
        "if (g.dup !== 1) throw new Error('duplicate spec must stay at 1, got: ' + g.dup);\n"
        "if (g.missingErr) throw new Error('missing-group activate rejected: ' + g.missingErr);\n"
        "if (g.missing !== null)\n"
        "  throw new Error('create-if-missing activate must resolve null, got: ' +\n"
        "                  JSON.stringify(g.missing));\n"
        "if (g.noGroupCode !== 'invalid_contract')\n"
        "  throw new Error('missing-group deactivate must reject invalid_contract, got: ' +\n"
        "                  g.noGroupCode);\n"
        "if (g.noCloseCode !== 'invalid_contract')\n"
        "  throw new Error('missing-group close must reject invalid_contract, got: ' +\n"
        "                  g.noCloseCode);",
        "slice-group-add-check.mjs");
  check(runtime,
        "import { groups } from 'rime:window';\n"
        "globalThis.groupCloseAll = null;\n"
        "globalThis.groupCloseAllErr = null;\n"
        "groups.close('slice_group', 'all')\n"
        "  .then(r => { globalThis.groupCloseAll = r; },\n"
        "        e => { globalThis.groupCloseAllErr = String(e); });",
        "slice-group-closeall.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.groupCloseAllErr)\n"
        "  throw new Error('group close all rejected: ' + globalThis.groupCloseAllErr);\n"
        "const r = globalThis.groupCloseAll;\n"
        "if (!r || r.closed !== 2)\n"
        "  throw new Error('group close all must close exactly 2, got: ' +\n"
        "                  JSON.stringify(r));\n"
        "if (r.activated !== null)\n"
        "  throw new Error('group close all must not activate, got: ' +\n"
        "                  JSON.stringify(r.activated));",
        "slice-group-closeall-check.mjs");
  std::vector<WindowInfo> group_slice_left;
  rime::win32::WindowQuery group_slice_query;
  group_slice_query.title = "Rime Group Slice";
  group_slice_query.title_match_mode = rime::win32::TitleMatchMode::StartsWith;
  assert(service.query(group_slice_query, group_slice_left).ok());
  assert(group_slice_left.empty());

  // z-order writes (WinMoveTop/WinMoveBottom) and the WinActivateBottom
  // composition: list() keeps the EnumWindows top-to-bottom order, so the
  // last match of a query is the bottom-most window AHK would activate.
  HWND zslice_a = nullptr;
  HWND zslice_b = nullptr;
  assert(service.ui()
             .call([&] {
               zslice_a = CreateWindowExW(0, L"STATIC", L"Rime ZSlice A",
                                          WS_OVERLAPPED | WS_VISIBLE, 40, 140, 160, 120, nullptr,
                                          nullptr, GetModuleHandleW(nullptr), nullptr);
               zslice_b = CreateWindowExW(0, L"STATIC", L"Rime ZSlice B",
                                          WS_OVERLAPPED | WS_VISIBLE, 240, 140, 160, 120, nullptr,
                                          nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(zslice_a != nullptr);
               assert(zslice_b != nullptr);
             })
             .ok());
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.zorderOut = {};\n"
        "(async () => {\n"
        "  const spec = { title: 'Rime ZSlice', matchMode: 'startswith' };\n"
        "  const initial = await windows.list(spec);\n"
        "  const a = initial.find(w => w.title === 'Rime ZSlice A');\n"
        "  const b = initial.find(w => w.title === 'Rime ZSlice B');\n"
        "  if (!a || !b) throw new Error('zorder victims must both be listed');\n"
        "  await windows.zorder(a.id, 'bottom');\n"
        "  const sank = await windows.list(spec);\n"
        "  if (sank.length !== 2 || sank[0].id !== b.id || sank[1].id !== a.id)\n"
        "    throw new Error('A must sink below B: ' +\n"
        "                    JSON.stringify(sank.map(w => w.title)));\n"
        "  await windows.zorder(a.id, 'top');\n"
        "  const risen = await windows.list(spec);\n"
        "  if (risen.length !== 2 || risen[0].id !== a.id || risen[1].id !== b.id)\n"
        "    throw new Error('A must rise above B: ' +\n"
        "                    JSON.stringify(risen.map(w => w.title)));\n"
        "  let syncErr = null;\n"
        "  try { windows.zorder(a.id, 'middle'); }\n"
        "  catch (e) { syncErr = String(e); }\n"
        "  if (!syncErr || !syncErr.includes('placement'))\n"
        "    throw new Error('bad placement must throw synchronously: ' + syncErr);\n"
        // WinActivateBottom: focus the last match. SetForegroundWindow may
        // be refused by the foreground lock, so only the success path is
        // asserted back in the checker.
        "  const bottomMost = risen[risen.length - 1];\n"
        "  let focusErr = null;\n"
        "  try { await windows.focus(bottomMost.id); }\n"
        "  catch (e) { focusErr = String(e); }\n"
        "  const act = await windows.active();\n"
        "  return { actedOn: bottomMost.id, activeId: act ? act.id : null, focusErr };\n"
        "})().then(v => { globalThis.zorderOut.value = v; },\n"
        "         e => { globalThis.zorderOut.error = String(e); });",
        "slice-zorder.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.zorderOut.error)\n"
        "  throw new Error(globalThis.zorderOut.error);\n"
        "const v = globalThis.zorderOut.value;\n"
        "if (!v) throw new Error('zorder segment produced no value');\n"
        "if (v.focusErr === null && v.activeId !== v.actedOn)\n"
        "  throw new Error('activating the bottom-most match must focus it: ' +\n"
        "                  JSON.stringify(v));",
        "slice-zorder-check.mjs");
  rime::win32::WindowQuery zslice_query;
  zslice_query.title = "Rime ZSlice";
  zslice_query.title_match_mode = rime::win32::TitleMatchMode::StartsWith;
  std::vector<WindowInfo> zslice_left;
  assert(service.query(zslice_query, zslice_left).ok());
  for (const auto& leftover : zslice_left) {
    assert(service.close(leftover.id).ok());
  }

  // redraw round-trips the unchanged snapshot; kill force-closes through
  // the executor (same-process WM_CLOSE path) and the id goes stale.
  HWND kr_victim = nullptr;
  assert(service.ui()
             .call([&] {
               kr_victim = CreateWindowExW(0, L"STATIC", L"Rime KR Slice",
                                           WS_OVERLAPPED | WS_VISIBLE, 80, 300, 200, 140,
                                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(kr_victim != nullptr);
             })
             .ok());
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.krOut = {};\n"
        "(async () => {\n"
        "  const spec = { title: 'Rime KR Slice', matchMode: 'startswith' };\n"
        "  const [w] = await windows.list(spec);\n"
        "  if (!w) throw new Error('kill/redraw victim must be listed');\n"
        "  const redrawSnap = await windows.redraw(w.id);\n"
        "  if (redrawSnap.id !== w.id || !redrawSnap.visible)\n"
        "    throw new Error('redraw must resolve the unchanged snapshot');\n"
        "  const killSnap = await windows.kill(w.id);\n"
        "  if (killSnap.id !== w.id)\n"
        "    throw new Error('kill must resolve the pre-close snapshot');\n"
        "  try {\n"
        "    await windows.info(w.id);\n"
        "    throw new Error('info on the killed id must reject');\n"
        "  } catch (e) {\n"
        "    if (!String(e).includes('window no longer exists')) throw e;\n"
        "  }\n"
        "  return { killed: w.id };\n"
        "})().then(v => { globalThis.krOut.value = v; },\n"
        "         e => { globalThis.krOut.error = String(e); });",
        "slice-kill-redraw.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.krOut.error) throw new Error(globalThis.krOut.error);\n"
        "if (!globalThis.krOut.value || !globalThis.krOut.value.killed)\n"
        "  throw new Error('kill/redraw segment produced no value');",
        "slice-kill-redraw-check.mjs");
  rime::win32::WindowQuery kr_leftover_query;
  kr_leftover_query.title = "Rime KR Slice";
  kr_leftover_query.title_match_mode = rime::win32::TitleMatchMode::StartsWith;
  std::vector<WindowInfo> kr_leftover;
  assert(service.query(kr_leftover_query, kr_leftover).ok());
  assert(kr_leftover.empty());

  // minimizeall posts the shell tray command: a dedicated full-overlapped
  // victim minimizes, then the undo restores it (the shell skips borderless
  // windows, and the main slice window is borderless). Both effects are
  // polled with runtime.delay; the undo runs in a finally so a failure
  // never leaves the desktop minimized. The settle budget covers both 3s
  // polling rounds.
  HWND ma_victim = nullptr;
  assert(service.ui()
             .call([&] {
               ma_victim = CreateWindowExW(0, L"STATIC", L"Rime MASlice Victim",
                                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 360, 340, 240, 160,
                                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(ma_victim != nullptr);
             })
             .ok());
  // Freshly created windows need a beat before the shell's minimize pass.
  std::this_thread::sleep_for(200ms);
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "import { runtime } from 'rime:runtime';\n"
        "globalThis.maOut = {};\n"
        "(async () => {\n"
        "  const until = async (checkFn, timeoutMs) => {\n"
        "    const deadline = Date.now() + timeoutMs;\n"
        "    for (;;) {\n"
        "      if (await checkFn()) return true;\n"
        "      if (Date.now() >= deadline) return false;\n"
        "      await runtime.delay(20, null);\n"
        "    }\n"
        "  };\n"
        "  const [victim] = await windows.list({ title: 'Rime MASlice Victim',\n"
        "                                        matchMode: 'startswith' });\n"
        "  if (!victim) throw new Error('minimizeall victim must be listed');\n"
        "  const before = await windows.info(victim.id);\n"
        "  let minimized = false;\n"
        "  let restored = false;\n"
        "  try {\n"
        "    await windows.minimizeAll();\n"
        "    minimized = await until(\n"
        "      async () => (await windows.info(victim.id)).minimized, 3000);\n"
        "  } finally {\n"
        "    await windows.minimizeAllUndo();\n"
        "    restored = await until(\n"
        "      async () => !(await windows.info(victim.id)).minimized, 3000);\n"
        "  }\n"
        "  return { wasMinimized: before.minimized, minimized, restored };\n"
        "})().then(v => { globalThis.maOut.value = v; },\n"
        "         e => { globalThis.maOut.error = String(e); });",
        "slice-minimizeall.mjs");
  assert(runtime.settle(15000ms).ok());
  check(runtime,
        "if (globalThis.maOut.error) throw new Error(globalThis.maOut.error);\n"
        "const v = globalThis.maOut.value;\n"
        "if (!v) throw new Error('minimizeall segment produced no value');\n"
        "if (v.wasMinimized) throw new Error('minimizeall victim started minimized');\n"
        "if (!v.minimized) throw new Error('minimizeAll did not minimize the victim');\n"
        "if (!v.restored) throw new Error('minimizeAllUndo did not restore the victim');",
        "slice-minimizeall-check.mjs");
  rime::win32::WindowQuery ma_query;
  ma_query.title = "Rime MASlice Victim";
  ma_query.title_match_mode = rime::win32::TitleMatchMode::StartsWith;
  std::vector<WindowInfo> ma_leftover;
  assert(service.query(ma_query, ma_leftover).ok());
  assert(ma_leftover.size() == 1);
  assert(service.ui().call([&] { DestroyWindow(ma_victim); }).ok());

  // window.set.* writes on a dedicated victim: topmost toggles, enabled
  // disables then toggles back, the title round-trips, style/exstyle bits
  // add and clear, layered alpha and the color key set and clear, and the
  // region shape lands (observed through the info snapshot's region field)
  // then clears again. The finally block restores every value before the
  // query check, and the synchronous argument TypeErrors dispatch nothing
  // (so the trace asserts below stay at two actions per type).
  HWND set_victim = nullptr;
  assert(service.ui()
             .call([&] {
               set_victim =
                   CreateWindowExW(0, L"STATIC", L"Rime SetSlice Victim",
                                   WS_OVERLAPPEDWINDOW | WS_VISIBLE, 440, 340, 240, 160,
                                   nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(set_victim != nullptr);
             })
             .ok());
  // Freshly created windows need a beat: SetWindowPos topmost does not
  // stick on a window that has not finished its initial show.
  std::this_thread::sleep_for(200ms);
  rime::win32::WindowQuery set_pre_query;
  set_pre_query.title = "Rime SetSlice Victim";
  set_pre_query.title_match_mode = rime::win32::TitleMatchMode::StartsWith;
  std::vector<WindowInfo> set_pre_match;
  assert(service.query(set_pre_query, set_pre_match).ok());
  assert(set_pre_match.size() == 1);
  const WindowInfo set_pre = set_pre_match.front();
  check(runtime,
        "import { windows } from 'rime:window';\n"
        "globalThis.setOut = {};\n"
        "(async () => {\n"
        "  const [victim] = await windows.list({ title: 'Rime SetSlice Victim',\n"
        "                                        matchMode: 'startswith' });\n"
        "  if (!victim) throw new Error('set victim must be listed');\n"
        "  const before = await windows.info(victim.id);\n"
        "  let typeErrors = 0;\n"
        "  try { windows.setTitle(victim.id, 42); }\n"
        "  catch (e) { if (e instanceof TypeError) typeErrors++; }\n"
        "  try { windows.setEnabled(victim.id, 7); }\n"
        "  catch (e) { if (e instanceof TypeError) typeErrors++; }\n"
        "  try { windows.setStyle(victim.id, 123); }\n"
        "  catch (e) { if (e instanceof TypeError) typeErrors++; }\n"
        "  try { windows.setTransparent(victim.id, 300); }\n"
        "  catch (e) { if (e instanceof TypeError) typeErrors++; }\n"
        "  try { windows.setRegion(victim.id, 7); }\n"
        "  catch (e) { if (e instanceof TypeError) typeErrors++; }\n"
        "  const out = {};\n"
        "  try {\n"
        "    await windows.setTitle(victim.id, 'Rime SetSlice Renamed');\n"
        "    out.title = (await windows.info(victim.id)).title;\n"
        "    await windows.setEnabled(victim.id, false);\n"
        "    out.disabled = !(await windows.info(victim.id)).enabled;\n"
        "    await windows.setEnabled(victim.id, -1);\n"
        "    out.reenabled = (await windows.info(victim.id)).enabled;\n"
        "    await windows.setAlwaysOnTop(victim.id);\n"
        "    out.topmost = (await windows.info(victim.id)).alwaysOnTop;\n"
        "    await windows.setStyle(victim.id, '+0x02000000');\n"
        "    out.styleSet = ((await windows.info(victim.id)).style & 0x02000000) !== 0;\n"
        "    await windows.setExStyle(victim.id, '+0x08000000');\n"
        "    out.exSet = ((await windows.info(victim.id)).exStyle & 0x08000000) !== 0;\n"
        "    await windows.setTransparent(victim.id, 0x80);\n"
        "    out.alpha = (await windows.info(victim.id)).transparent;\n"
        "    await windows.setTransColor(victim.id, '0xFF0000 100');\n"
        "    out.tcol = (await windows.info(victim.id)).transColor;\n"
        "    out.tcolAlpha = (await windows.info(victim.id)).transparent;\n"
        "    await windows.setRegion(victim.id, '10-10 W100 H50');\n"
        "    out.region = (await windows.info(victim.id)).region;\n"
        "  } finally {\n"
        "    await windows.setTitle(victim.id, 'Rime SetSlice Victim');\n"
        "    await windows.setAlwaysOnTop(victim.id, 0);\n"
        "    await windows.setStyle(victim.id, '0x' + before.style.toString(16));\n"
        "    await windows.setExStyle(victim.id, '0x' + before.exStyle.toString(16));\n"
        "    await windows.setTransparent(victim.id, before.transparent);\n"
        "    await windows.setTransColor(victim.id, before.transColor || 'off');\n"
        "    await windows.setRegion(victim.id, '');\n"
        "  }\n"
        "  out.regionCleared = (await windows.info(victim.id)).region === '';\n"
        "  return { beforeTopmost: before.alwaysOnTop, typeErrors, ...out };\n"
        "})().then(v => { globalThis.setOut.value = v; },\n"
        "         e => { globalThis.setOut.error = String(e); });",
        "slice-set.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.setOut.error) throw new Error(globalThis.setOut.error);\n"
        "const v = globalThis.setOut.value;\n"
        "if (!v) throw new Error('set segment produced no value');\n"
        "if (v.beforeTopmost) throw new Error('set victim started topmost');\n"
        "if (v.typeErrors !== 5 || !v.topmost || !v.disabled || !v.reenabled ||\n"
        "    v.title !== 'Rime SetSlice Renamed' || !v.styleSet || !v.exSet ||\n"
        "    v.alpha !== 128 || v.tcol !== '0xFF0000' || v.tcolAlpha !== 100 ||\n"
        "    v.region !== '10,10,110,60' || !v.regionCleared)\n"
        "  throw new Error('set segment failed: ' + JSON.stringify(v));",
        "slice-set-check.mjs");
  rime::win32::WindowQuery set_query;
  set_query.title = "Rime SetSlice Victim";
  set_query.title_match_mode = rime::win32::TitleMatchMode::StartsWith;
  std::vector<WindowInfo> set_leftover;
  assert(service.query(set_query, set_leftover).ok());
  assert(set_leftover.size() == 1);
  assert(set_leftover.front().title == "Rime SetSlice Victim");
  assert(!set_leftover.front().always_on_top);
  // The finally block restored every set-stage write: style, exstyle,
  // layered alpha, the color key and the region all match the pre-script
  // snapshot (the region restore leaves no region at all).
  assert(set_leftover.front().style == set_pre.style);
  assert(set_leftover.front().ex_style == set_pre.ex_style);
  assert(set_leftover.front().transparent == set_pre.transparent);
  assert(set_leftover.front().trans_color == set_pre.trans_color);
  assert(set_leftover.front().region.empty());
  assert(service.ui().call([&] { DestroyWindow(set_victim); }).ok());

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

  // A cancellation armed on the window READ entry rejects the pending
  // promise with code cancelled: reads skip the dispatcher, so this covers
  // the host async binding (begin_async) instead of kernel pre-dispatch --
  // the write-entry premature refusal above covers the kernel side.
  check(runtime,
        "import { runtime } from 'rime:runtime';\n"
        "import { windows } from 'rime:window';\n"
        "const cid = runtime.cancellation();\n"
        "globalThis.readCancelledOutcome = null;\n"
        "globalThis.readCancelledCode = null;\n"
        "globalThis.readCancelledMessage = null;\n"
        "windows.list({ cancellationId: cid })\n"
        "  .then(() => { globalThis.readCancelledOutcome = 'resolved'; },\n"
        "        e => { globalThis.readCancelledOutcome = 'rejected';\n"
        "               globalThis.readCancelledCode = e.code;\n"
        "               globalThis.readCancelledMessage = e.message; });\n"
        "runtime.cancel(cid);",
        "slice-read-cancel.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.readCancelledOutcome !== 'rejected')\n"
        "  throw new Error('cancelled read must reject, got: ' +\n"
        "                  globalThis.readCancelledOutcome);\n"
        "if (globalThis.readCancelledCode !== 'cancelled')\n"
        "  throw new Error('read cancel code: ' + globalThis.readCancelledCode);\n"
        "if (globalThis.readCancelledMessage !== 'cancelled')\n"
        "  throw new Error('read cancel message: ' + globalThis.readCancelledMessage);",
        "slice-read-cancel-check.mjs");

  // Trace, grouped by action type (no fragile global totals). Sources:
  // - window.hide/show/minimize/maximize/restore/close: one Started +
  //   one Finished each (segments 3/5/5b/5c/6);
  // - window.focus: two pairs - segment 3 plus the zorder segment's
  //   WinActivateBottom composition (a foreground-lock refusal still
  //   records both);
  // - window.zorder: two pairs (bottom then top) in the zorder segment;
  // - window.kill / window.redraw: one pair each in the kill-redraw segment;
  // - window.minimizeall / window.minimizeall.undo: one pair each in the
  //   minimizeall segment (fire-and-forget shell tray post);
  // - window.set.title / window.set.enabled / window.set.alwaysontop /
  //   window.set.style / window.set.exstyle / window.set.transparent /
  //   window.set.transcolor / window.set.region: two pairs each in the set
  //   segment (write plus finally-restored write); the synchronous
  //   argument TypeErrors dispatch nothing;
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
  for (const char* type : {"window.hide", "window.show", "window.minimize",
                           "window.maximize", "window.restore", "window.close"}) {
    assert(started_count[type] == 1);
  }
  // focus: segment 3 plus the WinActivateBottom composition in the zorder
  // segment (a foreground-lock refusal still records the pair). zorder:
  // bottom then top; kill/redraw: one pair each.
  assert(started_count["window.focus"] == 2);
  assert(started_count["window.zorder"] == 2);
  assert(started_count["window.kill"] == 1);
  assert(started_count["window.redraw"] == 1);
  // minimizeall: minimize then undo, one pair each.
  assert(started_count["window.minimizeall"] == 1);
  assert(started_count["window.minimizeall.undo"] == 1);
  // set segment: one write plus one finally-restored write per type; the
  // sync argument TypeErrors never reach the dispatcher.
  assert(started_count["window.set.title"] == 2);
  assert(started_count["window.set.enabled"] == 2);
  assert(started_count["window.set.alwaysontop"] == 2);
  assert(started_count["window.set.style"] == 2);
  assert(started_count["window.set.exstyle"] == 2);
  assert(started_count["window.set.transparent"] == 2);
  assert(started_count["window.set.transcolor"] == 2);
  assert(started_count["window.set.region"] == 2);
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
