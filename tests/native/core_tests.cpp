// Realism: L6 - verifies concurrent cancellation races, queue backpressure
// stress, and coalescing storms on Kernel and Dispatcher under multi-threaded
// execution, proving thread-safety and trace invariants.

#include "rime/core/runtime.hpp"
#include "rime/action/kernel.hpp"
#include "rime/action/dispatcher.hpp"
#include "rime/core/clock.hpp"
#include "rime/desktop/host.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
      rime::action::Result cancelled_res;
      cancelled_res.id = action.id;
      cancelled_res.cancelled = true;
      cancelled_res.detail = "cancelled";
      cancelled_res.error = {rime::core::Error::Code::Cancelled, "cancelled"};
      return cancelled_res;
    }
    rime::action::Result success_res;
    success_res.id = action.id;
    success_res.succeeded = true;
    success_res.detail = "moved";
    success_res.error = rime::core::Error::none();
    return success_res;
  }
};

class YieldingEchoExecutor final : public rime::action::Executor {
 public:
  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken token) override {
    for (int i = 0; i < 15; ++i) {
      if (token.cancelled()) {
        rime::action::Result cancelled_res;
        cancelled_res.id = action.id;
        cancelled_res.cancelled = true;
        cancelled_res.detail = "cancelled";
        cancelled_res.error = {rime::core::Error::Code::Cancelled, "cancelled"};
        return cancelled_res;
      }
      std::this_thread::yield();
    }
    rime::action::Result success_res;
    success_res.id = action.id;
    success_res.succeeded = true;
    success_res.detail = "moved";
    success_res.error = rime::core::Error::none();
    return success_res;
  }
};

rime::action::Action make_action(rime::core::ActionId id, std::string idempotency_key = "",
                                 std::string payload = "{}") {
  rime::action::Action action;
  action.id = id;
  action.schema_version = 1;
  action.source = {"user", "core-test"};
  action.type = "window.move";
  action.capability = "windows.window.write";
  action.target = {"window", "active"};
  action.deadline_unix_ms = future_deadline();
  action.payload = std::move(payload);
  action.idempotency_key = std::move(idempotency_key);
  return action;
}

