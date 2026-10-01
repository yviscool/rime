#include "rime/core/runtime.hpp"
#include "rime/action/kernel.hpp"
#include "rime/action/dispatcher.hpp"
#include "rime/desktop/host.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <unordered_set>

namespace {
class EchoExecutor final : public rime::action::Executor {
 public:
  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken token) override {
    if (token.cancelled()) {
      return {action.id, false, true, "cancelled", {rime::core::Error::Code::Cancelled, "cancelled"}};
    }
    return {action.id, true, false, "moved", rime::core::Error::none()};
  }
};
}

int main() {
  rime::core::EventQueue queue(1);
  assert(queue.push({1, rime::core::EventKind::Input, "one", ""}) ==
         rime::core::QueueStatus::Accepted);
  assert(queue.push({2, rime::core::EventKind::Input, "two", ""}) ==
         rime::core::QueueStatus::Full);
  queue.close();
  assert(queue.push({3, rime::core::EventKind::Input, "three", ""}) ==
         rime::core::QueueStatus::Closed);

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::core::Runtime runtime(4, trace);
  std::string received;
  runtime.set_handler([&](const rime::core::Event& event, rime::core::CancellationToken token) {
    if (!token.cancelled()) received = event.name;
  });
  assert(runtime.start().ok());
  assert(runtime.post({0, rime::core::EventKind::Input, "window.move", "left"}).ok());
  assert(runtime.pump() == 1);
  assert(received == "window.move");
  assert(runtime.stop().ok());
  assert(runtime.post({0, rime::core::EventKind::Input, "late", ""}).code ==
         rime::core::Error::Code::InvalidState);
  assert(!trace->snapshot().empty());

  auto policy = std::make_shared<rime::action::StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"window.write"});
  rime::action::Kernel kernel(policy, trace);
  assert(kernel.register_executor("window.move", std::make_shared<EchoExecutor>()).ok());
  const rime::action::Action action{42,
                                    1,
                                    {"user", "local"},
                                    "window.move",
                                    "window.write",
                                    {"window", "active"},
                                    {},
                                    1,
                                    0,
                                    "{\"position\":\"left\"}",
                                    "move-active-1"};
  const auto success = kernel.execute(action);
  assert(success.succeeded && success.id == 42);
  const auto denied = kernel.execute(
      {43, 1, {"user", "local"}, "window.move", "process.launch", {"window", "active"}, {}, 1,
       0, "{}", ""});
  assert(denied.error.code == rime::core::Error::Code::CapabilityDenied);
  const auto unsupported = kernel.execute(
      {44, 1, {"user", "local"}, "window.close", "window.write", {"window", "active"}, {}, 1,
       0, "{}", ""});
  assert(unsupported.error.code == rime::core::Error::Code::Unsupported);
  rime::core::CancellationSource cancelled;
  cancelled.cancel();
  const auto cancelled_result = kernel.execute(action, cancelled.token());
  assert(cancelled_result.cancelled);
  assert(trace->snapshot().size() >= 3);
  rime::action::Dispatcher dispatcher(kernel, 1);
  assert(dispatcher.submit(action) == rime::action::DispatchStatus::Accepted);
  assert(dispatcher.submit(action) == rime::action::DispatchStatus::Full);
  const auto dispatched = dispatcher.pump(1);
  assert(dispatched.size() == 1 && dispatched.front().succeeded);
  dispatcher.close();
  assert(dispatcher.submit(action) == rime::action::DispatchStatus::Closed);
  rime::core::Runtime host_runtime(2);
  rime::desktop::DesktopHost desktop_host(host_runtime);
  assert(desktop_host.start().ok());
  assert(desktop_host.state() == rime::desktop::HostState::Running);
  assert(desktop_host.stop().ok());
  assert(desktop_host.state() == rime::desktop::HostState::Stopped);
  return 0;
}
