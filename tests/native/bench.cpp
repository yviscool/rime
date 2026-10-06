// Benchmark harness for the runtime hot paths.
//
// NOT a test: this binary is deliberately not registered with add_test -
// its numbers are timing-dependent and would turn ctest flaky by design.
// It IS compiled by every gate (bun run test / test:asan build all
// targets), so it cannot silently rot, and it does run light sanity checks:
// a broken pipeline must exit nonzero instead of printing plausible numbers.
//
// Usage (manual, from the repo root after a build):
//   build/msvc/tests/native/Release/rime_bench.exe
//   build/msvc/tests/native/Release/rime_bench.exe --input-latency [count]
//
// What it covers (see docs/performance/SPEC.md for the metric system):
//   action.submit / action.pump / action.submit+pump - L3 pipeline legs
//   kernel.allows / executor.noop                 - L3 stage micro bounds
//   event.push+try_pop / event.post->handler      - L1 queue and dispatch
//   runtime.create+start+stop / worker.start+stop - cold-start proxies
//   timer.schedule+cancel (JS builds) / input.inject->hook (interactive)
// Results are indicative single-run measurements, not committed baselines:
// percentiles (p50/p90/p95/p99/p99.9/max), never a bare mean.
//
// --input-latency needs an interactive desktop (it injects real F24 keys
// through the input hook) and an idle desktop for stable numbers. Results
// are indicative single-run measurements, not committed baselines.
//
// Realism: n/a (not a test - no add_test, no contract claims).

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/event_queue.hpp"
#include "rime/core/runtime.hpp"
#include "rime/core/trace.hpp"
#include "rime/core/worker.hpp"
#include "rime/win32/input.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef RIME_BENCH_HAVE_JS
#include "rime/js/timer.hpp"
#endif

