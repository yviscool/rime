// Realism: L3 - the real timer component runs against ManualClock; every
// fire, cancel and ordering expectation is decided by advancing the clock
// in the test, never by waiting on real time.

#include "rime/core/clock.hpp"
#include "rime/js/timer.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <thread>

using rime::core::ManualClock;
using rime::js::TimerService;

int main() {
  {
    ManualClock clock;
    TimerService service(&clock);
    std::promise<void> fired;
    std::future<void> future = fired.get_future();
    assert(service.schedule(std::chrono::seconds(10), [&fired] { fired.set_value(); }) != 0);
    // Liveness before the negative read: a canary with an earlier deadline
    // must be dispatched first, so "the 10s timer has not fired" can no
    // longer be satisfied by a worker that never ran. Only the worker hop is
    // real time here - the deadline itself comes from the injected
    // ManualClock (R1 seam), so nothing waits on the wall clock. The negative
    // read is a bounded condition wait: future.wait_for returns ready the
    // moment the promise is satisfied, so an early fire fails inside the
    // window instead of being sampled once after a sleep (anti-cheat #5).
    std::promise<void> canary;
    std::future<void> canary_future = canary.get_future();
    assert(service.schedule(std::chrono::milliseconds(0), [&canary] { canary.set_value(); }) != 0);
    assert(canary_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    clock.advance(std::chrono::seconds(10));
    assert(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  }

  {
    ManualClock clock;
    TimerService service(&clock);
    std::atomic<bool> fired{false};
    const std::uint64_t id = service.schedule(std::chrono::seconds(5),
                                              [&fired] { fired.store(true); });
    assert(id != 0);
    assert(service.cancel(id));
    // Positive event as liveness proof: a live timer carrying the exact
    // deadline of the cancelled one, so observing it land proves the worker
    // woke and walked the queue at that deadline instead of the thread merely
    // being idle. Only then is the cancelled callback asserted never to have
    // run - the walk happened-before this read, so there is no race and no
    // wall-clock sleep (anti-cheat #5).
    std::promise<void> canary;
    std::future<void> canary_future = canary.get_future();
    assert(service.schedule(std::chrono::seconds(5), [&canary] { canary.set_value(); }) != 0);
    clock.advance(std::chrono::seconds(5));
    assert(canary_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(!fired.load());
    assert(service.pending() == 0);
  }

  {
    ManualClock clock;
    TimerService service(&clock);
    std::promise<void> late;
    std::promise<void> early;
    std::future<void> late_future = late.get_future();
    std::future<void> early_future = early.get_future();
    assert(service.schedule(std::chrono::seconds(10), [&late] { late.set_value(); }) != 0);
    assert(service.schedule(std::chrono::seconds(5), [&early] { early.set_value(); }) != 0);
    assert(service.pending() == 2);
    clock.advance(std::chrono::seconds(5));
    assert(early_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(late_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    assert(service.pending() == 1);
    clock.advance(std::chrono::seconds(5));
    assert(late_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(service.pending() == 0);
  }

  {
    ManualClock clock;
    TimerService service(&clock);
    std::atomic<int> fires{0};
    assert(service.schedule(std::chrono::seconds(1), [&fires] { fires.fetch_add(1); }) != 0);
    assert(service.pending() == 1);
    service.stop();
    assert(service.pending() == 0);
    // No sleep here: stop() clears the queue (pending() above is the
    // observable proof) and joins the worker, so a callback can only come
    // from an entry that no longer exists. Advancing past the stale deadline
    // afterwards is the positive delivery attempt that must stay silent -
    // waiting on the wall clock would prove nothing extra (anti-cheat #5).
    clock.advance(std::chrono::seconds(1));
    assert(fires.load() == 0);
    service.stop();
  }

  return 0;
}
