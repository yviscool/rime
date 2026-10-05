// Realism: L6 - production wiring (JS runtime + Kernel + real UIA service
// and executor) drives a fixture window the test builds itself, including
// denied/invoke failure paths asserted through Trace and side effects.

// Needs an interactive desktop, exclusive run: builds a real target window
// and drives it through the rime:automation module (find/read/invoke).
#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/automation/uia_executor.hpp"
#include "rime/automation/uia_service.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/js_automation.hpp"

#include <windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;

void run(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js step failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

// Owns a top-level window with a uniquely named button on its own pumping
// thread. UIA calls answer through SendMessage (WM_GETOBJECT, invoke ->
// WM_COMMAND), so the window must pump while executors wait inside service
// calls - otherwise every call would deadlock against itself.
class TargetWindow final {
 public:
  TargetWindow() = default;
  ~TargetWindow() { stop(); }
  TargetWindow(const TargetWindow&) = delete;
  TargetWindow& operator=(const TargetWindow&) = delete;

  bool start() {
    ready_.store(false);
    thread_ = std::thread([this] { run_thread(); });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready_.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(10ms);
    }
    return ready_.load();
  }

  void stop() {
    if (!thread_.joinable()) return;
    PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    thread_.join();
  }

  [[nodiscard]] bool invoked() const { return invoked_.load(); }

 private:
  static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
      auto* self = reinterpret_cast<TargetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (self) self->invoked_.store(true);
      return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  void run_thread() {
    static const wchar_t* k_class = L"RimeAutomationSliceTarget";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &TargetWindow::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = k_class;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    (void)RegisterClassW(&window_class);

    HWND window = CreateWindowExW(
        0, k_class, L"Rime Automation Slice Target", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 400, 260, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) return;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    HWND button = CreateWindowExW(
        0, L"BUTTON", L"RimeAutomationSliceOK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 40, 40,
        240, 36, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)),
        window_class.hInstance, nullptr);
    if (!button) {
      DestroyWindow(window);
      return;
    }
    ShowWindow(window, SW_SHOW);
    thread_id_ = GetCurrentThreadId();
    ready_.store(true);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    DestroyWindow(window);
  }

  std::thread thread_;
  std::atomic<bool> ready_{false};
  std::atomic<bool> invoked_{false};
  DWORD thread_id_{0};
};

}  // namespace