namespace {

using rime::action::Action;
using rime::action::Dispatcher;
using rime::action::Executor;
using rime::action::Kernel;
using rime::action::Result;
using rime::action::StaticCapabilityPolicy;
using rime::core::Error;
using rime::core::Event;
using rime::core::EventKind;
using rime::core::EventQueue;
using rime::core::InMemoryTrace;
using rime::core::SchedulerPolicy;
using rime::core::TraceEntry;
using rime::core::TraceKind;

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct Stats {
  double p50_us;
  double p90_us;
  double p95_us;
  double p99_us;
  double p999_us;
  double max_us;
  double ops_per_s;
};

Stats compute_stats(std::vector<std::int64_t> samples, const std::int64_t total_ns) {
  Stats stats{};
  if (samples.empty()) return stats;
  std::sort(samples.begin(), samples.end());
  const auto at = [&](const double p) {
    const auto index = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
    return static_cast<double>(samples[index]);
  };
  stats.p50_us = at(0.50) / 1000.0;
  stats.p90_us = at(0.90) / 1000.0;
  stats.p95_us = at(0.95) / 1000.0;
  stats.p99_us = at(0.99) / 1000.0;
  stats.p999_us = at(0.999) / 1000.0;
  stats.max_us = static_cast<double>(samples.back()) / 1000.0;
  stats.ops_per_s = total_ns > 0
                        ? static_cast<double>(samples.size()) * 1e9 / static_cast<double>(total_ns)
                        : 0.0;
  return stats;
}

void report(const char* name, const std::size_t iters, const Stats& stats) {
  std::printf("%-28s iters=%-7zu p50=%9.2fus p90=%9.2fus p95=%9.2fus p99=%9.2fus "
              "p99.9=%9.2fus max=%9.2fus ops/s=%12.0f\n",
              name, iters, stats.p50_us, stats.p90_us, stats.p95_us, stats.p99_us,
              stats.p999_us, stats.max_us, stats.ops_per_s);
}

int fail(const char* message) {
  std::fprintf(stderr, "rime_bench: sanity check failed: %s\n", message);
  return 1;
}

class NoopExecutor final : public Executor {
 public:
  Result execute(const Action& action, rime::core::CancellationToken) override {
    return {action.id, true, false, "ok", Error::none()};
  }
};

Action make_action(const rime::core::ActionId id) {
  Action action;
  action.id = id;
  action.schema_version = 1;
  action.source = {"bench", "rime_bench"};
  action.type = "bench.noop";
  action.capability = "bench.test";
  action.target = {"none", "bench"};
  action.deadline_unix_ms = static_cast<std::uint64_t>(unix_ms_now() + 60'000);
  action.payload = "{}";
  return action;
}

// submit + pump + execute end to end: the cost one action pays to cross the
// whole in-process pipeline.
int bench_action_pipeline(const std::size_t iters) {
  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"bench.test"});
  auto trace = std::make_shared<InMemoryTrace>();
  Kernel kernel(policy, trace);
  if (!kernel.register_executor("bench.noop", std::make_shared<NoopExecutor>()).ok()) {
    return fail("executor registration");
  }
  Dispatcher dispatcher(kernel, 64);

  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  std::vector<std::int64_t> submit_samples;
  submit_samples.reserve(iters);
  std::vector<std::int64_t> pump_samples;
  pump_samples.reserve(iters);
  std::size_t executed = 0;
  const std::int64_t started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const Action action = make_action(static_cast<rime::core::ActionId>(index) + 1);
    const std::int64_t t0 = now_ns();
    if (dispatcher.submit(action) != rime::action::DispatchStatus::Accepted) {
      return fail("submit refused");
    }
    const std::int64_t t1 = now_ns();
    const std::vector<Result> results = dispatcher.pump(1);
    const std::int64_t t2 = now_ns();
    if (results.size() != 1 || !results.front().succeeded) {
      return fail("action did not succeed");
    }
    ++executed;
    submit_samples.push_back(t1 - t0);
    pump_samples.push_back(t2 - t1);
    samples.push_back(t2 - t0);
  }
  const std::int64_t total = now_ns() - started;
  if (executed != iters || dispatcher.size() != 0) {
    return fail("action accounting");
  }
  report("action.submit", iters, compute_stats(std::move(submit_samples), total));
  report("action.pump", iters, compute_stats(std::move(pump_samples), total));
  report("action.submit+pump", iters, compute_stats(std::move(samples), total));
  return 0;
}

// Capability-gate cost in isolation: the price every action pays before it
// may execute. Uses the same policy object the pipeline bench wires in.
int bench_capability_check(const std::size_t iters) {
  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"bench.test"});
  auto trace = std::make_shared<InMemoryTrace>();
  Kernel kernel(policy, trace);
  std::size_t allowed = 0;
  const std::int64_t started = now_ns();
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    if (kernel.allows("bench.test")) ++allowed;
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  if (allowed != iters) {
    return fail("capability accounting");
  }
  report("kernel.allows", iters, compute_stats(std::move(samples), total));
  return 0;
}

// Executor body in isolation (no queue, no dispatch): bounds the cheapest
// possible action from below.
int bench_executor_direct(const std::size_t iters) {
  NoopExecutor executor;
  const Action action = make_action(1);
  std::size_t succeeded = 0;
  const std::int64_t started = now_ns();
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    if (executor.execute(action, {}).succeeded) ++succeeded;
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  if (succeeded != iters) {
    return fail("executor accounting");
  }
  report("executor.noop", iters, compute_stats(std::move(samples), total));
  return 0;
}

// push + try_pop round trip through the bounded queue with its policy.
int bench_event_queue(const std::size_t iters) {
  EventQueue queue(SchedulerPolicy::bounded(1024));
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  const std::int64_t started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    if (queue.push({static_cast<rime::core::Sequence>(index) + 1, EventKind::Input, "bench",
                    "payload", ""}) != rime::core::QueueStatus::Accepted) {
      return fail("event push refused");
    }
    if (!queue.try_pop().has_value()) {
      return fail("event pop empty");
    }
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  if (queue.size() != 0) {
    return fail("event queue not drained");
  }
  report("event.push+try_pop", iters, compute_stats(std::move(samples), total));
  return 0;
}

