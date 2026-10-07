// Realism: L5 - production JS wiring (rime:ui module, real GuiService on a
// real UiThread pump) drives this machine's own modal dialogs and shell
// state: the MsgBox/InputBox with AHK's T option really open (a watcher
// thread finds the #32770 window by title while the runtime settles - a
// facade that answered "Timeout" without ever showing a dialog cannot pass),
// really close themselves on the WM_TIMER, and really hand back the AHK
// words ("Timeout") and the edit contents that InputBox.cpp returns on the
// timeout path. The tooltip's show/clear is observed by enumerating this
// process's own TOOLTIPS_CLASS windows, and the tray icon by the same
// independent NIM_MODIFY shell probe gui_tests uses - never by asking the
// service that created them. Nothing dispatches an Action, which the empty
// trace proves. The argument contract (non-string text, non-object options,
// fractional buttons, infinite timeout, non-boolean mute) throws before any
// worker starts. A second capability-less runtime proves the gate on all
// five calls: denied on the worker body, never reaching the pump. The
// dialogs' window-existence, X/ESC and click paths are covered at the
// service level by tests/native/gui_tests.cpp; this slice proves the
// JS promise pipeline end to end.

#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/gui.hpp"
#include "rime/win32/js_ui.hpp"
#include "rime/win32/ui_thread.hpp"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;
using rime::win32::GuiService;
using rime::win32::GuiModuleBinding;
using rime::win32::UiThread;

void require_ok(const rime::core::Error& error, const char* what) {
  if (!error.ok()) {
    std::fprintf(stderr, "%s failed: %s: %s\n", what,
                 rime::core::error_code_name(error.code), error.message.c_str());
    std::fflush(stderr);
    std::abort();
  }
}

void check(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js check failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::fflush(stderr);
    std::abort();
  }
}

void require(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "require failed: %s\n", what);
    std::fflush(stderr);
    std::abort();
  }
}

// Condition polling with a timeout: the dialog's appearance is the condition,
// never a bare sleep. Returns a joinable thread that sets *seen when the
// dialog with `title` exists (bounded at 5s, polling every 10ms).
std::thread dialog_watcher(const wchar_t* title, std::atomic<bool>* seen) {
  return std::thread([title, seen] {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (std::chrono::steady_clock::now() < until) {
      if (FindWindowW(L"#32770", title)) {
        seen->store(true);
        return;
      }
      Sleep(10);
    }
  });
}

int count_own_visible_tooltips() {
  int count = 0;
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto* out = reinterpret_cast<int*>(parameter);
        wchar_t class_name[64]{};
        if (GetClassNameW(window, class_name, 64) == 0) return TRUE;
        if (wcscmp(class_name, L"tooltips_class32") != 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        if (!IsWindowVisible(window)) return TRUE;
        ++*out;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&count));
  return count;
}

// Independent shell observation, same probe as gui_tests: NIM_MODIFY with
// NIF_TIP answers whether the icon with this uID exists right now.
bool tray_icon_present(HWND owner) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = owner;
  nid.uID = 1;
  nid.uFlags = NIF_TIP;
  lstrcpyW(nid.szTip, L"rime");
  return Shell_NotifyIconW(NIM_MODIFY, &nid) == TRUE;
}

}  // namespace

