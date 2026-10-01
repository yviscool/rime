// Needs an interactive desktop, exclusive run: builds a real target window
// and drives it through UI Automation from a dedicated COM MTA thread.
#include "rime/automation/uia_service.hpp"

#include <windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::automation::ElementSnapshot;
using rime::automation::FindQuery;
using rime::automation::UiaService;
using rime::automation::UiaServiceState;
using Code = rime::core::Error::Code;

// Owns a top-level window with a uniquely named button on its own pumping
// thread. UIA calls answer through SendMessage (WM_GETOBJECT, invoke ->
// WM_COMMAND), so the window must pump while the test thread waits inside
// service calls - otherwise every call would deadlock against itself.
class TargetWindow final {
 public:
  TargetWindow() = default;
  ~TargetWindow() { stop(); }
  TargetWindow(const TargetWindow&) = delete;
  TargetWindow& operator=(const TargetWindow&) = delete;

  bool start() {
    ready_.store(false);
    thread_ = std::thread([this] { run(); });
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

  // Set when the button's WM_COMMAND / BN_CLICKED reaches the window proc.
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

  void run() {
    static const wchar_t* k_class = L"RimeAutomationNativeTarget";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &TargetWindow::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = k_class;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // A duplicate registration (re-run after an aborted test) is harmless:
    // the existing class uses the same window proc.
    (void)RegisterClassW(&window_class);

    HWND window = CreateWindowExW(
        0, k_class, L"Rime Automation Native Target", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 400, 260, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) return;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    HWND button = CreateWindowExW(
        0, L"BUTTON", L"RimeAutomationNativeOK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 40, 40,
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

  UiaService service;

  // Work before start is rejected.
  FindQuery warmup;
  warmup.name = "RimeAutomationNativeOK";
  std::vector<ElementSnapshot> found;
  assert(service.find(warmup, found).code == Code::InvalidState);
  assert(service.start().ok());
  assert(!service.start().ok());  // start-once
  assert(service.state() == UiaServiceState::Running);

  // find() locates the uniquely named button and registers it.
  FindQuery query;
  query.name = "RimeAutomationNativeOK";
  query.control_type = "button";
  assert(service.find(query, found).ok());
  assert(found.size() == 1);
  const std::uint64_t element_id = found[0].id;
  assert(element_id > 0);
  assert(found[0].name == "RimeAutomationNativeOK");
  assert(found[0].control_type == "button");
  assert(found[0].enabled);
  assert(found[0].width > 0 && found[0].height > 0);
  assert(service.element_count() == 1);

  // read() round-trips the registered snapshot.
  ElementSnapshot read_back;
  assert(service.read(element_id, read_back).ok());
  assert(read_back.id == element_id);
  assert(read_back.name == "RimeAutomationNativeOK");
  assert(read_back.control_type == "button");
  assert(read_back.enabled);

  // invoke() presses the button: the window thread receives WM_COMMAND.
  assert(service.invoke(element_id).ok());
  const auto invoked_by = std::chrono::steady_clock::now() + 5s;
  while (!target.invoked() && std::chrono::steady_clock::now() < invoked_by) {
    std::this_thread::sleep_for(10ms);
  }
  assert(target.invoked());

  // Contract violations answer without side effects.
  FindQuery empty;
  assert(service.find(empty, found).code == Code::InvalidContract);
  FindQuery unknown_type;
  unknown_type.control_type = "gizmo";
  assert(service.find(unknown_type, found).code == Code::InvalidContract);
  FindQuery zero_max;
  zero_max.name = "RimeAutomationNativeOK";
  zero_max.max_results = 0;
  assert(service.find(zero_max, found).code == Code::InvalidContract);
  FindQuery scoped_unknown;
  scoped_unknown.name = "RimeAutomationNativeOK";
  scoped_unknown.from_id = 999'999;
  assert(service.find(scoped_unknown, found).code == Code::TargetGone);

  // Unknown ids answer target_gone; release drops references exactly once.
  ElementSnapshot scratch;
  assert(service.read(999'999, scratch).code == Code::TargetGone);
  assert(service.invoke(999'999).code == Code::TargetGone);
  assert(service.release(element_id));
  assert(!service.release(element_id));
  assert(service.element_count() == 0);

  // Destroying the window invalidates live references: the stale entry is
  // dropped and the element disappears from later searches.
  assert(service.find(query, found).ok());
  assert(found.size() == 1);
  const std::uint64_t stale_id = found[0].id;
  target.stop();
  ElementSnapshot stale_read;
  assert(service.read(stale_id, stale_read).code == Code::TargetGone);
  assert(service.element_count() == 0);
  assert(service.find(query, found).ok());
  assert(found.empty());

  // Stop is repeatable; later calls are refused and release answers false.
  assert(service.stop().ok());
  assert(service.stop().ok());
  assert(service.state() == UiaServiceState::Stopped);
  assert(service.find(query, found).code == Code::InvalidState);
  assert(!service.release(1));

  return 0;
}
