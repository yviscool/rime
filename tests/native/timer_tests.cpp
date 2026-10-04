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
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
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
    clock.advance(std::chrono::seconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
    clock.advance(std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(fires.load() == 0);
    service.stop();
  }

  return 0;
}
