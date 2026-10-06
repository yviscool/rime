// Realism: L6 - production wiring (JS runtime + Kernel + real WindowService
// and ControlExecutor) drives a fixture window the test builds itself,
// including denied/sync-error paths and side-effect observation.

// Needs an interactive desktop, exclusive run: builds a real target window
// and drives it through the rime:control module (resolve/click/setText/
// getText plus the synchronous queries).
#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/control_executor.hpp"
#include "rime/win32/js_control.hpp"
#include "rime/win32/window.hpp"

#include <windows.h>

#include <commctrl.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
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

void check(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  run(runtime, source, filename);
}

// Owns a top-level window with a uniquely named button and edit on its own
// pumping thread. Clicks arrive as posted messages and SendMessageTimeout
// round-trips run while the test thread waits inside service calls.
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

  [[nodiscard]] std::size_t clicks() const { return clicks_.load(); }

 private:
  static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
      auto* self = reinterpret_cast<TargetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (self) ++self->clicks_;
      return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  void run_thread() {
    static const wchar_t* k_class = L"RimeControlJsTarget";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &TargetWindow::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = k_class;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    (void)RegisterClassW(&window_class);

    HWND window = CreateWindowExW(
        0, k_class, L"Rime Control JS Target", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        480, 360, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) return;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    HWND button = CreateWindowExW(
        0, L"BUTTON", L"RimeControlJsOK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 40, 40, 240, 36,
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), window_class.hInstance, nullptr);
    HWND edit = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL, 40, 100,
        240, 32, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)), window_class.hInstance,
        nullptr);
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                 40, 150, 240, 120, window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(3)),
                                 window_class.hInstance, nullptr);
    HWND check = CreateWindowExW(0, L"BUTTON", L"RimeControlJsCheck",
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 40, 230, 240, 28,
                                 window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(4)),
                                 window_class.hInstance, nullptr);
    if (!button || !edit || !combo || !check) {
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
  std::atomic<std::size_t> clicks_{0};
  DWORD thread_id_{0};
};

bool poll_true(std::chrono::milliseconds budget, const std::function<bool()>& probe) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (probe()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return probe();
}

}  // namespace

