#include "rime/core/runtime.hpp"
#include "rime/action/kernel.hpp"
#include "rime/action/dispatcher.hpp"
#include "rime/desktop/host.hpp"

#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_set>

namespace {

// Actions in tests carry a live deadline; the kernel rejects expired ones
// with Timeout before reaching the executor.
std::uint64_t future_deadline() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      60'000);
}

std::uint64_t expired_deadline() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() -
      1'000);
}

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
  assert(queue.push({1, rime::core::EventKind::Input, "one", "", ""}) ==
         rime::core::QueueStatus::Accepted);
  assert(queue.push({2, rime::core::EventKind::Input, "two", "", ""}) ==
         rime::core::QueueStatus::Full);
  queue.close();
  assert(queue.push({3, rime::core::EventKind::Input, "three", "", ""}) ==
         rime::core::QueueStatus::Closed);

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::core::Runtime runtime(4, trace);
  std::string received;
  runtime.set_handler([&](const rime::core::Event& event, rime::core::CancellationToken token) {
    if (!token.cancelled()) received = event.name;
  });
  assert(runtime.start().ok());
  assert(runtime.post({0, rime::core::EventKind::Input, "window.move", "left", ""}).ok());
  assert(runtime.pump() == 1);
  assert(received == "window.move");
  assert(runtime.stop().ok());
  assert(runtime.post({0, rime::core::EventKind::Input, "late", "", ""}).code ==
         rime::core::Error::Code::InvalidState);
  assert(!trace->snapshot().empty());

  auto policy = std::make_shared<rime::action::StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.window.write"});
  rime::action::Kernel kernel(policy, trace);
  assert(kernel.register_executor("window.move", std::make_shared<EchoExecutor>()).ok());
  const rime::action::Action action{42,
                                    1,
                                    {"user", "local"},
                                    "window.move",
                                    "windows.window.write",
                                    {"window", "active"},
                                    {},
                                    future_deadline(),
                                    0,
                                    "{\"position\":\"left\"}",
                                    "move-active-1"};
  const auto success = kernel.execute(action);
  assert(success.succeeded && success.id == 42);
  const auto denied = kernel.execute(
      {43, 1, {"user", "local"}, "window.move", "process.launch", {"window", "active"}, {},
       future_deadline(), 0, "{}", ""});
  assert(denied.error.code == rime::core::Error::Code::CapabilityDenied);
  const auto unsupported = kernel.execute(
      {44, 1, {"user", "local"}, "window.close", "windows.window.write", {"window", "active"}, {},
       future_deadline(), 0, "{}", ""});
  assert(unsupported.error.code == rime::core::Error::Code::Unsupported);
  const auto expired = kernel.execute(
      {45, 1, {"user", "local"}, "window.move", "windows.window.write", {"window", "active"}, {},
       expired_deadline(), 0, "{}", ""});
  assert(expired.error.code == rime::core::Error::Code::Timeout);
  // A declared precondition is refused, not silently ignored: the kernel has
  // no evaluator vocabulary, so honoring it is impossible and skipping it
  // would break the caller's contract.
  const auto preconditioned = kernel.execute(
      {46, 1, {"user", "local"}, "window.move", "windows.window.write", {"window", "active"},
       {{"window.foreground", "true"}}, future_deadline(), 0, "{}", ""});
  assert(preconditioned.error.code == rime::core::Error::Code::Unsupported);
  assert(preconditioned.error.message.find("window.foreground") != std::string::npos);
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

  // Coalescing decision: a same-idempotency-key mutation submitted while the
  // predecessor is still queued supersedes it deterministically: the loser
  // is refused as cancelled with the supersede detail, exactly one Result is
  // produced per submit, and only the winner reaches the executor.
  rime::action::Kernel coalesce_kernel(policy, trace);
  assert(coalesce_kernel.register_executor("window.move", std::make_shared<EchoExecutor>()).ok());
  rime::action::Dispatcher coalescing(coalesce_kernel,
                                      rime::core::SchedulerPolicy::coalescing(8));
  rime::action::Action successor = action;
  successor.id = 143;
  assert(coalescing.submit(action) == rime::action::DispatchStatus::Accepted);
  assert(coalescing.submit(successor) == rime::action::DispatchStatus::Coalesced);
  assert(coalescing.size() == 1);
  const auto coalesced_results = coalescing.pump(1);
  assert(coalesced_results.size() == 2);
  bool winner_ran = false;
  bool loser_superseded = false;
  for (const auto& result : coalesced_results) {
    if (result.id == 143 && result.succeeded) winner_ran = true;
    if (result.id == 42 && result.cancelled && result.detail == "superseded by newer action") {
      loser_superseded = true;
    }
  }
  assert(winner_ran && loser_superseded);

  // Trace integrity: entries from runtime + kernel sharing one sink are
  // strictly ordered by the global sequence, every ActionStarted pairs with
  // a later ActionFinished for the same action id, and non-action entries
  // never carry an action id.
  const auto entries = trace->snapshot();
  assert(!entries.empty());
  for (std::size_t index = 1; index < entries.size(); ++index) {
    assert(entries[index].sequence > entries[index - 1].sequence);
  }
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& entry = entries[index];
    if (entry.kind == rime::core::TraceKind::ActionStarted) {
      assert(entry.action_id != 0);
      // Envelope: Started carries the capability, but no result yet.
      assert(!entry.capability.empty());
      assert(entry.result_code.empty());
      assert(entry.duration_ms == 0);
      bool paired = false;
      for (std::size_t later = index + 1; later < entries.size(); ++later) {
        if (entries[later].action_id == entry.action_id &&
            entries[later].kind == rime::core::TraceKind::ActionFinished) {
          paired = true;
          break;
        }
      }
      assert(paired);
    } else if (entry.kind == rime::core::TraceKind::ActionFinished) {
      assert(entry.action_id != 0);
      // Envelope: Finished always names the capability, the contract result
      // code, and a measured executor duration (0 only if sub-millisecond).
      assert(!entry.capability.empty());
      assert(!entry.result_code.empty());
      assert(entry.duration_ms < 60'000);
    } else if (entry.kind == rime::core::TraceKind::ActionAccepted) {
      assert(entry.action_id != 0);
      // Envelope: queued decisions name the capability but have no result
      // (the action has not run) and no duration.
      assert(!entry.capability.empty());
      assert(entry.result_code.empty());
      assert(entry.duration_ms == 0);
    } else if (entry.kind == rime::core::TraceKind::ActionRefused) {
      assert(entry.action_id != 0);
      // Envelope: refused decisions never reach an executor, so they carry
      // the capability and contract result code with a zero duration.
      assert(!entry.capability.empty());
      assert(!entry.result_code.empty());
      assert(entry.duration_ms == 0);
    } else {
      assert(entry.action_id == 0);
      // Envelope is action-only: event/state entries never carry it.
      assert(entry.capability.empty());
      assert(entry.result_code.empty());
      assert(entry.duration_ms == 0);
    }
  }
  // Result codes reflect the pipeline outcome, not just the executor's.
  auto result_code_for = [&](rime::core::ActionId id) {
    for (const auto& entry : entries) {
      if (entry.action_id == id && entry.kind == rime::core::TraceKind::ActionFinished) {
        return entry.result_code;
      }
    }
    return std::string{};
  };
  assert(result_code_for(42) == "none");
  assert(result_code_for(43) == "capability_denied");
  assert(result_code_for(44) == "unsupported");
  assert(result_code_for(45) == "timeout");
  assert(result_code_for(46) == "unsupported");

  rime::core::Runtime host_runtime(2);
  rime::desktop::DesktopHost desktop_host(host_runtime);
  assert(desktop_host.start().ok());
  assert(desktop_host.state() == rime::desktop::HostState::Running);
  assert(desktop_host.stop().ok());
  assert(desktop_host.state() == rime::desktop::HostState::Stopped);
  return 0;
}