int main() {
  UiThread ui;
  require_ok(ui.start(), "UiThread starts");
  GuiService gui_service;
  gui_service.set_ui_thread(&ui);
  const HWND owner = reinterpret_cast<HWND>(ui.message_window());
  require(owner != nullptr, "pump exposes its message window");

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{"ui.create"}),
      trace);
  GuiModuleBinding binding{&gui_service, &kernel};

  rime::js::Runtime runtime;
  require_ok(rime::win32::register_ui_module(runtime, &binding), "ui module registration");
  require_ok(runtime.start(), "runtime start");

  // ---- argument contract: synchronous TypeErrors, no worker, no window ----
  check(runtime,
        "import { ui } from 'rime:ui';\n"
        "globalThis.argFailures = 'pending';\n"
        "const capture = (key, call) => {\n"
        "  try { call(); return 'resolved'; }\n"
        "  catch (e) { return e.constructor.name; }\n"
        "};\n"
        "globalThis.argFailures = {\n"
        "  text: capture('text', () => ui.msgBox(123)),\n"
        "  options: capture('options', () => ui.msgBox('x', 'bad')),\n"
        "  buttons: capture('buttons', () => ui.msgBox('x', { buttons: 1.5 })),\n"
        "  timeout: capture('timeout', () => ui.msgBox('x', { timeout: Infinity })),\n"
        "  mute: capture('mute', () => ui.trayTip('x', { mute: 1 })),\n"
        "};",
        "ui-args.mjs");
  check(runtime,
        "const got = globalThis.argFailures;\n"
        "if (typeof got !== 'object')\n"
        "  throw new Error('the argument contract must report: ' + JSON.stringify(got));\n"
        "for (const key of ['text', 'options', 'buttons', 'timeout', 'mute']) {\n"
        "  if (got[key] !== 'TypeError')\n"
        "    throw new Error(key + ' must throw a TypeError: ' + JSON.stringify(got));\n"
        "}",
        "ui-args-check.mjs");

  // ---- MsgBox: a real dialog that outlives its own 0.5s T option ----------
  {
    std::atomic<bool> seen{false};
    std::thread watcher = dialog_watcher(L"rime-ui-msgbox", &seen);
    check(runtime,
          "import { ui } from 'rime:ui';\n"
          "globalThis.msg = 'pending';\n"
          "globalThis.msgStart = Date.now();\n"
          "ui.msgBox('rime slice text', { title: 'rime-ui-msgbox', buttons: 0,\n"
          "                               timeout: 0.5 })\n"
          "  .then(r => { globalThis.msg = r; },\n"
          "        e => { globalThis.msg = 'error:' + e.code + ':' + e.message; });",
          "ui-msgbox.mjs");
    const auto settled = runtime.settle(10000ms);
    watcher.join();
    require_ok(settled, "msgBox settle");
    require(seen.load(), "the MsgBox dialog really appeared");
    check(runtime,
          "if (globalThis.msg !== 'Timeout')\n"
          "  throw new Error('a T=0.5 MsgBox must resolve \"Timeout\": ' +\n"
          "                   JSON.stringify(globalThis.msg));\n"
          "const elapsed = Date.now() - globalThis.msgStart;\n"
          "if (elapsed < 400)\n"
          "  throw new Error('the dialog must wait out its T option: ' + elapsed + 'ms');\n"
          "if (elapsed >= 9000)\n"
          "  throw new Error('the T option must close the dialog: ' + elapsed + 'ms');",
          "ui-msgbox-check.mjs");
  }

  // ---- InputBox: Timeout hands the edit contents back (InputBox.cpp:148) --
  {
    std::atomic<bool> seen{false};
    std::thread watcher = dialog_watcher(L"rime-ui-input", &seen);
    check(runtime,
          "import { ui } from 'rime:ui';\n"
          "globalThis.input = 'pending';\n"
          "globalThis.inputStart = Date.now();\n"
          "ui.inputBox('type here', { title: 'rime-ui-input', value: 'seed',\n"
          "                           timeout: 0.5 })\n"
          "  .then(r => { globalThis.input = r; },\n"
          "        e => { globalThis.input = 'error:' + e.code + ':' + e.message; });",
          "ui-input.mjs");
    const auto settled = runtime.settle(10000ms);
    watcher.join();
    require_ok(settled, "inputBox settle");
    require(seen.load(), "the InputBox dialog really appeared");
    check(runtime,
          "const got = globalThis.input;\n"
          "if (typeof got !== 'object')\n"
          "  throw new Error('inputBox must resolve an object: ' + JSON.stringify(got));\n"
          "if (got.result !== 'Timeout')\n"
          "  throw new Error('a T=0.5 InputBox must report Timeout: ' + JSON.stringify(got));\n"
          "if (got.value !== 'seed')\n"
          "  throw new Error('the edit contents must come back on Timeout: ' +\n"
          "                   JSON.stringify(got));\n"
          "const elapsed = Date.now() - globalThis.inputStart;\n"
          "if (elapsed < 400)\n"
          "  throw new Error('the dialog must wait out its T option: ' + elapsed + 'ms');",
          "ui-input-check.mjs");
  }

  // ---- ToolTip: show/clear observed through this process's own windows ----
  require(count_own_visible_tooltips() == 0, "no tooltip window exists at the start");
  check(runtime,
        "import { ui } from 'rime:ui';\n"
        "globalThis.tip = 'pending';\n"
        "ui.toolTip('rime slice tip', { x: 120, y: 120, index: 1 })\n"
        "  .then(r => { globalThis.tip = r; },\n"
        "        e => { globalThis.tip = 'error:' + e.code + ':' + e.message; });",
        "ui-tooltip.mjs");
  require_ok(runtime.settle(5000ms), "toolTip show settle");
  check(runtime,
        "if (globalThis.tip !== null)\n"
        "  throw new Error('toolTip must resolve null: ' + JSON.stringify(globalThis.tip));",
        "ui-tooltip-check.mjs");
  require(count_own_visible_tooltips() == 1, "the tooltip window exists and is visible");

  check(runtime,
        "import { ui } from 'rime:ui';\n"
        "globalThis.tipClear = 'pending';\n"
        "ui.toolTip('')\n"
        "  .then(r => { globalThis.tipClear = r; },\n"
        "        e => { globalThis.tipClear = 'error:' + e.code + ':' + e.message; });",
        "ui-tooltip-clear.mjs");
  require_ok(runtime.settle(5000ms), "toolTip clear settle");
  check(runtime,
        "if (globalThis.tipClear !== null)\n"
        "  throw new Error('clearing a tooltip must resolve null: ' +\n"
        "                   JSON.stringify(globalThis.tipClear));",
        "ui-tooltip-clear-check.mjs");
  require(count_own_visible_tooltips() == 0, "the tooltip window is gone after clear");

  // An out-of-range slot rejects with the service's own InvalidContract,
  // mapped to the kernel-style code name across the promise boundary.
  check(runtime,
        "import { ui } from 'rime:ui';\n"
        "globalThis.tipBad = null;\n"
        "ui.toolTip('x', { index: 21 })\n"
        "  .then(r => { globalThis.tipBad = 'resolved:' + JSON.stringify(r); },\n"
        "        e => { globalThis.tipBad = e; });",
        "ui-tooltip-bad.mjs");
  require_ok(runtime.settle(5000ms), "toolTip invalid settle");
  check(runtime,
        "const failure = globalThis.tipBad;\n"
        "if (typeof failure === 'string')\n"
        "  throw new Error('an out-of-range tooltip index must reject: ' + failure);\n"
        "if (!failure || failure.code !== 'invalid_contract')\n"
        "  throw new Error('the rejection must be invalid_contract: ' +\n"
        "                   JSON.stringify(failure && failure.code));",
        "ui-tooltip-bad-check.mjs");

  // ---- tray: shell-observed icon + balloon paths --------------------------
  require(!tray_icon_present(owner), "a fresh process has no tray icon yet");
  check(runtime,
        "import { ui } from 'rime:ui';\n"
        "globalThis.tray = 'pending';\n"
        "ui.traySetIcon('')\n"
        "  .then(r => { globalThis.tray = r; },\n"
        "        e => { globalThis.tray = 'error:' + e.code + ':' + e.message; });",
        "ui-tray.mjs");
  require_ok(runtime.settle(5000ms), "traySetIcon settle");
  check(runtime,
        "if (globalThis.tray !== null)\n"
        "  throw new Error('traySetIcon must resolve null: ' + JSON.stringify(globalThis.tray));",
        "ui-tray-check.mjs");
  require(tray_icon_present(owner), "the shell now reports the tray icon");

  check(runtime,
        "import { ui } from 'rime:ui';\n"
        "globalThis.balloon = 'pending';\n"
        "ui.trayTip('slice balloon', { title: 'rime slice', icon: 0x40, mute: true,\n"
        "                             freeze: false })\n"
        "  .then(r => { globalThis.balloon = r; },\n"
        "        e => { globalThis.balloon = 'error:' + e.code + ':' + e.message; });",
        "ui-balloon.mjs");
  require_ok(runtime.settle(5000ms), "trayTip settle");
  check(runtime,
        "if (globalThis.balloon !== null)\n"
        "  throw new Error('trayTip must resolve null: ' + JSON.stringify(globalThis.balloon));",
        "ui-balloon-check.mjs");

  // None of it dispatched an Action: the capability read is the whole audit
  // surface, so the trace must still be empty across every call above.
  require(trace->snapshot().empty(), "no Action was traced by any ui call");

  // Sweep the tray icon while the pump still runs (the same order Bootstrap
  // stop() uses), then prove the shell lost it - again through the probe.
  require_ok(gui_service.stop(), "gui service stop");
  require(!tray_icon_present(owner), "stop removed the tray icon from the shell");
  require_ok(runtime.stop(), "runtime stop");
  require_ok(ui.stop(), "UiThread stops");

  {
    // Capability-less runtime: all five calls are denied on their first
    // worker slice, before any dialog or shell call can happen. The JS lane
    // is process-wide, so this runtime only starts after the first one
    // released it (the same rule the sound slice follows).
    rime::action::Kernel denied_kernel(
        std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{}),
        std::make_shared<rime::core::InMemoryTrace>());
    GuiModuleBinding denied_binding{&gui_service, &denied_kernel};
    rime::js::Runtime denied_runtime;
    require_ok(rime::win32::register_ui_module(denied_runtime, &denied_binding),
               "denied module registration");
    require_ok(denied_runtime.start(), "denied runtime start");
    check(denied_runtime,
          "import { ui } from 'rime:ui';\n"
          "globalThis.denied = [];\n"
          "const record = (where, promise) => promise.then(\n"
          "  value => globalThis.denied.push(\n"
          "    { where, code: 'resolved', message: JSON.stringify(value) }),\n"
          "  e => globalThis.denied.push({ where, code: e.code, message: e.message }));\n"
          // timeout 0.1 keeps a broken gate honest: if the capability check
          // ever moved behind the pump, the dialog would open for a beat
          // instead of denying - and the rejection code would not match.
          "record('msgBox', ui.msgBox('x', { timeout: 0.1 }));\n"
          "record('inputBox', ui.inputBox('x', { timeout: 0.1 }));\n"
          "record('toolTip', ui.toolTip('x'));\n"
          "record('traySetIcon', ui.traySetIcon(''));\n"
          "record('trayTip', ui.trayTip('x'));",
          "ui-denied.mjs");
    require_ok(denied_runtime.settle(5000ms), "denied settle");
    check(denied_runtime,
          "if (globalThis.denied.length !== 5)\n"
          "  throw new Error('all five calls must settle: ' + JSON.stringify(globalThis.denied));\n"
          "for (const entry of globalThis.denied) {\n"
          "  if (entry.code !== 'capability_denied')\n"
          "    throw new Error(entry.where + ' must be denied: ' + JSON.stringify(entry));\n"
          "  if (entry.message.indexOf('ui.create') === -1)\n"
          "    throw new Error(entry.where + ' must name ui.create: ' + entry.message);\n"
          "}",
          "ui-denied-check.mjs");
    require_ok(denied_runtime.stop(), "denied runtime stop");
  }

  std::printf("ui slice passed\n");
  return 0;
}