int main() {
  TargetWindow target;
  assert(target.start());

  rime::win32::WindowService windows;
  assert(windows.start().ok());

  // Resolve the fixture window id natively; JS only ever sees the number.
  rime::win32::WindowQuery query;
  query.title = "Rime Control JS Target";
  query.title_match_mode = rime::win32::TitleMatchMode::Exact;
  std::vector<rime::win32::WindowInfo> found_windows;
  assert(windows.query(query, found_windows).ok() && found_windows.size() == 1);
  const std::uint64_t window_id = found_windows.front().id;

  // Capability gate first (own runtime): an empty policy rejects reads by
  // name (sync throw). Runs before the main runtime starts because only one
  // JS lane owner exists per process.
  {
    auto denied_trace = std::make_shared<rime::core::InMemoryTrace>();
    rime::action::Kernel denied_kernel(
        std::make_shared<rime::action::StaticCapabilityPolicy>(
            std::unordered_set<std::string>{}),
        denied_trace);
    rime::action::Dispatcher denied_dispatcher(denied_kernel,
                                               rime::action::default_dispatch_policy());
    std::atomic<std::uint64_t> denied_next{0};
    rime::win32::ControlModuleBinding denied_binding;
    denied_binding.service = &windows;
    denied_binding.kernel = &denied_kernel;
    denied_binding.dispatcher = &denied_dispatcher;
    denied_binding.next_action_id = &denied_next;
    rime::js::Runtime denied_runtime;
    assert(rime::win32::register_control_module(denied_runtime, &denied_binding).ok());
    assert(denied_runtime.start().ok());
    run(denied_runtime,
        "import { control } from 'rime:control';\n"
        "let denied = false;\n"
        "try { control.isVisible(1); }\n"
        "catch (e) { denied = String(e && e.message || e).includes('windows.window.read'); }\n"
        "if (!denied) throw new Error('capability denial must name windows.window.read');\n",
        "control-denied.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    assert(denied_runtime.stop().ok());
  }

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
                                  std::unordered_set<std::string>{"windows.window.read",
                                                                  "windows.automation.control"}),
                              trace);
  auto executor = std::make_shared<rime::win32::ControlExecutor>(windows);
  for (const char* type :
       {"control.click", "control.focus", "control.settext", "control.gettext",
        "control.sendtext"}) {
    assert(kernel.register_executor(type, executor).ok());
  }
  rime::action::Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  std::atomic<std::uint64_t> next_action_id{0};
  rime::win32::ControlModuleBinding binding;
  binding.service = &windows;
  binding.kernel = &kernel;
  binding.dispatcher = &dispatcher;
  binding.next_action_id = &next_action_id;
  rime::js::Runtime runtime;
  assert(rime::win32::register_control_module(runtime, &binding).ok());
  assert(runtime.start().ok());

  // Wiring is part of the contract.
  {
    rime::win32::ControlModuleBinding partial;
    partial.service = &windows;
    partial.kernel = &kernel;
    rime::js::Runtime partial_runtime;
    assert(rime::win32::register_control_module(partial_runtime, &partial).code ==
           rime::core::Error::Code::InvalidContract);
  }

  run(runtime,
      "import { control } from 'rime:control';\n"
      "globalThis.control = control;\n"
      "globalThis.windowId = " +
          std::to_string(window_id) + ";\n",
      "control-setup.mjs");

  // resolve() by ClassNN; a query (no Action) leaves the trace empty.
  check(runtime,
        "globalThis.button = await globalThis.control.resolve(globalThis.windowId, "
        "  { classNN: 'Button1' });\n"
        "if (typeof globalThis.button.id !== 'number' || globalThis.button.id <= 0)\n"
        "  throw new Error('resolve must return a positive id');\n"
        "if (globalThis.button.classNN !== 'Button1')\n"
        "  throw new Error('resolve must echo the ClassNN');\n"
        "globalThis.edit = await globalThis.control.resolve(globalThis.windowId, "
        "  { auto: 'Edit1' });\n"
        "if (globalThis.edit.classNN !== 'Edit1') throw new Error('auto resolve failed');\n",
        "control-resolve.mjs");
  assert(runtime.settle(5000ms).ok());
  assert(trace->snapshot().empty());

  // Shape errors are synchronous TypeErrors.
  check(runtime,
        "for (const bad of [\n"
        "  () => globalThis.control.resolve(),\n"
        "  () => globalThis.control.resolve(globalThis.windowId),\n"
        "  () => globalThis.control.resolve(globalThis.windowId, { classNN: 'B1', text: 'x' }),\n"
        "  () => globalThis.control.resolve(0, { classNN: 'B1' }),\n"
        "  () => globalThis.control.click(),\n"
        "  () => globalThis.control.click(1.5),\n"
        "  () => globalThis.control.setText(globalThis.edit.id),\n"
        "  () => globalThis.control.setText(globalThis.edit.id, 42),\n"
        "  () => globalThis.control.isVisible(),\n"
        "]) {\n"
        "  let threw = false;\n"
        "  try { bad(); } catch (e) { threw = e instanceof TypeError; }\n"
        "  if (!threw) throw new Error('shape error must be a TypeError');\n"
        "}\n",
        "control-shape.mjs");
  assert(runtime.settle(5000ms).ok());

  // A miss rejects invalid_contract (a result, shaped by the service).
  check(runtime,
        "let missed = false;\n"
        "try {\n"
        "  await globalThis.control.resolve(globalThis.windowId, { classNN: 'Nope99' });\n"
        "} catch (e) { missed = String(e && e.message || e).includes('no control matches'); }\n"
        "if (!missed) throw new Error('a resolve miss must reject');\n",
        "control-miss.mjs");
  assert(runtime.settle(5000ms).ok());

  // click() dispatches an Action; the posted messages land as a real click.
  const std::size_t clicks_before = target.clicks();
  check(runtime,
        "globalThis.clicked = await globalThis.control.click(globalThis.button.id);\n"
        "if (!globalThis.clicked || globalThis.clicked.clicks !== 1)\n"
        "  throw new Error('click must resolve { clicks: 1 }');\n",
        "control-click.mjs");
  assert(runtime.settle(5000ms).ok());
  assert(poll_true(5s, [&] { return target.clicks() == clicks_before + 1; }));

  // setText/getText round trip through the pipeline.
  check(runtime,
        "await globalThis.control.setText(globalThis.edit.id, 'rime-control-js');\n"
        "globalThis.back = await globalThis.control.getText(globalThis.edit.id);\n"
        "if (!globalThis.back || globalThis.back.text !== 'rime-control-js')\n"
        "  throw new Error('setText/getText round trip failed: ' +\n"
        "                   JSON.stringify(globalThis.back));\n",
        "control-text.mjs");
  assert(runtime.settle(5000ms).ok());

  // Synchronous queries read straight through (no Action dispatched).
  check(runtime,
        "if (globalThis.control.isVisible(globalThis.button.id) !== true)\n"
        "  throw new Error('button must be visible');\n"
        "if (globalThis.control.isEnabled(globalThis.button.id) !== true)\n"
        "  throw new Error('button must be enabled');\n"
        "const rect = globalThis.control.rect(globalThis.button.id);\n"
        "if (!(rect.width > 0 && rect.height > 0))\n"
        "  throw new Error('rect must be non-empty: ' + JSON.stringify(rect));\n"
        "if (globalThis.control.dispose(globalThis.button.id) !== true)\n"
        "  throw new Error('dispose of a live id must be true');\n"
        "if (globalThis.control.dispose(999999) !== false)\n"
        "  throw new Error('dispose of a dead id must be false');\n",
        "control-sync.mjs");
  assert(runtime.settle(5000ms).ok());

  // ---- Phase 2: list verbs on a ComboBox ------------------------------------
  check(runtime,
        "globalThis.combo = await globalThis.control.resolve(globalThis.windowId, "
        "  { classNN: 'ComboBox1' });\n"
        "if (globalThis.combo.classNN !== 'ComboBox1') throw new Error('combo resolve failed');\n"
        "const added = await globalThis.control.listAdd(globalThis.combo.id, 'red');\n"
        "if (!added || added.index !== 1) throw new Error('listAdd must resolve { index: 1 }');\n"
        "await globalThis.control.listAdd(globalThis.combo.id, 'green');\n"
        "await globalThis.control.listAdd(globalThis.combo.id, 'blue');\n"
        "const found = await globalThis.control.listFind(globalThis.combo.id, 'green');\n"
        "if (!found || found.index !== 2) throw new Error('listFind must resolve 2');\n"
        "const miss = await globalThis.control.listFind(globalThis.combo.id, 'nope');\n"
        "if (!miss || miss.index !== 0) throw new Error('a find miss must resolve { index: 0 }');\n"
        "await globalThis.control.listChoose(globalThis.combo.id, { index: 3 });\n"
        "const cur = await globalThis.control.listIndex(globalThis.combo.id);\n"
        "if (!cur || cur.index !== 3) throw new Error('listIndex must resolve 3');\n"
        "const choice = await globalThis.control.listChoice(globalThis.combo.id);\n"
        "if (!choice || choice.text !== 'blue') throw new Error('listChoice must be blue');\n"
        "const items = await globalThis.control.listItems(globalThis.combo.id, 10);\n"
        "if (!items || JSON.stringify(items.items) !== '[\"red\",\"green\",\"blue\"]')\n"
        "  throw new Error('listItems mismatch: ' + JSON.stringify(items));\n"
        "await globalThis.control.listDelete(globalThis.combo.id, 2);\n"
        "const after = await globalThis.control.listItems(globalThis.combo.id);\n"
        "if (JSON.stringify(after.items) !== '[\"red\",\"blue\"]')\n"
        "  throw new Error('listDelete failed: ' + JSON.stringify(after));\n",
        "control-list.mjs");
  assert(runtime.settle(5000ms).ok());

  // Shape errors for the new verbs stay synchronous TypeErrors.
  check(runtime,
        "for (const bad of [\n"
        "  () => globalThis.control.listAdd(globalThis.combo.id),\n"
        "  () => globalThis.control.listAdd(globalThis.combo.id, 42),\n"
        "  () => globalThis.control.listDelete(globalThis.combo.id, 0),\n"
        "  () => globalThis.control.listChoose(globalThis.combo.id, {}),\n"
        "  () => globalThis.control.listChoose(globalThis.combo.id, { index: 1, text: 'x' }),\n"
        "  () => globalThis.control.listItems(globalThis.combo.id, 0),\n"
        "  () => globalThis.control.tabSelect(globalThis.combo.id, 0),\n"
        "  () => globalThis.control.editLine(globalThis.edit.id, 0),\n"
        "  () => globalThis.control.setChecked(globalThis.combo.id, 2),\n"
        "]) {\n"
        "  let threw = false;\n"
        "  try { bad(); } catch (e) { threw = e instanceof TypeError; }\n"
        "  if (!threw) throw new Error('phase-2 shape error must be a TypeError');\n"
        "}\n",
        "control-list-shape.mjs");
  assert(runtime.settle(5000ms).ok());

  // Checkbox through the pipeline (posted clicks converge like the native test).
  check(runtime,
        "globalThis.check = await globalThis.control.resolve(globalThis.windowId, "
        "  { text: 'RimeControlJsCheck' });\n"
        "const off = await globalThis.control.isChecked(globalThis.check.id);\n"
        "if (!off || off.checked !== false) throw new Error('fresh checkbox must be off');\n"
        "await globalThis.control.setChecked(globalThis.check.id, true);\n"
        "globalThis.checkOn = null;\n"
        "for (let i = 0; i < 200; i++) {\n"
        "  const s = await globalThis.control.isChecked(globalThis.check.id);\n"
        "  if (s && s.checked === true) { globalThis.checkOn = s; break; }\n"
        "}\n"
        "if (!globalThis.checkOn) throw new Error('checkbox never turned on');\n",
        "control-check.mjs");
  assert(runtime.settle(15000ms).ok());

  assert(runtime.stop().ok());
  assert(windows.stop().ok());
  target.stop();
  return 0;
}
