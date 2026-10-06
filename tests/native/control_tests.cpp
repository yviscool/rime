// Realism: L5 - real Win32 control messages against a fixture window the
// test creates, asserts and destroys itself; needs an interactive desktop
// and runs exclusively.
#include "rime/win32/control_executor.hpp"
#include "rime/win32/window.hpp"

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/trace.hpp"

#include <windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::action::Action;
using rime::action::Dispatcher;
using rime::action::Kernel;
using rime::action::Result;
using rime::action::StaticCapabilityPolicy;
using rime::core::Error;
using rime::core::InMemoryTrace;
using rime::win32::ControlExecutor;
using rime::win32::ControlInfo;
using rime::win32::WindowService;
using Code = rime::core::Error::Code;

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

  void run() {
    static const wchar_t* k_class = L"RimeControlNativeTarget";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &TargetWindow::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = k_class;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    (void)RegisterClassW(&window_class);

    HWND window = CreateWindowExW(
        0, k_class, L"Rime Control Native Target", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 480, 300, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) return;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    HWND button = CreateWindowExW(
        0, L"BUTTON", L"RimeControlNativeOK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 40, 40,
        240, 36, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)),
        window_class.hInstance, nullptr);
    HWND edit = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL, 40, 100,
        240, 32, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2)),
        window_class.hInstance, nullptr);
    if (!button || !edit) {
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

std::int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Action make_action(rime::core::ActionId id, const std::string& type, const std::string& target_id,
                   const std::string& payload) {
  Action action;
  action.id = id;
  action.schema_version = 1;
  action.source = {"test", "control_tests"};
  action.type = type;
  action.capability = "windows.automation.control";
  action.target = {"control", target_id};
  action.deadline_unix_ms = static_cast<std::uint64_t>(unix_ms_now() + 60'000);
  action.payload = payload;
  return action;
}

Result run_one(Dispatcher& dispatcher, const Action& action) {
  assert(dispatcher.submit(action) == rime::action::DispatchStatus::Accepted);
  std::vector<Result> results = dispatcher.pump(1);
  assert(results.size() == 1);
  return results.front();
}

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

  WindowService windows;
  assert(windows.start().ok());

  // This single-threaded run IS the worker lane while pumping.
  assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Worker).ok());

  // Resolve the fixture window by title through the service, then its
  // controls by ClassNN (the resolve path rime:control reuses).
  rime::win32::WindowQuery query;
  query.title = "Rime Control Native Target";
  query.title_match_mode = rime::win32::TitleMatchMode::Exact;
  std::vector<rime::win32::WindowInfo> found_windows;
  assert(windows.query(query, found_windows).ok() && found_windows.size() == 1);
  const std::uint64_t window_id = found_windows.front().id;

  std::vector<ControlInfo> controls;
  assert(windows.controls(window_id, controls).ok());
  std::uint64_t button_id = 0;
  std::uint64_t edit_id = 0;
  for (const auto& control : controls) {
    if (control.class_nn == "Button1") button_id = control.id;
    if (control.class_nn == "Edit1") edit_id = control.id;
  }
  assert(button_id != 0 && edit_id != 0);

  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.automation.control"});
  auto trace = std::make_shared<InMemoryTrace>();
  Kernel kernel(policy, trace);
  auto executor = std::make_shared<ControlExecutor>(windows);
  for (const char* type :
       {"control.click", "control.focus", "control.settext", "control.gettext",
        "control.sendtext"}) {
    assert(kernel.register_executor(type, executor).ok());
  }
  Dispatcher dispatcher(kernel, 64);

  const std::string button = std::to_string(button_id);
  const std::string edit = std::to_string(edit_id);

  // click posts down+up: the fixture observes exactly one BN_CLICKED.
  const std::size_t clicks_before = target.clicks();
  Result clicked = run_one(dispatcher, make_action(1, "control.click", button, "{}"));
  assert(clicked.succeeded);
  assert(poll_true(5s, [&] { return target.clicks() == clicks_before + 1; }));

  // count 0 is a silent no-op.
  Result noop = run_one(dispatcher, make_action(2, "control.click", button,
                                                "{\"count\": 0}"));
  assert(noop.succeeded);
  assert(target.clicks() == clicks_before + 1);

  // down-only then up-only halves complete one logical click.
  Result down = run_one(dispatcher, make_action(3, "control.click", button,
                                                "{\"phase\": \"down\"}"));
  assert(down.succeeded);
  Result up = run_one(
      dispatcher, make_action(4, "control.click", button, "{\"phase\": \"up\"}"));
  assert(up.succeeded);
  assert(poll_true(5s, [&] { return target.clicks() == clicks_before + 2; }));

  // focus never reports failure (AHK rule); a dead id fails as TargetGone.
  Result focused = run_one(dispatcher, make_action(5, "control.focus", button, "{}"));
  assert(focused.succeeded);
  Result focus_gone =
      run_one(dispatcher, make_action(6, "control.focus", "18446744073709551615", "{}"));
  assert(!focus_gone.succeeded &&
         focus_gone.error.code == Code::TargetGone);

  // setText/getText round trip on the edit.
  Result set = run_one(dispatcher, make_action(7, "control.settext", edit,
                                                "{\"text\": \"rime-control-1\"}"));
  assert(set.succeeded);
  Result got = run_one(dispatcher, make_action(8, "control.gettext", edit, "{}"));
  assert(got.succeeded);
  assert(got.value.find("text") && got.value.find("text")->as_string() == "rime-control-1");

  // sendText appends raw chars (caret sits past the set text). Posted
  // keystrokes land asynchronously behind the pump, so a synchronous read
  // races them by design (AHK has the same property) - poll until the
  // expected text shows up instead of asserting once.
  Result sent = run_one(dispatcher, make_action(9, "control.sendtext", edit,
                                                "{\"text\": \"-abc\"}"));
  assert(sent.succeeded);
  std::string combined;
  {
    std::uint64_t poll_id = 20;
    bool settled = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && !settled) {
      Result probe =
          run_one(dispatcher, make_action(poll_id++, "control.gettext", edit, "{}"));
      assert(probe.succeeded);
      combined = probe.value.find("text")->as_string();
      settled = combined.find("-abc") != std::string::npos;
      if (!settled) std::this_thread::sleep_for(5ms);
    }
    assert(settled);
  }

  // Contract rejections stay InvalidContract, never silent fallbacks.
  Result bad_button = run_one(dispatcher, make_action(11, "control.click", button,
                                                       "{\"button\": \"laser\"}"));
  assert(!bad_button.succeeded && bad_button.error.code == Code::InvalidContract);
  Result bad_count = run_one(dispatcher, make_action(12, "control.click", button,
                                                      "{\"count\": -1}"));
  assert(!bad_count.succeeded && bad_count.error.code == Code::InvalidContract);
  Result bad_phase = run_one(dispatcher, make_action(13, "control.click", button,
                                                      "{\"phase\": \"sideways\"}"));
  assert(!bad_phase.succeeded && bad_phase.error.code == Code::InvalidContract);
  Result bad_text = run_one(dispatcher, make_action(14, "control.settext", edit,
                                                     "{\"text\": 42}"));
  assert(!bad_text.succeeded && bad_text.error.code == Code::InvalidContract);
  Result bad_target = run_one(dispatcher, make_action(15, "control.click", "0", "{}"));
  assert(!bad_target.succeeded && bad_target.error.code == Code::InvalidContract);
  Result gone = run_one(dispatcher, make_action(16, "control.gettext", "999999", "{}"));
  assert(!gone.succeeded && gone.error.code == Code::TargetGone);

  // Unknown types and wrong target kinds never dispatch.
  Action unknown = make_action(17, "control.frob", button, "{}");
  Result unsupported = run_one(dispatcher, unknown);
  assert(!unsupported.succeeded);
  Action wrong_kind = make_action(18, "control.click", button, "{}");
  wrong_kind.target = {"window", button};
  Result wrong = run_one(dispatcher, wrong_kind);
  assert(!wrong.succeeded && wrong.error.code == Code::InvalidContract);

  // Capability gate: a kernel without the grant refuses by name.
  auto denied_policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{});
  Kernel denied_kernel(denied_policy, trace);
  assert(denied_kernel.register_executor("control.gettext", executor).ok());
  Dispatcher denied_dispatcher(denied_kernel, 64);
  Action gated = make_action(19, "control.gettext", edit, "{}");
  assert(denied_dispatcher.submit(gated) == rime::action::DispatchStatus::Accepted);
  std::vector<Result> gated_results = denied_dispatcher.pump(1);
  assert(gated_results.size() == 1 && !gated_results.front().succeeded);

  // Sync queries through the service: visibility, enabled, rect, liveness.
  bool visible = false;
  assert(windows.control_is_visible(button_id, visible).ok() && visible);
  bool enabled = false;
  assert(windows.control_is_enabled(button_id, enabled).ok() && enabled);
  rime::win32::Rect rect{};
  assert(windows.control_rect(button_id, rect).ok() && rect.width() > 0 && rect.height() > 0);
  bool alive = false;
  assert(windows.control_alive(button_id, alive).ok() && alive);
  bool dead_alive = true;
  assert(windows.control_alive(999999, dead_alive).ok() && !dead_alive);

  assert(windows.stop().ok());
  target.stop();
  rime::core::LaneRegistry::instance().release(rime::core::Lane::Worker);
  return 0;
}
