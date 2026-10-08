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
        "  unknownEvent: seen(() => g.OnEvent('Escape', () => {})),\n"
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
        "expectRange(s.unknownEvent, 'unknown event: Escape');\n"
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
