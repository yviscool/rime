#include "rime/js/timer.hpp"

#include <algorithm>

namespace rime::js {

TimerService::TimerService(const rime::core::Clock* clock)
    : clock_(clock ? clock : &rime::core::SystemClock::instance()),
      thread_(&TimerService::run, this) {
  if (clock_->manual()) {
    clock_->on_advance([this] {
      std::lock_guard lock(mutex_);
      condition_.notify_all();
    });
  }
}

TimerService::~TimerService() { stop(); }

std::uint64_t TimerService::schedule(std::chrono::milliseconds delay, Callback callback) {
  std::uint64_t id = 0;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return 0;
    id = next_sequence_++;
    const auto deadline = clock_->now() + delay;
    entries_.push_back({deadline, id, std::move(callback)});
    // TODO(perf): full re-sort on every schedule is O(N log N); kept to avoid
    // changing the container/headers. Revisit with a heap if timers grow.
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& left, const Entry& right) {
                if (left.deadline != right.deadline) return left.deadline < right.deadline;
                return left.sequence < right.sequence;
              });
  }
  condition_.notify_one();
  return id;
}

bool TimerService::cancel(const std::uint64_t id) {
  {
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [id](const Entry& entry) { return entry.sequence == id; });
    if (found == entries_.end()) return false;
    entries_.erase(found);
  }
  condition_.notify_one();
  return true;
}

void TimerService::stop() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      // already stopped; still join below if needed
    } else {
      stopping_ = true;
      entries_.clear();
    }
  }
  condition_.notify_all();
  // Self-join guard: stop() may run on the worker itself (e.g. via a timer
  // callback); joining the current thread would deadlock/terminate.
  if (thread_.joinable() && std::this_thread::get_id() == thread_.get_id()) return;
  if (thread_.joinable()) thread_.join();
}

std::size_t TimerService::pending() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

void TimerService::run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    if (stopping_) break;
    if (entries_.empty()) {
      condition_.wait(lock, [this] { return stopping_ || !entries_.empty(); });
      continue;
    }
    const auto now = clock_->now();
    if (entries_.front().deadline > now) {
      if (clock_->manual()) {
        condition_.wait(lock, [this] {
          return stopping_ || entries_.empty() ||
                 clock_->now() >= entries_.front().deadline;
        });
      } else {
        condition_.wait_until(lock, entries_.front().deadline);
      }
      continue;
    }
    Callback callback = std::move(entries_.front().callback);
    entries_.erase(entries_.begin());
    lock.unlock();
    try {
      callback();
    } catch (...) {
      // Swallowing here is a worker-boundary requirement: exceptions must not
      // escape the timer thread. Deliberately no on_error callback - adding
      // one would chain new public API through this low-level service.
    }
    lock.lock();
  }
}

}  // namespace rime::js
