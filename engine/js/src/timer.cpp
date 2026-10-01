#include "rime/js/timer.hpp"

#include <algorithm>

namespace rime::js {

TimerService::TimerService() : thread_(&TimerService::run, this) {}

TimerService::~TimerService() { stop(); }

std::uint64_t TimerService::schedule(std::chrono::milliseconds delay, Callback callback) {
  std::uint64_t id = 0;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return 0;
    id = next_sequence_++;
    const auto deadline = std::chrono::steady_clock::now() + delay;
    entries_.push_back({deadline, id, std::move(callback)});
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
    const auto now = std::chrono::steady_clock::now();
    if (entries_.front().deadline > now) {
      condition_.wait_until(lock, entries_.front().deadline);
      continue;
    }
    Callback callback = std::move(entries_.front().callback);
    entries_.erase(entries_.begin());
    lock.unlock();
    try {
      callback();
    } catch (...) {
      // Timer callbacks must not throw across the worker boundary.
    }
    lock.lock();
  }
}

}  // namespace rime::js
