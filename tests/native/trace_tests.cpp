// Realism: L3
//
// Trace envelope contract: queue wait, write timestamps and the event handler
// segment. The units under test are the real Dispatcher + Kernel + Runtime;
// only the clock seam is injected (ManualClock), except for the single case
// that pins the production stamping path - Dispatcher has no clock seam (its
// header is out of scope for this change), so that case polls the real
// system clock with a bounded condition loop instead of sleeping.
//
// Expectations come from injected data (ManualClock::advance deltas, the
// poll threshold) or from OS observation (before/after wall readings), never
// from recomputing the implementation's formula.

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/clock.hpp"
#include "rime/core/runtime.hpp"
#include "rime/core/trace.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using rime::action::Action;
using rime::action::DispatchStatus;
using rime::action::Dispatcher;
using rime::action::Executor;
using rime::action::Kernel;
using rime::action::Result;
using rime::action::StaticCapabilityPolicy;
using rime::core::ActionId;
using rime::core::CancellationToken;
using rime::core::Event;
using rime::core::EventKind;
using rime::core::InMemoryTrace;
using rime::core::ManualClock;
using rime::core::SchedulerPolicy;
using rime::core::SystemClock;
using rime::core::TraceEntry;
using rime::core::TraceKind;

class EchoExecutor final : public Executor {
 public:
  Result execute(const Action& action, CancellationToken token) override {
    if (token.cancelled()) {
      return {action.id, false, true, "cancelled",
              {rime::core::Error::Code::Cancelled, "cancelled"}};
    }
    return {action.id, true, false, "moved", rime::core::Error::none()};
  }
};

// Aggregated init stops before accepted_unix_ms on purpose: production
// actions arrive with 0 so Dispatcher::submit stamps them.
Action make_action(const ActionId id) {
  return Action{id,
                1,
                {"user", "local"},
                "window.move",
                "windows.window.write",
                {"window", "active"},
                {},
                4102444800000ULL,
                0,
                "{\"position\":\"left\"}",
                ""};
}

const TraceEntry* find_entry(const std::vector<TraceEntry>& entries, const TraceKind kind,
                             const ActionId id) {
  for (const TraceEntry& entry : entries) {
    if (entry.kind == kind && entry.action_id == id) return &entry;
  }
  return nullptr;
}

std::size_t count_entries(const std::vector<TraceEntry>& entries, const TraceKind kind,
                          const std::string& subject) {
  std::size_t count = 0;
  for (const TraceEntry& entry : entries) {
    if (entry.kind == kind && entry.subject == subject) ++count;
  }
  return count;
}

}  // namespace

