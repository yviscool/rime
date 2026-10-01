#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rime::js {

// Single-worker timer used for async native completions (delay, debounce).
// stop() prevents new callbacks and joins the worker so no completion can
// outlive its Host.
class TimerService final {
 public:
  using Callback = std::function<void()>;

  TimerService();
  ~TimerService();

  TimerService(const TimerService&) = delete;
  TimerService& operator=(const TimerService&) = delete;

  // Returns a timer id (0 when the service is stopping).
  std::uint64_t schedule(std::chrono::milliseconds delay, Callback callback);
  // Removes a not-yet-fired timer. Returns false when unknown or fired.
  bool cancel(std::uint64_t id);
  void stop();
  [[nodiscard]] std::size_t pending() const;

 private:
  void run();

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  struct Entry {
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t sequence;
    Callback callback;
  };
  std::vector<Entry> entries_;
  std::uint64_t next_sequence_{1};
  bool stopping_{false};
  std::thread thread_;
};

}  // namespace rime::js
