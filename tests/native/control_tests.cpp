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

#include <commctrl.h>

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
    INITCOMMONCONTROLSEX common{};
    common.dwSize = sizeof(common);
    common.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&common);

    HWND window = CreateWindowExW(
        0, k_class, L"Rime Control Native Target", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 480, 420, nullptr, nullptr, window_class.hInstance, nullptr);
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
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                 40, 150, 240, 120, window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(3)),
                                 window_class.hInstance, nullptr);
    HWND list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | LBS_NOTIFY, 40, 230,
                                240, 60, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(4)),
                                window_class.hInstance, nullptr);
    HWND edit_multi = CreateWindowExW(
        0, L"EDIT", L"l1\r\nl2\r\nl3", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                          ES_AUTOVSCROLL | WS_VSCROLL,
        300, 40, 140, 100, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5)),
        window_class.hInstance, nullptr);
    HWND check = CreateWindowExW(0, L"BUTTON", L"RimeControlNativeCheck",
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 300, 150, 140, 28,
                                 window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(6)),
                                 window_class.hInstance, nullptr);
    HWND tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 300, 200, 140,
                               80, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7)),
                               window_class.hInstance, nullptr);
    if (!button || !edit || !combo || !list || !edit_multi || !check || !tab) {
      DestroyWindow(window);
      return;
    }
    for (int page = 0; page < 2; ++page) {
      TCITEMW item{};
      item.mask = TCIF_TEXT;
      wchar_t label[16] = {0};
      label[0] = static_cast<wchar_t>(L'P' + page);
      label[1] = static_cast<wchar_t>(L'1' + page);
      item.pszText = label;
      if (TabCtrl_InsertItem(tab, page, &item) < 0) {
        DestroyWindow(window);
        return;
      }
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
  std::uint64_t combo_id = 0;
  std::uint64_t list_id = 0;
  std::uint64_t edit_multi_id = 0;
  std::uint64_t check_id = 0;
  std::uint64_t tab_id = 0;
  for (const auto& control : controls) {
    if (control.class_nn == "Button1") button_id = control.id;
    if (control.class_nn == "Edit1") edit_id = control.id;
    if (control.class_nn == "ComboBox1") combo_id = control.id;
    if (control.class_nn == "ListBox1") list_id = control.id;
    if (control.class_nn == "Edit2") edit_multi_id = control.id;
    if (control.class_nn == "Button2") check_id = control.id;
    if (control.class_nn == "SysTabControl321") tab_id = control.id;
  }
  assert(button_id != 0 && edit_id != 0 && combo_id != 0 && list_id != 0 &&
         edit_multi_id != 0 && check_id != 0 && tab_id != 0);

  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.automation.control"});
  auto trace = std::make_shared<InMemoryTrace>();
  Kernel kernel(policy, trace);
  auto executor = std::make_shared<ControlExecutor>(windows);
  for (const char* type :
       {"control.click", "control.focus", "control.settext", "control.gettext",
        "control.sendtext", "control.list.add", "control.list.delete", "control.list.choose",
        "control.list.find", "control.list.index", "control.list.choice", "control.list.items",
        "control.tab.select", "control.edit.count", "control.edit.caret", "control.edit.line",
        "control.edit.selected", "control.edit.paste", "control.setchecked",
        "control.ischecked"}) {
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

  // ---- Phase 2: list family on the ComboBox ---------------------------------
  const std::string combo = std::to_string(combo_id);
  rime::core::ActionId next = 100;
  auto run = [&](const std::string& type, const std::string& target,
                 const std::string& payload) -> Result {
    return run_one(dispatcher, make_action(next++, type, target, payload));
  };
  Result add_a = run("control.list.add", combo, "{\"text\": \"alpha\"}");
  assert(add_a.succeeded);
  Result add_b = run("control.list.add", combo, "{\"text\": \"beta\"}");
  assert(add_b.succeeded);
  Result add_c = run("control.list.add", combo, "{\"text\": \"gamma\"}");
  assert(add_c.succeeded);
  Result find_b = run("control.list.find", combo, "{\"text\": \"beta\"}");
  assert(find_b.succeeded);
  assert(find_b.value.find("index") &&
         find_b.value.find("index")->as_number() == 2.0);
  Result find_miss = run("control.list.find", combo, "{\"text\": \"nope\"}");
  assert(find_miss.succeeded);
  assert(find_miss.value.find("index")->as_number() == 0.0);  // miss is a result
  Result choose_2 = run("control.list.choose", combo, "{\"index\": 2}");
  assert(choose_2.succeeded);
  Result index_is_2 = run("control.list.index", combo, "{}");
  assert(index_is_2.succeeded);
  assert(index_is_2.value.find("index")->as_number() == 2.0);
  Result choice_2 = run("control.list.choice", combo, "{\"index\": 2}");
  assert(choice_2.succeeded);
  assert(choice_2.value.find("text")->as_string() == "beta");
  Result choice_cur = run("control.list.choice", combo, "{}");
  assert(choice_cur.succeeded);
  assert(choice_cur.value.find("text")->as_string() == "beta");
  Result items = run("control.list.items", combo, "{\"limit\": 10}");
  assert(items.succeeded);
  {
    const auto* list = items.value.find("items");
    assert(list && list->is_array() && list->size() == 3);
    assert(list->as_array()[0].as_string() == "alpha");
    assert(list->as_array()[2].as_string() == "gamma");
  }
  Result del_2 = run("control.list.delete", combo, "{\"index\": 2}");
  assert(del_2.succeeded);
  Result find_gone = run("control.list.find", combo, "{\"text\": \"beta\"}");
  assert(find_gone.succeeded && find_gone.value.find("index")->as_number() == 0.0);
  Result choose_text = run("control.list.choose", combo, "{\"text\": \"gamma\"}");
  assert(choose_text.succeeded);
  Result index_is_2b = run("control.list.index", combo, "{}");
  assert(index_is_2b.succeeded && index_is_2b.value.find("index")->as_number() == 2.0);

  // Same verbs on a single-select ListBox (message table branch).
  const std::string listbox = std::to_string(list_id);
  assert(run("control.list.add", listbox, "{\"text\": \"x\"}").succeeded);
  assert(run("control.list.add", listbox, "{\"text\": \"y\"}").succeeded);
  Result lchoose = run("control.list.choose", listbox, "{\"text\": \"y\"}");
  assert(lchoose.succeeded);
  Result lindex = run("control.list.index", listbox, "{}");
  assert(lindex.succeeded && lindex.value.find("index")->as_number() == 2.0);

  // Wrong-kind and shape rejections.
  Result add_button =
      run("control.list.add", button, "{\"text\": \"z\"}");
  assert(!add_button.succeeded && add_button.error.code == Code::InvalidContract);
  Result choose_neither = run("control.list.choose", combo, "{}");
  assert(!choose_neither.succeeded && choose_neither.error.code == Code::InvalidContract);
  Result choose_both =
      run("control.list.choose", combo, "{\"index\": 1, \"text\": \"a\"}");
  assert(!choose_both.succeeded && choose_both.error.code == Code::InvalidContract);
  Result del_zero = run("control.list.delete", combo, "{\"index\": 0}");
  assert(!del_zero.succeeded && del_zero.error.code == Code::InvalidContract);
  Result items_zero = run("control.list.items", combo, "{\"limit\": 0}");
  assert(!items_zero.succeeded && items_zero.error.code == Code::InvalidContract);
  Result items_huge = run("control.list.items", combo, "{\"limit\": 10001}");
  assert(!items_huge.succeeded && items_huge.error.code == Code::InvalidContract);

  // ---- Tab + multiline edit + checkbox --------------------------------------
  const std::string tab = std::to_string(tab_id);
  assert(run("control.tab.select", tab, "{\"index\": 2}").succeeded);
  Result tab_zero = run("control.tab.select", tab, "{\"index\": 0}");
  assert(!tab_zero.succeeded && tab_zero.error.code == Code::InvalidContract);
  Result tab_wrong = run("control.tab.select", button, "{\"index\": 1}");
  assert(!tab_wrong.succeeded && tab_wrong.error.code == Code::InvalidContract);

  const std::string edit_multi = std::to_string(edit_multi_id);
  Result count3 = run("control.edit.count", edit_multi, "{}");
  assert(count3.succeeded && count3.value.find("lines")->as_number() == 3.0);
  Result line2 = run("control.edit.line", edit_multi, "{\"line\": 2}");
  assert(line2.succeeded && line2.value.find("text")->as_string() == "l2");
  Result line_past = run("control.edit.line", edit_multi, "{\"line\": 9}");
  assert(!line_past.succeeded && line_past.error.code == Code::InvalidContract);
  Result caret11 = run("control.edit.caret", edit_multi, "{}");
  assert(caret11.succeeded && caret11.value.find("line")->as_number() == 1.0 &&
         caret11.value.find("col")->as_number() == 1.0);
  Result paste = run("control.edit.paste", edit_multi, "{\"text\": \"X\"}");
  assert(paste.succeeded);
  Result line1x = run("control.edit.line", edit_multi, "{\"line\": 1}");
  assert(line1x.succeeded && line1x.value.find("text")->as_string() == "Xl1");
  Result selected_empty = run("control.edit.selected", edit_multi, "{}");
  assert(selected_empty.succeeded && selected_empty.value.find("text")->as_string().empty());

  const std::string check = std::to_string(check_id);
  Result is_unchecked = run("control.ischecked", check, "{}");
  assert(is_unchecked.succeeded && !is_unchecked.value.find("checked")->as_bool());
  auto read_checked = [&](bool& state) -> bool {    Result probe = run("control.ischecked", check, "{}");
    if (!probe.succeeded) return false;
    const auto* field = probe.value.find("checked");
    if (!field) return false;
    state = field->as_bool();
    return true;
  };
  // Posted clicks land asynchronously behind the pump (same race as
  // sendText): poll the BM_GETCHECK state instead of asserting once.
  bool seen = false;
  {
    Result set_on = run("control.setchecked", check, "{\"checked\": 1}");
    assert(set_on.succeeded);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool state = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (read_checked(state) && state) {
        seen = true;
        break;
      }
      std::this_thread::sleep_for(5ms);
    }
    assert(seen);
  }
  {
    Result toggle = run("control.setchecked", check, "{\"checked\": -1}");
    assert(toggle.succeeded);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool state = true;
    bool cleared = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (read_checked(state) && !state) {
        cleared = true;
        break;
      }
      std::this_thread::sleep_for(5ms);
    }
    assert(cleared);
  }
  Result bad_check = run("control.setchecked", check, "{\"checked\": 2}");
  assert(!bad_check.succeeded && bad_check.error.code == Code::InvalidContract);

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