int main() {
  TargetWindow target;
  if (!target.start()) return 1;

  rime::automation::UiaService service;
  assert(service.start().ok());

  // The three queries run as Actions through the same bounded, traced
  // pipeline every module mutation uses.
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.automation.find", "windows.automation.read",
                                      "windows.automation.invoke"}),
      trace);
  using Op = rime::automation::UiaExecutor::Op;
  assert(kernel
             .register_executor("automation.find",
                                std::make_shared<rime::automation::UiaExecutor>(service, Op::Find))
             .ok());
  assert(kernel
             .register_executor("automation.read",
                                std::make_shared<rime::automation::UiaExecutor>(service, Op::Read))
             .ok());
  assert(kernel
             .register_executor(
                 "automation.invoke",
                 std::make_shared<rime::automation::UiaExecutor>(service, Op::Invoke))
             .ok());
  rime::action::Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  std::atomic<std::uint64_t> next_action_id{0};
  rime::win32::AutomationModuleBinding binding;
  binding.service = &service;
  binding.kernel = &kernel;
  binding.dispatcher = &dispatcher;
  binding.next_action_id = &next_action_id;
  rime::js::Runtime runtime;
  assert(rime::win32::register_automation_module(runtime, &binding).ok());
  assert(runtime.start().ok());

  // Wiring is part of the contract: a binding without the dispatcher is
  // rejected at register time.
  {
    rime::win32::AutomationModuleBinding partial;
    partial.service = &service;
    partial.kernel = &kernel;
    rime::js::Runtime partial_runtime;
    assert(rime::win32::register_automation_module(partial_runtime, &partial).code ==
           rime::core::Error::Code::InvalidContract);
  }

  // Shape errors are synchronous TypeErrors; nothing is queued for them.
  run(runtime,
      "import { automation } from 'rime:automation';\n"
      "globalThis.automation = automation;\n"
      "globalThis.typeErrors = {};\n"
      "const expectTypeError = (name, fn) => {\n"
      "  try { fn(); globalThis.typeErrors[name] = 'notThrown'; }\n"
      "  catch (e) { globalThis.typeErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectTypeError('findArity', () => automation.find());\n"
      "expectTypeError('findNotObject', () => automation.find(7));\n"
      "expectTypeError('findEmpty', () => automation.find({}));\n"
      "expectTypeError('findNameType', () => automation.find({ name: 5 }));\n"
      "expectTypeError('findControlType', () => automation.find({ name: 'x', controlType: 9 }));\n"
      "expectTypeError('findFromIdNegative', () => automation.find({ name: 'x', fromId: -1 }));\n"
      "expectTypeError('findFromIdFraction', () => automation.find({ name: 'x', fromId: 1.5 }));\n"
      "expectTypeError('findMaxResults', () => automation.find({ name: 'x', maxResults: 0 }));\n"
      "expectTypeError('readArity', () => automation.read());\n"
      "expectTypeError('readZero', () => automation.read(0));\n"
      "expectTypeError('readNegative', () => automation.read(-1));\n"
      "expectTypeError('readString', () => automation.read('4'));\n"
      "expectTypeError('readFraction', () => automation.read(1.5));\n"
      "expectTypeError('invokeArity', () => automation.invoke());\n"
      "expectTypeError('invokeNegative', () => automation.invoke(-3));\n"
      "expectTypeError('releaseArity', () => automation.release());\n"
      "expectTypeError('releaseString', () => automation.release('1'));\n"
      "expectTypeError('badOptions', () => automation.read(1, 'options'));",
      "automation-validate.mjs");
  run(runtime,
      "const expected = ['findArity', 'findNotObject', 'findEmpty', 'findNameType',\n"
      "                  'findControlType', 'findFromIdNegative', 'findFromIdFraction',\n"
      "                  'findMaxResults', 'readArity', 'readZero', 'readNegative',\n"
      "                  'readString', 'readFraction', 'invokeArity', 'invokeNegative',\n"
      "                  'releaseArity', 'releaseString', 'badOptions'];\n"
      "for (const key of expected) {\n"
      "  if (globalThis.typeErrors[key] !== true)\n"
      "    throw new Error('expected TypeError for ' + key + ', got ' +\n"
      "                    globalThis.typeErrors[key]);\n"
      "}",
      "automation-validate-check.mjs");

  // find() resolves one snapshot for the uniquely named button.
  run(runtime,
      "globalThis.findResult = null;\n"
      "globalThis.findError = null;\n"
      "automation.find({ name: 'RimeAutomationSliceOK', controlType: 'button' })\n"
      "  .then(r => { globalThis.findResult = r; },\n"
      "        e => { globalThis.findError = String(e) + ' code=' + e.code; });",
      "automation-find.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.findError)\n"
      "  throw new Error('find failed: ' + globalThis.findError);\n"
      "const elements = globalThis.findResult && globalThis.findResult.elements;\n"
      "if (!Array.isArray(elements) || elements.length !== 1)\n"
      "  throw new Error('expected exactly one element: ' +\n"
      "                  JSON.stringify(globalThis.findResult));\n"
      "const el = elements[0];\n"
      "if (typeof el.id !== 'number' || el.id <= 0) throw new Error('bad element id');\n"
      "if (el.name !== 'RimeAutomationSliceOK') throw new Error('bad name: ' + el.name);\n"
      "if (el.controlType !== 'button') throw new Error('bad controlType: ' + el.controlType);\n"
      "if (typeof el.automationId !== 'string') throw new Error('bad automationId');\n"
      "if (typeof el.enabled !== 'boolean' || !el.enabled)\n"
      "  throw new Error('the button must be enabled');\n"
      "if (typeof el.x !== 'number' || typeof el.y !== 'number') throw new Error('bad origin');\n"
      "if (typeof el.width !== 'number' || el.width <= 0 || el.height <= 0)\n"
      "  throw new Error('bad bounds');\n"
      "globalThis.elementId = el.id;",
      "automation-find-check.mjs");

  // read() round-trips the same registered element.
  run(runtime,
      "globalThis.readResult = null;\n"
      "globalThis.readError = null;\n"
      "automation.read(globalThis.elementId)\n"
      "  .then(r => { globalThis.readResult = r; },\n"
      "        e => { globalThis.readError = String(e) + ' code=' + e.code; });",
      "automation-read.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.readError)\n"
      "  throw new Error('read failed: ' + globalThis.readError);\n"
      "const el = globalThis.readResult;\n"
      "if (!el || el.id !== globalThis.elementId) throw new Error('read returned a foreign id');\n"
      "if (el.name !== 'RimeAutomationSliceOK') throw new Error('bad read name: ' + el.name);\n"
      "if (el.controlType !== 'button' || el.enabled !== true)\n"
      "  throw new Error('bad read shape: ' + JSON.stringify(el));",
      "automation-read-check.mjs");

  // invoke() presses the button; the window thread receives WM_COMMAND.
  run(runtime,
      "globalThis.invokeResult = null;\n"
      "globalThis.invokeError = null;\n"
      "automation.invoke(globalThis.elementId)\n"
      "  .then(r => { globalThis.invokeResult = r; },\n"
      "        e => { globalThis.invokeError = String(e) + ' code=' + e.code; });",
      "automation-invoke.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.invokeError)\n"
      "  throw new Error('invoke failed: ' + globalThis.invokeError);\n"
      "if (!globalThis.invokeResult || globalThis.invokeResult.invoked !== true)\n"
      "  throw new Error('invoke must resolve with { invoked: true }: ' +\n"
      "                  JSON.stringify(globalThis.invokeResult));",
      "automation-invoke-check.mjs");
  // UIA may finish Invoke() before the WM_COMMAND reaches the window
  // procedure, so give the target thread the same grace the native test
  // allows.
  const auto invoked_by = std::chrono::steady_clock::now() + 5s;
  while (!target.invoked() && std::chrono::steady_clock::now() < invoked_by) {
    std::this_thread::sleep_for(10ms);
  }
  assert(target.invoked());

  // release() is a local drop: first call true, unknown and repeat false.
  run(runtime,
      "globalThis.releaseFirst = automation.release(globalThis.elementId);\n"
      "globalThis.releaseSecond = automation.release(globalThis.elementId);\n"
      "globalThis.releaseUnknown = automation.release(999999);",
      "automation-release.mjs");
  run(runtime,
      "if (globalThis.releaseFirst !== true)\n"
      "  throw new Error('first release must be true');\n"
      "if (globalThis.releaseSecond !== false)\n"
      "  throw new Error('second release must be false');\n"
      "if (globalThis.releaseUnknown !== false)\n"
      "  throw new Error('unknown release must be false');",
      "automation-release-check.mjs");
  assert(service.element_count() == 0);

  // A non-matching query resolves with an empty list.
  run(runtime,
      "globalThis.noMatchResult = null;\n"
      "globalThis.noMatchError = null;\n"
      "automation.find({ name: 'NoSuchButtonAnywhere9' })\n"
      "  .then(r => { globalThis.noMatchResult = r; },\n"
      "        e => { globalThis.noMatchError = String(e) + ' code=' + e.code; });",
      "automation-no-match.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.noMatchError)\n"
      "  throw new Error('no-match find failed: ' + globalThis.noMatchError);\n"
      "if (!globalThis.noMatchResult ||\n"
      "    !Array.isArray(globalThis.noMatchResult.elements) ||\n"
      "    globalThis.noMatchResult.elements.length !== 0)\n"
      "  throw new Error('expected an empty element list: ' +\n"
      "                  JSON.stringify(globalThis.noMatchResult));",
      "automation-no-match-check.mjs");

  // Unknown ids and an unknown find scope reject with target_gone.
  run(runtime,
      "globalThis.goneRead = null;\n"
      "globalThis.goneFind = null;\n"
      "automation.read(999999)\n"
      "  .then(() => { globalThis.goneRead = 'resolved'; },\n"
      "        e => { globalThis.goneRead = { code: e.code, message: e.message }; });\n"
      "automation.find({ name: 'RimeAutomationSliceOK', fromId: 999999 })\n"
      "  .then(() => { globalThis.goneFind = 'resolved'; },\n"
      "        e => { globalThis.goneFind = { code: e.code, message: e.message }; });",
      "automation-gone.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "for (const key of ['goneRead', 'goneFind']) {\n"
      "  const outcome = globalThis[key];\n"
      "  if (!outcome || outcome === 'resolved')\n"
      "    throw new Error(key + ' must reject, got ' + outcome);\n"
      "  if (outcome.code !== 'target_gone')\n"
      "    throw new Error(key + ' must reject with target_gone, got ' + outcome.code);\n"
      "  if (!outcome.message || outcome.message.indexOf('gone') < 0)\n"
      "    throw new Error(key + ' must explain the missing target: ' + outcome.message);\n"
      "}",
      "automation-gone-check.mjs");

  // Register a fresh element, then destroy the window: the stale reference
  // answers target_gone, is dropped, and later searches see nothing.
  run(runtime,
      "globalThis.staleFindResult = null;\n"
      "globalThis.staleFindError = null;\n"
      "automation.find({ name: 'RimeAutomationSliceOK', controlType: 'button' })\n"
      "  .then(r => { globalThis.staleFindResult = r; },\n"
        "        e => { globalThis.staleFindError = String(e) + ' code=' + e.code; });",
      "automation-stale-find.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.staleFindError)\n"
      "  throw new Error('stale find failed: ' + globalThis.staleFindError);\n"
      "const elements = globalThis.staleFindResult.elements;\n"
      "if (!Array.isArray(elements) || elements.length !== 1)\n"
      "  throw new Error('expected one registered element');\n"
      "globalThis.staleId = elements[0].id;",
      "automation-stale-find-check.mjs");
  assert(service.element_count() == 1);

  target.stop();
  run(runtime,
      "globalThis.staleRead = null;\n"
      "globalThis.afterDestroy = null;\n"
      "automation.read(globalThis.staleId)\n"
      "  .then(() => { globalThis.staleRead = 'resolved'; },\n"
      "        e => { globalThis.staleRead = { code: e.code, message: e.message }; });\n"
      "automation.find({ name: 'RimeAutomationSliceOK' })\n"
      "  .then(r => { globalThis.afterDestroy = r.elements.length; },\n"
      "        e => { globalThis.afterDestroy = String(e) + ' code=' + e.code; });",
      "automation-after-destroy.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (!globalThis.staleRead || globalThis.staleRead === 'resolved' ||\n"
      "    globalThis.staleRead.code !== 'target_gone')\n"
      "  throw new Error('stale element must reject with target_gone: ' +\n"
      "                  JSON.stringify(globalThis.staleRead));\n"
      "if (globalThis.afterDestroy !== 0)\n"
      "  throw new Error('destroyed target must disappear from searches, got ' +\n"
      "                  globalThis.afterDestroy);",
      "automation-after-destroy-check.mjs");
  assert(service.element_count() == 0);

  // The main runtime releases the JS lane; the denied runtime below needs it.
  assert(runtime.stop().ok());

  // Capability denials throw Errors naming the missing policy entry - and
  // are not TypeErrors, so callers can tell shape bugs from policy bugs.
  {
    rime::action::Kernel denied_kernel(
        std::make_shared<rime::action::StaticCapabilityPolicy>(
            std::unordered_set<std::string>{}),
        trace);
    rime::action::Dispatcher denied_dispatcher(denied_kernel,
                                               rime::action::default_dispatch_policy());
    rime::win32::AutomationModuleBinding denied_binding;
    denied_binding.service = &service;
    denied_binding.kernel = &denied_kernel;
    denied_binding.dispatcher = &denied_dispatcher;
    denied_binding.next_action_id = &next_action_id;
    rime::js::Runtime denied_runtime;
    assert(rime::win32::register_automation_module(denied_runtime, &denied_binding).ok());
    assert(denied_runtime.start().ok());
    run(denied_runtime,
        "import { automation } from 'rime:automation';\n"
        "globalThis.denied = {};\n"
        "try { automation.find({ name: 'x' }); }\n"
        "catch (e) { globalThis.denied.find = { notType: !(e instanceof TypeError),\n"
        "                                        message: e.message }; }\n"
        "try { automation.read(1); }\n"
        "catch (e) { globalThis.denied.read = { notType: !(e instanceof TypeError),\n"
        "                                       message: e.message }; }\n"
        "try { automation.invoke(1); }\n"
        "catch (e) { globalThis.denied.invoke = { notType: !(e instanceof TypeError),\n"
        "                                         message: e.message }; }",
        "automation-deny.mjs");
    run(denied_runtime,
        "const cases = [['find', 'windows.automation.find'],\n"
        "               ['read', 'windows.automation.read'],\n"
        "               ['invoke', 'windows.automation.invoke']];\n"
        "for (const [key, capability] of cases) {\n"
        "  const outcome = globalThis.denied[key];\n"
        "  if (!outcome || !outcome.notType)\n"
        "    throw new Error(key + ' must deny with a plain Error: ' +\n"
        "                    JSON.stringify(outcome));\n"
        "  if (outcome.message.indexOf(capability) < 0)\n"
        "    throw new Error(key + ' must name ' + capability + ': ' + outcome.message);\n"
        "}",
        "automation-deny-check.mjs");
    assert(denied_runtime.stop().ok());
  }

  // Stopped service: queries refuse, release answers false.
  assert(service.stop().ok());
  assert(service.stop().ok());
  {
    rime::automation::FindQuery query;
    query.name = "RimeAutomationSliceOK";
    std::vector<rime::automation::ElementSnapshot> leftover;
    assert(service.find(query, leftover).code == rime::core::Error::Code::InvalidState);
    assert(!service.release(1));
  }
  return 0;
}
