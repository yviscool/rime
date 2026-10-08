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
//   build/msvc/tests/native/Release/rime_bench.exe --pressure [moves] [producers]
//   build/msvc/tests/native/Release/rime_bench.exe --l4-activate [iters]
//
// --pressure and --l4-activate need an interactive desktop (real SendInput /
// a real fixture window) and an idle desktop for stable numbers.
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
#include "rime/core/lane.hpp"
#include "rime/core/runtime.hpp"
#include "rime/core/trace.hpp"
#include "rime/core/worker.hpp"
#include "rime/win32/input.hpp"
#include "rime/win32/input_probe.hpp"
#include "rime/win32/window.hpp"
#include "rime/win32/clipboard.hpp"
#include "rime/win32/window_executor.hpp"
#include "rime/win32/clipboard_executor.hpp"
#include "rime/automation/uia_service.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
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
#include "rime/js/abi.hpp"
#include "rime/js/host.hpp"
#include "rime/js/timer.hpp"

#include "quickjs.h"
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
// Full HostAbi cold start: construct + load a trivial module + execute to
// Executed, per iteration. One level above Host create/destroy: adds module
// detection, evaluation and the Loaded -> Executed transition (but no native
// module bindings - those belong to the bootstrap harness, L4).
int bench_hostabi_cold_start(const std::size_t iters) {
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  const std::int64_t started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    {
      rime::js::HostAbi abi;
      if (!abi.load("export const bench_value = 40 + 2;", "bench.mjs").ok()) {
        return fail("abi load");
      }
      if (!abi.execute().ok() || abi.state() != rime::js::HostAbiState::Executed) {
        return fail("abi execute");
      }
    }
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  report("js.abi.load+execute", iters, compute_stats(std::move(samples), total));
  return 0;
}

// QuickJS runtime + context create/destroy in isolation: one leg of the
// Host cold-start total above (the rest is the Timer thread, the module
// loader and builtin registration).
int bench_quickjs_runtime(const std::size_t iters) {
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  const std::int64_t started = now_ns();
  std::size_t built = 0;
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    JSRuntime* runtime = JS_NewRuntime();
    JSContext* context = runtime ? JS_NewContext(runtime) : nullptr;
    if (context) {
      JS_FreeContext(context);
      JS_FreeRuntime(runtime);
      ++built;
    } else if (runtime) {
      JS_FreeRuntime(runtime);
    }
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  if (built != iters) {
    return fail("quickjs runtime accounting");
  }
  report("quickjs.new+free", iters, compute_stats(std::move(samples), total));
  return 0;
}

// TimerService create/destroy in isolation: the thread-spawn leg of Host
// cold start (a Host owns one TimerService).
int bench_timer_lifecycle(const std::size_t iters) {
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  const std::int64_t started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    {
      rime::js::TimerService service;
    }
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  report("timer.create+destroy", iters, compute_stats(std::move(samples), total));
  return 0;
}

// Host cold start: create + destroy per iteration (JS_NewRuntime/Context,
// module loader, builtin rime:runtime registration, TimerService thread).
// The process's first Host additionally pays DLL/code-page costs, so the
// first sample is kept - it is part of cold start, not an outlier.
int bench_host_cold_start(const std::size_t iters) {
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  const std::int64_t started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    {
      rime::js::Host host;
    }
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  {
    rime::js::Host probe;
    if (!probe.eval("1 + 1;", "<bench>").ok()) {
      return fail("host eval probe");
    }
  }
  report("js.host.create+destroy", iters, compute_stats(std::move(samples), total));
  return 0;
}

// Trivial script eval on one warm Host: parse + compile + run of `1 + 1`.
int bench_host_eval(const std::size_t iters) {
  rime::js::Host host;
  std::vector<std::int64_t> samples;
  samples.reserve(iters);
  const std::int64_t started = now_ns();
  for (std::size_t index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    if (!host.eval("1 + 1;", "<bench>").ok()) {
      return fail("host eval");
    }
    samples.push_back(now_ns() - t0);
  }
  const std::int64_t total = now_ns() - started;
  report("js.host.eval-trivial", iters, compute_stats(std::move(samples), total));
  return 0;
}

// QuickJS call-overhead reference ONLY (not a native->JS leg): one eval runs
// a million intra-JS calls; the per-call number bounds callback cost from
// below. The real hook -> JS-callback path needs the bootstrap harness
// (L4) and is deliberately not faked here.
int bench_js_call_reference() {
  rime::js::Host host;
  const std::int64_t t0 = now_ns();
  if (!host.eval("globalThis.__acc = 0; const __f = (x) => x + 1;"
                 " for (let i = 0; i < 1000000; ++i) { globalThis.__acc += __f(i); }"
                 " if (globalThis.__acc !== 500000500000) throw new Error('bad acc');",
                 "<bench>")
           .ok()) {
    return fail("js call reference");
  }
  const std::int64_t total = now_ns() - t0;
  std::printf("%-28s calls=%-7d per-call=%9.2fns total=%9.2fms (reference only)\n", "js.1M-calls",
              1000000, static_cast<double>(total) / 1000000.0, static_cast<double>(total) / 1e6);
  return 0;
}

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