// one TraceSink::record (mutex + sequence stamp + push_back).
int bench_trace_record(const std::size_t iters) {
  InMemoryTrace trace;
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  const std::int64_t started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    trace.record({0, TraceKind::ActionFinished, "bench.noop", "succeeded",
                  static_cast<rime::core::ActionId>(index) + 1, "bench.test", "none", 1});
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  if (trace.snapshot().size() != iters) {
    return fail("trace entry count");
  }
  report("trace.record", iters, compute_stats(std::move(samples), total));
  return 0;
}

// Native event dispatch: post one event, pump it, stamp the handler.
// Measures the post -> handler leg (queue + dispatch + callback entry) on
// the calling thread - the native half of hotkey-to-callback latency. The
// QuickJS callback leg is measured by the JS slice tests, not here.
int bench_event_dispatch(const std::size_t iters) {
  using rime::core::Event;
  using rime::core::EventKind;
  using rime::core::Runtime;
  Runtime runtime(1024);
  if (!runtime.start().ok()) {
    return fail("runtime start");
  }
  std::int64_t handler_ns = 0;
  std::size_t handled = 0;
  runtime.set_handler([&](const Event&, rime::core::CancellationToken) {
    handler_ns = now_ns();
    ++handled;
  });
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  const std::int64_t dispatch_started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    handler_ns = 0;
    const std::int64_t t0 = now_ns();
    if (!runtime.post({static_cast<rime::core::Sequence>(index) + 1, EventKind::Input, "bench",
                       "payload", ""})
             .ok()) {
      return fail("event post refused");
    }
    if (runtime.pump(1) != 1 || handler_ns == 0) {
      return fail("event not dispatched");
    }
    samples.push_back(handler_ns - t0);
  }
  if (handled != iters) {
    return fail("event handler accounting");
  }
  report("event.post->handler", iters,
         compute_stats(std::move(samples), now_ns() - dispatch_started));
  if (!runtime.stop().ok()) {
    return fail("runtime stop");
  }
  return 0;
}

// Component lifecycle: create + start + stop + destroy per iteration. A
// proxy for cold-start cost at the Runtime/Worker level (full Host cold
// start additionally loads QuickJS + modules - measured by the host harness,
// not here).
int bench_lifecycle(const std::size_t iters) {
  using rime::core::Runtime;
  std::vector<std::int64_t> runtime_samples;
  runtime_samples.reserve(iters);
  const std::int64_t lifecycle_started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    {
      Runtime runtime(64);
      if (!runtime.start().ok()) {
        return fail("runtime start");
      }
      if (!runtime.stop().ok()) {
        return fail("runtime stop");
      }
    }
    runtime_samples.push_back(now_ns() - t0);
  }
  report("runtime.create+start+stop", iters,
         compute_stats(std::move(runtime_samples), now_ns() - lifecycle_started));

  std::vector<std::int64_t> worker_samples;
  worker_samples.reserve(iters);
  const std::int64_t worker_started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    auto& worker = rime::core::WorkerService::instance();
    if (!worker.start().ok()) {
      return fail("worker start");
    }
    worker.stop();
    worker_samples.push_back(now_ns() - t0);
  }
  report("worker.start+stop", iters,
         compute_stats(std::move(worker_samples), now_ns() - worker_started));
  return 0;
}