int main() {
  // Clock first: ManualClock must outlive every component that reads it.
  ManualClock clock;
  auto trace = std::make_shared<InMemoryTrace>();
  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.window.write"});

  Kernel kernel(policy, trace, &clock);
  assert(kernel.register_executor("window.move", std::make_shared<EchoExecutor>()).ok());
  Dispatcher dispatcher(kernel, SchedulerPolicy::bounded(4));

  // --- queue_wait_ms: injected accept time -> exact value ----------------
  {
    Action queued = make_action(1);
    const std::int64_t accepted_at = clock.unix_ms();
    queued.accepted_unix_ms = static_cast<std::uint64_t>(accepted_at);
    assert(dispatcher.submit(queued) == DispatchStatus::Accepted);
    // 75 ms of queue dwell, injected through the clock under test; the
    // expected value is this literal, not a value read back from the code.
    clock.advance(std::chrono::milliseconds(75));
    assert(dispatcher.pump(1).size() == 1);

    const std::vector<TraceEntry> entries = trace->snapshot();
    const TraceEntry* accepted = find_entry(entries, TraceKind::ActionAccepted, 1);
    assert(accepted != nullptr);
    assert(accepted->queue_wait_ms == 0);
    assert(accepted->duration_ms == 0);
    assert(accepted->action_id == 1);
    assert(accepted->capability == "windows.window.write");
    assert(accepted->result_code.empty());

    const TraceEntry* started = find_entry(entries, TraceKind::ActionStarted, 1);
    assert(started != nullptr);
    assert(started->queue_wait_ms == 75);
    assert(started->duration_ms == 0);
    assert(started->result_code.empty());
    assert(started->capability == "windows.window.write");

    const TraceEntry* finished = find_entry(entries, TraceKind::ActionFinished, 1);
    assert(finished != nullptr);
    assert(finished->queue_wait_ms == 0);
    assert(finished->result_code == "none");
    assert(finished->sequence > started->sequence);
    assert(started->sequence > accepted->sequence);
    // ManualClock does not move while the echo executor runs.
    assert(finished->duration_ms == 0);
  }

  // --- queue_wait_ms stays 0 when no Dispatcher stamped the action --------
  {
    Action direct = make_action(2);
    assert(direct.accepted_unix_ms == 0);
    const Result result = kernel.execute(direct);
    assert(result.succeeded);
    const std::vector<TraceEntry> entries = trace->snapshot();
    const TraceEntry* started = find_entry(entries, TraceKind::ActionStarted, 2);
    assert(started != nullptr);
    assert(started->queue_wait_ms == 0);
    const TraceEntry* finished = find_entry(entries, TraceKind::ActionFinished, 2);
    assert(finished != nullptr);
    assert(finished->queue_wait_ms == 0);

    // Domain guard: the production Dispatcher stamps the real system clock
    // while this Kernel reads ManualClock. The delta would be negative, so
    // the kernel clamps to 0 instead of wrapping around.
    Dispatcher manual_dispatcher(kernel, SchedulerPolicy::bounded(4));
    assert(manual_dispatcher.submit(make_action(3)) == DispatchStatus::Accepted);
    assert(manual_dispatcher.pump(1).size() == 1);
    const std::vector<TraceEntry> mismatch_entries = trace->snapshot();
    const TraceEntry* mismatched = find_entry(mismatch_entries, TraceKind::ActionStarted, 3);
    assert(mismatched != nullptr);
    assert(mismatched->queue_wait_ms == 0);
  }

  // --- refused/queued/state entries never carry a queue wait -------------
  {
    Dispatcher bounded(kernel, 1);
    assert(bounded.submit(make_action(11)) == DispatchStatus::Accepted);
    assert(bounded.submit(make_action(12)) == DispatchStatus::Full);
    bounded.close();
    assert(bounded.pump(4).size() == 1);

    const std::vector<TraceEntry> entries = trace->snapshot();
    const TraceEntry* refused = find_entry(entries, TraceKind::ActionRefused, 12);
    assert(refused != nullptr);
    assert(refused->queue_wait_ms == 0);
    assert(refused->duration_ms == 0);
    assert(refused->result_code == "queue_full");
    bool closed_state_seen = false;
    for (const TraceEntry& entry : entries) {
      if (entry.kind == TraceKind::StateChanged && entry.subject == "dispatcher") {
        assert(entry.detail == "closed");
        assert(entry.queue_wait_ms == 0);
        assert(entry.duration_ms == 0);
        closed_state_seen = true;
      }
    }
    assert(closed_state_seen);
  }

  // --- production stamping path: Dispatcher stamps an unset accept time ---
  {
    auto system_trace = std::make_shared<InMemoryTrace>();
    Kernel system_kernel(policy, system_trace);
    assert(system_kernel.register_executor("window.move", std::make_shared<EchoExecutor>()).ok());
    Dispatcher system_dispatcher(system_kernel, SchedulerPolicy::bounded(4));

    Action action = make_action(10);
    assert(action.accepted_unix_ms == 0);
    const std::int64_t before_submit = SystemClock::instance().unix_ms();
    assert(system_dispatcher.submit(action) == DispatchStatus::Accepted);
    const std::int64_t after_submit = SystemClock::instance().unix_ms();

    // Bounded condition poll on the real clock (the only real-clock wait in
    // this file: Dispatcher has no Clock seam). Escape hatch instead of an
    // unbounded spin, so a stalled clock fails instead of hanging.
    bool observed = false;
    const auto poll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!observed) {
      if (SystemClock::instance().unix_ms() - after_submit >= 5) {
        observed = true;
      } else if (std::chrono::steady_clock::now() > poll_deadline) {
        break;
      } else {
        std::this_thread::yield();
      }
    }
    assert(observed && "system clock did not advance 5 ms within 2 s");
    assert(system_dispatcher.pump(1).size() == 1);
    const std::int64_t after_pump = SystemClock::instance().unix_ms();

    const std::vector<TraceEntry> entries = system_trace->snapshot();
    const TraceEntry* started = find_entry(entries, TraceKind::ActionStarted, 10);
    assert(started != nullptr);
    // accepted <= after_submit, execution began >= after_submit + 5 ms.
    assert(started->queue_wait_ms >= 5);
    assert(started->queue_wait_ms <=
           static_cast<std::uint64_t>(after_pump - before_submit));
  }

  // --- unix_ms: stamped on every entry, monotone with sequence -----------
  {
    const std::vector<TraceEntry> entries = trace->snapshot();
    assert(entries.size() >= 9);
    for (std::size_t index = 0; index < entries.size(); ++index) {
      assert(entries[index].unix_ms > 0);
      if (index > 0) {
        assert(entries[index].sequence > entries[index - 1].sequence);
        assert(entries[index].unix_ms >= entries[index - 1].unix_ms);
      }
    }
  }

  // --- event handler segment: Started/Finished pair with injected length --
  {
    // Declaration order again: the ManualClock outlives the Runtime.
    ManualClock event_clock;
    auto event_trace = std::make_shared<InMemoryTrace>();
    rime::core::Runtime runtime(8, event_trace, &event_clock);
    runtime.set_handler([&](const Event& event, CancellationToken) {
      if (event.name == "handler.segment") {
        event_clock.advance(std::chrono::milliseconds(30));
      }
    });
    assert(runtime.start().ok());
    assert(runtime.post({0, EventKind::Input, "handler.segment", "", ""}).ok());
    assert(runtime.post({0, EventKind::Input, "handler.quick", "", ""}).ok());
    assert(runtime.pump(2) == 2);
    assert(runtime.stop().ok());

    const std::vector<TraceEntry> entries = event_trace->snapshot();
    assert(count_entries(entries, TraceKind::EventDispatchStarted, "handler.segment") == 1);
    assert(count_entries(entries, TraceKind::EventDispatchFinished, "handler.segment") == 1);
    assert(count_entries(entries, TraceKind::EventDispatchStarted, "handler.quick") == 1);
    assert(count_entries(entries, TraceKind::EventDispatchFinished, "handler.quick") == 1);

    const TraceEntry* segment_finished = nullptr;
    const TraceEntry* segment_started = nullptr;
    const TraceEntry* quick_finished = nullptr;
    for (const TraceEntry& entry : entries) {
      if (entry.kind == TraceKind::EventDispatchFinished && entry.subject == "handler.segment") {
        segment_finished = &entry;
      } else if (entry.kind == TraceKind::EventDispatchStarted &&
                 entry.subject == "handler.segment") {
        segment_started = &entry;
      } else if (entry.kind == TraceKind::EventDispatchFinished &&
                 entry.subject == "handler.quick") {
        quick_finished = &entry;
      }
    }
    assert(segment_finished != nullptr && segment_started != nullptr);
    assert(segment_finished->duration_ms == 30);
    assert(segment_started->duration_ms == 0);
    assert(segment_finished->sequence > segment_started->sequence);
    assert(segment_finished->unix_ms > 0);
    assert(segment_started->unix_ms > 0);
    // Events are not actions: no action envelope, no queue wait.
    assert(segment_finished->action_id == 0);
    assert(segment_finished->queue_wait_ms == 0);
    assert(segment_finished->capability.empty());
    assert(segment_finished->result_code.empty());
    // The event that did not move the clock reports a zero-length segment.
    assert(quick_finished != nullptr);
    assert(quick_finished->duration_ms == 0);

    // Nothing outside EventDispatchFinished carries a duration here.
    for (const TraceEntry& entry : entries) {
      if (entry.kind == TraceKind::EventDispatchFinished && entry.subject == "handler.segment") {
        continue;
      }
      if (entry.kind == TraceKind::EventDispatchFinished && entry.subject == "handler.quick") {
        continue;
      }
      assert(entry.duration_ms == 0);
      assert(entry.queue_wait_ms == 0);
    }
  }

  return 0;
}
