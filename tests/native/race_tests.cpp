// Realism: L3 - the real Dispatcher, Kernel and InMemoryTrace run in
// process; only the environment is substituted (a promise/future rendezvous
// the test owns and a ManualClock). Nothing is slept into place: every
// ordering is decided by a gate, and every assertion pins a code value, a
// count or a trace sequence instead of "it did not crash".
//
// Covers the AGENTS cancel-race obligations the engine already exposes:
//   1. cancel vs completion while an executor is inside Kernel::execute
//   2. deadline expiry vs completion (ManualClock advanced concurrently)
//   3. dispatcher close (stop) vs submit
//
// Expected values come from the kernel contract (kernel.cpp: cancellation
// wins over the post-commit deadline recheck; an expired deadline reports
// Timeout), never from logic re-implemented inside this file.

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/cancellation.hpp"
#include "rime/core/clock.hpp"
#include "rime/core/scheduler_policy.hpp"
#include "rime/core/trace.hpp"
#include "rime/core/types.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::action::Action;
using rime::action::DispatchStatus;
using rime::action::Dispatcher;
using rime::action::Executor;
using rime::action::Kernel;
using rime::action::Result;
using rime::action::StaticCapabilityPolicy;
using rime::core::ActionId;
using rime::core::CancellationSource;
using rime::core::CancellationToken;
using rime::core::Error;
using rime::core::InMemoryTrace;
using rime::core::ManualClock;
using rime::core::TraceEntry;
using rime::core::TraceKind;

// Bounded condition wait: a rendezvous that never arrives fails an assert
// with a message instead of parking ctest forever. Never used as a sleep.
constexpr std::chrono::milliseconds kWaitBudget = 10s;

constexpr int kCancelRounds = 60;      // 20 per ordering
constexpr int kDeadlineRounds = 30;    // 10 per ordering

// Test-owned rendezvous for an executor running inside Kernel::execute.
struct Gate {
  std::promise<void> entered;
  std::promise<void> release;
  std::future<void> entered_future;
  std::future<void> release_future;
  std::atomic<int> enter_count{0};
  std::atomic<int> leave_count{0};

  Gate() : entered_future(entered.get_future()), release_future(release.get_future()) {}
};

// Blocks on the gate and always reports success on the way out, so the
// terminal state under test is decided by the kernel's own cancellation /
// deadline recheck (kernel.cpp:129-143), not by the executor. It never
// reads the token itself: a token-aware executor would hide the recheck.
class GateExecutor final : public Executor {
 public:
  explicit GateExecutor(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}

  Result execute(const Action& action, CancellationToken) override {
    gate_->enter_count.fetch_add(1, std::memory_order_acq_rel);
    gate_->entered.set_value();
    gate_->release_future.wait();
    gate_->leave_count.fetch_add(1, std::memory_order_acq_rel);
    return {action.id, true, false, "gate passed", Error::none()};
  }

 private:
  std::shared_ptr<Gate> gate_;
};

class SuccessExecutor final : public Executor {
 public:
  Result execute(const Action& action, CancellationToken token) override {
    if (token.cancelled()) {
      return {action.id, false, true, "cancelled", {Error::Code::Cancelled, "cancelled"}};
    }
    return {action.id, true, false, "accepted", Error::none()};
  }
};

Action make_action(const ActionId id, const std::uint64_t deadline_unix_ms,
                   std::string payload = "{}") {
  Action action;
  action.id = id;
  action.schema_version = 1;
  action.source = {"user", "race-test"};
  action.type = "window.move";
  action.capability = "windows.window.write";
  action.target = {"window", "active"};
  action.deadline_unix_ms = deadline_unix_ms;
  action.payload = std::move(payload);
  return action;
}

std::uint64_t future_deadline_ms() {
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         60'000;
}

// Waits for the executor to reach the gate; ready/timeout only, so a broken
// rendezvous fails the assert instead of hanging.
bool wait_until_entered(const std::shared_ptr<Gate>& gate) {
  return gate->entered_future.wait_for(kWaitBudget) == std::future_status::ready;
}

std::vector<TraceEntry> entries_of(const std::vector<TraceEntry>& entries, const TraceKind kind,
                                   const ActionId id) {
  std::vector<TraceEntry> matches;
  for (const TraceEntry& entry : entries) {
    if (entry.kind == kind && entry.action_id == id) matches.push_back(entry);
  }
  return matches;
}

bool sequences_strictly_increasing(const std::vector<TraceEntry>& entries) {
  for (std::size_t index = 1; index < entries.size(); ++index) {
    if (entries[index].sequence <= entries[index - 1].sequence) return false;
  }
  return true;
}