// L5 pressure: blast absolute mouse moves through the real SendInput ->
// hook -> subscriber path and observe what breaks first: achieved rate,
// subscriber-seen count, service drop counter, inject->callback latency.
// No throttling is applied on purpose - the reported moves/s is what the
// desktop actually absorbed, not a target. Needs an interactive desktop;
// the cursor is parked back where it started afterwards.
int run_pressure(const int total_moves, const int producers) {
  using rime::win32::InputEvent;
  using rime::win32::InputService;
  using rime::win32::SendMouseAction;
  using rime::win32::SendMouseStep;

  POINT origin{};
  if (!GetCursorPos(&origin)) {
    return fail("cursor position unreadable");
  }
  InputService service;
  if (!service.start().ok()) {
    return fail("input service start");
  }
  std::atomic<std::size_t> received{0};
  // Claim-ordered send stamps (lock-free slot per claim) and arrival-ordered
  // callback stamps (hook thread is serial). Pairing by index is exact for
  // one producer and approximate under OS reordering for many; negative
  // pairs are counted as reordered, never hidden inside the percentiles.
  std::vector<std::int64_t> send_ns(static_cast<std::size_t>(total_moves), 0);
  std::vector<std::int64_t> arrive_ns;
  arrive_ns.reserve(static_cast<std::size_t>(total_moves));
  std::mutex sample_mutex;
  const auto subscription = service.subscribe([&](const InputEvent& event) {
    if (event.kind != rime::win32::InputEventKind::Mouse ||
        event.mouse_action != rime::win32::MouseAction::Move || !event.self_injected) {
      return;
    }
    std::lock_guard lock(sample_mutex);
    if (arrive_ns.size() < static_cast<std::size_t>(total_moves)) {
      arrive_ns.push_back(now_ns());
    }
    ++received;
  });
  if (subscription == 0) {
    (void)service.stop();
    return fail("input subscription refused");
  }

  std::atomic<int> sent{0};
  std::atomic<int> refused{0};
  const int base_x = origin.x;
  const int base_y = origin.y;
  const std::int64_t blast_started = now_ns();
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      int step = 0;
      for (;;) {
        const int claimed = sent.fetch_add(1);
        if (claimed >= total_moves) break;
        // Small deterministic oscillation around the start point: every
        // producer walks the same 8-point pattern offset by its index, so
        // the run is reproducible and the cursor never leaves the area.
        const int dx = ((claimed + p) % 8) - 4;
        const int dy = (((claimed + p) / 8) % 8) - 4;
        send_ns[static_cast<std::size_t>(claimed)] = now_ns();
        if (!service
                 .send_mouse({{SendMouseAction::Move, base_x + dx * 5, base_y + dy * 5, 1}})
                 .ok()) {
          send_ns[static_cast<std::size_t>(claimed)] = 0;
          ++refused;
          break;
        }
        ++step;
      }
    });
  }
  // Pair claim-ordered send stamps with arrival-ordered callback stamps.
  // Exact for one producer, approximate under OS reordering for many.
  for (auto& thread : threads) thread.join();
  const std::int64_t blast_ns = now_ns() - blast_started;
  // Drain: wait (bounded) until callbacks catch up with sends.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (received.load() < static_cast<std::size_t>(sent.load() - refused.load()) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const std::uint64_t dropped = service.dropped_events();
  (void)service.unsubscribe(subscription);
  (void)service.stop();
  SetCursorPos(origin.x, origin.y);
  if (refused.load() != 0) {
    std::fprintf(stderr, "rime_bench: pressure aborted: %d sends refused\n", refused.load());
    return 1;
  }
  std::vector<std::int64_t> pair_ns;
  pair_ns.reserve(arrive_ns.size());
  std::size_t reordered = 0;
  const std::size_t pairs = std::min(arrive_ns.size(), static_cast<std::size_t>(sent.load()));
  for (std::size_t i = 0; i < pairs; ++i) {
    if (send_ns[i] == 0 || arrive_ns[i] < send_ns[i]) {
      ++reordered;
      continue;
    }
    pair_ns.push_back(arrive_ns[i] - send_ns[i]);
  }
  const double seconds = static_cast<double>(blast_ns) / 1e9;
  std::printf("%-28s moves=%-7d producers=%-3d achieved=%10.0f/s received=%-7zu dropped=%-7llu\n",
              "pressure.mouse-blast", total_moves, producers,
              seconds > 0 ? static_cast<double>(sent.load()) / seconds : 0.0, received.load(),
              static_cast<unsigned long long>(dropped));
  if (!pair_ns.empty()) {
    // NOTE: capture the count before moving (argument evaluation order
    // would otherwise read the size after the move).
    const std::size_t pair_count = pair_ns.size();
    report("pressure.inject->callback", pair_count,
           compute_stats(std::move(pair_ns), blast_ns));
  }
  if (reordered > 0) {
    std::printf("%-28s reordered-pairs=%zu (excluded from latency percentiles)\n",
              "pressure.mouse-blast", reordered);
  }
  return 0;
}