void test_rapid_cancellation_race(
    const std::shared_ptr<const rime::action::CapabilityPolicy>& policy) {
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(policy, trace);
  assert(kernel.register_executor("window.move", std::make_shared<YieldingEchoExecutor>()).ok());

  // Part A: Concurrent Direct Kernel Execution with Cancellation Race
  {
    constexpr std::size_t kDirectActions = 120;
    constexpr std::size_t kThreads = 4;
    std::vector<rime::action::Result> results(kDirectActions);
    std::vector<rime::core::CancellationSource> sources(kDirectActions);
    std::atomic<bool> start_gate{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (std::size_t t = 0; t < kThreads; ++t) {
      workers.emplace_back([&, t] {
        while (!start_gate.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (std::size_t i = t; i < kDirectActions; i += kThreads) {
          const auto id = static_cast<rime::core::ActionId>(10'000 + i);
          results[i] = kernel.execute(make_action(id), sources[i].token());
        }
      });
    }

    std::thread canceler([&] {
      while (!start_gate.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t i = 0; i < kDirectActions; ++i) {
        if (i % 2 == 0) {
          sources[i].cancel();
        }
        if (i % 8 == 0) {
          std::this_thread::yield();
        }
      }
    });

    start_gate.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    canceler.join();

    for (std::size_t i = 0; i < kDirectActions; ++i) {
      const auto& res = results[i];
      assert(res.id == static_cast<rime::core::ActionId>(10'000 + i));
      if (res.cancelled) {
        assert(!res.succeeded);
        assert(res.error.code == rime::core::Error::Code::Cancelled);
      } else {
        assert(res.succeeded);
        assert(res.error.ok());
      }
    }
  }

  // Part B: Dispatcher Pipeline Rapid Cancellation Race
  {
    constexpr std::size_t kDispatcherActions = 160;
    constexpr std::size_t kSubmitThreads = 4;
    rime::action::Dispatcher dispatcher(kernel, kDispatcherActions + 32);
    std::vector<rime::core::CancellationSource> sources(kDispatcherActions);
    std::vector<rime::action::Result> dispatched_results;
    std::mutex results_mutex;
    std::atomic<bool> start_gate{false};
    std::atomic<bool> submitters_done{false};

    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitThreads);
    for (std::size_t t = 0; t < kSubmitThreads; ++t) {
      submitters.emplace_back([&, t] {
        while (!start_gate.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (std::size_t i = t; i < kDispatcherActions; i += kSubmitThreads) {
          const auto id = static_cast<rime::core::ActionId>(20'000 + i);
          const auto status = dispatcher.submit(make_action(id));
          assert(status == rime::action::DispatchStatus::Accepted);
        }
      });
    }

    std::thread canceler([&] {
      while (!start_gate.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t i = 0; i < kDispatcherActions; ++i) {
        if (i % 3 == 0) {
          sources[i].cancel();
        }
        if (i % 12 == 0) {
          std::this_thread::yield();
        }
      }
    });

    std::thread pumper([&] {
      while (!start_gate.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      while (!submitters_done.load(std::memory_order_acquire) || dispatcher.size() > 0) {
        auto batch = dispatcher.pump(16, [&](const rime::action::Action& act) {
          const auto idx = act.id - 20'000;
          if (idx < kDispatcherActions) {
            return sources[static_cast<std::size_t>(idx)].token();
          }
          return rime::core::CancellationToken{};
        });
        if (!batch.empty()) {
          std::lock_guard lock(results_mutex);
          dispatched_results.insert(dispatched_results.end(), batch.begin(), batch.end());
        } else {
          std::this_thread::yield();
        }
      }
    });

    start_gate.store(true, std::memory_order_release);
    for (auto& s : submitters) s.join();
    submitters_done.store(true, std::memory_order_release);
    canceler.join();
    pumper.join();

    // Drain any remaining actions
    while (dispatcher.size() > 0) {
      auto drained = dispatcher.pump(32, [&](const rime::action::Action& act) {
        const auto idx = act.id - 20'000;
        if (idx < kDispatcherActions) {
          return sources[static_cast<std::size_t>(idx)].token();
        }
        return rime::core::CancellationToken{};
      });
      dispatched_results.insert(dispatched_results.end(), drained.begin(), drained.end());
    }

    assert(dispatched_results.size() == kDispatcherActions);
    std::unordered_map<rime::core::ActionId, rime::action::Result> result_map;
    for (const auto& res : dispatched_results) {
      assert(result_map.find(res.id) == result_map.end());
      result_map[res.id] = res;
      if (res.cancelled) {
        assert(!res.succeeded);
        assert(res.error.code == rime::core::Error::Code::Cancelled);
      } else {
        assert(res.succeeded);
        assert(res.error.ok());
      }
    }
    assert(result_map.size() == kDispatcherActions);
  }

  // Trace verification for all rapid cancellation actions
  const auto entries = trace->snapshot();
  assert(!entries.empty());
  for (std::size_t i = 1; i < entries.size(); ++i) {
    assert(entries[i].sequence > entries[i - 1].sequence);
  }
  for (const auto& entry : entries) {
    if (entry.kind == rime::core::TraceKind::ActionStarted) {
      assert(entry.action_id != 0);
      assert(!entry.capability.empty());
      bool found_finished = false;
      for (const auto& other : entries) {
        if (other.sequence > entry.sequence && other.action_id == entry.action_id &&
            other.kind == rime::core::TraceKind::ActionFinished) {
          found_finished = true;
          break;
        }
      }
      assert(found_finished);
    }
  }
}

void test_dispatcher_queue_stress_flood(
    const std::shared_ptr<const rime::action::CapabilityPolicy>& policy) {
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(policy, trace);
  assert(kernel.register_executor("window.move", std::make_shared<EchoExecutor>()).ok());

  // Phase A: Rapid Burst Flood exceeding capacity with no pumping
  {
    constexpr std::size_t kCapacity = 16;
    constexpr std::size_t kFloodActions = 200;
    constexpr std::size_t kThreads = 4;
    rime::action::Dispatcher dispatcher(kernel, kCapacity);
    std::atomic<bool> start_gate{false};
    std::atomic<std::size_t> accepted_count{0};
    std::atomic<std::size_t> full_count{0};
    std::vector<std::thread> flooders;
    flooders.reserve(kThreads);

    for (std::size_t t = 0; t < kThreads; ++t) {
      flooders.emplace_back([&, t] {
        while (!start_gate.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (std::size_t i = t; i < kFloodActions; i += kThreads) {
          const auto id = static_cast<rime::core::ActionId>(30'000 + i);
          const auto status = dispatcher.submit(make_action(id));
          if (status == rime::action::DispatchStatus::Accepted) {
            accepted_count.fetch_add(1, std::memory_order_relaxed);
          } else if (status == rime::action::DispatchStatus::Full) {
            full_count.fetch_add(1, std::memory_order_relaxed);
          } else {
            assert(false && "unexpected dispatch status in burst flood");
          }
        }
      });
    }

    start_gate.store(true, std::memory_order_release);
    for (auto& f : flooders) f.join();

    assert(accepted_count.load() == kCapacity);
    assert(full_count.load() == kFloodActions - kCapacity);
    assert(dispatcher.size() == kCapacity);

    // Drain the accepted actions cleanly
    const auto pumped = dispatcher.pump(kCapacity + 10);
    assert(pumped.size() == kCapacity);
    for (const auto& res : pumped) {
      assert(res.succeeded);
      assert(res.error.ok());
    }
    assert(dispatcher.size() == 0);
  }

  // Phase B: Concurrent Flood & Drain with Backpressure
  {
    constexpr std::size_t kCapacity = 24;
    constexpr std::size_t kTotalActions = 300;
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 2;
    rime::action::Dispatcher dispatcher(kernel, kCapacity);
    std::atomic<bool> start_gate{false};
    std::atomic<bool> producers_done{false};
    std::atomic<std::size_t> accepted_count{0};
    std::atomic<std::size_t> full_count{0};
    std::vector<rime::action::Result> pumped_results;
    std::mutex results_mutex;

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (std::size_t p = 0; p < kProducers; ++p) {
      producers.emplace_back([&, p] {
        while (!start_gate.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (std::size_t i = p; i < kTotalActions; i += kProducers) {
          const auto id = static_cast<rime::core::ActionId>(40'000 + i);
          const auto status = dispatcher.submit(make_action(id));
          if (status == rime::action::DispatchStatus::Accepted) {
            accepted_count.fetch_add(1, std::memory_order_relaxed);
          } else if (status == rime::action::DispatchStatus::Full) {
            full_count.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (std::size_t c = 0; c < kConsumers; ++c) {
      consumers.emplace_back([&] {
        while (!start_gate.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        while (!producers_done.load(std::memory_order_acquire) || dispatcher.size() > 0) {
          auto batch = dispatcher.pump(8);
          if (!batch.empty()) {
            std::lock_guard lock(results_mutex);
            pumped_results.insert(pumped_results.end(), batch.begin(), batch.end());
          } else {
            std::this_thread::yield();
          }
        }
      });
    }

    start_gate.store(true, std::memory_order_release);
    for (auto& p : producers) p.join();
    producers_done.store(true, std::memory_order_release);
    for (auto& c : consumers) c.join();

    // Final drain
    auto final_drain = dispatcher.pump(kCapacity + 10);
    pumped_results.insert(pumped_results.end(), final_drain.begin(), final_drain.end());

    assert(accepted_count.load() + full_count.load() == kTotalActions);
    assert(pumped_results.size() == accepted_count.load());
    assert(dispatcher.size() == 0);
  }

  // Verify trace entries strictly increasing and full events recorded
  const auto entries = trace->snapshot();
  assert(!entries.empty());
  std::size_t queue_full_traces = 0;
  for (std::size_t i = 1; i < entries.size(); ++i) {
    assert(entries[i].sequence > entries[i - 1].sequence);
  }
  for (const auto& entry : entries) {
    if (entry.kind == rime::core::TraceKind::ActionRefused && entry.result_code == "queue_full") {
      assert(entry.action_id != 0);
      assert(entry.detail == "rejected: queue full");
      ++queue_full_traces;
    }
  }
  assert(queue_full_traces > 0);
}

void test_coalescing_cancellation_storm(
    const std::shared_ptr<const rime::action::CapabilityPolicy>& policy) {
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(policy, trace);
  assert(kernel.register_executor("window.move", std::make_shared<YieldingEchoExecutor>()).ok());

  constexpr std::size_t kCapacity = 32;
  constexpr std::size_t kStormActions = 240;
  constexpr std::size_t kSubmitThreads = 4;
  constexpr std::size_t kNumKeys = 6;
  rime::action::Dispatcher dispatcher(kernel, rime::core::SchedulerPolicy::coalescing(kCapacity));

  std::vector<rime::core::CancellationSource> sources(kStormActions);
  std::vector<rime::action::Result> pumped_results;
  std::mutex results_mutex;
  std::atomic<bool> start_gate{false};
  std::atomic<bool> submitters_done{false};
  std::atomic<std::size_t> accepted_count{0};
  std::atomic<std::size_t> coalesced_count{0};
  std::atomic<std::size_t> full_count{0};

  std::vector<std::thread> submitters;
  submitters.reserve(kSubmitThreads);
  for (std::size_t t = 0; t < kSubmitThreads; ++t) {
    submitters.emplace_back([&, t] {
      while (!start_gate.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t i = t; i < kStormActions; i += kSubmitThreads) {
        const auto id = static_cast<rime::core::ActionId>(50'000 + i);
        const std::string key = "storm-key-" + std::to_string(i % kNumKeys);
        const auto status = dispatcher.submit(make_action(id, key));
        if (status == rime::action::DispatchStatus::Accepted) {
          accepted_count.fetch_add(1, std::memory_order_relaxed);
        } else if (status == rime::action::DispatchStatus::Coalesced) {
          coalesced_count.fetch_add(1, std::memory_order_relaxed);
        } else if (status == rime::action::DispatchStatus::Full) {
          full_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  std::thread canceler([&] {
    while (!start_gate.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (std::size_t i = 0; i < kStormActions; ++i) {
      if (i % 2 == 0) {
        sources[i].cancel();
      }
      if (i % 8 == 0) {
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&] {
    while (!start_gate.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    while (!submitters_done.load(std::memory_order_acquire) || dispatcher.size() > 0) {
      auto batch = dispatcher.pump(8, [&](const rime::action::Action& act) {
        const auto idx = act.id - 50'000;
        if (idx < kStormActions) {
          return sources[static_cast<std::size_t>(idx)].token();
        }
        return rime::core::CancellationToken{};
      });
      if (!batch.empty()) {
        std::lock_guard lock(results_mutex);
        pumped_results.insert(pumped_results.end(), batch.begin(), batch.end());
      } else {
        std::this_thread::yield();
      }
    }
  });

  start_gate.store(true, std::memory_order_release);
  for (auto& s : submitters) s.join();
  submitters_done.store(true, std::memory_order_release);
  canceler.join();
  consumer.join();

  // Drain any remaining actions from the dispatcher
  while (dispatcher.size() > 0) {
    auto drained = dispatcher.pump(32, [&](const rime::action::Action& act) {
      const auto idx = act.id - 50'000;
      if (idx < kStormActions) {
        return sources[static_cast<std::size_t>(idx)].token();
      }
      return rime::core::CancellationToken{};
    });
    pumped_results.insert(pumped_results.end(), drained.begin(), drained.end());
  }

  // Final flush to catch any remaining suppressed results
  auto remaining_suppressed = dispatcher.pump(1, [](const rime::action::Action&) {
    return rime::core::CancellationToken{};
  });
  pumped_results.insert(pumped_results.end(), remaining_suppressed.begin(), remaining_suppressed.end());

  assert(accepted_count.load() + coalesced_count.load() + full_count.load() == kStormActions);
  assert(pumped_results.size() == accepted_count.load() + coalesced_count.load());
  assert(dispatcher.size() == 0);

  // Invariant verification on each result
  for (const auto& res : pumped_results) {
    assert(res.id >= 50'000 && res.id < 50'000 + kStormActions);
    if (res.cancelled) {
      assert(!res.succeeded);
      assert(res.error.code == rime::core::Error::Code::Cancelled);
    } else {
      assert(res.succeeded);
      assert(res.error.ok());
    }
  }

  // Trace Invariant Verification
  const auto entries = trace->snapshot();
  assert(!entries.empty());
  for (std::size_t i = 1; i < entries.size(); ++i) {
    assert(entries[i].sequence > entries[i - 1].sequence);
  }

  std::unordered_map<rime::core::ActionId, std::vector<rime::core::TraceEntry>> action_entries;
  for (const auto& entry : entries) {
    if (entry.action_id >= 50'000 && entry.action_id < 50'000 + kStormActions) {
      action_entries[entry.action_id].push_back(entry);
    }
  }

  for (const auto& [id, list] : action_entries) {
    bool has_accepted = false;
    bool has_started = false;
    bool has_finished = false;
    bool has_refused = false;
    std::uint64_t started_seq = 0;
    std::uint64_t finished_seq = 0;

    for (const auto& item : list) {
      assert(item.action_id == id);
      assert(!item.capability.empty());
      if (item.kind == rime::core::TraceKind::ActionAccepted) {
        has_accepted = true;
      } else if (item.kind == rime::core::TraceKind::ActionStarted) {
        has_started = true;
        started_seq = item.sequence;
      } else if (item.kind == rime::core::TraceKind::ActionFinished) {
        has_finished = true;
        finished_seq = item.sequence;
      } else if (item.kind == rime::core::TraceKind::ActionRefused) {
        has_refused = true;
      }
    }

    if (has_accepted) {
      assert(has_finished || has_refused);
    }
    if (has_started) {
      assert(has_finished);
      assert(finished_seq > started_seq);
    }
  }
}

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
      if (entry.kind == rime::core::TraceKind::EventDispatchFinished) {
        // Handler segment timing is measured (SystemClock here), so it may
        // exceed 0 ms; it only has to stay inside a sane wall window.
        assert(entry.duration_ms < 60'000);
      } else {
        assert(entry.duration_ms == 0);
      }
      // Queue wait is a Started-only field: never filled elsewhere.
      assert(entry.queue_wait_ms == 0);
    }
    // Every entry is stamped with its record time by the sink.
    assert(entry.unix_ms > 0);
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

  {
    rime::core::ManualClock clock;
    rime::action::Kernel manual_kernel(policy, trace, &clock);
    assert(manual_kernel.register_executor("window.move", std::make_shared<EchoExecutor>()).ok());
    const std::uint64_t deadline = static_cast<std::uint64_t>(clock.unix_ms() + 100);
    const auto within_budget = manual_kernel.execute(
        {47, 1, {"user", "local"}, "window.move", "windows.window.write", {"window", "active"}, {},
         deadline, 0, "{}", ""});
    assert(within_budget.succeeded);
    clock.advance(std::chrono::milliseconds(100));
    const auto over_budget = manual_kernel.execute(
        {48, 1, {"user", "local"}, "window.move", "windows.window.write", {"window", "active"}, {},
         deadline, 0, "{}", ""});
    assert(over_budget.error.code == rime::core::Error::Code::Timeout);
  }

  rime::core::Runtime host_runtime(2);
  rime::desktop::DesktopHost desktop_host(host_runtime);
  assert(desktop_host.start().ok());
  assert(desktop_host.state() == rime::desktop::HostState::Running);
  // start() can only be called once
  const auto second_start = desktop_host.start();
  assert(!second_start.ok());
  assert(second_start.code == rime::core::Error::Code::InvalidState);

  assert(desktop_host.stop().ok());
  assert(desktop_host.state() == rime::desktop::HostState::Stopped);
  // stop() is idempotent
  assert(desktop_host.stop().ok());
  assert(desktop_host.state() == rime::desktop::HostState::Stopped);

  test_rapid_cancellation_race(policy);
  test_dispatcher_queue_stress_flood(policy);
  test_coalescing_cancellation_storm(policy);

  return 0;
}