// One terminal entry per action, matching the Result the caller observed.
void assert_single_terminal(const std::shared_ptr<InMemoryTrace>& trace, const ActionId id,
                            const Result& result) {
  const std::vector<TraceEntry> entries = trace->snapshot();
  assert(!entries.empty());
  assert(sequences_strictly_increasing(entries));
  const std::vector<TraceEntry> started = entries_of(entries, TraceKind::ActionStarted, id);
  const std::vector<TraceEntry> finished = entries_of(entries, TraceKind::ActionFinished, id);
  assert(started.size() == 1);  // the executor really ran
  assert(finished.size() == 1);  // exactly one terminal state, never double-written
  assert(finished[0].result_code ==
         std::string(rime::core::error_code_name(result.error.code)));
}

enum class CancelOrder { CancelFirst, CompleteFirst, CancelRacesCompletion };

// Round 1: an action cancelled while its executor is parked inside the gate.
void run_cancel_round(const CancelOrder order, const ActionId id) {
  auto trace = std::make_shared<InMemoryTrace>();
  // Declared before the kernel: a ManualClock must outlive the components
  // that read it (clock.hpp lifecycle contract).
  ManualClock clock;
  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.window.write"});
  Kernel kernel(policy, trace, &clock);
  auto gate = std::make_shared<Gate>();
  const auto registered = kernel.register_executor("window.move", std::make_shared<GateExecutor>(gate));
  assert(registered.ok());
  Dispatcher dispatcher(kernel, 8);
  CancellationSource source;

  const Action action = make_action(id, static_cast<std::uint64_t>(clock.unix_ms()) + 60'000);
  assert(dispatcher.submit(action) == DispatchStatus::Accepted);

  std::vector<Result> results;
  std::thread pump([&] {
    results = dispatcher.pump(8, [&source](const Action&) { return source.token(); });
  });

  assert(wait_until_entered(gate));  // the executor is inside Kernel::execute

  switch (order) {
    case CancelOrder::CancelFirst:
      // cancel happens-before gate release happens-before the kernel's
      // post-commit check, so Cancelled is the only legal outcome.
      source.cancel();
      gate->release.set_value();
      break;
    case CancelOrder::CompleteFirst:
      gate->release.set_value();
      break;
    case CancelOrder::CancelRacesCompletion: {
      std::thread canceller([&source] { source.cancel(); });
      gate->release.set_value();
      canceller.join();
      break;
    }
  }
  pump.join();
  if (order == CancelOrder::CompleteFirst) {
    // Completion is final before the cancel lands: the late cancel must not
    // rewrite the result or add a second terminal trace entry.
    source.cancel();
  }

  assert(results.size() == 1);
  const Result& result = results.front();
  assert(result.id == id);
  if (order == CancelOrder::CancelFirst) {
    assert(result.cancelled);
    assert(!result.succeeded);
    assert(result.error.code == Error::Code::Cancelled);
  } else if (order == CancelOrder::CompleteFirst) {
    assert(!result.cancelled);
    assert(result.succeeded);
    assert(result.error.ok());
  } else {
    // Racing: either terminal is legal, nothing else is.
    assert(result.cancelled || result.succeeded);
    if (result.cancelled) {
      assert(!result.succeeded);
      assert(result.error.code == Error::Code::Cancelled);
    } else {
      assert(result.error.ok());
    }
  }

  // No leaks: the executor ran exactly once and left exactly once, the queue
  // drained, and the trace still holds one terminal entry after the late
  // cancel above.
  assert(gate->enter_count.load() == 1);
  assert(gate->leave_count.load() == 1);
  assert(dispatcher.size() == 0);
  assert_single_terminal(trace, id, result);
}

enum class DeadlineOrder { BeforeExpiry, AfterExpiry, ExpiryRacesCompletion };

// Round 2: ManualClock pushes the deadline past expiry while the executor is
// parked, so the kernel's post-commit recheck races the clock advance.
void run_deadline_round(const DeadlineOrder order, const ActionId id) {
  auto trace = std::make_shared<InMemoryTrace>();
  ManualClock clock;
  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.window.write"});
  Kernel kernel(policy, trace, &clock);
  auto gate = std::make_shared<Gate>();
  const auto registered = kernel.register_executor("window.move", std::make_shared<GateExecutor>(gate));
  assert(registered.ok());
  Dispatcher dispatcher(kernel, 8);

  const auto base = clock.unix_ms();
  const Action action = make_action(id, static_cast<std::uint64_t>(base) + 1'000);
  assert(dispatcher.submit(action) == DispatchStatus::Accepted);

  std::vector<Result> results;
  std::thread pump([&] { results = dispatcher.pump(8); });

  assert(wait_until_entered(gate));  // pre-dispatch deadline check already passed

  if (order == DeadlineOrder::BeforeExpiry) {
    gate->release.set_value();
  } else if (order == DeadlineOrder::AfterExpiry) {
    // Advance first and finish the advance before the executor may return,
    // so the post-commit recheck must observe an expired deadline.
    clock.advance(2'000ms);
    gate->release.set_value();
  } else {
    std::thread advancer([&clock] { clock.advance(2'000ms); });
    gate->release.set_value();
    advancer.join();
  }
  pump.join();

  assert(results.size() == 1);
  const Result& result = results.front();
  assert(result.id == id);
  const bool reported_ok =
      result.succeeded && result.error.ok() && !result.cancelled;
  const bool reported_timeout = !result.succeeded && !result.cancelled &&
                                result.error.code == Error::Code::Timeout;
  // Exactly one of the two legal outcomes - never both, never a third.
  assert(reported_ok || reported_timeout);
  if (order == DeadlineOrder::BeforeExpiry) assert(reported_ok);
  if (order == DeadlineOrder::AfterExpiry) assert(reported_timeout);

  assert(gate->enter_count.load() == 1);
  assert(gate->leave_count.load() == 1);
  assert(dispatcher.size() == 0);
  assert_single_terminal(trace, id, result);
}

// Round 3: stop (dispatcher close) racing a submitter thread. No ordering is
// assumed; every submission must land in exactly one legal state, and the
// actions accepted before the stop must still produce exactly one Result.
void run_stop_submit_race() {
  auto trace = std::make_shared<InMemoryTrace>();
  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.window.write"});
  Kernel kernel(policy, trace);
  const auto registered = kernel.register_executor("window.move", std::make_shared<SuccessExecutor>());
  assert(registered.ok());

  constexpr int kSubmissions = 200;
  // Capacity above the submission count: a queue_full result would be a real
  // policy outcome, not the stop race under test.
  Dispatcher dispatcher(kernel, 256);
  std::vector<DispatchStatus> statuses(kSubmissions, DispatchStatus::Accepted);
  std::atomic<int> submitted{0};

  std::thread submitter([&] {
    for (int index = 0; index < kSubmissions; ++index) {
      statuses[static_cast<std::size_t>(index)] =
          dispatcher.submit(make_action(3'000 + static_cast<ActionId>(index), future_deadline_ms(),
                                        "p" + std::to_string(index)));
      submitted.fetch_add(1, std::memory_order_release);
    }
  });

  // Bounded condition polling: close only once the submitter has proved it
  // is running, so the stop really races the submissions.
  const auto close_deadline = std::chrono::steady_clock::now() + kWaitBudget;
  while (submitted.load(std::memory_order_acquire) == 0 &&
         std::chrono::steady_clock::now() < close_deadline) {
    std::this_thread::yield();
  }
  assert(submitted.load(std::memory_order_acquire) > 0);
  dispatcher.close();
  submitter.join();

  int accepted = 0;
  int refused = 0;
  for (const DispatchStatus status : statuses) {
    // Never full (capacity), never a crash: stop only accepts or refuses.
    assert(status == DispatchStatus::Accepted || status == DispatchStatus::Closed);
    if (status == DispatchStatus::Accepted) {
      ++accepted;
    } else {
      ++refused;
    }
  }
  assert(accepted + refused == kSubmissions);
  assert(accepted > 0);

  // Deterministic post-condition of a stopped dispatcher.
  assert(dispatcher.closed());
  assert(dispatcher.submit(make_action(9'000, future_deadline_ms(), "late")) ==
         DispatchStatus::Closed);

  // Every accepted action still yields exactly one Result.
  const std::vector<Result> results = dispatcher.pump(256);
  assert(results.size() == static_cast<std::size_t>(accepted));
  for (const Result& result : results) {
    assert(result.succeeded);
    assert(!result.cancelled);
    assert(result.error.ok());
  }
  assert(dispatcher.size() == 0);

  const std::vector<TraceEntry> entries = trace->snapshot();
  assert(sequences_strictly_increasing(entries));
  std::size_t accepted_traces = 0;
  std::size_t refused_traces = 0;
  std::size_t closed_traces = 0;
  for (const TraceEntry& entry : entries) {
    if (entry.kind == TraceKind::ActionAccepted) ++accepted_traces;
    if (entry.kind == TraceKind::ActionRefused) ++refused_traces;
    if (entry.kind == TraceKind::StateChanged && entry.detail == "closed") ++closed_traces;
  }
  assert(accepted_traces == static_cast<std::size_t>(accepted));
  assert(refused_traces == static_cast<std::size_t>(refused) + 1);  // + the late submit
  assert(closed_traces == 1);  // close() traces its single transition once
}

}  // namespace

int main() {
  for (int round = 0; round < kCancelRounds; ++round) {
    run_cancel_round(static_cast<CancelOrder>(round % 3),
                     1'000 + static_cast<ActionId>(round));
  }
  for (int round = 0; round < kDeadlineRounds; ++round) {
    run_deadline_round(static_cast<DeadlineOrder>(round % 3),
                       20'000 + static_cast<ActionId>(round));
  }
  run_stop_submit_race();
  return 0;
}