// L4 first task: query + focus a self-created fixture window end to end.
// The fixture lives on its own thread in this process (a stepping stone
// toward the standalone BenchmarkTarget.exe in docs/performance/HARNESS.md):
// created, asserted and destroyed inside the run, unique title per run so no
// stray window can match. Foreground confirmation is sampled, not gating:
// the OS may legitimately move foreground elsewhere on a live desktop, so a
// stolen foreground is reported as data, while our own focus()/query()
// failures exit nonzero.
namespace l4_fixture {

constexpr wchar_t kClass[] = L"RimeBenchTarget";
constexpr const char* kTitlePrefix = "RimeBenchTarget";

struct Fixture;
LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

struct Fixture {
  static constexpr int kButtonId = 1001;
  std::string title;
  std::string button_name;
  HWND hwnd{nullptr};
  HWND button{nullptr};
  DWORD thread_id{0};
  std::atomic<bool> ready{false};
  std::atomic<bool> failed{false};
  std::atomic<std::size_t> clicks{0};
  std::thread thread;

  void run() {
    WNDCLASSW klass{};
    klass.lpfnWndProc = window_proc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kClass;
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      failed = true;
      return;
    }
    const std::wstring wide_title(title.begin(), title.end());
    // Off-screen + toolwindow: the fixture must never visibly pop on the
    // tester's desktop and must not take a taskbar button, while staying a
    // real activatable top-level window (hidden windows cannot foreground).
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, wide_title.c_str(), WS_OVERLAPPEDWINDOW,
                           screen_w + 100, 100, 400, 300, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
      failed = true;
      return;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    if (!button_name.empty()) {
      const std::wstring wide_button(button_name.begin(), button_name.end());
      button = CreateWindowExW(0, L"BUTTON", wide_button.c_str(),
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 20, 200, 40, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonId)),
                               GetModuleHandleW(nullptr), nullptr);
      if (!button) {
        failed = true;
        return;
      }
    }
    thread_id = GetCurrentThreadId();
    ShowWindow(hwnd, SW_SHOW);
    ready = true;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    DestroyWindow(hwnd);
    hwnd = nullptr;
    UnregisterClassW(kClass, GetModuleHandleW(nullptr));
  }

  bool start() {
    thread = std::thread([this] { run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready.load() && !failed.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ready.load();
  }

  void finish() {
    if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (thread.joinable()) thread.join();
  }
};

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  if (message == WM_COMMAND && LOWORD(wparam) == Fixture::kButtonId &&
      HIWORD(wparam) == BN_CLICKED) {
    auto* fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (fixture) ++fixture->clicks;
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace l4_fixture

int run_l4_activate(const int iters) {
  using rime::win32::TitleMatchMode;
  using rime::win32::WindowInfo;
  using rime::win32::WindowQuery;
  using rime::win32::WindowService;

  const std::string title = std::string(l4_fixture::kTitlePrefix) + " " +
                            std::to_string(GetCurrentProcessId()) + "." +
                            std::to_string(now_ns());
  l4_fixture::Fixture fixture;
  fixture.title = title;
  if (!fixture.start()) {
    return fail("fixture window did not appear");
  }

  WindowService service;
  if (!service.start().ok()) {
    fixture.finish();
    return fail("window service start");
  }
  // Bounded wait until the service can resolve our own window.
  WindowQuery query;
  query.title = title;
  query.title_match_mode = TitleMatchMode::Exact;
  std::vector<WindowInfo> found;
  const auto visible_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    found.clear();
    if (service.query(query, found).ok() && !found.empty()) break;
    if (std::chrono::steady_clock::now() >= visible_deadline) {
      (void)service.stop();
      fixture.finish();
      return fail("fixture window not resolvable");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::vector<std::int64_t> query_ns;
  std::vector<std::int64_t> focus_ns;
  std::vector<std::int64_t> total_ns;
  query_ns.reserve(static_cast<std::size_t>(iters));
  focus_ns.reserve(static_cast<std::size_t>(iters));
  total_ns.reserve(static_cast<std::size_t>(iters));
  std::size_t focus_ok = 0;
  std::size_t foreground_confirmed = 0;
  std::size_t foreground_checked = 0;
  const std::int64_t run_started = now_ns();
  for (int index = 0; index < iters; ++index) {
    const std::int64_t t0 = now_ns();
    std::vector<WindowInfo> windows;
    if (!service.query(query, windows).ok() || windows.empty()) {
      (void)service.stop();
      fixture.finish();
      return fail("fixture query failed mid-run");
    }
    const std::int64_t t1 = now_ns();
    if (!service.focus(windows.front().id).ok()) {
      (void)service.stop();
      fixture.finish();
      return fail("fixture focus failed mid-run");
    }
    const std::int64_t t2 = now_ns();
    ++focus_ok;
    query_ns.push_back(t1 - t0);
    focus_ns.push_back(t2 - t1);
    total_ns.push_back(t2 - t0);
    if (index % 16 == 0) {
      ++foreground_checked;
      std::optional<WindowInfo> active;
      if (service.active(active).ok() && active.has_value() && active->title == title) {
        ++foreground_confirmed;
      }
    }
  }
  // Confirmed switch: focus, then poll active() until the foreground really
  // is our window (bounded). Unlike l4.focus (request cost), this is the
  // number comparable with AHK's WinActivate+WinWaitActive round trip.
  std::vector<std::int64_t> confirmed_ns;
  confirmed_ns.reserve(50);
  const std::int64_t confirmed_started = now_ns();
  for (int index = 0; index < 50; ++index) {
    const std::int64_t t0 = now_ns();
    std::vector<WindowInfo> windows;
    if (!service.query(query, windows).ok() || windows.empty()) break;
    if (!service.focus(windows.front().id).ok()) break;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool confirmed = false;
    while (std::chrono::steady_clock::now() < deadline) {
      std::optional<WindowInfo> active;
      if (service.active(active).ok() && active.has_value() && active->title == title) {
        confirmed = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!confirmed) break;
    confirmed_ns.push_back(now_ns() - t0);
  }
  (void)service.stop();
  fixture.finish();
  if (focus_ok != static_cast<std::size_t>(iters)) {
    return fail("focus accounting");
  }
  const std::int64_t run_total = now_ns() - run_started;
  report("l4.query", static_cast<std::size_t>(iters),
         compute_stats(std::move(query_ns), run_total));
  report("l4.focus", static_cast<std::size_t>(iters),
         compute_stats(std::move(focus_ns), run_total));
  report("l4.query+focus", static_cast<std::size_t>(iters),
         compute_stats(std::move(total_ns), run_total));
  if (confirmed_ns.size() == 50) {
    const std::size_t confirmed_count = confirmed_ns.size();
    report("l4.focus.confirmed", confirmed_count,
           compute_stats(std::move(confirmed_ns), now_ns() - confirmed_started));
  } else {
    std::printf("%-28s confirmed=%zu/50 (foreground stolen by the desktop?)\n", "l4.focus",
                confirmed_ns.size());
  }
  std::printf("%-28s foreground=%zu/%zu (sampled; OS may legitimately move it)\n",
              "l4.foreground", foreground_confirmed, foreground_checked);
  return 0;
}

// L4 clipboard round trip: write a unique payload, read it back, compare.
// Saves the desktop clipboard first and restores it afterwards (L5 rule:
// create, assert, clean up inside the run).
int run_l4_clipboard(const int iters) {
  using rime::win32::ClipboardService;
  ClipboardService service;
  std::vector<std::uint8_t> saved;
  if (!service.save_all(saved).ok()) {
    return fail("clipboard save (desktop clipboard busy?)");
  }
  const std::string payload =
      "rime-bench-clipboard-" + std::to_string(GetCurrentProcessId()) + "." + std::to_string(now_ns());
  std::vector<std::int64_t> write_ns;
  std::vector<std::int64_t> read_ns;
  std::vector<std::int64_t> total_ns;
  write_ns.reserve(static_cast<std::size_t>(iters));
  read_ns.reserve(static_cast<std::size_t>(iters));
  total_ns.reserve(static_cast<std::size_t>(iters));
  const std::int64_t run_started = now_ns();
  int status = 0;
  for (int index = 0; index < iters && status == 0; ++index) {
    const std::int64_t t0 = now_ns();
    if (!service.write_text(payload).ok()) {
      status = fail("clipboard write mid-run");
      break;
    }
    const std::int64_t t1 = now_ns();
    std::string back;
    if (!service.read_text(back).ok() || back != payload) {
      status = fail("clipboard roundtrip mismatch");
      break;
    }
    const std::int64_t t2 = now_ns();
    write_ns.push_back(t1 - t0);
    read_ns.push_back(t2 - t1);
    total_ns.push_back(t2 - t0);
  }
  std::uint32_t restored = 0;
  if (!service.restore_all(saved, restored).ok()) {
    return fail("clipboard restore");
  }
  if (status != 0) return status;
  const std::int64_t run_total = now_ns() - run_started;
  report("l4.clipboard.write", static_cast<std::size_t>(iters),
         compute_stats(std::move(write_ns), run_total));
  report("l4.clipboard.read", static_cast<std::size_t>(iters),
         compute_stats(std::move(read_ns), run_total));
  report("l4.clipboard.roundtrip", static_cast<std::size_t>(iters),
         compute_stats(std::move(total_ns), run_total));
  return 0;
}

// L4 UIA task: find the fixture's button from the desktop root and invoke
// it, verifying the click lands (WM_COMMAND observed). Standard BUTTON gets
// its Invoke pattern from the OS MSAA bridge - no custom provider needed.
int run_l4_uia(const int iters) {
  using rime::automation::ElementSnapshot;
  using rime::automation::FindQuery;
  using rime::automation::UiaService;

  const std::string token =
      std::to_string(GetCurrentProcessId()) + "." + std::to_string(now_ns());
  l4_fixture::Fixture fixture;
  fixture.title = std::string(l4_fixture::kTitlePrefix) + " " + token;
  fixture.button_name = std::string(l4_fixture::kTitlePrefix) + ".OK " + token;
  if (!fixture.start()) {
    return fail("fixture window did not appear");
  }
  UiaService service;
  if (!service.start().ok()) {
    fixture.finish();
    return fail("uia service start");
  }
  int status = 0;
  std::vector<std::int64_t> find_ns;
  std::vector<std::int64_t> invoke_ns;
  std::vector<std::int64_t> total_ns;
  std::vector<std::int64_t> find_scoped_ns;
  find_ns.reserve(static_cast<std::size_t>(iters));
  invoke_ns.reserve(static_cast<std::size_t>(iters));
  total_ns.reserve(static_cast<std::size_t>(iters));
  find_scoped_ns.reserve(static_cast<std::size_t>(iters));
  // One-time scope resolution (untimed): the fixture window element becomes
  // the subtree root for the scoped series. If it goes stale mid-run the
  // scoped sample is skipped and re-resolved, never failed.
  std::uint64_t window_element_id = 0;
  {
    FindQuery scope_query;
    scope_query.name = fixture.title;
    scope_query.max_results = 8;
    std::vector<ElementSnapshot> scope_found;
    if (service.find(scope_query, scope_found).ok() && !scope_found.empty()) {
      window_element_id = scope_found.front().id;
    }
  }
  const std::int64_t run_started = now_ns();
  for (int index = 0; index < iters && status == 0; ++index) {
    const std::size_t clicks_before = fixture.clicks.load();
    const std::int64_t t0 = now_ns();
    FindQuery query;
    query.name = fixture.button_name;
    query.control_type = "button";
    query.max_results = 8;
    std::vector<ElementSnapshot> found;
    if (!service.find(query, found).ok() || found.empty()) {
      status = fail("uia find failed mid-run");
      break;
    }
    const std::uint64_t id = found.front().id;
    const std::int64_t t1 = now_ns();
    if (!service.invoke(id).ok()) {
      status = fail("uia invoke failed mid-run");
      break;
    }
    const std::int64_t t2 = now_ns();
    (void)service.release(id);
    // The invoke returned; the click message may still be in flight.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fixture.clicks.load() == clicks_before &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (fixture.clicks.load() == clicks_before) {
      status = fail("uia click never landed");
      break;
    }
    find_ns.push_back(t1 - t0);
    invoke_ns.push_back(t2 - t1);
    total_ns.push_back(t2 - t0);
    // P0-3 scoped contrast: same button, subtree rooted at the fixture
    // window element. A stale scope is re-resolved outside the timed
    // section; iterations without a scope simply contribute no sample.
    if (window_element_id != 0) {
      FindQuery scoped;
      scoped.name = fixture.button_name;
      scoped.control_type = "button";
      scoped.max_results = 8;
      scoped.from_id = window_element_id;
      std::vector<ElementSnapshot> scoped_found;
      const std::int64_t t3 = now_ns();
      if (service.find(scoped, scoped_found).ok() && !scoped_found.empty()) {
        find_scoped_ns.push_back(now_ns() - t3);
        (void)service.release(scoped_found.front().id);
      } else {
        window_element_id = 0;
        FindQuery re_scope;
        re_scope.name = fixture.title;
        re_scope.max_results = 8;
        std::vector<ElementSnapshot> re_found;
        if (service.find(re_scope, re_found).ok() && !re_found.empty()) {
          window_element_id = re_found.front().id;
        }
      }
    }
  }
  (void)service.stop();
  fixture.finish();
  if (status != 0) return status;
  const std::int64_t run_total = now_ns() - run_started;
  report("l4.uia.find", static_cast<std::size_t>(iters),
         compute_stats(std::move(find_ns), run_total));
  report("l4.uia.invoke", static_cast<std::size_t>(iters),
         compute_stats(std::move(invoke_ns), run_total));
  report("l4.uia.find+invoke", static_cast<std::size_t>(iters),
         compute_stats(std::move(total_ns), run_total));
  if (!find_scoped_ns.empty()) {
    // Hoisted: argument evaluation order is unspecified, so size() must be
    // read before the move below empties the vector.
    const std::size_t scoped_n = find_scoped_ns.size();
    report("l4.uia.find_scoped", scoped_n,
           compute_stats(std::move(find_scoped_ns), run_total));
  }
  return 0;
}

// L4 complex workflow: one heterogeneous task through the REAL pipeline -
// resolve the fixture id (context matching), then clipboard.write,
// window.focus and window.move(rect) as three traced Actions through one
// Dispatcher + Kernel + real executors + real Win32. The move stays
// off-screen (no desktop popup); the clipboard is saved/restored.
int run_l4_workflow(const int iters) {
  using rime::win32::ClipboardExecutor;
  using rime::win32::ClipboardService;
  using rime::win32::TitleMatchMode;
  using rime::win32::WindowExecutor;
  using rime::win32::WindowInfo;
  using rime::win32::WindowQuery;
  using rime::win32::WindowService;

  const std::string token =
      std::to_string(GetCurrentProcessId()) + "." + std::to_string(now_ns());
  l4_fixture::Fixture fixture;
  fixture.title = std::string(l4_fixture::kTitlePrefix) + " " + token;
  if (!fixture.start()) {
    return fail("fixture window did not appear");
  }
  WindowService windows;
  if (!windows.start().ok()) {
    fixture.finish();
    return fail("window service start");
  }
  ClipboardService clipboard;
  std::vector<std::uint8_t> saved_clip;
  if (!clipboard.save_all(saved_clip).ok()) {
    (void)windows.stop();
    fixture.finish();
    return fail("clipboard save");
  }
  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.window.write", "windows.clipboard.write"});
  auto trace = std::make_shared<InMemoryTrace>();
  Kernel kernel(policy, trace);
  auto window_executor = std::make_shared<WindowExecutor>(windows);
  auto clip_executor = std::make_shared<ClipboardExecutor>(clipboard);
  for (const char* type : {"window.focus", "window.move"}) {
    if (!kernel.register_executor(type, window_executor).ok()) {
      return fail("window executor registration");
    }
  }
  if (!kernel.register_executor("clipboard.write", clip_executor).ok()) {
    return fail("clipboard executor registration");
  }
  Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  // Executors require the worker lane (same rule the contract tests obey);
  // this single-threaded run IS the worker lane while pumping.
  struct WorkerLane {
    WorkerLane() : ok(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Worker).ok()) {}
    ~WorkerLane() { rime::core::LaneRegistry::instance().release(rime::core::Lane::Worker); }
    bool ok{false};
  };
  const WorkerLane lane;
  if (!lane.ok) {
    return fail("worker lane claim");
  }

  const std::string payload_text = "rime-bench-workflow-" + token;
  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  std::vector<std::int64_t> resolve_ns;
  std::vector<std::int64_t> write_ns;
  std::vector<std::int64_t> focus_ns;
  std::vector<std::int64_t> move_ns;
  std::vector<std::int64_t> total_ns;
  resolve_ns.reserve(static_cast<std::size_t>(iters));
  write_ns.reserve(static_cast<std::size_t>(iters));
  focus_ns.reserve(static_cast<std::size_t>(iters));
  move_ns.reserve(static_cast<std::size_t>(iters));
  total_ns.reserve(static_cast<std::size_t>(iters));
  int status = 0;
  std::uint64_t next_id = 1;
  const auto run_one = [&](const std::string& type, const std::string& capability,
                           const std::string& target_kind, const std::string& target_id,
                           const std::string& payload, std::int64_t& elapsed) -> bool {
    Action action;
    action.id = next_id++;
    action.schema_version = 1;
    action.source = {"bench", "rime_bench"};
    action.type = type;
    action.capability = capability;
    action.target = {target_kind, target_id};
    action.deadline_unix_ms = static_cast<std::uint64_t>(unix_ms_now() + 60'000);
    action.payload = payload;
    const std::int64_t t0 = now_ns();
    if (dispatcher.submit(action) != rime::action::DispatchStatus::Accepted) {
      status = fail(("workflow submit refused: " + type).c_str());
      return false;
    }
    const std::vector<Result> results = dispatcher.pump(1);
    elapsed = now_ns() - t0;
    if (results.size() != 1 || !results.front().succeeded) {
      status = fail(("workflow action failed: " + type + ": " + results.front().detail).c_str());
      return false;
    }
    return true;
  };
  WindowQuery query;
  query.title = fixture.title;
  query.title_match_mode = TitleMatchMode::Exact;
  const std::int64_t wf_started = now_ns();
  std::string last_wid;
  for (int index = 0; index < iters && status == 0; ++index) {
    const std::int64_t t0 = now_ns();
    std::vector<WindowInfo> windows_found;
    if (!windows.query(query, windows_found).ok() || windows_found.empty()) {
      status = fail("workflow resolve failed mid-run");
      break;
    }
    const std::string wid = std::to_string(windows_found.front().id);
    last_wid = wid;
    const std::int64_t t_resolve = now_ns();
    std::int64_t dt_write = 0;
    std::int64_t dt_focus = 0;
    std::int64_t dt_move = 0;
    const std::string move_payload =
        "{\"rect\":{\"x\":" + std::to_string(screen_w + 100 + (index % 8)) + "}}";
    if (!run_one("clipboard.write", "windows.clipboard.write", "clipboard", "default",
                 "{\"text\":\"" + payload_text + "\"}", dt_write) ||
        !run_one("window.focus", "windows.window.write", "window", wid, "{}", dt_focus) ||
        !run_one("window.move", "windows.window.write", "window", wid, move_payload, dt_move)) {
      break;
    }
    resolve_ns.push_back(t_resolve - t0);
    write_ns.push_back(dt_write);
    focus_ns.push_back(dt_focus);
    move_ns.push_back(dt_move);
    total_ns.push_back(dt_write + dt_focus + dt_move + (t_resolve - t0));
  }
  // P0-2 layered breakdown: the same window.focus action timed at each seam
  // (dispatcher.submit | dispatcher.pump | kernel.execute direct |
  // WindowService::focus direct). Same fixture, same lane, same build — the
  // four medians must bracket l4wf.window.focus above; no attribution by
  // subtraction across harnesses.
  std::vector<std::int64_t> focus_submit_ns;
  std::vector<std::int64_t> focus_pump_ns;
  std::vector<std::int64_t> focus_kernel_ns;
  std::vector<std::int64_t> focus_service_ns;
  focus_submit_ns.reserve(static_cast<std::size_t>(iters));
  focus_pump_ns.reserve(static_cast<std::size_t>(iters));
  focus_kernel_ns.reserve(static_cast<std::size_t>(iters));
  focus_service_ns.reserve(static_cast<std::size_t>(iters));
  if (status == 0 && !last_wid.empty()) {
    const std::uint64_t wid_num = std::stoull(last_wid);
    for (int index = 0; index < iters && status == 0; ++index) {
      Action action;
      action.id = next_id++;
      action.schema_version = 1;
      action.source = {"bench", "rime_bench"};
      action.type = "window.focus";
      action.capability = "windows.window.write";
      action.target = {"window", last_wid};
      action.deadline_unix_ms = static_cast<std::uint64_t>(unix_ms_now() + 60'000);
      action.payload = "{}";
      const std::int64_t t0 = now_ns();
      if (dispatcher.submit(action) != rime::action::DispatchStatus::Accepted) {
        status = fail("focus breakdown submit refused");
        break;
      }
      const std::int64_t t1 = now_ns();
      const std::vector<Result> results = dispatcher.pump(1);
      const std::int64_t t2 = now_ns();
      if (results.size() != 1 || !results.front().succeeded) {
        status = fail("focus breakdown mediated action failed");
        break;
      }
      focus_submit_ns.push_back(t1 - t0);
      focus_pump_ns.push_back(t2 - t1);
      Action direct = action;
      direct.id = next_id++;
      const std::int64_t t3 = now_ns();
      const Result kres = kernel.execute(direct);
      const std::int64_t t4 = now_ns();
      if (!kres.succeeded) {
        status = fail("focus breakdown kernel.execute failed");
        break;
      }
      focus_kernel_ns.push_back(t4 - t3);
      const std::int64_t t5 = now_ns();
      const rime::core::Error serr = windows.focus(wid_num);
      const std::int64_t t6 = now_ns();
      if (!serr.ok()) {
        status = fail("focus breakdown service.focus failed");
        break;
      }
      focus_service_ns.push_back(t6 - t5);
    }
  }
  std::string back;
  const bool roundtrip_ok = clipboard.read_text(back).ok() && back == payload_text;
  std::uint32_t restored = 0;
  if (!clipboard.restore_all(saved_clip, restored).ok()) {
    status = fail("clipboard restore");
  }
  (void)windows.stop();
  fixture.finish();
  if (status != 0) return status;
  if (!roundtrip_ok) {
    return fail("workflow clipboard mismatch");
  }
  const std::int64_t wf_total = now_ns() - wf_started;
  report("l4wf.resolve", static_cast<std::size_t>(iters),
         compute_stats(std::move(resolve_ns), wf_total));
  report("l4wf.clipboard.write", static_cast<std::size_t>(iters),
         compute_stats(std::move(write_ns), wf_total));
  report("l4wf.window.focus", static_cast<std::size_t>(iters),
         compute_stats(std::move(focus_ns), wf_total));
  report("l4wf.window.move", static_cast<std::size_t>(iters),
         compute_stats(std::move(move_ns), wf_total));
  report("l4wf.task", static_cast<std::size_t>(iters), compute_stats(std::move(total_ns), wf_total));
  // Hoisted: argument evaluation order is unspecified, so size() must be
  // read before the moves below empty the vectors.
  const std::size_t breakdown_n = focus_submit_ns.size();
  report("l4wf.focus.submit", breakdown_n,
         compute_stats(std::move(focus_submit_ns), wf_total));
  report("l4wf.focus.pump", breakdown_n,
         compute_stats(std::move(focus_pump_ns), wf_total));
  report("l4wf.focus.kernel_direct", breakdown_n,
         compute_stats(std::move(focus_kernel_ns), wf_total));
  report("l4wf.focus.service_direct", breakdown_n,
         compute_stats(std::move(focus_service_ns), wf_total));
  return 0;
}

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
  int incomplete = 0;
  std::vector<std::int64_t> prep_ns;      // bench lock + timestamp (loop overhead floor)
  std::vector<std::int64_t> service_ns;   // send_entry -> pre_inject (validate+build+mark)
  std::vector<std::int64_t> inject_ns;    // pre_inject -> post_inject (SendInput syscall)
  std::vector<std::int64_t> traverse_ns;  // pre_inject -> hook_entry (OS traversal)
  std::vector<std::int64_t> hook_ns;      // hook_entry -> enqueue_done (proc + queue)
  std::vector<std::int64_t> queue_ns;     // enqueue_done -> deliver (pump wake + wait)
  for (int index = 0; index < count; ++index) {
    {
      std::lock_guard lock(mutex);
      injected_ns = now_ns();
    }
    const std::int64_t t_prep = now_ns();
    rime::win32::input_probe::clear();
    if (!rime::win32::input_probe::arm(VK_F24)) {
      ++failures;
      break;
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
    lock.unlock();
    prep_ns.push_back(t_prep - injected_ns);
    rime::win32::input_probe::Stages stages;
    if (!rime::win32::input_probe::read(stages) || stages.deliver_ns == 0 ||
        stages.hook_entry == 0) {
      // A foreign event stole a first-match stamp, or a stage never ran:
      // count it, keep the headline sample, skip the layers for this iter.
      ++incomplete;
      continue;
    }
    const auto ns = [](std::uint64_t v) { return static_cast<std::int64_t>(v); };
    service_ns.push_back(ns(stages.pre_inject) - ns(stages.send_entry));
    inject_ns.push_back(ns(stages.post_inject) - ns(stages.pre_inject));
    traverse_ns.push_back(ns(stages.hook_entry) - ns(stages.pre_inject));
    hook_ns.push_back(ns(stages.enqueue_done) - ns(stages.hook_entry));
    queue_ns.push_back(ns(stages.deliver_ns) - ns(stages.enqueue_done));
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
  // Hoisted: argument evaluation order is unspecified, so sizes must be
  // read before the moves below empty the vectors.
  const std::size_t layer_n = service_ns.size();
  if (incomplete != 0) {
    std::fprintf(stderr, "rime_bench: input layers incomplete for %d of %d iters\n",
                 incomplete, count);
  }
  report("input.inject->hook callback", static_cast<std::size_t>(count),
         compute_stats(std::move(samples), 0));
  const std::size_t prep_n = prep_ns.size();
  report("input.prep", prep_n, compute_stats(std::move(prep_ns), 0));
  report("input.service", layer_n, compute_stats(std::move(service_ns), 0));
  report("input.inject", layer_n, compute_stats(std::move(inject_ns), 0));
  report("input.traverse", layer_n, compute_stats(std::move(traverse_ns), 0));
  report("input.hook", layer_n, compute_stats(std::move(hook_ns), 0));
  report("input.queue", layer_n, compute_stats(std::move(queue_ns), 0));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // No modal dialogs, ever: a broken bench (failed assert, fault) must die
  // on stderr with an exit code, never pop a message box on the tester's
  // desktop. Mirrors tools/win32-error-mode.ts for manual runs (the runner
  // sets it for CI children; do it here too so bare runs behave the same).
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  bool input_latency = false;
  int latency_count = 200;
  bool pressure = false;
  int pressure_moves = 2000;
  int pressure_producers = 2;
  bool l4_activate = false;
  int l4_iters = 200;
  bool l4_clipboard = false;
  int l4_clipboard_iters = 200;
  bool l4_uia = false;
  int l4_uia_iters = 50;
  bool l4_workflow = false;
  int l4_workflow_iters = 50;
  if (argc >= 2 && std::string(argv[1]) == "--input-latency") {
    input_latency = true;
    if (argc >= 3) latency_count = std::atoi(argv[2]);
    if (latency_count <= 0) {
      return fail("--input-latency count must be positive");
    }
  } else if (argc >= 2 && std::string(argv[1]) == "--pressure") {
    pressure = true;
    if (argc >= 3) pressure_moves = std::atoi(argv[2]);
    if (argc >= 4) pressure_producers = std::atoi(argv[3]);
    if (pressure_moves <= 0 || pressure_producers <= 0) {
      return fail("--pressure moves and producers must be positive");
    }
  } else if (argc >= 2 && std::string(argv[1]) == "--l4-activate") {
    l4_activate = true;
    if (argc >= 3) l4_iters = std::atoi(argv[2]);
    if (l4_iters <= 0) {
      return fail("--l4-activate iters must be positive");
    }
  } else if (argc >= 2 && std::string(argv[1]) == "--l4-clipboard") {
    l4_clipboard = true;
    if (argc >= 3) l4_clipboard_iters = std::atoi(argv[2]);
    if (l4_clipboard_iters <= 0) {
      return fail("--l4-clipboard iters must be positive");
    }
  } else if (argc >= 2 && std::string(argv[1]) == "--l4-uia") {
    l4_uia = true;
    if (argc >= 3) l4_uia_iters = std::atoi(argv[2]);
    if (l4_uia_iters <= 0) {
      return fail("--l4-uia iters must be positive");
    }
  } else if (argc >= 2 && std::string(argv[1]) == "--l4-workflow") {
    l4_workflow = true;
    if (argc >= 3) l4_workflow_iters = std::atoi(argv[2]);
    if (l4_workflow_iters <= 0) {
      return fail("--l4-workflow iters must be positive");
    }
  } else if (argc >= 2) {
    return fail("unknown argument (try --input-latency [count], --pressure [moves] "
                "[producers], --l4-activate [iters], --l4-clipboard [iters], "
                "--l4-uia [iters], --l4-workflow [iters])");
  }

  if (input_latency) return run_input_latency(latency_count);
  if (pressure) return run_pressure(pressure_moves, pressure_producers);
  if (l4_activate) return run_l4_activate(l4_iters);
  if (l4_clipboard) return run_l4_clipboard(l4_clipboard_iters);
  if (l4_uia) return run_l4_uia(l4_uia_iters);
  if (l4_workflow) return run_l4_workflow(l4_workflow_iters);

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
  if (const int abi_status = bench_hostabi_cold_start(200); abi_status != 0) {
    return abi_status;
  }
  if (const int qjs_status = bench_quickjs_runtime(200); qjs_status != 0) {
    return qjs_status;
  }
  if (const int timer_life_status = bench_timer_lifecycle(200); timer_life_status != 0) {
    return timer_life_status;
  }
  if (const int cold_status = bench_host_cold_start(200); cold_status != 0) {
    return cold_status;
  }
  if (const int eval_status = bench_host_eval(1'000); eval_status != 0) {
    return eval_status;
  }
  if (const int call_status = bench_js_call_reference(); call_status != 0) {
    return call_status;
  }
  if (const int timer_status = bench_timer_schedule_cancel(5'000); timer_status != 0) {
    return timer_status;
  }
#endif
  return 0;
}