#ifdef RIME_BENCH_HAVE_JS
// schedule + cancel while the worker sleeps: the lock/condition cost a
// debounce loop pays per iteration. The one-hour delay never fires.
int bench_timer_schedule_cancel(const std::size_t iters) {
  rime::js::TimerService service;
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  std::size_t cancelled = 0;
  const std::int64_t started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    const auto id =
        service.schedule(std::chrono::hours(1), [] {});
    if (id == 0) {
      service.stop();
      return fail("timer schedule refused");
    }
    if (service.cancel(id)) ++cancelled;
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  service.stop();
  if (cancelled != iters || service.pending() != 0) {
    return fail("timer cancel accounting");
  }
  report("timer.schedule+cancel", iters, compute_stats(std::move(samples), total));
  return 0;
}
#endif

// Desktop latency: real F24 down presses injected through SendInput, timed
// until the low-level hook subscription observes them. Measures the native
// inject-to-callback leg (hook thread -> subscriber), not the JS layer.
int run_input_latency(const int count) {
  using rime::win32::InputEvent;
  using rime::win32::InputService;

  InputService service;
  std::mutex mutex;
  std::condition_variable condition;
  std::int64_t injected_ns = 0;
  int received = 0;
  std::vector<std::int64_t> samples;
  samples.reserve(static_cast<std::size_t>(count));

  if (!service.start().ok()) {
    return fail("input service start");
  }
  const auto subscription = service.subscribe([&](const InputEvent& event) {
    if (event.kind != rime::win32::InputEventKind::Key || event.vk != VK_F24 ||
        !event.key_down || !event.self_injected) {
      return;
    }
    const std::int64_t observed = now_ns();
    std::lock_guard lock(mutex);
    samples.push_back(observed - injected_ns);
    ++received;
    condition.notify_one();
  });
  if (subscription == 0) {
    (void)service.stop();
    return fail("input subscription refused");
  }

  int failures = 0;
  for (int index = 0; index < count; ++index) {
    {
      std::lock_guard lock(mutex);
      injected_ns = now_ns();
    }
    if (!service.send({{VK_F24, true}, {VK_F24, false}}).ok()) {
      ++failures;
      break;
    }
    std::unique_lock lock(mutex);
    if (!condition.wait_for(lock, std::chrono::seconds(1),
                            [&] { return received == index + 1; })) {
      ++failures;
      break;
    }
  }
  (void)service.unsubscribe(subscription);
  service.stop();
  if (failures != 0) {
    std::fprintf(stderr,
                 "rime_bench: input latency aborted after %d of %d events "
                 "(injection or delivery timeout)\n",
                 received, count);
    return 1;
  }
  if (received != count) {
    return fail("input event accounting");
  }
  report("input.inject->hook callback", static_cast<std::size_t>(count),
         compute_stats(std::move(samples), 0));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool input_latency = false;
  int latency_count = 200;
  if (argc >= 2 && std::string(argv[1]) == "--input-latency") {
    input_latency = true;
    if (argc >= 3) latency_count = std::atoi(argv[2]);
    if (latency_count <= 0) {
      return fail("--input-latency count must be positive");
    }
  } else if (argc >= 2) {
    return fail("unknown argument (try --input-latency [count])");
  }

  if (input_latency) return run_input_latency(latency_count);

  if (const int action_status = bench_action_pipeline(20'000); action_status != 0) {
    return action_status;
  }
  if (const int capability_status = bench_capability_check(100'000); capability_status != 0) {
    return capability_status;
  }
  if (const int executor_status = bench_executor_direct(100'000); executor_status != 0) {
    return executor_status;
  }
  if (const int dispatch_status = bench_event_dispatch(20'000); dispatch_status != 0) {
    return dispatch_status;
  }
  if (const int lifecycle_status = bench_lifecycle(200); lifecycle_status != 0) {
    return lifecycle_status;
  }
  if (const int event_status = bench_event_queue(100'000); event_status != 0) {
    return event_status;
  }
  if (const int trace_status = bench_trace_record(100'000); trace_status != 0) {
    return trace_status;
  }
#ifdef RIME_BENCH_HAVE_JS
  if (const int timer_status = bench_timer_schedule_cancel(5'000); timer_status != 0) {
    return timer_status;
  }
#endif
  return 0;
}
