#include "rime/action/kernel.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/trace.hpp"
#include "rime/win32/window.hpp"
#include "rime/win32/window_executor.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::win32::Rect;
using rime::win32::WindowInfo;
using rime::win32::WindowService;

constexpr wchar_t kTestWindowTitle[] = L"Rime WindowService Test Window";

std::optional<WindowInfo> find_by_title(const std::vector<WindowInfo>& windows) {
  for (const auto& window : windows) {
    if (window.title.find("Rime WindowService Test Window") != std::string::npos) return window;
  }
  return std::nullopt;
}

}  // namespace

int main() {
  WindowService service;
  std::vector<WindowInfo> windows;

  // Work before start is rejected.
  assert(!service.list(windows).ok());
  assert(service.start().ok());
  assert(!service.start().ok());  // start-once

  // Create the test window on the UI thread.
  HWND created = nullptr;
  assert(service.ui()
             .call([&] {
               created = CreateWindowExW(0, L"STATIC", kTestWindowTitle,
                                         WS_OVERLAPPED | WS_VISIBLE, 120, 80, 640, 480, nullptr,
                                         nullptr, GetModuleHandleW(nullptr), nullptr);
               assert(created != nullptr);
             })
             .ok());

  // list() reports it with the creation geometry and our process.
  assert(service.list(windows).ok());
  const auto found = find_by_title(windows);
  assert(found.has_value());
  const std::uint64_t id = found->id;
  assert(found->visible);
  assert(found->process_id == GetCurrentProcessId());
  assert(found->rect.left == 120);
  assert(found->rect.top == 80);
  assert(found->rect.width() == 640);
  assert(found->rect.height() == 480);

  // move("left") snaps to the left half of the primary work area.
  assert(service.move(id, "left").ok());
  WindowInfo moved;
  assert(service.info(id, moved).ok());
  RECT work{};
  assert(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE);
  const Rect left_half{work.left, work.top, work.left + (work.right - work.left) / 2,
                       work.bottom};
  assert(moved.rect == left_half);

  // placement_rect resolves independently and matches the move.
  Rect placement{};
  assert(service.placement_rect("left", placement).ok());
  assert(placement == left_half);
  assert(service.placement_rect("right", placement).ok());
  assert(placement.left == work.left + (work.right - work.left) / 2);
  assert(!service.placement_rect("diagonal", placement).ok());

  // Explicit rect moves round-trip through info().
  const Rect target{50, 60, 450, 360};
  assert(service.move_rect(id, target).ok());
  WindowInfo resized;
  assert(service.info(id, resized).ok());
  assert(resized.rect == target);

  // focus is a weak assertion by necessity: SetForegroundWindow may refuse
  // while another window owns the foreground (interactive/CI dependent), so
  // a foreground-lock ExecutionFailed is acceptable; stale ids are not.
  const auto focus_result = service.focus(id);
  assert(focus_result.ok() ||
         focus_result.code == rime::core::Error::Code::ExecutionFailed);

  // Snapshot fields beyond geometry: class, process image and state.
  WindowInfo snapshot;
  assert(service.info(id, snapshot).ok());
  // The system class registers as "Static"; queries match it case-insensitively.
  assert(snapshot.class_name == "Static");
  assert(snapshot.process_name.size() > 4);
  assert(snapshot.process_name.find(".exe") != std::string::npos ||
         snapshot.process_name.find(".EXE") != std::string::npos);
  assert(snapshot.state == "normal");
  assert(!snapshot.minimized);

  // WinTitle-style queries resolve on the UI lane: contains vs exact title,
  // class and executable filters, and the hidden window rule.
  rime::win32::WindowQuery by_title;
  by_title.title = "Rime WindowService";
  std::vector<WindowInfo> matched;
  assert(service.query(by_title, matched).ok());
  assert(find_by_title(matched).has_value());

  rime::win32::WindowQuery exact;
  exact.title = "Rime WindowService Test Window";
  exact.exact_title = true;
  matched.clear();
  assert(service.query(exact, matched).ok());
  assert(find_by_title(matched).has_value());

  rime::win32::WindowQuery exact_partial;
  exact_partial.title = "Rime WindowService";
  exact_partial.exact_title = true;
  matched.clear();
  assert(service.query(exact_partial, matched).ok());
  assert(!find_by_title(matched).has_value());

  rime::win32::WindowQuery by_class;
  by_class.class_name = "STATIC";
  matched.clear();
  assert(service.query(by_class, matched).ok());
  assert(find_by_title(matched).has_value());

  rime::win32::WindowQuery by_exe;
  char executable[MAX_PATH] = {};
  assert(GetModuleFileNameA(nullptr, executable, MAX_PATH) > 0);
  const std::string self_name = std::string(executable).substr(
      std::string(executable).find_last_of("\\/") + 1);
  by_exe.process_name = self_name;
  matched.clear();
  assert(service.query(by_exe, matched).ok());
  assert(find_by_title(matched).has_value());

  rime::win32::WindowQuery bogus;
  bogus.title = "No Such Window Title Anywhere";
  matched.clear();
  assert(service.query(bogus, matched).ok());
  assert(matched.empty());

  // State mutations: hide/show/minimize/maximize/restore round-trip through
  // the snapshot state machine.
  assert(service.hide(id).ok());
  WindowInfo hidden;
  assert(service.info(id, hidden).ok());
  assert(!hidden.visible);
  assert(hidden.state == "hidden");
  matched.clear();
  assert(service.query(by_title, matched).ok());
  assert(!find_by_title(matched).has_value());
  by_title.include_hidden = true;
  matched.clear();
  assert(service.query(by_title, matched).ok());
  assert(find_by_title(matched).has_value());
  by_title.include_hidden = false;

  assert(service.show(id).ok());
  WindowInfo shown;
  assert(service.info(id, shown).ok());
  assert(shown.visible);
  assert(shown.state == "normal");

  assert(service.minimize(id).ok());
  WindowInfo minimized;
  assert(service.info(id, minimized).ok());
  assert(minimized.minimized);
  assert(minimized.state == "minimized");

  assert(service.restore(id).ok());
  WindowInfo restored;
  assert(service.info(id, restored).ok());
  assert(!restored.minimized);
  assert(restored.state == "normal");

  assert(service.maximize(id).ok());
  WindowInfo maximized;
  assert(service.info(id, maximized).ok());
  assert(maximized.state == "maximized");
  assert(service.restore(id).ok());

  // close(): WM_CLOSE runs inline on this thread, so the id is stale once
  // the call returns.
  HWND disposable = nullptr;
  assert(service.ui()
             .call([&] {
               disposable = CreateWindowExW(0, L"STATIC", L"Rime Close Target",
                                            WS_OVERLAPPED | WS_VISIBLE, 40, 40, 320, 240,
                                            nullptr, nullptr, GetModuleHandleW(nullptr),
                                            nullptr);
               assert(disposable != nullptr);
             })
             .ok());
  std::vector<WindowInfo> close_match;
  rime::win32::WindowQuery close_query;
  close_query.title = "Rime Close Target";
  assert(service.query(close_query, close_match).ok());
  assert(close_match.size() == 1);
  const std::uint64_t close_id = close_match.front().id;
  assert(service.close(close_id).ok());
  WindowInfo gone;
  assert(service.info(close_id, gone).code == rime::core::Error::Code::InvalidState);

  // active() is a weak assertion by necessity: there may be no foreground
  // window at all (headless/locked session), so only a present value must
  // resolve to a live, inspectable window.
  std::optional<WindowInfo> active;
  assert(service.active(active).ok());
  if (active.has_value()) {
    WindowInfo again;
    assert(service.info(active->id, again).ok());
  }

  // WindowExecutor: a contract-valid window.move reaches the service and
  // records the trace pair.
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

  rime::action::Action move_action;
  move_action.id = 1;
  move_action.source = {"test", "win32_tests"};
  move_action.type = "window.move";
  move_action.capability = "windows.window.write";
  move_action.target = {"window", std::to_string(id)};
  move_action.payload = R"({"position":"left"})";
  move_action.deadline_unix_ms =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count()) +
      5000;

  const auto executed = kernel.execute(move_action);
  assert(executed.succeeded);
  assert(executed.value.is_object());
  assert(executed.value.find("id") != nullptr);
  WindowInfo executor_moved;
  assert(service.info(id, executor_moved).ok());
  assert(executor_moved.rect == left_half);
  const auto started_entries = [&] {
    std::size_t count = 0;
    for (const auto& entry : trace->snapshot()) {
      if (entry.kind == rime::core::TraceKind::ActionStarted &&
          entry.subject == "window.move") {
        ++count;
      }
    }
    return count;
  }();
  assert(started_entries == 1);

  // The executor dispatches every other write type through the kernel: each
  // resolves a snapshot and round-trips the visible state machine. focus is
  // weak (foreground-lock denial is environment-dependent, see above).
  const auto run_window_action = [&](const char* type, const std::string& payload,
                                     std::uint64_t action_id) {
    rime::action::Action action = move_action;
    action.id = action_id;
    action.type = type;
    action.payload = payload;
    return kernel.execute(action);
  };
  const auto focus_executed = run_window_action("window.focus", "{}", 6);
  assert(focus_executed.succeeded ||
         focus_executed.error.code == rime::core::Error::Code::ExecutionFailed);

  const auto hide_executed = run_window_action("window.hide", "{}", 7);
  assert(hide_executed.succeeded);
  assert(hide_executed.value.is_object());
  WindowInfo executor_hidden;
  const auto hidden_info = service.info(id, executor_hidden);
  assert(hidden_info.ok());
  assert(executor_hidden.state == "hidden");

  const auto show_executed = run_window_action("window.show", "{}", 8);
  assert(show_executed.succeeded);
  assert(show_executed.value.is_object());
  WindowInfo executor_shown;
  const auto shown_info = service.info(id, executor_shown);
  assert(shown_info.ok());
  assert(executor_shown.state == "normal");

  const auto minimize_executed = run_window_action("window.minimize", "{}", 9);
  assert(minimize_executed.succeeded);
  assert(minimize_executed.value.is_object());
  WindowInfo executor_minimized;
  const auto minimized_info = service.info(id, executor_minimized);
  assert(minimized_info.ok());
  assert(executor_minimized.state == "minimized");

  const auto restore_executed = run_window_action("window.restore", "{}", 10);
  assert(restore_executed.succeeded);
  assert(restore_executed.value.is_object());
  WindowInfo executor_restored;
  const auto restored_info = service.info(id, executor_restored);
  assert(restored_info.ok());
  assert(executor_restored.state == "normal");

  const auto maximize_executed = run_window_action("window.maximize", "{}", 11);
  assert(maximize_executed.succeeded);
  assert(maximize_executed.value.is_object());
  WindowInfo executor_maximized;
  const auto maximized_info = service.info(id, executor_maximized);
  assert(maximized_info.ok());
  assert(executor_maximized.state == "maximized");

  const auto restore_again = run_window_action("window.restore", "{}", 12);
  assert(restore_again.succeeded);
  WindowInfo executor_normal;
  const auto normal_info = service.info(id, executor_normal);
  assert(normal_info.ok());
  assert(executor_normal.state == "normal");

  // Capability denial happens before the executor runs: every registered
  // type is refused once under an empty policy and the window is untouched
  // (denial precedes dispatch, so even window.close is side-effect free).
  WindowInfo before_denied;
  const auto denied_snapshot = service.info(id, before_denied);
  assert(denied_snapshot.ok());
  rime::action::Kernel denied(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{}),
      trace);
  constexpr const char* kDeniedTypes[] = {"window.move",  "window.focus",    "window.close",
                                          "window.hide",  "window.show",     "window.minimize",
                                          "window.maximize", "window.restore"};
  for (const char* type : kDeniedTypes) {
    const auto registered = denied.register_executor(type, window_executor);
    assert(registered.ok());
  }
  std::uint64_t denied_id = 20;
  for (const char* type : kDeniedTypes) {
    rime::action::Action refused_action = move_action;
    refused_action.id = denied_id++;
    refused_action.type = type;
    refused_action.payload =
        std::string(type) == "window.move" ? R"({"position":"left"})" : "{}";
    const auto refused = denied.execute(refused_action);
    assert(!refused.succeeded);
    assert(refused.error.code == rime::core::Error::Code::CapabilityDenied);
  }
  WindowInfo unchanged;
  const auto unchanged_info = service.info(id, unchanged);
  assert(unchanged_info.ok());
  assert(unchanged.rect == before_denied.rect);
  assert(unchanged.state == before_denied.state);
  assert(unchanged.visible == before_denied.visible);

  // Malformed payload and unknown placement fail the contract check.
  rime::action::Action bad_payload = move_action;
  bad_payload.id = 2;
  bad_payload.payload = R"({"position":42})";
  const auto bad_payload_result = kernel.execute(bad_payload);
  assert(!bad_payload_result.succeeded);
  assert(bad_payload_result.error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action bad_placement = move_action;
  bad_placement.id = 3;
  bad_placement.payload = R"({"position":"diagonal"})";
  const auto bad_placement_result = kernel.execute(bad_placement);
  assert(!bad_placement_result.succeeded);
  assert(bad_placement_result.error.message.find("unknown window placement") !=
         std::string::npos);

  // The executor dispatches window.close: the pre-close snapshot is the
  // result value and the id goes stale immediately after.
  HWND exec_disposable = nullptr;
  assert(service.ui()
             .call([&] {
               exec_disposable = CreateWindowExW(0, L"STATIC", L"Rime Executor Close",
                                                 WS_OVERLAPPED | WS_VISIBLE, 60, 60, 320, 240,
                                                 nullptr, nullptr, GetModuleHandleW(nullptr),
                                                 nullptr);
               assert(exec_disposable != nullptr);
             })
             .ok());
  std::vector<WindowInfo> exec_close_match;
  rime::win32::WindowQuery exec_close_query;
  exec_close_query.title = "Rime Executor Close";
  assert(service.query(exec_close_query, exec_close_match).ok());
  assert(exec_close_match.size() == 1);
  rime::action::Action close_action = move_action;
  close_action.id = 4;
  close_action.type = "window.close";
  close_action.payload = "{}";
  close_action.target.id = std::to_string(exec_close_match.front().id);
  const auto close_result = kernel.execute(close_action);
  assert(close_result.succeeded);
  assert(close_result.value.is_object());
  assert(close_result.value.find("title") != nullptr);
  WindowInfo close_gone;
  assert(service.info(exec_close_match.front().id, close_gone).code ==
         rime::core::Error::Code::InvalidState);

  // An expired deadline is rejected by the kernel with Timeout.
  rime::action::Action expired = move_action;
  expired.id = 5;
  expired.deadline_unix_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() -
      1000);
  const auto expired_result = kernel.execute(expired);
  assert(!expired_result.succeeded);
  assert(expired_result.error.code == rime::core::Error::Code::Timeout);

  // A slow task occupies the pump; a queued call hits its deadline with the
  // dedicated Timeout code.
  std::thread slow([&] {
    assert(service.ui().call([] { std::this_thread::sleep_for(500ms); }).ok());
  });
  std::this_thread::sleep_for(100ms);
  const auto queued_timeout = service.ui().call([] {}, 20ms);
  assert(!queued_timeout.ok());
  assert(queued_timeout.code == rime::core::Error::Code::Timeout);
  slow.join();

  // Nested calls run inline on the UI thread.
  rime::core::Error nested = {rime::core::Error::Code::InvalidState, "unset"};
  assert(service.ui().call([&] { nested = service.ui().call([] {}); }).ok());
  assert(nested.ok());

  // Destroying the window invalidates its id.
  assert(service.ui().call([&] { DestroyWindow(created); }).ok());
  WindowInfo dead;
  assert(service.info(id, dead).code == rime::core::Error::Code::InvalidState);
  assert(service.move(id, "left").code == rime::core::Error::Code::InvalidState);

  // Stop is repeatable; work afterwards is rejected.
  assert(service.stop().ok());
  assert(service.stop().ok());
  assert(!service.list(windows).ok());

  // The UI lane was released and can be claimed again.
  assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Ui).ok());
  rime::core::LaneRegistry::instance().release(rime::core::Lane::Ui);

  WindowService second;
  assert(second.start().ok());
  assert(second.stop().ok());

  return 0;
}
