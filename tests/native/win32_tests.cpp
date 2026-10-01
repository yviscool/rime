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

  // focus: foreground-lock denial is acceptable, stale ids are not.
  const auto focus_result = service.focus(id);
  assert(focus_result.ok() ||
         focus_result.code == rime::core::Error::Code::ExecutionFailed);

  // active() resolves to a live, inspectable window.
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
          std::unordered_set<std::string>{"window.write"}),
      trace);
  assert(kernel
             .register_executor("window.move",
                                std::make_shared<rime::win32::WindowExecutor>(service))
             .ok());

  rime::action::Action move_action;
  move_action.id = 1;
  move_action.source = {"test", "win32_tests"};
  move_action.type = "window.move";
  move_action.capability = "window.write";
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

  // Capability denial happens before the executor runs.
  rime::action::Kernel denied(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{}),
      trace);
  assert(denied
             .register_executor("window.move",
                                std::make_shared<rime::win32::WindowExecutor>(service))
             .ok());
  const auto denied_result = denied.execute(move_action);
  assert(!denied_result.succeeded);
  assert(denied_result.error.code == rime::core::Error::Code::CapabilityDenied);
  WindowInfo unchanged;
  assert(service.info(id, unchanged).ok());
  assert(unchanged.rect == left_half);

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

  // A slow task occupies the pump; a queued call hits its deadline.
  std::thread slow([&] {
    assert(service.ui().call([] { std::this_thread::sleep_for(500ms); }).ok());
  });
  std::this_thread::sleep_for(100ms);
  assert(!service.ui().call([] {}, 20ms).ok());
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
