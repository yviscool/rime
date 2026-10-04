#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace rime::core {

// Time-source seam. Production runs on SystemClock; native tests inject
// ManualClock so dedupe windows, deadline expiry and timer scheduling are
// deterministic instead of sleeping on the wall clock.
class Clock {
 public:
  using time_point = std::chrono::steady_clock::time_point;

  virtual ~Clock() = default;

  // Monotonic domain: ordering, elapsed measurement, timer deadlines.
  [[nodiscard]] virtual time_point now() const = 0;
  // Wall domain: absolute Unix-epoch milliseconds (Action.deadline_unix_ms).
  [[nodiscard]] virtual std::int64_t unix_ms() const = 0;
  [[nodiscard]] virtual bool manual() const { return false; }
  // ManualClock invokes listeners after advance/set_unix_ms; SystemClock
  // never fires (the base implementation is a no-op). Const because callers
  // hold a `const Clock*` seam.
  virtual void on_advance(std::function<void()> listener) const {
    static_cast<void>(listener);
  }
};

class SystemClock final : public Clock {
 public:
  static SystemClock& instance() {
    static SystemClock clock;
    return clock;
  }

  [[nodiscard]] time_point now() const override { return std::chrono::steady_clock::now(); }

  [[nodiscard]] std::int64_t unix_ms() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
};

class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::int64_t unix_ms = 1'700'000'000'000) : unix_ms_(unix_ms) {}

  [[nodiscard]] time_point now() const override {
    std::lock_guard lock(mutex_);
    return origin_ + std::chrono::milliseconds(steady_offset_ms_);
  }

  [[nodiscard]] std::int64_t unix_ms() const override {
    std::lock_guard lock(mutex_);
    return unix_ms_;
  }

  [[nodiscard]] bool manual() const override { return true; }

  // Lifecycle contract: listeners are never unregistered. A ManualClock must
  // outlive every component that registered one (test-only seam; declare the
  // clock before the component so C++ reverse-order destruction tears the
  // component down first).
  void on_advance(std::function<void()> listener) const override {
    std::lock_guard lock(mutex_);
    listeners_.push_back(std::move(listener));
  }

  void advance(std::chrono::milliseconds delta) {
    if (delta.count() <= 0) return;
    std::vector<std::function<void()>> listeners;
    {
      std::lock_guard lock(mutex_);
      steady_offset_ms_ += delta.count();
      unix_ms_ += delta.count();
      listeners = listeners_;
    }
    fire(listeners);
  }

  void set_unix_ms(std::int64_t wall_ms) {
    std::vector<std::function<void()>> listeners;
    {
      std::lock_guard lock(mutex_);
      unix_ms_ = wall_ms;
      listeners = listeners_;
    }
    fire(listeners);
  }

 private:
  static void fire(const std::vector<std::function<void()>>& listeners) {
    for (const auto& listener : listeners) listener();
  }

  mutable std::mutex mutex_;
  const time_point origin_{std::chrono::steady_clock::now()};
  std::int64_t unix_ms_;
  std::int64_t steady_offset_ms_{0};
  mutable std::vector<std::function<void()>> listeners_;
};

}  // namespace rime::core
