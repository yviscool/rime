// Realism: L6 - production assembly (register_ui_module -> rime:ui ->
// GuiService on a real UiThread pump, production capability policy) driving
// the M6 batch-2 Gui/GuiControl object family. Every acceptance claim is
// pinned by an observation that the code under test does not produce:
//
//  - `ui.createGui` must not open a window (gui-menu.md 4.1): after the
//    constructor and the mirror reads have run, this process's own
//    top-level windows are enumerated by title and none may match.
//  - after Show, the HWND is found by EnumWindows on this process id and
//    the title, then GetWindowRect / GetWindowTextW are read straight from
//    Win32: `g.GetPos()` must agree with GetWindowRect, and `g.Title = ...`
//    must agree with GetWindowTextW. A facade answering from its own
//    mirror cannot satisfy either cross-check.
//  - the Resize handler is driven by a SetWindowPos this test issues
//    itself and the Close handler by a PostMessageW(WM_CLOSE) this test
//    issues itself; JS only observes. With a Close handler registered the
//    window must survive WM_CLOSE (IsWindow still true), and once that
//    handler is removed the same WM_CLOSE must destroy it (IsWindow goes
//    false) - AHK's default, decided by Win32 rather than by the service.
//  - the rejected Add (a Picture path that cannot load) must leave no
//    orphan control: the __Enum length is recounted afterwards.
//  - a control count and an identity chain (`g.__Item('v') ===` the object
//    Add resolved with, `g.__Item(0) ===` it, `[...g][0] ===` it) pin the
//    lookup and enumeration contract of gui-menu.md 4.2. Positions are
//    0-based; misses read undefined (modern doctrine, no UnsetItemError).
//  - the window-state commands (Minimize/Restore/Maximize/Hide/Show/Flash)
//    are each their own round trip and are read back with IsIconic,
//    IsZoomed and IsWindowVisible on the real HWND; SetFont and SetCue are
//    read back with WM_GETFONT and EM_GETCUEBANNER on the control child
//    FindWindowExW located by class and text. A method that resolves null
//    while doing nothing to the OS cannot pass.
//  - OnMessage, OnCommand and OnNotify are registered in JS and then driven
//    by messages this process sends: a posted raw number (with the Gui and
//    one control subscribed to the same number), a real BN_CLICKED produced
//    by BM_CLICK, and a composed NMHDR. The pump-side routing tables are
//    exercised, not only the JS registration path.
//  - nothing above dispatches an Action: the InMemoryTrace stays empty.
//  - callback and subscription counts captured before the first Gui are
//    recounted after Destroy, so the channel really went back to its
//    baseline (AGENTS 4.5 three-path teardown).
//  - a second, capability-less runtime proves the gate on `ui.createGui`:
//    capability_denied is thrown synchronously before any allocation, and
//    the window it would have opened never exists.
//
// The sync argument contract (bad Gui option, bad options/title/eventObj
// types, unknown event name, out-of-range addRemove, unknown control type,
// unknown show option, negative margin, __Item misses, __New re-entry)
// throws on the JS thread before any worker starts, which is why those are
// collected with try/catch instead of through a promise.

#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/gui.hpp"
#include "rime/win32/js_ui.hpp"
#include "rime/win32/ui_thread.hpp"

#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;
using rime::win32::GuiModuleBinding;
using rime::win32::GuiService;
using rime::win32::UiThread;

constexpr const wchar_t* kTitle = L"rime-gui-object-slice";
constexpr const wchar_t* kRenamedTitle = L"rime-gui-object-slice-2";
constexpr const wchar_t* kDeniedTitle = L"rime-gui-object-denied-slice";

void require_ok(const rime::core::Error& error, const char* what) {
  if (!error.ok()) {
    std::fprintf(stderr, "%s failed: %s: %s\n", what,
                 rime::core::error_code_name(error.code), error.message.c_str());
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

void check(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js check failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::fflush(stderr);
    std::abort();
  }
}

// This process's own top-level window carrying `title`, or nullptr. Never
// asks the service whether a window exists.
HWND find_own_window(const wchar_t* title) {
  struct Probe {
    const wchar_t* title;
    const DWORD pid;
    HWND found{nullptr};
  } probe{title, GetCurrentProcessId()};
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto* probe = reinterpret_cast<Probe*>(parameter);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != probe->pid) return TRUE;
        wchar_t text[256]{};
        if (GetWindowTextW(window, text, 256) == 0) return TRUE;
        if (wcscmp(text, probe->title) != 0) return TRUE;
        probe->found = window;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&probe));
  return probe.found;
}

std::wstring window_text(HWND window) {
  wchar_t text[256]{};
  if (GetWindowTextW(window, text, 256) == 0) return L"";
  return text;
}

// The first child of `parent` matching a class (and, when given, a window
// text). The test asks Win32 directly instead of the service's child map, so
// nothing being tested can answer this question for itself.
HWND find_child(const HWND parent, const wchar_t* class_name, const wchar_t* text = nullptr) {
  return FindWindowExW(parent, nullptr, class_name, text);
}

// WM_GETFONT on a real control: the font handle the OS is using right now.
HFONT font_of(const HWND control) {
  return reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
}

// EM_GETCUEBANNER = ECM_FIRST (0x1500) + 2 from CommCtrl.h, written as a
// number so this header does not have to pull commctrl.h in for one read.
// The cue text, not a return code, is the observation: it is what SetCue
// claims to have installed on the real control.
constexpr UINT kEmGetCueBanner = 0x1502;

std::wstring edit_cue(const HWND edit) {
  wchar_t buffer[128]{};
  SendMessageW(edit, kEmGetCueBanner, reinterpret_cast<WPARAM>(buffer),
               static_cast<LPARAM>(128));
  return buffer;
}

// The window that currently has keyboard focus inside the thread owning
// `window`. GetGUIThreadInfo answers across threads, so this test can read
// the pump thread's focus without touching it.
HWND focused_child(const HWND window) {
  const DWORD thread = GetWindowThreadProcessId(window, nullptr);
  GUITHREADINFO info{};
  info.cbSize = sizeof(info);
  if (GetGUIThreadInfo(thread, &info) == 0) return nullptr;
  return info.hwndFocus;
}

template <typename Predicate>
bool wait_for(Predicate predicate, const std::chrono::milliseconds timeout = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

// The four counters the teardown promise rests on: host callbacks (the
// per-Gui dispatch channel) and its subscription registry entries.
struct HostCounts {
  double callbacks{0};
  std::size_t subscriptions{0};
};

HostCounts host_counts(rime::js::Runtime& runtime) {
  const std::string text = runtime.inspect("{}").get();
  const auto parsed = rime::core::json::parse(text);
  if (!parsed.ok() || !parsed.value.has_value()) {
    std::fprintf(stderr, "inspect did not return JSON: %s\n", text.c_str());
    std::fflush(stderr);
    std::abort();
  }
  HostCounts counts;
  if (const auto* tasks = parsed.value->find("tasks"); tasks != nullptr) {
    if (const auto* callbacks = tasks->find("callbacks"); callbacks != nullptr) {
      counts.callbacks = callbacks->as_number();
    }
  }
  if (const auto* subscriptions = parsed.value->find("subscriptions"); subscriptions != nullptr) {
    if (subscriptions->is_array()) counts.subscriptions = subscriptions->size();
  }
  return counts;
}

// The boot module: the `waitFor` helper events_slice uses, so an event this
// test triggers from the OS side is waited for with a runtime timer instead
// of a bare sleep, and the assertion lands after the callback really ran.
const char* kBoot =
    "import { runtime } from 'rime:runtime';\n"
    "globalThis.waitFor = async (predicate, timeoutMs) => {\n"
    "  const deadline = Date.now() + timeoutMs;\n"
    "  while (Date.now() < deadline) {\n"
    "    if (predicate()) return true;\n"
    "    await runtime.delay(20, null);\n"
    "  }\n"
    "  return predicate();\n"
    "};\n"
    "globalThis.seen = (fn) => {\n"
    "  try { const value = fn(); return { threw: false, value: typeof value }; }\n"
    "  catch (e) {\n"
    "    return { threw: true,\n"
    "             ctor: e && e.constructor ? e.constructor.name : 'undefined',\n"
    "             code: (e && typeof e.code === 'string') ? e.code : '',\n"
    "             message: String((e && e.message) || '') };\n"
    "  }\n"
    "};\n"
    "globalThis.track = (bucket, name, promise) => {\n"
    "  bucket[name] = 'pending';\n"
    "  promise.then(value => { bucket[name] = value; },\n"
    "               error => { bucket[name] = {\n"
    "                   code: (error && error.code) || '',\n"
    "                   ctor: error && error.constructor ? error.constructor.name : '?',\n"
    "                   message: String((error && error.message) || '') }; });\n"
    "};\n";

}  // namespace

