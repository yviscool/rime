#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/event_queue.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/runtime.hpp"
#include "rime/core/shutdown.hpp"
#include "rime/core/worker.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using rime::core::Delivery;
using rime::core::Event;
using rime::core::EventKind;
using rime::core::EventQueue;
using rime::core::OverflowPolicy;
using rime::core::QueueStatus;
using rime::core::SchedulerPolicy;

class EchoExecutor final : public rime::action::Executor {
 public:
  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken token) override {
    if (token.cancelled()) {
      return {action.id, false, true, "cancelled",
              {rime::core::Error::Code::Cancelled, "cancelled"}};
    }
    return {action.id, true, false, "done", rime::core::Error::none()};
  }
};

rime::action::Action make_action(rime::core::ActionId id, std::string idempotency_key = "") {
  return rime::action::Action{id,
                              1,
                              {"user", "local"},
                              "window.move",
                              "windows.window.write",
                              {"window", "active"},
                              {},
                              4102444800000ULL,
                              0,
                              "{\"position\":\"left\"}",
                              std::move(idempotency_key)};
}

}  // namespace

int main() {
  // --- Overflow policies -------------------------------------------------
  {
    EventQueue reject_queue(1);
    assert(reject_queue.push({1, EventKind::Input, "one", "", ""}) == QueueStatus::Accepted);
    assert(reject_queue.push({2, EventKind::Input, "two", "", ""}) == QueueStatus::Full);
    assert(reject_queue.size() == 1);
    reject_queue.close();
    assert(reject_queue.push({3, EventKind::Input, "three", "", ""}) == QueueStatus::Closed);
  }
  {
    EventQueue dropping_queue(SchedulerPolicy::dropping(1));
    assert(dropping_queue.push({1, EventKind::Input, "one", "a", ""}) == QueueStatus::Accepted);
    assert(dropping_queue.push({2, EventKind::Input, "two", "b", ""}) == QueueStatus::Accepted);
    assert(dropping_queue.dropped() == 1);
    assert(dropping_queue.size() == 1);
    const auto kept = dropping_queue.try_pop();
    assert(kept && kept->name == "two" && kept->payload == "b");
  }
  {
    EventQueue coalescing_queue(SchedulerPolicy::coalescing(4));
    assert(coalescing_queue.push({7, EventKind::Input, "first", "a", "win:1"}) ==
           QueueStatus::Accepted);
    assert(coalescing_queue.push({8, EventKind::Input, "second", "b", "win:1"}) ==
           QueueStatus::Coalesced);
    assert(coalescing_queue.size() == 1);
    const auto merged = coalescing_queue.try_pop();
    assert(merged && merged->name == "second" && merged->payload == "b");
    assert(merged->sequence == 7);
    assert(coalescing_queue.push({9, EventKind::Input, "other", "", "win:2"}) ==
           QueueStatus::Accepted);
    assert(coalescing_queue.size() == 1);
  }
  {
    EventQueue dedupe_queue(SchedulerPolicy::coalescing(4, 60000));
    assert(dedupe_queue.push({1, EventKind::Input, "n", "a", "key"}) == QueueStatus::Accepted);
    assert(dedupe_queue.push({2, EventKind::Input, "n", "b", "key"}) == QueueStatus::Deduped);
    assert(dedupe_queue.push({3, EventKind::Input, "n", "b", "other"}) == QueueStatus::Accepted);
    assert(dedupe_queue.size() == 2);
  }

  // --- M2-D dispatch decisions ------------------------------------------
  // The instruction-level knobs (#MaxThreads, #MaxThreadsPerHotkey,
  // #Suspend, #InputLevel, #HotIfTimeout, #MaxThreadsBuffer) resolve to
  // these pure decisions; module code must consult them instead of
  // inventing its own load rules.
  {
    // #MaxThreads: 0 = unlimited, otherwise a hard cap on in-flight work.
    SchedulerPolicy policy = SchedulerPolicy::events();
    assert(policy.admits_total(0));
    assert(policy.admits_total(99999));
    policy.max_concurrency = 3;
    assert(policy.admits_total(0));
    assert(policy.admits_total(2));
    assert(!policy.admits_total(3));
    assert(!policy.admits_total(4));

    // #MaxThreadsPerHotkey: cap 0 = unlimited, cap n refuses delivery n+1.
    assert(policy.admit_subscription(0, 0) == Delivery::Deliver);
    assert(policy.admit_subscription(7, 0) == Delivery::Deliver);
    assert(policy.admit_subscription(0, 1) == Delivery::Deliver);
    assert(policy.admit_subscription(1, 1) == Delivery::Drop);
    assert(policy.admit_subscription(3, 4) == Delivery::Deliver);
    assert(policy.admit_subscription(4, 4) == Delivery::Drop);

    // Repeat while in flight: coalescing policies buffer (#MaxThreadsBuffer
    // keeps one pending item), rejecting policies drop it.
    assert(policy.admit_repeat(false) == Delivery::Deliver);
    assert(policy.admit_repeat(true) == Delivery::Buffer);
    SchedulerPolicy rejecting;
    assert(rejecting.overflow == OverflowPolicy::Reject);
    assert(rejecting.admit_repeat(true) == Delivery::Drop);
    assert(rejecting.admit_repeat(false) == Delivery::Deliver);

    // #Suspend: only exempt registrations still match.
    assert(SchedulerPolicy::dispatch_allowed(false, false));
    assert(SchedulerPolicy::dispatch_allowed(false, true));
    assert(!SchedulerPolicy::dispatch_allowed(true, false));
    assert(SchedulerPolicy::dispatch_allowed(true, true));

    // #InputLevel: the event's injection level must reach the registration.
    assert(SchedulerPolicy::level_allowed(0, 0));
    assert(SchedulerPolicy::level_allowed(1, 1));
    assert(SchedulerPolicy::level_allowed(2, 3));
    assert(!SchedulerPolicy::level_allowed(1, 0));
    assert(!SchedulerPolicy::level_allowed(2, 1));

    // #HotIfTimeout: within budget is met, over budget fails closed, 0 off.
    SchedulerPolicy timed;
    assert(timed.hot_if_met(0));
    assert(timed.hot_if_met(1000000000000ULL));
    timed.hot_if_timeout_ms = 1000;
    assert(timed.hot_if_met(0));
    assert(timed.hot_if_met(999));
    assert(timed.hot_if_met(1000));
    assert(!timed.hot_if_met(1001));

    // events() defaults mirror the instruction defaults (AHK semantics).
    SchedulerPolicy defaults = SchedulerPolicy::events();
    assert(defaults.capacity == 64);
    assert(defaults.overflow == OverflowPolicy::CoalesceByKey);
    assert(defaults.max_concurrency == 0);
    assert(defaults.max_concurrency_per_subscription == 1);
    assert(defaults.input_level == 0);
    assert(defaults.hot_if_timeout_ms == 1000);
  }

  // --- Nested pump keeps one scheduler and FIFO order --------------------
  {
    auto trace = std::make_shared<rime::core::InMemoryTrace>();
    rime::core::Runtime runtime(8, trace);
    std::vector<std::string> order;
    bool stop_from_handler_denied = false;
    bool cross_thread_pump_rejected = false;
    runtime.set_handler([&](const Event& event, rime::core::CancellationToken) {
      order.push_back("start:" + event.name);
      if (event.name == "first") {
        assert(runtime.stop().code == rime::core::Error::Code::InvalidState);
        stop_from_handler_denied = true;
        assert(runtime.post({0, EventKind::Input, "second", "", ""}).ok());
        std::thread outsider([&] { cross_thread_pump_rejected = runtime.pump() == 0; });
        outsider.join();
        assert(runtime.pump() == 1);
        order.push_back("nested-done");
      }
      order.push_back("end:" + event.name);
    });
    assert(runtime.start().ok());
    assert(runtime.post({0, EventKind::Input, "first", "", ""}).ok());
    assert(runtime.pump() == 1);
    assert(order.size() == 5);
    assert(order[0] == "start:first");
    assert(order[1] == "start:second");
    assert(order[2] == "end:second");
    assert(order[3] == "nested-done");
    assert(order[4] == "end:first");
    assert(stop_from_handler_denied);
    assert(cross_thread_pump_rejected);
    assert(runtime.stop().ok());
    assert(trace->snapshot().size() >= 6);
  }

  // --- Lane ownership ----------------------------------------------------
  {
    rime::core::LaneRegistry::instance().reset();
    assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Js).ok());
    assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Js).ok());
    assert(rime::core::require_lane(rime::core::Lane::Js).ok());
    bool foreign_claim_failed = false;
    std::thread outsider([&] {
      const auto result = rime::core::LaneRegistry::instance().claim(rime::core::Lane::Js);
      foreign_claim_failed = result.code == rime::core::Error::Code::InvalidState;
      assert(rime::core::require_lane(rime::core::Lane::Js).code ==
             rime::core::Error::Code::InvalidState);
    });
    outsider.join();
    assert(foreign_claim_failed);
    rime::core::LaneRegistry::instance().release(rime::core::Lane::Js);
    assert(rime::core::require_lane(rime::core::Lane::Js).code ==
           rime::core::Error::Code::InvalidState);
    assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Ui).ok());
    rime::core::LaneRegistry::instance().reset();
  }

  // --- Worker lane service ----------------------------------------------
  {
    auto& worker = rime::core::WorkerService::instance();
    assert(!worker.running());
    assert(worker.start().ok());
    assert(worker.start().ok());  // idempotent while running
    assert(worker.running());
    assert(worker.pending() == 0);

    // Posted work observes the worker lane claim.
    std::promise<bool> lane_ok;
    auto lane_future = lane_ok.get_future();
    assert(worker.post([&lane_ok] {
      lane_ok.set_value(rime::core::require_lane(rime::core::Lane::Worker).ok());
    }));
    assert(lane_future.get());

    // stop() while a task runs drops the queued tail instead of executing it.
    std::promise<void> gate;
    std::promise<void> started;
    auto started_future = started.get_future();
    std::atomic<int> ran{0};
    assert(worker.post([&] {
      started.set_value();
      gate.get_future().wait();
    }));
    started_future.get();  // first task holds the worker
    assert(worker.post([&] { ran.fetch_add(1); }));
    std::thread releaser([&gate] {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      gate.set_value();
    });
    worker.stop();  // rejects new posts, drops the queue, joins the running task
    releaser.join();
    assert(!worker.running());
    assert(ran.load() == 0);  // queued tail was dropped, not executed
    assert(!worker.post([] {}));  // stopped worker rejects new posts
    worker.stop();  // idempotent
  }

  // --- Shutdown sequence -------------------------------------------------
  {
    auto trace = std::make_shared<rime::core::InMemoryTrace>();
    rime::core::ShutdownSequence shutdown(trace);
    assert(shutdown.phase() == rime::core::ShutdownPhase::Running);
    assert(shutdown.advance(rime::core::ShutdownPhase::RejectingNewWork).ok());
    assert(shutdown.advance(rime::core::ShutdownPhase::RejectingNewWork).ok());
    assert(shutdown.advance(rime::core::ShutdownPhase::Running).code ==
           rime::core::Error::Code::InvalidState);
    assert(shutdown.advance(rime::core::ShutdownPhase::CancellingTasks).ok());
    assert(shutdown.retain("hook:keyboard").ok());
    assert(shutdown.retain("hook:keyboard").code == rime::core::Error::Code::InvalidState);
    assert(!shutdown.idle() && shutdown.outstanding().size() == 1);
    assert(shutdown.release("hook:keyboard").ok());
    assert(shutdown.release("hook:keyboard").code == rime::core::Error::Code::InvalidState);
    assert(shutdown.idle());
    assert(shutdown.advance(rime::core::ShutdownPhase::UninstallingCallbacks).ok());
    assert(shutdown.advance(rime::core::ShutdownPhase::StoppingWorkers).ok());
    assert(shutdown.advance(rime::core::ShutdownPhase::DestroyingUi).ok());
    assert(shutdown.advance(rime::core::ShutdownPhase::ClosingJs).ok());
    assert(shutdown.advance(rime::core::ShutdownPhase::Completed).ok());
    assert(shutdown.completed());
    assert(shutdown.advance(rime::core::ShutdownPhase::StoppingWorkers).code ==
           rime::core::Error::Code::InvalidState);
    assert(shutdown.retain("late").code == rime::core::Error::Code::InvalidState);
    // Shutdown entries share the global sequence and stay strictly ordered.
    const auto shutdown_trace = trace->snapshot();
    assert(!shutdown_trace.empty());
    for (std::size_t index = 1; index < shutdown_trace.size(); ++index) {
      assert(shutdown_trace[index].sequence > shutdown_trace[index - 1].sequence);
      assert(shutdown_trace[index].action_id == 0);
    }

    rime::core::ShutdownSequence failed;
    assert(failed.fail("boom").ok());
    assert(failed.failed());
    assert(failed.advance(rime::core::ShutdownPhase::RejectingNewWork).code ==
           rime::core::Error::Code::InvalidState);
    assert(failed.fail("again").ok());
  }

  // --- Dispatcher policies always produce exactly one result -------------
  {
    auto policy = std::make_shared<rime::action::StaticCapabilityPolicy>(
        std::unordered_set<std::string>{"windows.window.write"});
    auto trace = std::make_shared<rime::core::InMemoryTrace>();
    rime::action::Kernel kernel(policy, trace);
    assert(kernel.register_executor("window.move", std::make_shared<EchoExecutor>()).ok());

    rime::action::Dispatcher coalescing(kernel, SchedulerPolicy::coalescing(4));
    assert(coalescing.submit(make_action(1)) == rime::action::DispatchStatus::Accepted);
    assert(coalescing.submit(make_action(2)) == rime::action::DispatchStatus::Coalesced);
    assert(coalescing.size() == 1);
    auto coalesced_results = coalescing.pump(8);
    assert(coalesced_results.size() == 2);
    const std::size_t succeeded =
        static_cast<std::size_t>(std::count_if(coalesced_results.begin(), coalesced_results.end(),
                                               [](const rime::action::Result& result) {
                                                 return result.succeeded;
                                               }));
    const std::size_t cancelled =
        static_cast<std::size_t>(std::count_if(coalesced_results.begin(), coalesced_results.end(),
                                               [](const rime::action::Result& result) {
                                                 return result.cancelled;
                                              }));
    assert(succeeded == 1 && cancelled == 1);

    rime::action::Dispatcher dropping(kernel, SchedulerPolicy::dropping(1));
    assert(dropping.submit(make_action(3)) == rime::action::DispatchStatus::Accepted);
    assert(dropping.submit(make_action(4)) == rime::action::DispatchStatus::Accepted);
    assert(dropping.dropped() == 1);
    auto dropped_results = dropping.pump(8);
    assert(dropped_results.size() == 2);

    rime::action::Dispatcher bounded(kernel, 1);
    assert(bounded.submit(make_action(5)) == rime::action::DispatchStatus::Accepted);
    assert(bounded.submit(make_action(6)) == rime::action::DispatchStatus::Full);
    bounded.close();
    assert(bounded.submit(make_action(7)) == rime::action::DispatchStatus::Closed);

    // Every queue decision lands in the shared sink: queued actions are
    // Accepted, superseded/dropped/rejected/closed ones are Refused with the
    // contract result code, and close() flips the dispatcher's traced state.
    const auto dispatch_trace = trace->snapshot();
    const auto trace_has = [&](const rime::core::TraceKind kind, const rime::core::ActionId id,
                               const std::string& detail) {
      return std::any_of(dispatch_trace.begin(), dispatch_trace.end(),
                         [&](const rime::core::TraceEntry& entry) {
                           return entry.kind == kind && entry.action_id == id &&
                                  entry.detail.find(detail) != std::string::npos;
                         });
    };
    assert(trace_has(rime::core::TraceKind::ActionAccepted, 1, "queued"));
    assert(trace_has(rime::core::TraceKind::ActionRefused, 1, "superseded by newer action"));
    assert(trace_has(rime::core::TraceKind::ActionAccepted, 2, "queued"));
    assert(trace_has(rime::core::TraceKind::ActionRefused, 3, "dropped by queue policy"));
    assert(trace_has(rime::core::TraceKind::ActionAccepted, 4, "queued"));
    assert(trace_has(rime::core::TraceKind::ActionAccepted, 5, "queued"));
    assert(trace_has(rime::core::TraceKind::ActionRefused, 6, "queue full"));
    assert(trace_has(rime::core::TraceKind::ActionRefused, 7, "dispatcher closed"));
    const auto refused_full =
        std::find_if(dispatch_trace.begin(), dispatch_trace.end(),
                     [](const rime::core::TraceEntry& entry) {
                       return entry.kind == rime::core::TraceKind::ActionRefused &&
                              entry.action_id == 6;
                     });
    assert(refused_full != dispatch_trace.end());
    assert(refused_full->result_code == "queue_full");
    assert(std::any_of(dispatch_trace.begin(), dispatch_trace.end(),
                       [](const rime::core::TraceEntry& entry) {
                         return entry.kind == rime::core::TraceKind::StateChanged &&
                                entry.subject == "dispatcher" && entry.detail == "closed";
                       }));
  }

  return 0;
}
