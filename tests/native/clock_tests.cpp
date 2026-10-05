// Realism: L3 - the real clock abstractions run in process; only the time
// source is substituted (ManualClock advanced by the test) plus bounded
// stress loops that assert ordering counts, never wall-clock sleeps.

#include "rime/core/clock.hpp"

#include <cassert>
#include <chrono>
#include <thread>

using rime::core::Clock;
using rime::core::ManualClock;
using rime::core::SystemClock;

int main() {
  {
    ManualClock clock;
    assert(clock.manual());
    assert(clock.unix_ms() == 1'700'000'000'000);
    const auto origin = clock.now();
    assert(origin <= std::chrono::steady_clock::now());
    assert(clock.now() == origin);
  }

  {
    ManualClock custom(42);
    assert(custom.unix_ms() == 42);
    assert(custom.now() <= std::chrono::steady_clock::now());
  }

  {
    ManualClock clock;
    const auto origin = clock.now();
    const std::int64_t wall_origin = clock.unix_ms();
    clock.advance(std::chrono::milliseconds(100));
    assert(clock.now() - origin == std::chrono::milliseconds(100));
    assert(clock.unix_ms() - wall_origin == 100);

    clock.set_unix_ms(1'800'000'000'000);
    assert(clock.unix_ms() == 1'800'000'000'000);
    assert(clock.now() - origin == std::chrono::milliseconds(100));

    clock.advance(std::chrono::milliseconds(50));
    clock.advance(std::chrono::milliseconds(70));
    assert(clock.now() - origin == std::chrono::milliseconds(220));
    assert(clock.unix_ms() == 1'800'000'000'000 + 120);
  }

  {
    ManualClock clock;
    const auto origin = clock.now();
    const std::int64_t wall_origin = clock.unix_ms();
    clock.advance(std::chrono::milliseconds(0));
    clock.advance(std::chrono::milliseconds(-5));
    clock.set_unix_ms(clock.unix_ms());
    assert(clock.now() == origin);
    assert(clock.unix_ms() == wall_origin);
  }

  {
    ManualClock clock;
    const auto origin = clock.now();
    int fires = 0;
    Clock::time_point observed{};
    clock.on_advance([&] {
      ++fires;
      observed = clock.now();
    });
    assert(clock.unix_ms() == 1'700'000'000'000);
    clock.advance(std::chrono::milliseconds(100));
    assert(fires == 1);
    assert(observed == clock.now());
    assert(observed - origin == std::chrono::milliseconds(100));
    clock.set_unix_ms(1'900'000'000'000);
    assert(fires == 2);
    assert(observed == clock.now());
    assert(clock.unix_ms() == 1'900'000'000'000);
    int second_fires = 0;
    clock.on_advance([&] { ++second_fires; });
    clock.advance(std::chrono::milliseconds(1));
    assert(fires == 3);
    assert(second_fires == 1);
  }

  {
    SystemClock& system = SystemClock::instance();
    assert(&system == &SystemClock::instance());
    assert(!system.manual());
    assert(system.unix_ms() > 1'600'000'000'000);
    const auto system_now = system.now();
    const auto real_now = std::chrono::steady_clock::now();
    assert(system_now <= real_now);
    int fires = 0;
    system.on_advance([&] { ++fires; });
    assert(fires == 0);
  }

  {
    ManualClock clock;
    const auto origin = clock.now();
    const std::int64_t wall_origin = clock.unix_ms();
    bool monotonic = true;
    std::thread reader([&] {
      Clock::time_point previous = clock.now();
      for (int index = 0; index < 1000; ++index) {
        const Clock::time_point current = clock.now();
        if (current < previous) monotonic = false;
        previous = current;
        const std::int64_t wall = clock.unix_ms();
        if (wall < wall_origin) monotonic = false;
      }
    });
    for (int index = 0; index < 1000; ++index) {
      clock.advance(std::chrono::milliseconds(1));
    }
    reader.join();
    assert(monotonic);
    assert(clock.now() - origin == std::chrono::milliseconds(1000));
    assert(clock.unix_ms() - wall_origin == 1000);
  }

  return 0;
}