int main() {
  UiThread ui;
  require_ok(ui.start(), "UiThread starts");
  GuiService gui_service;
  gui_service.set_ui_thread(&ui);

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{"ui.create"}),
      trace);
  GuiModuleBinding binding{&gui_service, &kernel};

  rime::js::Runtime runtime;
  require_ok(rime::win32::register_ui_module(runtime, &binding), "ui module registration");
  require_ok(runtime.start(), "runtime start");
  check(runtime, kBoot, "gui-object-boot.mjs");

  // Baseline before the first Gui exists: Destroy has to land back here.
  const HostCounts baseline = host_counts(runtime);

  // ---- S1: the synchronous contract (no worker, no window) ----------------
  check(runtime,
        "import { ui } from 'rime:ui';\n"
        "const seen = globalThis.seen;\n"
        "globalThis.g = ui.createGui('+Resize +ToolWindow', 'rime-gui-object-slice');\n"
        "const g = globalThis.g;\n"
        "globalThis.sync = {\n"
        "  createBadOption: seen(() => ui.createGui('+Bogus', 'x')),\n"
        "  createBadOptions: seen(() => ui.createGui(7)),\n"
        "  createBadTitle: seen(() => ui.createGui('', 7)),\n"
        "  createBadEventObj: seen(() => ui.createGui('', 't', 7)),\n"
        "  hwndGet: seen(() => g.Hwnd),\n"
        "  hwndSet: seen(() => { g.Hwnd = 1; }),\n"
        "  fontGet: seen(() => g.FontHandle),\n"
        "  fontSet: seen(() => { g.FontHandle = 1; }),\n"
        "  unknownEvent: seen(() => g.OnEvent('Focus', () => {})),\n"
        "  badAddRemove: seen(() => g.OnEvent('Close', () => {}, 5)),\n"
        "  badAddRemoveType: seen(() => g.OnEvent('Close', () => {}, 'x')),\n"
        "  unknownControl: seen(() => g.Add('Nope')),\n"
        "  emptyAdd: seen(() => g.Add()),\n"
        "  unknownGuiOption: seen(() => g.Opt('+Bogus')),\n"
        "  unknownShowOption: seen(() => g.Show('bogus')),\n"
        "  labelOption: seen(() => g.AddText('gLabel', 'x')),\n"
        "  missingItem: seen(() => g.__Item('nope')),\n"
        "  badItemKey: seen(() => g.__Item(true)),\n"
        "  noItemArgs: seen(() => g.__Item()),\n"
        "  constructTwice: seen(() => g.__New()),\n"
        "  negativeMargin: seen(() => { g.MarginX = -1; }),\n"
        "  badTitleWrite: seen(() => { g.Title = 7; }),\n"
        "  initialName: g.Name,\n"
        "};\n",
        "gui-object-contract.mjs");
  check(runtime,
        "const s = globalThis.sync;\n"
        "const fail = (key, detail) => { throw new Error(key + ': ' + detail); };\n"
        "for (const key of ['createBadOption','createBadOptions','createBadTitle',\n"
        "                   'createBadEventObj','hwndGet','hwndSet','fontGet','fontSet',\n"
        "                   'unknownEvent','badAddRemove','badAddRemoveType',\n"
        "                   'unknownControl','emptyAdd','unknownGuiOption',\n"
        "                   'unknownShowOption','labelOption','badItemKey',\n"
        "                   'noItemArgs','constructTwice','negativeMargin','badTitleWrite']) {\n"
        "  if (!s[key] || s[key].threw !== true) fail(key, 'must throw synchronously');\n"
        "}\n"
        "const expectError = (entry, code, needle) => {\n"
        "  if (entry.ctor !== 'Error') fail(code, 'ctor=' + entry.ctor);\n"
        "  if (entry.code !== code) fail(code, 'code=' + entry.code);\n"
        "  if (entry.message.indexOf(needle) === -1) fail(code, 'message=' + entry.message);\n"
        "};\n"
        "expectError(s.hwndGet, 'unsupported_by_policy', 'raw window and font handles');\n"
        "expectError(s.hwndSet, 'unsupported_by_policy', 'raw window and font handles');\n"
        "expectError(s.fontGet, 'unsupported_by_policy', 'raw window and font handles');\n"
        "expectError(s.fontSet, 'unsupported_by_policy', 'raw window and font handles');\n"
        "if (s.missingItem.threw !== false || s.missingItem.value !== 'undefined')\n"
        "  fail('missingItem', 'unknown names read undefined, never throw');\n"
        "const expectRange = (entry, needle) => {\n"
        "  if (entry.ctor !== 'RangeError') fail(needle, 'ctor=' + entry.ctor);\n"
        "  if (entry.message.indexOf(needle) === -1) fail(needle, 'message=' + entry.message);\n"
        "};\n"
        "expectRange(s.createBadOption, 'unknown gui option: bogus');\n"
        "expectRange(s.unknownGuiOption, 'unknown gui option: bogus');\n"
        "expectRange(s.unknownShowOption, 'unknown show option: bogus');\n"
        "expectRange(s.unknownEvent, 'unknown event: Focus');\n"
        "expectRange(s.badAddRemove, 'addRemove must be -1, 0 or 1');\n"
        "expectRange(s.unknownControl, 'unsupported control type: Nope');\n"
        "expectRange(s.labelOption, 'control label options are not supported');\n"
        "expectRange(s.negativeMargin, 'MarginX must not be negative');\n"
        "const expectType = (entry, needle) => {\n"
        "  if (entry.ctor !== 'TypeError') fail(needle, 'ctor=' + entry.ctor);\n"
        "  if (entry.message.indexOf(needle) === -1) fail(needle, 'message=' + entry.message);\n"
        "};\n"
        "expectType(s.createBadOptions, 'options must be a string');\n"
        "expectType(s.createBadTitle, 'title must be a string');\n"
        "expectType(s.createBadEventObj, 'eventObj must be an object');\n"
        "expectType(s.badAddRemoveType, 'addRemove must be a number');\n"
        "expectType(s.emptyAdd, 'Add(controlType');\n"
        "expectType(s.badItemKey, 'expects a name or a position');\n"
        "expectType(s.noItemArgs, '__Item(index)');\n"
        "expectType(s.badTitleWrite, 'Title must be a string');\n"
        "if (s.initialName !== '') fail('initialName', JSON.stringify(s.initialName));\n"
        "if (s.constructTwice.code !== 'already_constructed')\n"
        "  fail('constructTwice', JSON.stringify(s.constructTwice));\n"
        // `g.OnEvent('Close', fn)` on a Gui is legal (gui_event_name allows
        // it), so it is deliberately absent here: registering it would set
        // the interest bit this test later clears. The control-level set is
        // what batch 2 closes, and is checked once a control exists.
        "if (globalThis.g.__brand !== 'gui') fail('brand', globalThis.g.__brand);\n",
        "gui-object-contract-check.mjs");

  // ---- S2: mirror reads materialize nothing -------------------------------
  check(runtime,
        "const g = globalThis.g;\n"
        "globalThis.mirror = {};\n"
        "globalThis.track(globalThis.mirror, 'title', g.Title);\n"
        "globalThis.track(globalThis.mirror, 'backColor', g.BackColor);\n"
        "globalThis.track(globalThis.mirror, 'marginX', g.MarginX);\n"
        "globalThis.track(globalThis.mirror, 'marginY', g.MarginY);\n",
        "gui-object-mirror.mjs");
  require(find_own_window(kTitle) == nullptr,
          "createGui and the mirror reads must not have created a window");
  require_ok(runtime.settle(5000ms), "mirror settle");
  check(runtime,
        "const m = globalThis.mirror;\n"
        "if (m.title !== 'rime-gui-object-slice')\n"
        "  throw new Error('Title mirror: ' + JSON.stringify(m.title));\n"
        "if (m.backColor !== '')\n"
        "  throw new Error('BackColor mirror: ' + JSON.stringify(m.backColor));\n"
        "if (m.marginX !== null || m.marginY !== null)\n"
        "  throw new Error('unset margins: ' + JSON.stringify([m.marginX, m.marginY]));\n",
        "gui-object-mirror-check.mjs");

  // ---- S3/S4: nine constructor shapes, identity and enumeration -----------
  check(runtime,
        "const g = globalThis.g;\n"
        "globalThis.r = {};\n"
        "globalThis.chain = Promise.resolve(null);\n"
        "const step = (name, make) => {\n"
        "  globalThis.chain = globalThis.chain.then(make).then(\n"
        "    value => { globalThis.r[name] = value; },\n"
        "    error => { globalThis.r[name] = {\n"
        "        code: (error && error.code) || '',\n"
        "        ctor: error && error.constructor ? error.constructor.name : '?',\n"
        "        message: String((error && error.message) || '') }; });\n"
        "};\n"
        "step('edit', () => g.AddEdit('x10 y10 w200 vMyEdit', 'seed'));\n"
        "step('text', () => g.AddText('x10 y40 w200', 'hello'));\n"
        "step('check', () => g.AddCheckBox('x10 y70 vMyCheck', 'tick'));\n"
        "step('prog', () => g.AddProgress('x10 y100 w150 vMyProg', 40));\n"
        "step('button', () => g.AddButton('x10 y130 w80', 'OK'));\n"
        "step('generic', () => g.Add('Radio', 'x10 y160 vMyRadio', 'pick'));\n"
        "step('pic', () => g.AddPicture('x1 y1',\n"
        "                               'C:\\\\rime-slice-no-such-picture.bmp'));\n",
        "gui-object-add.mjs");
  require_ok(runtime.settle(10000ms), "add settle");
  // The window exists as soon as the first control landed (the pump creates
  // it), but AHK's default style carries no WS_VISIBLE, so it is still
  // hidden - which is what makes the Show assertion below non-vacuous.
  const HWND created = find_own_window(kTitle);
  require(created != nullptr, "the Adds materialized the Gui window");
  require(IsWindowVisible(created) == FALSE, "Add must leave the window hidden");
  check(runtime,
        "const g = globalThis.g;\n"
        "const r = globalThis.r;\n"
        "const fail = (key, detail) => { throw new Error(key + ': ' + detail); };\n"
        "for (const key of ['edit','text','check','prog','button','generic']) {\n"
        "  if (!r[key] || typeof r[key] !== 'object' || r[key].code)\n"
        "    fail(key, JSON.stringify(r[key]));\n"
        "}\n"
        "if (r.edit.Type !== 'Edit' || r.edit.Name !== 'MyEdit' ||\n"
        "    r.edit.ClassNN !== 'Edit1')\n"
        "  fail('edit identity', JSON.stringify([r.edit.Type, r.edit.Name, r.edit.ClassNN]));\n"
        "if (r.edit.Gui !== g) fail('edit.Gui', 'must be the owning Gui object');\n"
        // Type/ClassNN/Name/Gui are synchronous; Text is a pump round trip
        // and is read through the promise in the S6 block below.
        "if (r.text.Type !== 'Text' || r.text.ClassNN !== 'Text1')\n"
        "  fail('text', JSON.stringify([r.text.Type, r.text.ClassNN]));\n"
        "if (r.check.Type !== 'CheckBox' || r.check.ClassNN !== 'CheckBox1' ||\n"
        "    r.check.Name !== 'MyCheck')\n"
        "  fail('check', JSON.stringify([r.check.Type, r.check.ClassNN, r.check.Name]));\n"
        "if (r.prog.Type !== 'Progress' || r.prog.ClassNN !== 'Progress1')\n"
        "  fail('prog', JSON.stringify([r.prog.Type, r.prog.ClassNN]));\n"
        "if (r.button.Type !== 'Button' || r.button.ClassNN !== 'Button1')\n"
        "  fail('button', JSON.stringify([r.button.Type, r.button.ClassNN]));\n"
        "if (r.generic.Type !== 'Radio' || r.generic.Name !== 'MyRadio' ||\n"
        "    r.generic.ClassNN !== 'Radio1')\n"
        "  fail('generic', JSON.stringify([r.generic.Type, r.generic.Name, r.generic.ClassNN]));\n"
        // The picture path cannot load: the pump says so, and a rejected Add
        // must not leave an orphan behind.
        "if (!r.pic || r.pic.code !== 'invalid_contract' ||\n"
        "    r.pic.message.indexOf('picture could not be loaded') === -1)\n"
        "  fail('pic', JSON.stringify(r.pic));\n"
        "const list = g.__Enum();\n"
        "if (!Array.isArray(list) || list.length !== 6)\n"
        "  fail('__Enum', JSON.stringify(list && list.length));\n"
        "if (g.__Enum(2).length !== 2) fail('__Enum(2)', 'must stop at the limit');\n"
        "const spread = [...g];\n"
        "if (spread.length !== 6) fail('iterator', JSON.stringify(spread.length));\n"
        "if (list[0] !== r.edit || spread[0] !== r.edit)\n"
        "  fail('enumeration order', 'the Edit added first must come first');\n"
        "if (g.__Item('MyEdit') !== r.edit) fail('__Item by name', 'identity');\n"
        "if (g.__Item(0) !== r.edit) fail('__Item by position', 'identity');\n"
        "if (g.__Item(5) !== r.generic) fail('__Item last', 'identity');\n"
        "if (g.__Item(-1) !== undefined) fail('__Item negative', 'must read undefined');\n"
        "if (g.__Item(9) !== undefined) fail('__Item past end', 'must read undefined');\n",
        "gui-object-add-check.mjs");

  // ---- S5: Show is what makes the window visible --------------------------
  check(runtime,
        "import { ui } from 'rime:ui';\n"
        "globalThis.track(globalThis.r, 'show', globalThis.g.Show());\n",
        "gui-object-show.mjs");
  require_ok(runtime.settle(10000ms), "show settle");
  check(runtime,
        "if (globalThis.r.show !== null)\n"
        "  throw new Error('Show must resolve null: ' +\n"
        "                   JSON.stringify(globalThis.r.show));\n",
        "gui-object-show-check.mjs");
  const HWND hwnd = find_own_window(kTitle);
  require(hwnd == created, "Show must not have swapped the window handle");
  require(IsWindowVisible(hwnd) != FALSE, "Show leaves the window visible");
  require(wait_for([&] { return !IsIconic(hwnd) && IsWindowVisible(hwnd); }),
          "the shown window is observable and not minimized");

  // ---- S6/S7: pump reads, and the value contract on a control -------------
  check(runtime,
        "const g = globalThis.g;\n"
        "const r = globalThis.r;\n"
        "globalThis.out = {};\n"
        // Text is read through its promise: 'hello'/'OK' are the content
        // arguments the Adds carried, observed after Show changed nothing.
        "globalThis.track(globalThis.out, 'textInitial', r.text.Text);\n"
        "globalThis.track(globalThis.out, 'buttonInitial', r.button.Text);\n"
        "globalThis.track(globalThis.out, 'editValue', r.edit.Value);\n"
        "globalThis.track(globalThis.out, 'editEnabled', r.edit.Enabled);\n"
        "globalThis.track(globalThis.out, 'editVisible', r.edit.Visible);\n"
        "globalThis.track(globalThis.out, 'progValue', r.prog.Value);\n"
        "globalThis.track(globalThis.out, 'focused', g.FocusedCtrl);\n"
        "globalThis.track(globalThis.out, 'title', g.Title);\n"
        // Reading Value off a Text rejects rather than throws: the shape is
        // known on the JS thread, so the getter hands back a rejected
        // Promise carrying the same ValueError a write would throw.
        "globalThis.track(globalThis.out, 'textValueRead', r.text.Value);\n"
        "globalThis.sync2 = {\n"
        "  textValueWrite: globalThis.seen(() => { r.text.Value = 1; }),\n"
        "  textTextWrite: globalThis.seen(() => { r.text.Text = 7; }),\n"
        "  textEnabledWrite: globalThis.seen(() => { r.text.Enabled = 'x'; }),\n"
        "  editValueWrite: globalThis.seen(() => { r.edit.Value = 7; }),\n"
        "  progValueWrite: globalThis.seen(() => { r.prog.Value = 200; }),\n"
        "  ctrlHwndGet: globalThis.seen(() => r.text.Hwnd),\n"
        "  ctrlHwndSet: globalThis.seen(() => { r.text.Hwnd = 1; }),\n"
        "  controlUnknownEvent: globalThis.seen(\n"
        "      () => r.text.OnEvent('Close', () => {})),\n"
        "};\n",
        "gui-object-ctrl-read.mjs");
  require_ok(runtime.settle(10000ms), "control read settle");
  check(runtime,
        "const o = globalThis.out;\n"
        "const fail = (key, detail) => { throw new Error(key + ': ' + detail); };\n"
        "if (o.textInitial !== 'hello') fail('textInitial', JSON.stringify(o.textInitial));\n"
        "if (o.buttonInitial !== 'OK') fail('buttonInitial', JSON.stringify(o.buttonInitial));\n"
        "if (o.editValue !== 'seed') fail('editValue', JSON.stringify(o.editValue));\n"
        "if (o.editEnabled !== true) fail('editEnabled', JSON.stringify(o.editEnabled));\n"
        "if (o.editVisible !== true) fail('editVisible', JSON.stringify(o.editVisible));\n"
        "if (o.progValue !== 40) fail('progValue', JSON.stringify(o.progValue));\n"
        // Nothing has been focused yet: the read must still be a real pump
        // round trip that answers null rather than a missing member.
        "if (o.focused !== null) fail('focused', JSON.stringify(o.focused));\n"
        "if (o.title !== 'rime-gui-object-slice') fail('title', JSON.stringify(o.title));\n"
        "const s = globalThis.sync2;\n"
        "const expectRange = (entry, needle) => {\n"
        "  if (entry.ctor !== 'RangeError') fail(needle, 'ctor=' + entry.ctor);\n"
        "  if (entry.message.indexOf(needle) === -1) fail(needle, 'message=' + entry.message);\n"
        "};\n"
        "expectRange(o.textValueRead, 'this control type has no value');\n"
        "expectRange(s.textValueWrite, 'this control type has no value');\n"
        "expectRange(s.editValueWrite, 'Edit value must be a string');\n"
        "expectRange(s.progValueWrite, 'progress value must be 0..100');\n"
        // The control-level event set closes with batch 2, so the Gui-only
        // names are refused here with the same ValueError the Gui gets.
        "expectRange(s.controlUnknownEvent, 'unknown event: Close');\n"
        "const expectType = (entry, needle) => {\n"
        "  if (entry.ctor !== 'TypeError') fail(needle, 'ctor=' + entry.ctor);\n"
        "  if (entry.message.indexOf(needle) === -1) fail(needle, 'message=' + entry.message);\n"
        "};\n"
        "expectType(s.textTextWrite, 'Text must be a string');\n"
        "expectType(s.textEnabledWrite, 'Enabled must be a boolean');\n"
        "if (s.ctrlHwndGet.code !== 'unsupported_by_policy' ||\n"
        "    s.ctrlHwndSet.code !== 'unsupported_by_policy' ||\n"
        "    s.ctrlHwndGet.ctor !== 'Error' || s.ctrlHwndSet.ctor !== 'Error')\n"
        "  fail('ctrlHwnd', JSON.stringify([s.ctrlHwndGet, s.ctrlHwndSet]));\n",
        "gui-object-ctrl-read-check.mjs");

  // ---- S7b: the rest of batch 2 -------------------------------------------
  // The Gui members S1-S7 did not reach (AddGroupBox, AddRadio, Submit,
  // SetFont, Opt, Hide/Maximize/Minimize/Restore/Flash) and the control
  // members (Focus, Redraw, SetFont, SetCue, Opt, Focused). Every promise
  // result is paired with an observation this process makes with its own
  // Win32 calls - FindWindowExW, WM_GETFONT, EM_GETCUEBANNER, IsIconic,
  // IsZoomed, IsWindowVisible, GetGUIThreadInfo - so a facade answering from
  // its own mirror could satisfy neither half alone.
  const HWND edit_hwnd = find_child(hwnd, L"EDIT", nullptr);
  require(edit_hwnd != nullptr, "FindWindowExW finds the Edit control");
  const HFONT font_before = font_of(edit_hwnd);
  require(font_before != nullptr, "the Edit starts with the Gui's font");
  require(edit_cue(edit_hwnd).empty(), "the Edit starts with no cue banner");

  check(runtime,
        "const g = globalThis.g;\n"
        "const r = globalThis.r;\n"
        "globalThis.out3 = { done: false };\n"
        "globalThis.chain3 = Promise.resolve(null);\n"
        "const step3 = (name, make) => {\n"
        "  globalThis.chain3 = globalThis.chain3.then(make).then(\n"
        "    value => { globalThis.out3[name] = value; },\n"
        "    error => { globalThis.out3[name] = {\n"
        "        code: (error && error.code) || '',\n"
        "        ctor: error && error.constructor ? error.constructor.name : '?',\n"
        "        message: String((error && error.message) || '') }; });\n"
        "};\n"
        "step3('group', () => g.AddGroupBox('x10 y160 w200 vMyGroup', 'box'));\n"
        "step3('radio2', () => g.AddRadio('x10 y190 w200 vMyRadio2', 'second'));\n"
        "step3('submit', () => g.Submit());\n"
        "step3('guiSetFont', () => g.SetFont('s11', 'Arial'));\n"
        "step3('cue', () => r.edit.SetCue('type here'));\n"
        "step3('redraw', () => r.edit.Redraw());\n"
        "globalThis.chain3 = globalThis.chain3.then(() => { globalThis.out3.done = true; });\n",
        "gui-object-commands.mjs");
  require_ok(runtime.settle(10000ms), "command settle");
  check(runtime,
        "const o = globalThis.out3;\n"
        "const g = globalThis.g;\n"
        "const fail = (k, d) => { throw new Error(k + ': ' + d); };\n"
        "if (o.done !== true) fail('chain', JSON.stringify(o));\n"
        "if (!o.group || typeof o.group !== 'object' || o.group.code)\n"
        "  fail('group', JSON.stringify(o.group));\n"
        "if (o.group.Type !== 'GroupBox' || o.group.Name !== 'MyGroup')\n"
        "  fail('group identity', JSON.stringify([o.group.Type, o.group.Name]));\n"
        "if (!o.radio2 || typeof o.radio2 !== 'object' || o.radio2.code)\n"
        "  fail('radio2', JSON.stringify(o.radio2));\n"
        "if (o.radio2.Type !== 'Radio' || o.radio2.ClassNN !== 'Radio2' ||\n"
        "    o.radio2.Name !== 'MyRadio2')\n"
        "  fail('radio2 identity',\n"
        "       JSON.stringify([o.radio2.Type, o.radio2.ClassNN, o.radio2.Name]));\n"
        "if (g.__Enum().length !== 8)\n"
        "  fail('__Enum after two more Adds', JSON.stringify(g.__Enum().length));\n"
        "const s = o.submit;\n"
        "if (!s || typeof s !== 'object') fail('submit', JSON.stringify(s));\n"
        "if (s.MyEdit !== 'seed') fail('submit MyEdit', JSON.stringify(s.MyEdit));\n"
        "if (s.MyGroup !== 'box') fail('submit MyGroup', JSON.stringify(s.MyGroup));\n"
        "if (typeof s.MyCheck !== 'number' || typeof s.MyRadio !== 'number' ||\n"
        "    typeof s.MyRadio2 !== 'number')\n"
        "  fail('submit values', JSON.stringify(s));\n"
        // gui_submit skips Progress: the absence is part of the contract.
        "if ('MyProg' in s) fail('submit MyProg', 'Progress must not be submitted');\n"
        "if (o.guiSetFont !== null) fail('guiSetFont', JSON.stringify(o.guiSetFont));\n"
        "if (o.cue !== null) fail('cue', JSON.stringify(o.cue));\n"
        "if (o.redraw !== null) fail('redraw', JSON.stringify(o.redraw));\n",
        "gui-object-commands-check.mjs");
  require(font_of(edit_hwnd) != font_before,
          "g.SetFont must reach WM_GETFONT on the real Edit control");
  // The round trip only answers if this process declares ComCtl32 v6 in its
  // manifest (engine/win32/resources/visual-styles.rc): Windows refuses
  // EM_SETCUEBANNER outright without it, which is exactly the silent drop
  // the manifest exists to prevent.
  require(edit_cue(edit_hwnd) == L"type here",
          "r.edit.SetCue must reach EM_GETCUEBANNER on the real Edit control");

  const HFONT font_after_gui = font_of(edit_hwnd);
  check(runtime,
        "const g = globalThis.g;\n"
        "const r = globalThis.r;\n"
        "globalThis.out4 = { done: false };\n"
        "globalThis.chain4 = Promise.resolve(null);\n"
        "const step4 = (name, make) => {\n"
        "  globalThis.chain4 = globalThis.chain4.then(make).then(\n"
        "    value => { globalThis.out4[name] = value; },\n"
        "    error => { globalThis.out4[name] = {\n"
        "        code: (error && error.code) || '',\n"
        "        ctor: error && error.constructor ? error.constructor.name : '?',\n"
        "        message: String((error && error.message) || '') }; });\n"
        "};\n"
        "step4('ctrlSetFont', () => r.edit.SetFont('s12', 'Arial'));\n"
        "step4('focus', () => r.edit.Focus());\n"
        "step4('focused', () => g.FocusedCtrl);\n"
        "step4('editFocused', () => r.edit.Focused);\n"
        "globalThis.chain4 = globalThis.chain4.then(() => { globalThis.out4.done = true; });\n",
        "gui-object-focus.mjs");
  require_ok(runtime.settle(10000ms), "focus settle");
  require(font_of(edit_hwnd) != font_after_gui,
          "r.edit.SetFont must replace the Edit's font handle a second time");
  // Read back through GetGUIThreadInfo on the pump thread: proves Focus()
  // moved real keyboard focus instead of just resolving.
  require(focused_child(hwnd) == edit_hwnd,
          "r.edit.Focus must move keyboard focus to the real Edit control");
  check(runtime,
        "const o = globalThis.out4;\n"
        "const r = globalThis.r;\n"
        "const fail = (k, d) => { throw new Error(k + ': ' + d); };\n"
        "if (o.done !== true) fail('chain', JSON.stringify(o));\n"
        "if (o.ctrlSetFont !== null) fail('ctrlSetFont', JSON.stringify(o.ctrlSetFont));\n"
        "if (o.focus !== null) fail('focus', JSON.stringify(o.focus));\n"
        "if (o.focused !== r.edit)\n"
        "  fail('FocusedCtrl', 'got ' + JSON.stringify(o.focused));\n"
        "if (o.editFocused !== true)\n"
        "  fail('Focused', 'got ' + JSON.stringify(o.editFocused));\n",
        "gui-object-focus-check.mjs");

  // Opt: proven by the option reaching the real window, not by the promise
  // resolving. Each call is settled, checked for null, then observed with
  // this process's own Win32 read before the next call undoes it.
  const auto run_opt = [&](const char* call, const char* key, const char* observed_what,
                           const auto& observed) {
    check(runtime, std::string("globalThis.track(globalThis.out3, '") + key + "', " + call +
                       ");\n",
          std::string("gui-object-") + key + ".mjs");
    require_ok(runtime.settle(10000ms), (std::string(key) + " settle").c_str());
    check(runtime,
          std::string("if (globalThis.out3['") + key + "'] !== null)\n"
          "  throw new Error('" + key +
              " must resolve null: ' + JSON.stringify(globalThis.out3['" + key + "']));\n",
          std::string("gui-object-") + key + "-check.mjs");
    require(observed(), observed_what);
  };
  run_opt("globalThis.g.Opt('-Resize')", "guiOptOff",
          "g.Opt('-Resize') must clear WS_SIZEBOX on the real window",
          [&] { return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_SIZEBOX) == 0; });
  run_opt("globalThis.g.Opt('+Resize')", "guiOptOn",
          "g.Opt('+Resize') must restore WS_SIZEBOX on the real window",
          [&] { return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_SIZEBOX) != 0; });
  run_opt("globalThis.r.edit.Opt('+Disabled')", "ctrlOptOff",
          "r.edit.Opt('+Disabled') must clear IsWindowEnabled on the real Edit",
          [&] { return IsWindowEnabled(edit_hwnd) == FALSE; });
  run_opt("globalThis.r.edit.Opt('-Disabled')", "ctrlOptOn",
          "r.edit.Opt('-Disabled') must restore IsWindowEnabled on the real Edit",
          [&] { return IsWindowEnabled(edit_hwnd) != FALSE; });

  // Window state: one round trip per command, so the observation lands
  // between two commands instead of after all of them.
  int cmd_step = 0;
  const auto run_gui_cmd = [&](const char* method) {
    ++cmd_step;
    const std::string key = method;
    const std::string id = std::to_string(cmd_step);
    check(runtime, "globalThis.track(globalThis.out3, '" + key + id + "', globalThis.g." + key +
                       "());\n",
          "gui-object-cmd-" + key + id + ".mjs");
    const std::string settle_what = key + " settle";
    require_ok(runtime.settle(10000ms), settle_what.c_str());
    check(runtime,
          "if (globalThis.out3['" + key + id + "'] !== null)\n"
          "  throw new Error('" + key + " must resolve null: ' + JSON.stringify(globalThis.out3['" +
              key + id + "']));\n",
          "gui-object-cmd-" + key + id + "-check.mjs");
  };

  run_gui_cmd("Minimize");
  require(IsIconic(hwnd) != FALSE, "Minimize must reach IsIconic on the real window");
  run_gui_cmd("Restore");
  require(IsIconic(hwnd) == FALSE, "Restore must clear IsIconic");
  run_gui_cmd("Maximize");
  require(IsZoomed(hwnd) != FALSE, "Maximize must reach IsZoomed on the real window");
  run_gui_cmd("Restore");
  require(IsZoomed(hwnd) == FALSE, "Restore must clear IsZoomed");
  run_gui_cmd("Hide");
  require(IsWindowVisible(hwnd) == FALSE, "Hide must reach IsWindowVisible");
  run_gui_cmd("Show");
  require(IsWindowVisible(hwnd) != FALSE, "Show must bring the window back");
  run_gui_cmd("Flash");
  require(IsWindow(hwnd) && IsWindowVisible(hwnd) != FALSE && IsIconic(hwnd) == FALSE,
          "Flash leaves the window alive, visible and restored");

  // ---- S7c: the three notification channels -------------------------------
  // Registered here, then driven by messages this process sends to the real
  // HWND: a posted raw number for OnMessage (the Gui and one control
  // subscribe to the same number - the set semantics gui-menu.md 4.3
  // describes), a real BN_CLICKED produced by BM_CLICK on the push button for
  // OnCommand, and a composed NMHDR for OnNotify - no batch-2 control emits
  // WM_NOTIFY on its own.
  check(runtime,
        "const g = globalThis.g;\n"
        "const r = globalThis.r;\n"
        "globalThis.ev2 = { message: [], command: [], notify: [] };\n"
        "const N = 0x8037;\n"
        "globalThis.track(globalThis.ev2, 'guiMsgReg',\n"
        "    g.OnMessage(N, (target, w, l, msg) => {\n"
        "      globalThis.ev2.message.push({ who: 'gui', self: target === g,\n"
        "                                   w, l, msg });\n"
        "    }));\n"
        "globalThis.track(globalThis.ev2, 'ctrlMsgReg',\n"
        "    r.text.OnMessage(N, (target, w, l, msg) => {\n"
        "      globalThis.ev2.message.push({ who: 'ctrl', self: target === r.text,\n"
        "                                   w, l, msg });\n"
        "    }));\n"
        "globalThis.track(globalThis.ev2, 'cmdReg',\n"
        "    r.button.OnCommand(0, (target, code) => {\n"
        "      globalThis.ev2.command.push({ self: target === r.button, code });\n"
        "    }));\n"
        "globalThis.track(globalThis.ev2, 'notifyReg',\n"
        "    r.button.OnNotify(-2, (target, code) => {\n"
        "      globalThis.ev2.notify.push({ self: target === r.button, code });\n"
        "    }));\n",
        "gui-object-channels-register.mjs");
  require_ok(runtime.settle(10000ms), "channel register settle");

  const HWND button_hwnd = find_child(hwnd, L"BUTTON", L"OK");
  require(button_hwnd != nullptr, "FindWindowExW finds the push button by class and text");
  require(PostMessageW(hwnd, 0x8037, 7, 9) != FALSE, "PostMessageW(raw message number)");
  SendMessageW(button_hwnd, BM_CLICK, 0, 0);
  NMHDR notification{};
  notification.hwndFrom = button_hwnd;
  notification.idFrom = static_cast<UINT_PTR>(GetDlgCtrlID(button_hwnd));
  notification.code = static_cast<UINT_PTR>(static_cast<unsigned int>(-2));  // NM_CLICK
  SendMessageW(hwnd, WM_NOTIFY, static_cast<WPARAM>(notification.idFrom),
               reinterpret_cast<LPARAM>(&notification));
  // The raw number was posted, so it sits in the pump thread's queue until
  // that thread runs again; the two sends above are synchronous and their
  // tasks already exist. A service round trip supplies that run:
  // UiThread::call wakes the pump with a posted message of its own, posted
  // after ours, and one thread dispatches posted messages in order - so by
  // the time GetPos answers, the raw number has been through the window
  // procedure. settle() then waits for the tasks those dispatches queued.
  check(runtime,
        "globalThis.__roundtrip = await globalThis.g.GetPos();\n",
        "gui-object-channels-pump.mjs");
  require_ok(runtime.settle(10000ms), "channel event settle");
  check(runtime,
        "const e = globalThis.ev2;\n"
        "const fail = (k, d) => { throw new Error(k + ': ' + d); };\n"
        "for (const k of ['guiMsgReg', 'ctrlMsgReg', 'cmdReg', 'notifyReg']) {\n"
        "  if (e[k] !== null) fail(k, JSON.stringify(e[k]));\n"
        "}\n"
        "if (e.message.length !== 2 || e.command.length !== 1 || e.notify.length !== 1)\n"
        "  fail('channel counts', JSON.stringify(e));\n"
        "const guiMsg = e.message.find(x => x.who === 'gui');\n"
        "const ctrlMsg = e.message.find(x => x.who === 'ctrl');\n"
        "if (!guiMsg || guiMsg.self !== true || guiMsg.w !== 7 || guiMsg.l !== 9 ||\n"
        "    guiMsg.msg !== 0x8037)\n"
        "  fail('gui message', JSON.stringify(guiMsg));\n"
        "if (!ctrlMsg || ctrlMsg.self !== true || ctrlMsg.msg !== 0x8037)\n"
        "  fail('ctrl message', JSON.stringify(ctrlMsg));\n"
        "if (e.command.length !== 1 || e.command[0].self !== true ||\n"
        "    e.command[0].code !== 0)\n"
        "  fail('command', JSON.stringify(e.command));\n"
        "if (e.notify.length !== 1 || e.notify[0].self !== true ||\n"
        "    e.notify[0].code !== -2)\n"
        "  fail('notify', JSON.stringify(e.notify));\n",
        "gui-object-channels-check.mjs");

  // ---- S8/S9: setters write through, and Win32 agrees ---------------------
  check(runtime,
        "const g = globalThis.g;\n"
        "const r = globalThis.r;\n"
        "g.Title = 'rime-gui-object-slice-2';\n"
        "g.Name = 'mainGui';\n"
        "g.MarginX = 14;\n"
        "g.MarginY = 9;\n"
        "g.BackColor = '#123456';\n"
        "r.text.Text = 'world';\n"
        "r.edit.Value = 'changed2';\n"
        "r.check.Value = 1;\n"
        "r.prog.Value = 75;\n"
        "r.text.Enabled = 0;\n"
        // out2 is opened here so the Move promise lands in the same bucket
        // the readback module later fills with pump reads.
        "globalThis.out2 = {};\n"
        "globalThis.track(globalThis.out2, 'move', g.Move(140, 90, 360, 210));\n",
        "gui-object-setters.mjs");
  require_ok(runtime.settle(10000ms), "setter settle");
  check(runtime,
        "const g = globalThis.g;\n"
        "const r = globalThis.r;\n"
        "globalThis.track(globalThis.out2, 'title', g.Title);\n"
        "globalThis.track(globalThis.out2, 'marginX', g.MarginX);\n"
        "globalThis.track(globalThis.out2, 'marginY', g.MarginY);\n"
        "globalThis.track(globalThis.out2, 'backColor', g.BackColor);\n"
        "globalThis.track(globalThis.out2, 'text', r.text.Text);\n"
        "globalThis.track(globalThis.out2, 'editValue', r.edit.Value);\n"
        "globalThis.track(globalThis.out2, 'checkValue', r.check.Value);\n"
        "globalThis.track(globalThis.out2, 'progValue', r.prog.Value);\n"
        "globalThis.track(globalThis.out2, 'enabled', r.text.Enabled);\n"
        "globalThis.track(globalThis.out2, 'pos', g.GetPos());\n"
        "globalThis.track(globalThis.out2, 'clientPos', g.GetClientPos());\n",
        "gui-object-readback.mjs");
  require_ok(runtime.settle(10000ms), "readback settle");
  check(runtime,
        "const o = globalThis.out2;\n"
        "const fail = (key, detail) => { throw new Error(key + ': ' + detail); };\n"
        "if (o.title !== 'rime-gui-object-slice-2') fail('title', JSON.stringify(o.title));\n"
        "if (o.marginX !== 14 || o.marginY !== 9)\n"
        "  fail('margins', JSON.stringify([o.marginX, o.marginY]));\n"
        "if (o.backColor !== '123456') fail('backColor', JSON.stringify(o.backColor));\n"
        "if (o.text !== 'world') fail('text', JSON.stringify(o.text));\n"
        "if (o.editValue !== 'changed2') fail('editValue', JSON.stringify(o.editValue));\n"
        "if (o.checkValue !== 1) fail('checkValue', JSON.stringify(o.checkValue));\n"
        "if (o.progValue !== 75) fail('progValue', JSON.stringify(o.progValue));\n"
        "if (o.enabled !== false) fail('enabled', JSON.stringify(o.enabled));\n"
        "if (o.move !== null) fail('move', JSON.stringify(o.move));\n"
        "const want = { x: 140, y: 90, width: 360, height: 210 };\n"
        "for (const key of ['x', 'y', 'width', 'height']) {\n"
        "  if (o.pos[key] !== want[key])\n"
        "    fail('pos.' + key, JSON.stringify(o.pos));\n"
        "}\n"
        "if (!(o.clientPos.width > 0 && o.clientPos.width <= want.width))\n"
        "  fail('clientPos.width', JSON.stringify(o.clientPos));\n"
        "if (!(o.clientPos.height > 0 && o.clientPos.height <= want.height))\n"
        "  fail('clientPos.height', JSON.stringify(o.clientPos));\n",
        "gui-object-readback-check.mjs");

  // Win32, not the service: the window text and the window rect must say
  // what the JS mirror claims.
  require(window_text(hwnd) == kRenamedTitle,
          "g.Title = ... must reach GetWindowTextW on the real window");
  RECT rect{};
  require(GetWindowRect(hwnd, &rect) != FALSE, "GetWindowRect works");
  require(rect.left == 140 && rect.top == 90, "GetWindowRect agrees with Move x/y");
  require(rect.right - rect.left == 360, "GetWindowRect agrees with Move width");
  require(rect.bottom - rect.top == 210, "GetWindowRect agrees with Move height");

  // ---- S9b: batch-3a list family (construct + OS readback + Change) -----
  // Three implementations (ListBox/ComboBox/DropDownList) plus the AddDDL
  // ledger alias; rows come from string arrays, a bare string is one row.
  // Placed while the main Gui is alive (S13 destroys it); later sections
  // only assert per-member behavior and channel baselines, never control
  // counts, so four extra children disturb nothing downstream.
  check(runtime,
        "const g = globalThis.g;\n"
        "globalThis.wv = { change: [] };\n"
        "globalThis.track(globalThis.wv, 'list',\n"
        "  g.AddListBox('x10 y200 w200 h80 vMyList', ['a', 'b', 'c']));\n"
        "globalThis.track(globalThis.wv, 'combo',\n"
        "  g.AddComboBox('x10 y300 w200', ['x', 'y']));\n"
        "globalThis.track(globalThis.wv, 'ddl',\n"
        "  g.AddDropDownList('x10 y340 w200', ['p', 'q']));\n"
        "globalThis.track(globalThis.wv, 'ddlAlias', g.AddDDL('x10 y380 w200', ['m']));\n",
        "gui-object-list-create.mjs");
  require_ok(runtime.settle(10000ms), "list create settle");
  check(runtime,
        "const wv = globalThis.wv;\n"
        "if (wv.list.Type !== 'ListBox') throw new Error('list Type: ' + wv.list.Type);\n"
        "if (wv.combo.Type !== 'ComboBox') throw new Error('combo Type: ' + wv.combo.Type);\n"
        "if (wv.ddl.Type !== 'DropDownList') throw new Error('ddl Type: ' + wv.ddl.Type);\n"
        "if (wv.ddlAlias.Type !== 'DropDownList')\n"
        "  throw new Error('AddDDL must alias the DropDownList kind');\n"
        "globalThis.track(globalThis.wv, 'changeReg',\n"
        "  wv.list.OnEvent('Change', (target) => {\n"
        "    globalThis.wv.change.push(target === wv.list);\n"
        "  }));\n",
        "gui-object-list-types.mjs");
  require_ok(runtime.settle(10000ms), "list Change register settle");
  check(runtime,
        "if (globalThis.wv.changeReg !== null)\n"
        "  throw new Error('OnEvent(Change) must resolve null');\n",
        "gui-object-list-change-arm.mjs");
  {
    // OS readback: real WC_LISTBOXW/WC_COMBOBOXW children with real rows.
    HWND list = find_child(hwnd, L"LISTBOX");
    require(list != nullptr, "FindWindowExW finds the ListBox child");
    require(SendMessageW(list, LB_GETCOUNT, 0, 0) == 3, "LB_GETCOUNT sees three rows");
    wchar_t row[64]{};
    require(SendMessageW(list, LB_GETTEXT, 1, reinterpret_cast<LPARAM>(row)) != LB_ERR,
            "LB_GETTEXT reads back");
    require(wcscmp(row, L"b") == 0, "second row is 'b'");
    int combos = 0;
    std::string combo_firsts;
    int combo_total = 0;
    for (HWND child = find_child(hwnd, L"COMBOBOX"); child != nullptr;
         child = FindWindowExW(hwnd, child, L"COMBOBOX", nullptr)) {
      const LRESULT count = SendMessageW(child, CB_GETCOUNT, 0, 0);
      wchar_t first[64]{};
      SendMessageW(child, CB_GETLBTEXT, 0, reinterpret_cast<LPARAM>(first));
      char narrow[64]{};
      for (int i = 0; i < 63 && first[i] != 0; ++i) narrow[i] = static_cast<char>(first[i]);
      combo_firsts += narrow;
      combo_firsts += ',';
      combo_total += static_cast<int>(count);
      ++combos;
    }
    require(combos == 3, "ComboBox, DropDownList and the AddDDL alias all exist");
    require(combo_total == 5, "row counts are {2,2,1}");
    // First rows identify each box regardless of enumeration order.
    require(combo_firsts.find("x,") != std::string::npos &&
                combo_firsts.find("p,") != std::string::npos &&
                combo_firsts.find("m,") != std::string::npos,
            "first rows are x, p and m");
    // Route a real WM_COMMAND through the pump: selection change must reach
    // the registered Change handler with the control as target. The wait is
    // armed first so settle blocks on the polling timer (not on blind
    // quiescence, which would let the assert win the race with dispatch).
    check(runtime,
          "globalThis.wv.changeWait = waitFor(\n"
          "    () => globalThis.wv.change.length > 0, 4000)\n"
          "    .then(v => { globalThis.wv.changeSeen = v; });\n",
          "gui-object-list-change-arm2.mjs");
    require(PostMessageW(hwnd, WM_COMMAND,
                         MAKEWPARAM(GetDlgCtrlID(list), LBN_SELCHANGE),
                         reinterpret_cast<LPARAM>(list)) != FALSE,
            "WM_COMMAND/LBN_SELCHANGE posts through the real queue");
  }
  require_ok(runtime.settle(10000ms), "list change settle");
  check(runtime,
        "if (globalThis.wv.changeSeen !== true)\n"
        "  throw new Error('the Change handler never ran: ' +\n"
        "                  JSON.stringify(globalThis.wv.change));\n"
        "if (globalThis.wv.change.length !== 1 || globalThis.wv.change[0] !== true)\n"
        "  throw new Error('Change must fire once with the list as target: ' +\n"
        "                  JSON.stringify(globalThis.wv.change));\n",
        "gui-object-list-change.mjs");

  // ---- S9c: batch-3a date/slider family ----------------------------------
  // DateTime/MonthCal take YYYYMMDD strings, Slider/UpDown take 0..100
  // numbers; readback is OS messages (DTM_GETSYSTEMTIME/MCM_GETCURSEL/
  // TBM_GETPOS/UDM_GETPOS32) plus the JS Value reads through the new
  // read_value branches.
  check(runtime,
        "const g = globalThis.g;\n"
        "globalThis.wd = {};\n"
        "globalThis.track(globalThis.wd, 'date',\n"
        "  g.AddDateTime('x10 y460 w200', '20261008'));\n"
        "globalThis.track(globalThis.wd, 'month',\n"
        "  g.AddMonthCal('x10 y500 w220 h160', '20260228'));\n"
        "globalThis.track(globalThis.wd, 'badDate',\n"
        "  g.AddMonthCal('x10 y500 w220 h160', '20260229'));\n"
        "globalThis.track(globalThis.wd, 'slider', g.AddSlider('x10 y680 w200', 42));\n"
        "globalThis.track(globalThis.wd, 'updown', g.AddUpDown('x220 y680 w40', 7));\n",
        "gui-object-date-create.mjs");
  require_ok(runtime.settle(10000ms), "date create settle");
  check(runtime,
        "const wd = globalThis.wd;\n"
        "if (wd.date.Type !== 'DateTime') throw new Error('date Type: ' + wd.date.Type);\n"
        "if (wd.month.Type !== 'MonthCal') throw new Error('month Type: ' + wd.month.Type);\n"
        "if (wd.badDate === null || typeof wd.badDate !== 'object' ||\n"
        "    wd.badDate.code !== 'invalid_contract')\n"
        "  throw new Error('Feb 29 2026 must refuse invalid_contract: ' +\n"
        "                  JSON.stringify(wd.badDate));\n"
        "if (wd.slider.Type !== 'Slider') throw new Error('slider Type: ' + wd.slider.Type);\n"
        "if (wd.updown.Type !== 'UpDown') throw new Error('updown Type: ' + wd.updown.Type);\n"
        "globalThis.track(globalThis.wd, 'dateValue', wd.date.Value);\n"
        "globalThis.track(globalThis.wd, 'monthValue', wd.month.Value);\n"
        "globalThis.track(globalThis.wd, 'sliderValue', wd.slider.Value);\n"
        "globalThis.track(globalThis.wd, 'updownValue', wd.updown.Value);\n",
        "gui-object-date-types.mjs");
  require_ok(runtime.settle(10000ms), "date value settle");
  check(runtime,
        "const wd = globalThis.wd;\n"
        "if (wd.dateValue !== '20261008')\n"
        "  throw new Error('date Value: ' + JSON.stringify(wd.dateValue));\n"
        "if (wd.monthValue !== '20260228')\n"
        "  throw new Error('month Value: ' + JSON.stringify(wd.monthValue));\n"
        "if (wd.sliderValue !== 42) throw new Error('slider Value: ' + JSON.stringify(wd.sliderValue));\n"
        "if (wd.updownValue !== 7) throw new Error('updown Value: ' + JSON.stringify(wd.updownValue));\n",
        "gui-object-date-values.mjs");
  {
    // OS readback on the real common controls.
    HWND date = find_child(hwnd, L"SysDateTimePick32");
    require(date != nullptr, "FindWindowExW finds the DateTime child");
    SYSTEMTIME selected{};
    require(SendMessageW(date, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&selected)) ==
                GDT_VALID,
            "DTM_GETSYSTEMTIME succeeds");
    require(selected.wYear == 2026 && selected.wMonth == 10 && selected.wDay == 8,
            "the DateTime shows 2026-10-08");
    HWND month = find_child(hwnd, L"SysMonthCal32");
    require(month != nullptr, "FindWindowExW finds the MonthCal child");
    SYSTEMTIME current{};
    require(SendMessageW(month, MCM_GETCURSEL, 0, reinterpret_cast<LPARAM>(&current)) != FALSE,
            "MCM_GETCURSEL succeeds");
    require(current.wYear == 2026 && current.wMonth == 2 && current.wDay == 28,
            "the MonthCal shows 2026-02-28");
    HWND slider = find_child(hwnd, L"msctls_trackbar32");
    require(slider != nullptr, "FindWindowExW finds the Slider child");
    require(SendMessageW(slider, TBM_GETPOS, 0, 0) == 42, "TBM_GETPOS reads 42");
    HWND updown = find_child(hwnd, L"msctls_updown32");
    require(updown != nullptr, "FindWindowExW finds the UpDown child");
    BOOL updown_failed = TRUE;
    const LRESULT updown_pos =
        SendMessageW(updown, UDM_GETPOS32, 0, reinterpret_cast<LPARAM>(&updown_failed));
    require(updown_failed == FALSE && updown_pos == 7, "UDM_GETPOS32 reads 7");
  }

  // ---- S9d: batch-3a wave 3 (rich family) ----------------------------------
  // Tab/Link/Hotkey/ListView/TreeView/StatusBar construct; AddTab2/AddPic
  // are ledger aliases (same kinds). Readback is OS messages on the real
  // common controls; per-control Value semantics belong to 3b methods.
  //
  // The Pic alias needs a loadable image: a 1x1 BMP is written to %TEMP%
  // (deleted right after creation; the service keeps a private copy).
  std::wstring pic_path;
  {
    wchar_t temp_dir[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH, temp_dir) != 0, "GetTempPathW works");
    pic_path = std::wstring(temp_dir) + L"rime-gui-pic.bmp";
    unsigned char bmp[58]{};
    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[10] = 54;  // pixel data offset
    bmp[14] = 40;  // BITMAPINFOHEADER size
    bmp[18] = 1;   // width
    bmp[22] = 1;   // height
    bmp[26] = 1;   // planes
    bmp[28] = 24;  // bits per pixel
    bmp[54] = 0x12;
    bmp[55] = 0x34;
    bmp[56] = 0x56;
    HANDLE file = CreateFileW(pic_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    require(file != INVALID_HANDLE_VALUE, "scratch BMP is writable");
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, bmp, sizeof(bmp), &written, nullptr);
    CloseHandle(file);
    require(wrote && written == sizeof(bmp), "scratch BMP wrote fully");
  }
  std::string pic_js;
  {
    // JS string literal: backslashes escaped, UTF-8 bytes otherwise.
    int needed = WideCharToMultiByte(CP_UTF8, 0, pic_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    require(needed > 1, "BMP path converts to UTF-8");
    std::string utf8(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, pic_path.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
    pic_js.reserve(utf8.size() + 8);
    for (const char c : utf8) {
      if (c == '\\') pic_js += "\\\\";
      else pic_js += c;
    }
  }
  check(runtime,
        "const g = globalThis.g;\n"
        "globalThis.wr = {};\n"
        "globalThis.track(globalThis.wr, 'tab',\n"
        "  g.AddTab('x10 y720 w300 h120', ['T1', 'T2']));\n"
        "globalThis.track(globalThis.wr, 'tabAlias', g.AddTab2('x10 y720 w300 h120', ['U1']));\n"
        "globalThis.track(globalThis.wr, 'picAlias', g.AddPic('x10 y860 w64 h64', '" +
        pic_js + "'));\n"
        "globalThis.track(globalThis.wr, 'link',\n"
        "  g.AddLink('x10 y940 w300', 'go <a href=\"https://example.test\">here</a>'));\n"
        "globalThis.track(globalThis.wr, 'hotkey', g.AddHotkey('x10 y980 w200'));\n"
        "globalThis.track(globalThis.wr, 'hotkeyContent', g.AddHotkey('x10 y980 w200', 'Ctrl+C'));\n"
        "globalThis.track(globalThis.wr, 'listview',\n"
        "  g.AddListView('x340 y200 w300 h120', ['A', 'B']));\n"
        "globalThis.track(globalThis.wr, 'tree',\n"
        "  g.AddTreeView('x340 y340 w300 h140', ['r1', 'r2']));\n"
        "globalThis.track(globalThis.wr, 'status',\n"
        "  g.AddStatusBar('', ['left', 'right']));\n",
        "gui-object-rich-create.mjs");
  require_ok(runtime.settle(10000ms), "rich create settle");
  // The service keeps a private copy; the scratch file can go now.
  require(DeleteFileW(pic_path.c_str()) != FALSE, "scratch BMP is removed");
  check(runtime,
        "const wr = globalThis.wr;\n"
        "const fail = (key, detail) => { throw new Error(key + ': ' + detail); };\n"
        "if (wr.tab.Type !== 'Tab') fail('tab Type', wr.tab.Type);\n"
        "if (wr.tabAlias.Type !== 'Tab') fail('AddTab2 must alias Tab');\n"
        "if (wr.picAlias.Type !== 'Picture') fail('AddPic must alias Picture');\n"
        "if (wr.link.Type !== 'Link') fail('link Type', wr.link.Type);\n"
        "if (wr.hotkey.Type !== 'Hotkey') fail('hotkey Type', wr.hotkey.Type);\n"
        "if (wr.listview.Type !== 'ListView') fail('listview Type', wr.listview.Type);\n"
        "if (wr.tree.Type !== 'TreeView') fail('tree Type', wr.tree.Type);\n"
        "if (wr.status.Type !== 'StatusBar') fail('status Type', wr.status.Type);\n"
        "if (!wr.hotkeyContent || wr.hotkeyContent.code !== 'invalid_contract')\n"
        "  fail('hotkey content', 'construction content must refuse invalid_contract');\n",
        "gui-object-rich-types.mjs");
  {
    // OS readback on the real common controls.
    for (HWND dbg = FindWindowExW(hwnd, nullptr, nullptr, nullptr); dbg != nullptr;
         dbg = FindWindowExW(hwnd, dbg, nullptr, nullptr)) {
      wchar_t cls[64]{};
      GetClassNameW(dbg, cls, 64);
      RECT rc{};
      GetWindowRect(dbg, &rc);
      std::printf("DEBUG child cls=%ls w=%d h=%d vis=%d\n", cls, (int)(rc.right - rc.left),
                  (int)(rc.bottom - rc.top), (int)IsWindowVisible(dbg));
    }
    std::fflush(stdout);
    HWND tab = find_child(hwnd, L"SysTabControl32");
    require(tab != nullptr, "FindWindowExW finds the Tab child");
    require(SendMessageW(tab, TCM_GETITEMCOUNT, 0, 0) == 2, "TCM_GETITEMCOUNT sees two pages");
    HWND link = find_child(hwnd, L"SysLink");
    require(link != nullptr, "FindWindowExW finds the Link child");
    wchar_t link_text[128]{};
    require(GetWindowTextW(link, link_text, 128) > 0, "Link text reads back");
    require(wcsstr(link_text, L"here") != nullptr, "Link markup text round-trips");
    HWND hotkey = find_child(hwnd, L"msctls_hotkey32");
    require(hotkey != nullptr, "FindWindowExW finds the Hotkey child");
    require(SendMessageW(hotkey, HKM_GETHOTKEY, 0, 0) == 0, "Hotkey starts empty");
    HWND listview = find_child(hwnd, L"SysListView32");
    require(listview != nullptr, "FindWindowExW finds the ListView child");
    HWND header = reinterpret_cast<HWND>(SendMessageW(listview, LVM_GETHEADER, 0, 0));
    require(header != nullptr, "LVM_GETHEADER returns the header");
    require(SendMessageW(header, HDM_GETITEMCOUNT, 0, 0) == 2, "two columns landed");
    HWND tree = find_child(hwnd, L"SysTreeView32");
    require(tree != nullptr, "FindWindowExW finds the TreeView child");
    require(SendMessageW(tree, TVM_GETCOUNT, 0, 0) == 2, "TVM_GETCOUNT sees two roots");
    HWND status = nullptr;
    // NOTE: FindWindowExW(hwnd, nullptr, L"msctls_statusbar32", nullptr)
    // inexplicably misses this live, visible child (verified: same parent,
    // exact class bytes, IsWindow true) while manual enumeration with
    // wcscmp finds it every time. Do NOT "simplify" back to FindWindowExW
    // without re-proving on a real desktop.
    for (HWND child = FindWindowExW(hwnd, nullptr, nullptr, nullptr); child != nullptr;
         child = FindWindowExW(hwnd, child, nullptr, nullptr)) {
      wchar_t cls[64]{};
      GetClassNameW(child, cls, 64);
      if (wcscmp(cls, L"msctls_statusbar32") == 0) {
        status = child;
        break;
      }
    }
    require(SendMessageW(status, SB_GETPARTS, 0, 0) == 2, "SB_GETPARTS sees two parts");
    wchar_t part[64]{};
    require(SendMessageW(status, SB_GETTEXTW, 0, reinterpret_cast<LPARAM>(part)) > 0,
            "SB_GETTEXT reads back");
    require(wcscmp(part, L"left") == 0, "first part is 'left'");
  }

  // ---- S9e: batch-3b control methods ---------------------------------------
  // Every method resolves a service-observed value (row counts from
  // LVM_GETITEMCOUNT, ids from the pump registry, text from control reads);
  // a facade answering from its own mirror cannot produce them. C++
  // cross-checks below re-read the same state with raw OS messages.
  // A 1x1 ICO is written to %TEMP% for SetIcon (deleted after the call;
  // the owner keeps a private copy from then on). S9e and S9f each write
  // their own (same path, rewritten).
  std::wstring ico_path;
  std::string ico_js;
  const auto write_scratch_ico = [&] {
    wchar_t temp_dir[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH, temp_dir) != 0, "GetTempPathW works");
    ico_path = std::wstring(temp_dir) + L"rime-gui-icon.ico";
    // Hand-rolled 1x1 32bpp ICO. Offsets are file-absolute: ICONDIR
    // (0-5), entry (6-21, image at 22), BITMAPINFOHEADER (22-61: size at
    // 22, width at 26, height+xor+and at 30, planes at 34, bpp at 36,
    // sizeImage at 42), XOR mask (62-65, opaque red), AND mask (66-69).
    // The header fields sit immediately after the 4-byte size - an earlier
    // revision put them 2 bytes late (width at 28) and LoadImage rejected
    // every file; bisected against OneDrive.ico's real 16x16 image.
    unsigned char ico[70]{};
    ico[2] = 1;  // ICONDIR type = icon
    ico[4] = 1;  // one image
    ico[6] = 1;  // entry width 1
    ico[7] = 1;  // entry height 1 (0 would mean 256)
    ico[8] = 0;  // no palette
    ico[10] = 1;  // entry planes
    ico[12] = 32;  // 32bpp
    ico[14] = 48;  // image bytes (40 header + 4 xor + 4 and)
    ico[18] = 22;  // image offset (ICONDIR 6 + entry 16)
    ico[22] = 40;  // BITMAPINFOHEADER size
    ico[26] = 1;  // width
    ico[30] = 2;  // biHeight: xor rows + and rows
    ico[34] = 1;  // planes
    ico[36] = 32;  // bpp
    ico[42] = 8;  // image size (xor 4 + and 4)
    ico[62] = 0;  // xor pixel B
    ico[63] = 0;  // xor pixel G
    ico[64] = 0xFF;  // xor pixel R
    ico[65] = 0xFF;  // xor pixel A: opaque
    HANDLE file = CreateFileW(ico_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    require(file != INVALID_HANDLE_VALUE, "scratch ICO is writable");
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, ico, sizeof(ico), &written, nullptr);
    CloseHandle(file);
    require(wrote && written == sizeof(ico), "scratch ICO wrote fully");
    int needed =
        WideCharToMultiByte(CP_UTF8, 0, ico_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    require(needed > 1, "ICO path converts to UTF-8");
    std::string utf8(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ico_path.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
    ico_js.clear();
    ico_js.reserve(utf8.size() + 8);
    for (const char c : utf8) {
      if (c == '\\') ico_js += "\\\\";
      else ico_js += c;
    }
  };
  write_scratch_ico();
  check(runtime,
        "const g = globalThis.g;\n"
        "globalThis.m9e = {};\n"
        "globalThis.m9eDone = 'pending';\n"
        "(async () => {\n"
        "  const m = globalThis.m9e;\n"
        "  m.lb = await g.AddListBox('x10 y1100 w200 h60', ['a', 'b', 'c']);\n"
        "  m.added = await m.lb.Add(['d', 'e']);\n"
        "  m.choose = await m.lb.Choose(1);\n"
        "  m.chooseText = await m.lb.Choose('d');\n"
        "  m.deleted = await m.lb.Delete(0);\n"
        "  m.cleared = await m.lb.Delete();\n"
        "  m.cb = await g.AddComboBox('x220 y1100 w200', ['x', 'y']);\n"
        "  m.cbCue = await m.cb.SetCue('pick one');\n"
        "  m.ed = await g.AddEdit('x430 y1100 w200', 'keep');\n"
        "  m.edCue = await m.ed.SetCue('type here', false);\n"
        "  const lv = globalThis.wr.listview;\n"
        "  m.r0 = await lv.Add(['a1', 'b1']);\n"
        "  m.r1 = await lv.Add(['a2', 'b2'], true);\n"
        "  m.count = await lv.GetCount();\n"
        "  m.sel = await lv.GetCount('selected');\n"
        "  m.cols = await lv.GetCount('columns');\n"
        "  m.t = await lv.GetText(0, 1);\n"
        "  m.nx = await lv.GetNext();\n"
        "  m.nxNone = await lv.GetNext(1);\n"
        "  m.ins = await lv.Insert(1, ['m1', 'm2']);\n"
        "  m.mod = await lv.Modify(0, ['z1']);\n"
        "  m.t2 = await lv.GetText(0);\n"
        "  m.foc = await lv.Modify(2, undefined, undefined, true);\n"
        "  m.nxFoc = await lv.GetNext(-1, 'focused');\n"
        "  m.chkRow = await lv.Add(['c1', 'c2'], undefined, undefined, true);\n"
        "  m.nxChk = await lv.GetNext(-1, 'checked');\n"
        "  m.newCol = await lv.InsertCol(2, 'C', 80, 'right');\n"
        "  m.modCol = await lv.ModifyCol(0, 'A!', 120, 'center');\n"
        "  m.cols2 = await lv.GetCount('columns');\n"
        "  m.delCol = await lv.DeleteCol(2);\n"
        "  m.delRow = await lv.Delete(0);\n"
        "  const tv = globalThis.wr.tree;\n"
        "  m.n1 = await tv.Add('root3');\n"
        "  m.c1 = await tv.Add('child1', m.n1);\n"
        "  m.tcnt = await tv.GetCount();\n"
        "  m.tx = await tv.GetText(m.c1);\n"
        "  m.ch = await tv.GetChild(m.n1);\n"
        "  m.pa = await tv.GetParent(m.c1);\n"
        "  m.nxSib = await tv.GetNext(m.c1);\n"
        "  m.nxDoc = await tv.GetNext(m.n1, 'document');\n"
        "  m.first = await tv.GetNext();\n"
        "  m.prev = await tv.GetPrev(m.c1);\n"
        "  m.exp = await tv.Get(m.n1, 'expanded');\n"
        "  m.bold = await tv.Get(m.n1, 'bold');\n"
        "  m.modN = await tv.Modify(m.n1, 'root3!', true, true);\n"
        "  m.selN = await tv.GetSelection();\n"
        "  m.exp2 = await tv.Get(m.n1, 'expanded');\n"
        "  m.delN = await tv.Delete(m.n1);\n"
        "  m.tcnt2 = await tv.GetCount();\n"
        "  try { await tv.GetText(m.c1); m.stale = 'NO-REFUSE'; }\n"
        "  catch (e) { m.stale = e && e.code ? e.code : '?'; }\n"
        "  try { await lv.Delete(99); m.oob = 'NO-REFUSE'; }\n"
        "  catch (e) { m.oob = e && e.code ? e.code : '?'; }\n"
        "  try { await lv.SetParts([1]); m.kind = 'NO-REFUSE'; }\n"
        "  catch (e) { m.kind = (e && e.constructor ? e.constructor.name : '?') + ':' +\n"
        "    (e && e.message ? e.message : ''); }\n"
        "  try { await m.lb.SetCue('x'); m.cueKind = 'NO-REFUSE'; }\n"
        "  catch (e) { m.cueKind = e && e.code ? e.code : '?'; }\n"
        "  const sb = globalThis.wr.status;\n"
        "  m.parts = await sb.SetParts([100, 150]);\n"
        "  m.st1 = await sb.SetText('hello', 1);\n"
        "  m.st0 = await sb.SetText('plain', 0, false);\n"
        "  m.icon = await sb.SetIcon('" +
        ico_js + "');\n"
        "  const tab = globalThis.wr.tab;\n"
        "  m.use1 = await tab.UseTab(1);\n"
        "  m.useName = await tab.UseTab('T1');\n"
        "  m.dt = await g.AddDateTime('x10 y1200 w200');\n"
        "  m.fmt = await m.dt.SetFormat('yyyy');\n"
        "  m.hk = await globalThis.wr.hotkey.SetHotkey(['ctrl', 'shift'], 'A');\n"
        "})().then(() => { globalThis.m9eDone = 'ok'; },\n"
        "  e => { globalThis.m9eDone = { code: (e && e.code) || '',\n"
        "    ctor: e && e.constructor ? e.constructor.name : '?',\n"
        "    message: String((e && e.message) || '') }; });\n",
        "gui-object-3b-invoke.mjs");
  require_ok(runtime.settle(30000ms), "3b invoke settle");
  require(DeleteFileW(ico_path.c_str()) != FALSE, "scratch ICO is removed");
  check(runtime,
        "const m = globalThis.m9e;\n"
        "if (globalThis.m9eDone !== 'ok')\n"
        "  throw new Error('3b sequence failed: ' + JSON.stringify(globalThis.m9eDone));\n"
        "const eq = (key, actual, want) => {\n"
        "  if (actual !== want) throw new Error(key + ': got ' + JSON.stringify(actual) +\n"
        "    ', want ' + JSON.stringify(want));\n"
        "};\n"
        "eq('added', m.added, 5);\n"
        "eq('choose', m.choose, 1);\n"
        "eq('chooseText', m.chooseText, 3);\n"
        "eq('deleted', m.deleted, 4);\n"
        "eq('cleared', m.cleared, 0);\n"
        "eq('cbCue', m.cbCue, null);\n"
        "eq('edCue', m.edCue, null);\n"
        "eq('r0', m.r0, 0);\n"
        "eq('r1', m.r1, 1);\n"
        "eq('count', m.count, 2);\n"
        "eq('sel', m.sel, 1);\n"
        "eq('cols', m.cols, 2);\n"
        "eq('t', m.t, 'b1');\n"
        "eq('nx', m.nx, 1);\n"
        "eq('nxNone', m.nxNone, null);\n"
        "eq('ins', m.ins, 1);\n"
        "eq('mod', m.mod, 0);\n"
        "eq('t2', m.t2, 'z1');\n"
        "eq('foc', m.foc, 2);\n"
        "eq('nxFoc', m.nxFoc, 2);\n"
        "eq('nxChk', m.nxChk, m.chkRow);\n"
        "eq('newCol', m.newCol, 2);\n"
        "eq('modCol', m.modCol, 0);\n"
        "eq('cols2', m.cols2, 3);\n"
        "eq('delCol', m.delCol, 2);\n"
        "eq('delRow', m.delRow, 3);\n"
        "eq('tcnt', m.tcnt, 4);\n"
        "eq('tx', m.tx, 'child1');\n"
        "eq('ch', m.ch, m.c1);\n"
        "eq('pa', m.pa, m.n1);\n"
        "eq('nxSib', m.nxSib, null);\n"
        "eq('nxDoc', m.nxDoc, m.c1);\n"
        "eq('prev', m.prev, null);\n"
        "eq('exp', m.exp, false);\n"
        "eq('bold', m.bold, false);\n"
        "eq('modN', m.modN, m.n1);\n"
        "eq('selN', m.selN, m.n1);\n"
        "eq('exp2', m.exp2, true);\n"
        "eq('delN', m.delN, 2);\n"
        "eq('tcnt2', m.tcnt2, 2);\n"
        "eq('stale', m.stale, 'invalid_contract');\n"
        "eq('oob', m.oob, 'invalid_contract');\n"
        "eq('cueKind', m.cueKind, 'invalid_contract');\n"
        "eq('first', m.first, 1);\n"
        "eq('kind', m.kind, 'TypeError:SetParts() needs a StatusBar');\n"
        "eq('parts', m.parts, 2);\n"
        "eq('st1', m.st1, 1);\n"
        "eq('st0', m.st0, 0);\n"
        "eq('icon', m.icon, 0);\n"
        "eq('use1', m.use1, 1);\n"
        "eq('useName', m.useName, 0);\n"
        "eq('fmt', m.fmt, null);\n"
        "eq('hk', m.hk, null);\n",
        "gui-object-3b-assert.mjs");
  {
    // OS re-read of the same state: row/column counts and surviving text
    // on the real controls, the bar's parts/icon, the hotkey chord and the
    // DateTime year-only text. Final LV state is [m1, a2, c1] in 2 columns
    // (row 0 deleted, third column deleted); TV is back to its 2 roots.
    HWND listview = find_child(hwnd, L"SysListView32");
    require(listview != nullptr, "FindWindowExW finds the ListView child");
    require(SendMessageW(listview, LVM_GETITEMCOUNT, 0, 0) == 3, "three rows survive");
    HWND header =
        reinterpret_cast<HWND>(SendMessageW(listview, LVM_GETHEADER, 0, 0));
    require(SendMessageW(header, HDM_GETITEMCOUNT, 0, 0) == 2, "two columns survive");
    wchar_t cell[64]{};
    LVITEMW get_cell{};
    get_cell.mask = LVIF_TEXT;
    get_cell.iItem = 0;
    get_cell.iSubItem = 0;
    get_cell.pszText = cell;
    get_cell.cchTextMax = 64;
    require(SendMessageW(listview, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&get_cell)) != FALSE,
            "LVM_GETITEM reads row 0");
    require(wcscmp(cell, L"m1") == 0, "row 0 col 0 is 'm1'");
    HWND tree = find_child(hwnd, L"SysTreeView32");
    require(tree != nullptr, "FindWindowExW finds the TreeView child");
    require(SendMessageW(tree, TVM_GETCOUNT, 0, 0) == 2, "the subtree delete left two roots");
    HWND status = nullptr;
    for (HWND child = FindWindowExW(hwnd, nullptr, nullptr, nullptr); child != nullptr;
         child = FindWindowExW(hwnd, child, nullptr, nullptr)) {
      wchar_t cls[64]{};
      GetClassNameW(child, cls, 64);
      if (wcscmp(cls, L"msctls_statusbar32") == 0) {
        status = child;
        break;
      }
    }
    require(status != nullptr, "status bar enumerates");
    require(SendMessageW(status, SB_GETPARTS, 0, 0) == 2, "SetParts left two parts");
    wchar_t part1[64]{};
    SendMessageW(status, SB_GETTEXTW, 1, reinterpret_cast<LPARAM>(part1));
    require(wcscmp(part1, L"hello") == 0, "part 1 reads 'hello'");
    require(SendMessageW(status, SB_GETICON, 0, 0) != 0, "part 0 carries the icon");
    HWND hotkey = find_child(hwnd, L"msctls_hotkey32");
    require(hotkey != nullptr, "FindWindowExW finds the Hotkey child");
    require(SendMessageW(hotkey, HKM_GETHOTKEY, 0, 0) == MAKEWORD('A', HOTKEYF_CONTROL | HOTKEYF_SHIFT),
            "HKM_GETHOTKEY reads Ctrl+Shift+A");
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t year[8]{};
    swprintf_s(year, L"%04u", now.wYear);
    bool year_only = false;
    for (HWND dt = FindWindowExW(hwnd, nullptr, L"SysDateTimePick32", nullptr); dt != nullptr;
         dt = FindWindowExW(hwnd, dt, L"SysDateTimePick32", nullptr)) {
      wchar_t text[64]{};
      GetWindowTextW(dt, text, 64);
      if (wcscmp(text, year) == 0) year_only = true;
    }
    require(year_only, "one DateTime shows the year only");
    // Cue banners have no mirror to consult (there is no service-side
    // getter, same as AHK): read them with the OS getters. Child order is
    // not contractual, so every ComboBox/Edit is probed and at least one
    // must carry the banner this section set.
    bool combo_cue = false;
    for (HWND cb = FindWindowExW(hwnd, nullptr, L"ComboBox", nullptr); cb != nullptr;
         cb = FindWindowExW(hwnd, cb, L"ComboBox", nullptr)) {
      wchar_t cue[64]{};
      SendMessageW(cb, CB_GETCUEBANNER, reinterpret_cast<WPARAM>(cue), 64);
      if (wcscmp(cue, L"pick one") == 0) combo_cue = true;
    }
    require(combo_cue, "one ComboBox carries its cue banner");
    bool edit_cue = false;
    for (HWND ed = FindWindowExW(hwnd, nullptr, L"Edit", nullptr); ed != nullptr;
         ed = FindWindowExW(hwnd, ed, L"Edit", nullptr)) {
      wchar_t cue[64]{};
      SendMessageW(ed, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue), 64);
      if (wcscmp(cue, L"type here") == 0) edit_cue = true;
    }
    require(edit_cue, "one Edit carries its cue banner");
  }

  // ---- S9f: batch-3c menus ------------------------------------------------
  // Items are addressed by exact name or 0-based position; callbacks ride
  // the menu's own channel (MenuSelect), popup Show resolves the selected
  // name modally (TPM_RETURNCMD). The modal rounds need keystrokes while
  // the pump is inside TrackPopupMenuEx, so an injector thread posts them
  // (ui_slice.cpp dialog_watcher shape: condition poll with a timeout, the
  // assertions stay on settle + value reads). C++ re-reads HMENU state
  // with raw menu APIs; the menubar click is a real WM_COMMAND this test
  // posts itself (command id read with GetMenuItemID, like the OS).
  write_scratch_ico();
  check(runtime,
        "globalThis.m9fEvals = (globalThis.m9fEvals || 0) + 1;\n"
        "globalThis.m9f = {};\n"
        "globalThis.m9fDone = 'pending';\n"
        "(async () => {\n"
        "  const m = globalThis.m9f;\n"
        "  const { ui } = await import('rime:ui');\n"
        "  m.menu = ui.createMenu();\n"
        "  m.sub = ui.createMenu();\n"
        "  m.p0 = await m.menu.Add('alpha', () => { m.menu.Check('beta'); });\n"
        "  m.p1 = await m.menu.Add('beta');\n"
        "  m.p2 = await m.menu.Add();\n"
        "  m.p3 = await m.sub.Add('leaf', () => { m.sub.Check('leaf'); });\n"
        "  m.p4 = await m.menu.Add('more', m.sub);\n"
        "  m.ins = await m.menu.Insert('beta', 'mid');\n"
        "  m.chk = await m.menu.Check('alpha');\n"
        "  m.tgl = await m.menu.ToggleCheck('alpha');\n"
        "  m.un = await m.menu.Uncheck('alpha');\n"
        "  m.dis = await m.menu.Disable('beta');\n"
        "  m.tge = await m.menu.ToggleEnable('beta');\n"
        "  m.en = await m.menu.Enable('beta');\n"
        "  m.ren = await m.menu.Rename('mid', 'middle');\n"
        "  m.menu.Default = 'alpha';\n"
        "  m.def = await m.menu.Default;\n"
        "  m.menu.ClickCount = 2;\n"
        "  m.cc = await m.menu.ClickCount;\n"
        "  m.col = await m.menu.SetColor('#FF0000');\n"
        "  m.icon = await m.menu.SetIcon('alpha', '" +
        ico_js + "');\n"
        "  try { await m.menu.Add('cycle', m.menu); m.self = 'NO-REFUSE'; }\n"
        "  catch (e) { m.self = e && e.code ? e.code : '?'; }\n"
        "  try { await m.menu.Rename('alpha', 'beta'); m.dupe = 'NO-REFUSE'; }\n"
        "  catch (e) { m.dupe = e && e.code ? e.code : '?'; }\n"
        "  try { m.menu.Handle; m.handle = 'NO-REFUSE'; }\n"
        "  catch (e) { m.handle = e && e.code ? e.code : '?'; }\n"
        "  try { await m.menu.AddStandard(); m.std = 'NO-REFUSE'; }\n"
        "  catch (e) { m.std = e && e.code ? e.code : '?'; }\n"
        "  try { await m.menu.Check('missing'); m.miss = 'NO-REFUSE'; }\n"
        "  catch (e) { m.miss = e && e.code ? e.code : '?'; }\n"
        "  globalThis.g.MenuBar = m.menu;\n"
        "  m.bar = await globalThis.g.MenuBar;\n"
        "  m.barType = m.bar === null ? 'null' : (m.bar === m.menu ? 'same' : 'other');\n"
        "})().then(() => { globalThis.m9fDone = 'ok'; },\n"
        "  e => { globalThis.m9fDone = { code: (e && e.code) || '',\n"
        "    ctor: e && e.constructor ? e.constructor.name : '?',\n"
        "    message: String((e && e.message) || '') }; });\n",
        "gui-object-3c-invoke.mjs");
  require_ok(runtime.settle(30000ms), "3c invoke settle");
  check(runtime,
        "if (globalThis.m9fDone !== 'ok')\n"
        "  throw new Error('3c sequence failed: ' + JSON.stringify(globalThis.m9fDone));\n",
        "gui-object-3c-check.mjs");
  {
    // The menubar click: a real WM_COMMAND with the OS-issued command id
    // (GetMenuItemID position 0 = 'alpha'), exactly what a user click
    // delivers to the window proc. The pump routes it to the menu's
    // channel and the item callback fires.
    HMENU bar = GetMenu(hwnd);
    require(bar != nullptr, "GetMenu returns the attached bar");
    const UINT alpha_cmd = GetMenuItemID(bar, 0);
    require(alpha_cmd != static_cast<UINT>(-1), "position 0 has a command id");
    require(PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(alpha_cmd, 0), 0) != FALSE,
            "the test's own WM_COMMAND posts");
  }
  require_ok(runtime.settle(10000ms), "menubar click settle");
  {
    // The alpha callback runs m.menu.Check('beta'): poll the OS for the
    // effect with a deadline (condition poll, never a bare sleep). This is
    // the end-to-end proof that the menubar click reached the callback -
    // posted WM_COMMAND -> pump route -> channel -> dispatch -> callback
    // -> pump apply -> HMENU state.
    HMENU bar_probe = GetMenu(hwnd);
    require(bar_probe != nullptr, "GetMenu returns the attached bar");
    const UINT beta_probe = GetMenuItemID(bar_probe, 2);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool beta_checked = false;
    while (std::chrono::steady_clock::now() < until) {
      if ((GetMenuState(bar_probe, beta_probe, MF_BYCOMMAND) & MF_CHECKED) != 0) {
        beta_checked = true;
        break;
      }
      Sleep(10);
    }
    require(beta_checked, "the alpha callback's Check('beta') landed on the OS");
  }
  check(runtime,
        "globalThis.m9fShow = 'pending';\n"
        "(async () => {\n"
        "  const m = globalThis.m9f;\n"
        "  const { runtime } = await import('rime:runtime');\n"
        "  m.pick = await m.sub.Show(200, 200);\n"
        "  await runtime.delay(1000, null);\n"
        "  m.dismiss = await m.sub.Show(200, 200);\n"
        "})().then(() => { globalThis.m9fShow = 'ok'; },\n"
        "  e => { globalThis.m9fShow = { code: (e && e.code) || '',\n"
        "    ctor: e && e.constructor ? e.constructor.name : '?',\n"
        "    message: String((e && e.message) || '') }; });\n",
        "gui-object-3c-show.mjs");
  // Injector for the two modal rounds: DOWN+ENTER spam picks 'leaf' first
  // (resolves the name and fires its callback), then ESC spam dismisses
  // (resolves null). Pick-first is load-bearing, not cosmetic:
  // - single keystrokes race the menu's highlight timing, so both rounds
  //   repeat (probed standalone; WM_CANCELMODE does not unblock the loop,
  //   and cross-thread EndMenu is unreliable);
  // - key leftovers survive the popup they were meant for: ESC residue
  //   instantly dismisses a later popup, so the dismiss round runs LAST
  //   (its residue - ESC/DOWN/ENTER pairs - dies in the pump queue in
  //   milliseconds, long before S9i posts its own Escape).
  // Each round waits for a popup to appear, drives it, then waits for all
  // popups to close - no handle caching (the OS reuses destroyed #32768
  // handles, so a `last` filter would miss round two).
  auto own_popup = [&]() -> HWND {
    for (HWND w = FindWindowW(L"#32768", nullptr); w != nullptr;
         w = FindWindowExW(nullptr, w, L"#32768", nullptr)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(w, &pid);
      if (pid != GetCurrentProcessId()) continue;
      if (IsWindowVisible(w)) return w;
    }
    return nullptr;
  };
  std::atomic<int> injected{0};
  std::atomic<bool> injector_ok{true};
  std::thread injector([&] {
    for (int round = 0; round < 2; ++round) {
      HWND popup = nullptr;
      const auto appear_until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (!popup && std::chrono::steady_clock::now() < appear_until) {
        popup = own_popup();
        if (!popup) Sleep(10);
      }
      if (!popup) {
        injector_ok = false;
        return;
      }
      if (round == 0) {
        for (int i = 0; i < 6 && IsWindow(popup); ++i) {
          PostMessageW(popup, WM_KEYDOWN, VK_DOWN, 0);
          PostMessageW(popup, WM_KEYUP, VK_DOWN, 0);
          Sleep(200);  // inter-key pacing only; assertions stay on settle
        }
        for (int i = 0; i < 4 && IsWindow(popup); ++i) {
          PostMessageW(popup, WM_KEYDOWN, VK_RETURN, 0);
          PostMessageW(popup, WM_KEYUP, VK_RETURN, 0);
          Sleep(200);  // inter-key pacing only; assertions stay on settle
        }
        injected += 2;
      } else {
        for (int i = 0; i < 6 && IsWindow(popup); ++i) {
          PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
          PostMessageW(popup, WM_KEYUP, VK_ESCAPE, 0);
          Sleep(200);  // inter-key pacing only; assertions stay on settle
        }
        ++injected;
      }
      // Let this popup close before hunting the next one.
      const auto gone_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (own_popup() && std::chrono::steady_clock::now() < gone_until) Sleep(10);
    }
  });
  require_ok(runtime.settle(30000ms), "3c show settle");
  injector.join();
  require(injected.load() == 3, "injector drove both popups");
  require(DeleteFileW(ico_path.c_str()) != FALSE, "scratch ICO is removed");
  check(runtime,
        "if (globalThis.m9fShow !== 'ok')\n"
        "  throw new Error('3c show failed: ' + JSON.stringify(globalThis.m9fShow));\n"
        "const m = globalThis.m9f;\n"
        "const eq = (key, actual, want) => {\n"
        "  if (actual !== want) throw new Error(key + ': got ' + JSON.stringify(actual) +\n"
        "    ', want ' + JSON.stringify(want));\n"
        "};\n"
        "eq('p0', m.p0, 0);\n"
        "eq('p1', m.p1, 1);\n"
        "eq('p2', m.p2, 2);\n"
        "eq('p3', m.p3, 0);\n"
        "eq('p4', m.p4, 3);\n"
        "eq('ins', m.ins, 1);\n"
        "eq('chk', m.chk, null);\n"
        "eq('tgl', m.tgl, false);\n"
        "eq('un', m.un, null);\n"
        "eq('dis', m.dis, null);\n"
        "eq('tge', m.tge, true);\n"
        "eq('en', m.en, null);\n"
        "eq('ren', m.ren, 'mid');\n"
        "eq('def', m.def, 'alpha');\n"
        "eq('cc', m.cc, 2);\n"
        "eq('col', m.col, null);\n"
        "eq('icon', m.icon, null);\n"
        "eq('self', m.self, 'invalid_contract');\n"
        "eq('dupe', m.dupe, 'invalid_contract');\n"
        "eq('handle', m.handle, 'unsupported_by_policy');\n"
        "eq('std', m.std, 'unsupported_by_policy');\n"
        "eq('miss', m.miss, 'invalid_contract');\n"
        "eq('barType', m.barType, 'same');\n"
        "eq('dismiss', m.dismiss, null);\n"
        "eq('pick', m.pick, 'leaf');\n",
        "gui-object-3c-assert.mjs");
  {
    // OS re-read of the same HMENU: item count (alpha/middle/beta/sep/more),
    // alpha unchecked + enabled after the toggle round-trips, the submenu
    // slot, the red background brush, the alpha icon bitmap, the default
    // item and the attached bar itself.
    HMENU bar = GetMenu(hwnd);
    require(bar != nullptr, "the bar is still attached");
    require(GetMenuItemCount(bar) == 5, "five top-level items survive");
    const UINT alpha_cmd = GetMenuItemID(bar, 0);
    require((GetMenuState(bar, alpha_cmd, MF_BYCOMMAND) & MF_CHECKED) == 0,
            "alpha ends unchecked");
    require((GetMenuState(bar, alpha_cmd, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) == 0,
            "alpha ends enabled");
    const UINT beta_cmd = GetMenuItemID(bar, 2);
    require((GetMenuState(bar, beta_cmd, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) == 0,
            "beta ends enabled");
    require((GetMenuState(bar, beta_cmd, MF_BYCOMMAND) & MF_CHECKED) != 0,
            "beta ends checked by the alpha callback");
    HMENU sub = GetSubMenu(bar, 4);
    require(sub != nullptr, "position 4 opens the submenu");
    require(GetMenuItemCount(sub) == 1, "the submenu holds leaf");
    const UINT leaf_cmd = GetMenuItemID(sub, 0);
    const auto leaf_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool leaf_checked = false;
    while (std::chrono::steady_clock::now() < leaf_until) {
      if ((GetMenuState(sub, leaf_cmd, MF_BYCOMMAND) & MF_CHECKED) != 0) {
        leaf_checked = true;
        break;
      }
      Sleep(10);
    }
    require(leaf_checked, "the leaf callback's Check('leaf') landed on the OS");
    MENUINFO color_info{};
    color_info.cbSize = sizeof(color_info);
    color_info.fMask = MIM_BACKGROUND;
    require(GetMenuInfo(bar, &color_info) != FALSE, "GetMenuInfo reads back");
    require(color_info.hbrBack != nullptr, "SetColor left a background brush");
    MENUITEMINFOW icon_info{};
    icon_info.cbSize = sizeof(icon_info);
    icon_info.fMask = MIIM_BITMAP;
    require(GetMenuItemInfoW(bar, alpha_cmd, FALSE, &icon_info) != FALSE,
            "GetMenuItemInfo reads back");
    require(icon_info.hbmpItem != nullptr, "alpha carries the icon bitmap");
    require(GetMenuDefaultItem(bar, FALSE, 0) == alpha_cmd, "alpha is the default item");
  }
  // Menu Destroy detaches the bar, frees the pump record and releases the
  // dispatch channel (S16 baseline); members refuse InvalidState after.
  check(runtime,
        "const m = globalThis.m9f;\n"
        "m.menu.Destroy();\n"
        "m.sub.Destroy();\n"
        "globalThis.m9fDead = globalThis.seen(() => m.menu.Add('late'));\n",
        "gui-object-3c-destroy.mjs");
  require_ok(runtime.settle(10000ms), "menu destroy settle");
  check(runtime,
        "if (globalThis.m9fDead.code !== 'invalid_state')\n"
        "  throw new Error('dead menu: ' + JSON.stringify(globalThis.m9fDead));\n",
        "gui-object-3c-dead.mjs");
  require(GetMenu(hwnd) == nullptr, "Destroy detached the bar");

  // ---- S9g: batch-3d image lists -------------------------------------------
  // Icons land in a pump-owned ImageList (ImageList_GetImageCount reads
  // back); SetImageList wires it into ListView/TreeView (LVM/TVM_GETIMAGELIST
  // reads back) and row/node image indices stick (GETITEM iImage reads
  // back). Destroy detaches everywhere (GETIMAGELIST goes null).
  write_scratch_ico();
  check(runtime,
        "globalThis.m9g = {};\n"
        "globalThis.m9gDone = 'pending';\n"
        "(async () => {\n"
        "  const m = globalThis.m9g;\n"
        "  const { ui } = await import('rime:ui');\n"
        "  m.list = await ui.createImageList(16, 16);\n"
        "  m.i0 = await m.list.Add('" +
        ico_js + "');\n"
        "  const lv = globalThis.wr.listview;\n"
        "  const tv = globalThis.wr.tree;\n"
        "  m.setLv = await lv.SetImageList(m.list, 'small');\n"
        "  m.setTv = await tv.SetImageList(m.list);\n"
        "  m.rw = await lv.Add(['pic', 'row'], undefined, undefined, undefined, 0);\n"
        "  m.nd = await tv.Add('pnode', undefined, undefined, undefined, undefined, 0);\n"
        "  m.modRw = await lv.Modify(0, undefined, undefined, undefined, undefined, 0);\n"
        "  try { await lv.SetImageList(m.list, 'huge'); m.size = 'NO-REFUSE'; }\n"
        "  catch (e) { m.size = e && e.constructor ? e.constructor.name : '?'; }\n"
        "  try { await m.list.Add('C:\\\\no-such-file-xyz.ico'); m.bad = 'NO-REFUSE'; }\n"
        "  catch (e) { m.bad = e && e.code ? e.code : '?'; }\n"
        "  try { await ui.createImageList(0, 16); m.geo = 'NO-REFUSE'; }\n"
        "  catch (e) { m.geo = e && e.code ? e.code : '?'; }\n"
        "})().then(() => { globalThis.m9gDone = 'ok'; },\n"
        "  e => { globalThis.m9gDone = { code: (e && e.code) || '',\n"
        "    ctor: e && e.constructor ? e.constructor.name : '?',\n"
        "    message: String((e && e.message) || '') }; });\n",
        "gui-object-3d-invoke.mjs");
  require_ok(runtime.settle(30000ms), "3d invoke settle");
  require(DeleteFileW(ico_path.c_str()) != FALSE, "scratch ICO is removed");
  check(runtime,
        "if (globalThis.m9gDone !== 'ok')\n"
        "  throw new Error('3d sequence failed: ' + JSON.stringify(globalThis.m9gDone));\n"
        "const m = globalThis.m9g;\n"
        "const eq = (key, actual, want) => {\n"
        "  if (actual !== want) throw new Error(key + ': got ' + JSON.stringify(actual) +\n"
        "    ', want ' + JSON.stringify(want));\n"
        "};\n"
        "eq('i0', m.i0, 0);\n"
        "eq('setLv', m.setLv, null);\n"
        "eq('setTv', m.setTv, null);\n"
        "eq('rw', m.rw, 3);\n"
        "eq('modRw', m.modRw, 0);\n"
        "if (typeof m.nd !== 'number' || m.nd < 1)\n"
        "  throw new Error('nd: ' + JSON.stringify(m.nd));\n"
        "eq('size', m.size, 'TypeError');\n"
        "eq('bad', m.bad, 'invalid_contract');\n"
        "eq('geo', m.geo, 'invalid_contract');\n",
        "gui-object-3d-assert.mjs");
  {
    HWND listview = find_child(hwnd, L"SysListView32");
    require(listview != nullptr, "FindWindowExW finds the ListView child");
    HIMAGELIST small_list = reinterpret_cast<HIMAGELIST>(
        SendMessageW(listview, LVM_GETIMAGELIST, LVSIL_SMALL, 0));
    require(small_list != nullptr, "the small list is wired into the ListView");
    require(ImageList_GetImageCount(small_list) == 1, "one icon landed in the list");
    // Row 0 is the S9e survivor ('m1'); the S9g row 'pic' sits at index 3
    // with image 0 (S9e left 3 rows, S9g appended one).
    LVITEMW row{};
    row.mask = LVIF_IMAGE;
    row.iItem = 3;
    require(SendMessageW(listview, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&row)) != FALSE,
            "LVM_GETITEM reads the S9g row");
    require(row.iImage == 0, "the row carries image 0");
    HWND tree = find_child(hwnd, L"SysTreeView32");
    require(tree != nullptr, "FindWindowExW finds the TreeView child");
    HIMAGELIST normal = reinterpret_cast<HIMAGELIST>(
        SendMessageW(tree, TVM_GETIMAGELIST, TVSIL_NORMAL, 0));
    require(normal == small_list, "both controls share the one list");
    // The S9g node 'pnode' is the last root (S9d left r1/r2, S9e deleted
    // its subtree, S9g appended one): walk roots to the end.
    HTREEITEM node = reinterpret_cast<HTREEITEM>(
        SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(TVI_ROOT)));
    HTREEITEM last = node;
    while (node) {
      last = node;
      node = reinterpret_cast<HTREEITEM>(
          SendMessageW(tree, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(node)));
    }
    require(last != nullptr, "a root node exists");
    TVITEMW tvget{};
    tvget.hItem = last;
    tvget.mask = TVIF_TEXT | TVIF_IMAGE;
    wchar_t node_text[64]{};
    tvget.pszText = node_text;
    tvget.cchTextMax = 64;
    require(SendMessageW(tree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&tvget)) != FALSE,
            "TVM_GETITEM reads the last root");
    require(wcscmp(node_text, L"pnode") == 0, "the last root is the S9g node");
    require(tvget.iImage == 0, "the node carries image 0");
  }
  check(runtime,
        "globalThis.m9gGone = 'pending';\n"
        "(async () => {\n"
        "  const m = globalThis.m9g;\n"
        "  await m.list.Destroy();\n"
        "})().then(\n"
        "  () => { globalThis.m9gGone = 'ok'; },\n"
        "  e => { globalThis.m9gGone = { code: (e && e.code) || '',\n"
        "    message: String((e && e.message) || '') }; });\n",
        "gui-object-3d-destroy.mjs");
  require_ok(runtime.settle(10000ms), "image destroy settle");
  check(runtime,
        "if (globalThis.m9gGone !== 'ok')\n"
        "  throw new Error('3d destroy failed: ' + JSON.stringify(globalThis.m9gGone));\n",
        "gui-object-3d-destroy-check.mjs");
  {
    HWND listview = find_child(hwnd, L"SysListView32");
    require(SendMessageW(listview, LVM_GETIMAGELIST, LVSIL_SMALL, 0) == 0,
            "destroy detached the small list");
    HWND tree = find_child(hwnd, L"SysTreeView32");
    require(SendMessageW(tree, TVM_GETIMAGELIST, TVSIL_NORMAL, 0) == 0,
            "destroy detached the normal list");
  }

  // ---- S9h: batch-3e FromHwnd (miss paths) ---------------------------------
  // This slice runtime wires no hwnd resolver (production wires
  // WindowService::peek_hwnd in bootstrap), so every id misses with null -
  // TargetGone is a miss, never a throw. The hit paths run natively above
  // (gui_tests.cpp owns both sides of the resolver). Bad ids fail
  // synchronously before any worker starts.
  check(runtime,
        "globalThis.m9h = {};\n"
        "globalThis.m9hDone = 'pending';\n"
        "(async () => {\n"
        "  const m = globalThis.m9h;\n"
        "  const { ui } = await import('rime:ui');\n"
        "  m.miss = await ui.fromHwnd(4242);\n"
        "  m.missCtrl = await ui.fromHwndControl(4343);\n"
        "  m.zero = await ui.fromHwnd(0);\n"
        "  m.neg = await ui.fromHwnd(-1);\n"
        "  try { await ui.fromHwnd('x'); m.str = 'NO-REFUSE'; }\n"
        "  catch (e) { m.str = e && e.constructor ? e.constructor.name : '?'; }\n"
        "  try { await ui.fromHwnd(); m.arity = 'NO-REFUSE'; }\n"
        "  catch (e) { m.arity = e && e.constructor ? e.constructor.name : '?'; }\n"
        "})().then(() => { globalThis.m9hDone = 'ok'; },\n"
        "  e => { globalThis.m9hDone = { code: (e && e.code) || '',\n"
        "    ctor: e && e.constructor ? e.constructor.name : '?',\n"
        "    message: String((e && e.message) || '') }; });\n",
        "gui-object-3e-invoke.mjs");
  require_ok(runtime.settle(10000ms), "3e invoke settle");
  check(runtime,
        "if (globalThis.m9hDone !== 'ok')\n"
        "  throw new Error('3e sequence failed: ' + JSON.stringify(globalThis.m9hDone));\n"
        "const m = globalThis.m9h;\n"
        "const eq = (key, actual, want) => {\n"
        "  if (actual !== want) throw new Error(key + ': got ' + JSON.stringify(actual) +\n"
        "    ', want ' + JSON.stringify(want));\n"
        "};\n"
        "eq('miss', m.miss, null);\n"
        "eq('missCtrl', m.missCtrl, null);\n"
        "eq('zero', m.zero, null);\n"
        "eq('neg', m.neg, null);\n"
        "eq('str', m.str, 'TypeError');\n"
        "eq('arity', m.arity, 'TypeError');\n",
        "gui-object-3e-assert.mjs");

  // ---- S9i: batch-3e events (Escape/ContextMenu/DropFiles/ItemSelect) -----
  // New OnEvent names behind the same channel: Escape (VK_ESCAPE keydown),
  // ContextMenu (WM_CONTEXTMENU with x/y/control), DropFiles (a real HDROP
  // built here; DragFinish frees it pump-side), ItemSelect on ListView
  // (LVN_ITEMCHANGED selection flip) and TreeView (TVN_SELCHANGED).
  // Stimuli are posted/sent by this test; JS only observes through waitFor.
  check(runtime,
        "const g = globalThis.g;\n"
        "globalThis.ev9 = { esc: [], menu: [], drop: [], lvSel: [], tvSel: [] };\n"
        "globalThis.ev9Reg = 'pending';\n"
        "(async () => {\n"
        // Awaited one by one: each resolution means its interest mirror
        // applied pump-side, so the table is complete before any stimulus
        // (concurrent mirrors converge by stamp, but only awaiting proves
        // they all landed).
        "  await g.OnEvent('Escape', (t) => {\n"
        "    globalThis.ev9.esc.push(t === globalThis.g); });\n"
        "  await g.OnEvent('ContextMenu',\n"
        "    (t, x, y, c) => { globalThis.ev9.menu.push([t === globalThis.g, x, y, c]); });\n"
        "  await g.OnEvent('DropFiles',\n"
        "    (t, ...files) => { globalThis.ev9.drop.push([t === globalThis.g, ...files]); });\n"
        "  await globalThis.wr.listview.OnEvent('ItemSelect',\n"
        "    (t, row, sel) => { globalThis.ev9.lvSel.push([t === globalThis.wr.listview, row, sel]); });\n"
        "  await globalThis.wr.tree.OnEvent('ItemSelect',\n"
        "    (t, node) => { globalThis.ev9.tvSel.push([t === globalThis.wr.tree, node]); });\n"
        "})().then(() => { globalThis.ev9Reg = 'ok'; },\n"
        "  e => { globalThis.ev9Reg = { code: (e && e.code) || '',\n"
        "    message: String((e && e.message) || '') }; });\n",
        "gui-object-3e-events-register.mjs");
  require_ok(runtime.settle(10000ms), "3e events register settle");
  check(runtime,
        "if (globalThis.ev9Reg !== 'ok')\n"
        "  throw new Error('3e register failed: ' + JSON.stringify(globalThis.ev9Reg));\n",
        "gui-object-3e-events-registered.mjs");
  check(runtime,
        "globalThis.ev9.treeId = globalThis.wr.tree.id;\n"
        "globalThis.ev9.lvId = globalThis.wr.listview.id;\n",
        "gui-object-3e-events-ids.mjs");
  check(runtime,
        "globalThis.ev9Sel = 'pending';\n"
        "(async () => {\n"
        // Pump-thread selects (the service path): test-thread SendMessage
        // selects move the caret silently, while service selects notify.
        // Touch r1 first so the r2 flip always fires; last event is [., 2].
        // Runs BEFORE arming (below): each Modify is a slow pump round
        // trip, and waitFor budgets must cover stimuli-to-event only.
        "  const tv = globalThis.wr.tree;\n"
        "  await tv.Modify(1, undefined, true);\n"
        "  await tv.Modify(2, undefined, true);\n"
        "})().then(() => { globalThis.ev9Sel = 'ok'; },\n"
        "  e => { globalThis.ev9Sel = { code: (e && e.code) || '',\n"
        "    message: String((e && e.message) || '') }; });\n",
        "gui-object-3e-events-tvselect.mjs");
  require_ok(runtime.settle(10000ms), "3e tvselect settle");
  check(runtime,
        "if (globalThis.ev9Sel !== 'ok')\n"
        "  throw new Error('3e tvselect failed: ' + JSON.stringify(globalThis.ev9Sel));\n",
        "gui-object-3e-events-tvselected.mjs");
  check(runtime,
        "const ev = globalThis.ev9;\n"
        "ev.escWait = waitFor(() => ev.esc.length > 0, 4000).then(v => { ev.escSeen = v; });\n"
        "ev.menuWait = waitFor(() => ev.menu.length > 0, 4000).then(v => { ev.menuSeen = v; });\n"
        "ev.dropWait = waitFor(() => ev.drop.length > 0, 4000).then(v => { ev.dropSeen = v; });\n"
        "ev.lvWait = waitFor(() => ev.lvSel.length > 0, 4000).then(v => { ev.lvSeen = v; });\n",
        "gui-object-3e-events-arm.mjs");
  require(PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0) != FALSE, "Escape posts");
  require(PostMessageW(hwnd, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(hwnd),
                       MAKELPARAM(100, 200)) != FALSE,
          "ContextMenu posts");
  {
    // A real HDROP: DROPFILES + double-null-terminated UTF-16 list. The
    // pump's DragFinish releases it, so the test never frees it.
    const wchar_t* files[] = {L"C:\\drop\\a.txt", L"C:\\drop\\b.txt"};
    std::size_t chars = 1;
    for (const wchar_t* file : files) chars += wcslen(file) + 1;
    HGLOBAL mem = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    require(mem != nullptr, "drop memory allocates");
    auto* drop = static_cast<DROPFILES*>(GlobalLock(mem));
    drop->pFiles = sizeof(DROPFILES);
    drop->pt.x = 0;
    drop->pt.y = 0;
    drop->fNC = FALSE;
    drop->fWide = TRUE;
    wchar_t* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
    for (const wchar_t* file : files) {
      wcscpy(cursor, file);
      cursor += wcslen(file) + 1;
    }
    *cursor = L'\0';
    GlobalUnlock(mem);
    require(PostMessageW(hwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(mem), 0) != FALSE,
            "DropFiles posts");
  }
  {
    HWND listview = find_child(hwnd, L"SysListView32");
    require(listview != nullptr, "FindWindowExW finds the ListView child");
    // Row 0 ('m1') is unselected (S9e selected row 1); selecting it flips.
    ListView_SetItemState(listview, 0, LVIS_SELECTED, LVIS_SELECTED);
  }
  require_ok(runtime.settle(10000ms), "3e events settle");
  check(runtime,
        "const ev = globalThis.ev9;\n"
        "const fail = (key, detail) => { throw new Error(key + ': ' + detail); };\n"
        "if (ev.escSeen !== true || ev.esc.length !== 1 || ev.esc[0] !== true)\n"
        "  fail('escape', JSON.stringify([ev.escSeen, ev.esc]));\n"
        "if (ev.menuSeen !== true || ev.menu.length !== 1)\n"
        "  fail('contextmenu', JSON.stringify([ev.menuSeen, ev.menu]));\n"
        "const cm = ev.menu[0];\n"
        "if (cm[0] !== true || cm[1] !== 100 || cm[2] !== 200 || cm[3] !== 0)\n"
        "  fail('contextmenu args', JSON.stringify(cm));\n"
        "if (ev.dropSeen !== true || ev.drop.length !== 1)\n"
        "  fail('dropfiles', JSON.stringify([ev.dropSeen, ev.drop]));\n"
        "const df = ev.drop[0];\n"
        "if (df[0] !== true || df[1] !== 'C:\\\\drop\\\\a.txt' || df[2] !== 'C:\\\\drop\\\\b.txt')\n"
        "  fail('dropfiles args', JSON.stringify(df));\n"
        "if (ev.lvSeen !== true || ev.lvSel.length < 1)\n"
        "  fail('lvselect', JSON.stringify([ev.lvSeen, ev.lvSel]));\n"
        "const lv = ev.lvSel[ev.lvSel.length - 1];\n"
        "if (lv[0] !== true || lv[1] !== 0 || lv[2] !== 1)\n"
        "  fail('lvselect args', JSON.stringify(lv));\n"
        "if (ev.tvSel.length < 1)\n"
        "  fail('tvselect', JSON.stringify(ev.tvSel));\n"
        "const tv = ev.tvSel[ev.tvSel.length - 1];\n"
        "if (tv[0] !== true || tv[1] !== 2)\n"
        "  fail('tvselect args', JSON.stringify(tv));\n",
        "gui-object-3e-events-assert.mjs");

  // ---- S10/S11: Resize, driven by this test's own SetWindowPos -----------
  check(runtime,
        "const g = globalThis.g;\n"
        "globalThis.ev = { resize: [], close: [], resizeSeen: null, closeSeen: null };\n"
        "globalThis.track(globalThis.ev, 'resizeReg', g.OnEvent('Resize',\n"
        "    (target, minMax, width, height) => {\n"
        "      globalThis.ev.resize.push(\n"
        "          { self: target === globalThis.g, minMax, width, height });\n"
        "    }));\n"
        // The exact function reference S13 removes, so the interest bit is
        // cleared by identity rather than by a second, different handler.
        "globalThis.closeHandler = (target) => {\n"
        "  globalThis.ev.close.push(target === globalThis.g);\n"
        "};\n"
        "globalThis.track(globalThis.ev, 'closeReg', g.OnEvent('Close',\n"
        "    globalThis.closeHandler));\n",
        "gui-object-events-register.mjs");
  require_ok(runtime.settle(10000ms), "event register settle");
  check(runtime,
        "if (globalThis.ev.resizeReg !== null)\n"
        "  throw new Error('OnEvent(Resize) must resolve null: ' +\n"
        "                   JSON.stringify(globalThis.ev.resizeReg));\n"
        "if (globalThis.ev.closeReg !== null)\n"
        "  throw new Error('OnEvent(Close) must resolve null: ' +\n"
        "                   JSON.stringify(globalThis.ev.closeReg));\n"
        "globalThis.ev.resizeWait = waitFor(\n"
        "    () => globalThis.ev.resize.length > 0, 4000)\n"
        "    .then(v => { globalThis.ev.resizeSeen = v; });\n",
        "gui-object-events-arm.mjs");
  require(SetWindowPos(hwnd, nullptr, 0, 0, 420, 260,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
          "the test's own SetWindowPos resizes the window");
  require_ok(runtime.settle(10000ms), "resize event settle");
  check(runtime,
        "if (globalThis.ev.resizeSeen !== true)\n"
        "  throw new Error('the Resize handler never ran: ' +\n"
        "                   JSON.stringify(globalThis.ev.resize));\n"
        "const last = globalThis.ev.resize[globalThis.ev.resize.length - 1];\n"
        "if (last.self !== true) throw new Error('Resize target must be the Gui');\n"
        "if (typeof last.minMax !== 'number' || typeof last.width !== 'number' ||\n"
        "    typeof last.height !== 'number')\n"
        "  throw new Error('Resize args: ' + JSON.stringify(last));\n"
        "if (!(last.width > 0 && last.height > 0))\n"
        "  throw new Error('Resize size: ' + JSON.stringify(last));\n",
        "gui-object-resize-check.mjs");

  // ---- S12: Close survives while a handler is registered ------------------
  // The Close wait is armed only now: runtime.settle waits for every pending
  // delay timer, so arming it alongside the Resize wait would let it expire
  // inside the resize settle and report a timeout that never observed a
  // Close at all.
  check(runtime,
        "globalThis.ev.closeWait = waitFor(\n"
        "    () => globalThis.ev.close.length > 0, 4000)\n"
        "    .then(v => { globalThis.ev.closeSeen = v; });\n",
        "gui-object-close-arm.mjs");
  require(PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE, "PostMessageW(WM_CLOSE)");
  require_ok(runtime.settle(10000ms), "close event settle");
  // Win32 first: if the interest bit had not reached the record the window
  // would already be gone, which would make any JS answer meaningless.
  require(IsWindow(hwnd) != FALSE,
          "a registered Close handler keeps the window alive (AHK default)");
  check(runtime,
        "if (globalThis.ev.closeSeen !== true)\n"
        "  throw new Error('the Close handler never ran: seen=' +\n"
        "                   JSON.stringify(globalThis.ev.closeSeen) +\n"
        "                   ' close=' + JSON.stringify(globalThis.ev.close));\n"
        "if (globalThis.ev.close.length !== 1)\n"
        "  throw new Error('Close fired more than once: ' + globalThis.ev.close.length);\n"
        "if (globalThis.ev.close[0] !== true) throw new Error('Close target must be the Gui');\n",
        "gui-object-close-check.mjs");

  // ---- S13: without a handler the same WM_CLOSE destroys ------------------
  // The handler S10 registered is removed by reference, so the interest bit
  // for Close goes back to zero and Win32, not the service, decides next.
  check(runtime,
        "globalThis.track(globalThis.ev, 'removeClose',\n"
        "                  globalThis.g.OnEvent('Close',\n"
        "                                       globalThis.closeHandler, 0));\n",
        "gui-object-close-remove.mjs");
  require_ok(runtime.settle(10000ms), "close remove settle");
  check(runtime,
        "if (globalThis.ev.removeClose !== null)\n"
        "  throw new Error('removing a handler must resolve null: ' +\n"
        "                   JSON.stringify(globalThis.ev.removeClose));\n",
        "gui-object-close-remove-check.mjs");
  require(PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE, "second WM_CLOSE");
  require(wait_for([&] { return IsWindow(hwnd) == FALSE; }),
          "without a Close handler WM_CLOSE must destroy the window");

  // ---- S13b: an externally destroyed window releases its JS half ---------
  // A dedicated second Gui (the main one still belongs to S14's explicit
  // Destroy path): WM_DESTROY pushes __closed, dispatch runs the same
  // idempotent release as Destroy, so later verbs throw synchronously (not
  // rejecting promises) and the unload gate stops seeing this Gui. Before
  // the fix the channel leaked until an explicit Destroy or GC.
  check(runtime,
        "import { ui } from 'rime:ui';\n"
        "globalThis.xr = {};\n"
        "globalThis.g2 = ui.createGui('+Resize', 'rime-gui-xclose');\n"
        "globalThis.track(globalThis.xr, 'show', globalThis.g2.Show());\n",
        "gui-object-xclose-show.mjs");
  require_ok(runtime.settle(10000ms), "xclose show settle");
  HWND xhwnd = FindWindowW(nullptr, L"rime-gui-xclose");
  require(xhwnd != nullptr, "the xclose window really appeared");
  const std::size_t callbacks_before = host_counts(runtime).callbacks;
  require(PostMessageW(xhwnd, WM_CLOSE, 0, 0) != FALSE, "WM_CLOSE to the xclose window");
  require(wait_for([&] { return IsWindow(xhwnd) == FALSE; }),
          "WM_CLOSE destroys a handler-less window");
  require_ok(runtime.settle(10000ms), "closed-event settle");
  check(runtime,
        "const g2 = globalThis.g2;\n"
        "const seen = globalThis.seen;\n"
        "const fail = (key, detail) => { throw new Error(key + ': ' + detail); };\n"
        "const x = {\n"
        "  show: seen(() => g2.Show()),\n"
        "  add: seen(() => g2.AddText('x1 y1', 'x')),\n"
        "};\n"
        "for (const key of ['show', 'add']) {\n"
        "  if (!x[key].threw) fail(key, 'must throw synchronously after external close');\n"
        "  if (x[key].code !== 'invalid_state')\n"
        "    fail(key, 'code=' + x[key].code + ' ' + x[key].message);\n"
        "}\n",
        "gui-object-xclose-check.mjs");
  {
    const HostCounts after_close = host_counts(runtime);
    require(after_close.callbacks + 1 == callbacks_before,
            "an X-closed Gui releases its channel without explicit Destroy");
  }

  // ---- S14/S15: Destroy, and everything after it --------------------------
  // Note: S13's externally closed main Gui already ran the idempotent
  // release at __closed dispatch, so Show() throws synchronously here (the
  // same InvalidState the async path used to reject with); seen() captures
  // both shapes, and the assertion below only pins the code.
  check(runtime,
        "const g = globalThis.g;\n"
        "globalThis.after = {};\n"
        "globalThis.after.show = globalThis.seen(() => g.Show());\n"
        "globalThis.track(globalThis.after, 'destroy', g.Destroy());\n",
        "gui-object-destroy.mjs");
  require_ok(runtime.settle(10000ms), "destroy settle");
  check(runtime,
        "const a = globalThis.after;\n"
        "const fail = (key, detail) => { throw new Error(key + ': ' + detail); };\n"
        "if (!a.show || a.show.code !== 'invalid_state')\n"
        "  fail('show after death', JSON.stringify(a.show));\n"
        "if (a.destroy !== null) fail('destroy', JSON.stringify(a.destroy));\n"
        "const g = globalThis.g;\n"
        "const r = globalThis.r;\n"
        "const seen = globalThis.seen;\n"
        "const dead = {\n"
        "  show: seen(() => g.Show()),\n"
        "  add: seen(() => g.AddText('x1 y1', 'x')),\n"
        "  item: seen(() => g.__Item('MyEdit')),\n"
        "  enumerate: seen(() => g.__Enum()),\n"
        "  title: seen(() => g.Title),\n"
        "  editValue: seen(() => r.edit.Value),\n"
        "  name: g.Name,\n"
        "};\n"
        "for (const key of ['show', 'add', 'item', 'enumerate', 'title', 'editValue']) {\n"
        "  if (!dead[key].threw) fail(key, 'must throw after Destroy');\n"
        "  if (dead[key].code !== 'invalid_state')\n"
        "    fail(key, 'code=' + dead[key].code + ' ' + dead[key].message);\n"
        "}\n"
        "if (dead.name !== 'mainGui') fail('Name survives Destroy', dead.name);\n",
        "gui-object-destroy-check.mjs");
  require(!IsWindow(hwnd), "Destroy must leave no window behind");

  // ---- S16: the channel went back to its baseline -------------------------
  // Every completion from the post-Destroy rejections has drained first, so
  // the counters cannot look correct only because a task is still in flight.
  require_ok(runtime.settle(5000ms), "teardown settle");
  const HostCounts after = host_counts(runtime);
  require(after.callbacks == baseline.callbacks,
          "every Gui dispatch callback must be released by Destroy");
  require(after.subscriptions == baseline.subscriptions,
          "every Gui subscription must be released by Destroy");
  require(trace->snapshot().empty(), "no Action was dispatched by any Gui member");

  require_ok(gui_service.stop(), "gui service stop");
  require_ok(runtime.stop(), "runtime stop");
  require_ok(ui.stop(), "UiThread stops");

  // ---- S17: the capability gate on the factory itself ---------------------
  {
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
          "globalThis.denied = null;\n"
          "try {\n"
          "  ui.createGui('', 'rime-gui-object-denied-slice');\n"
          "  globalThis.denied = { threw: false };\n"
          "} catch (e) {\n"
          "  globalThis.denied = { threw: true,\n"
          "                        code: (e && e.code) || '',\n"
          "                        ctor: e && e.constructor ? e.constructor.name : '?' ,\n"
          "                        message: String((e && e.message) || '') };\n"
          "}\n",
          "gui-object-denied.mjs");
    check(denied_runtime,
          "const d = globalThis.denied;\n"
          "if (!d || d.threw !== true)\n"
          "  throw new Error('createGui must be denied: ' + JSON.stringify(d));\n"
          "if (d.code !== 'capability_denied')\n"
          "  throw new Error('denial code: ' + JSON.stringify(d));\n"
          "if (d.message.indexOf('ui.create') === -1)\n"
          "  throw new Error('denial must name ui.create: ' + d.message);\n",
          "gui-object-denied-check.mjs");
    require(find_own_window(kDeniedTitle) == nullptr,
            "a denied createGui must never reach the pump");
    require_ok(denied_runtime.stop(), "denied runtime stop");
  }

  std::printf("gui object slice passed\n");
  return 0;
}
