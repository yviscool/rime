#include "rime/core/event_queue.hpp"

#include <chrono>
#include <utility>

namespace rime::core {
namespace {

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

}  // namespace

EventQueue::EventQueue(const std::size_t capacity) : policy_(SchedulerPolicy::bounded(capacity)) {}

EventQueue::EventQueue(SchedulerPolicy policy) : policy_(policy) {}

QueueStatus EventQueue::push(Event event) {
  std::lock_guard lock(mutex_);
  if (closed_) return QueueStatus::Closed;
  if (policy_.capacity == 0) return QueueStatus::Full;

  const bool has_key = !event.key.empty();
  const std::uint64_t now = now_ms();
  if (policy_.dedupe_window_ms > 0 && has_key) {
    const auto found = last_accepted_ms_.find(event.key);
    if (found != last_accepted_ms_.end() && now - found->second < policy_.dedupe_window_ms) {
      return QueueStatus::Deduped;
    }
  }

  switch (policy_.overflow) {
    case OverflowPolicy::Reject:
      if (events_.size() >= policy_.capacity) return QueueStatus::Full;
      break;
    case OverflowPolicy::DropOldest:
      if (events_.size() >= policy_.capacity) {
        events_.pop_front();
        ++dropped_;
      }
      break;
    case OverflowPolicy::CoalesceByKey:
      if (has_key) {
        for (Event& pending : events_) {
          if (pending.key == event.key) {
            pending.kind = event.kind;
            pending.name = std::move(event.name);
            pending.payload = std::move(event.payload);
            if (policy_.dedupe_window_ms > 0) last_accepted_ms_[pending.key] = now;
            return QueueStatus::Coalesced;
          }
        }
      }
      if (events_.size() >= policy_.capacity) return QueueStatus::Full;
      break;
  }

  if (policy_.dedupe_window_ms > 0 && has_key) {
    if (last_accepted_ms_.size() > 1024) {
      for (auto iterator = last_accepted_ms_.begin(); iterator != last_accepted_ms_.end();) {
        if (now - iterator->second >= policy_.dedupe_window_ms) {
          iterator = last_accepted_ms_.erase(iterator);
        } else {
          ++iterator;
        }
      }
    }
    last_accepted_ms_[event.key] = now;
  }
  events_.push_back(std::move(event));
  condition_.notify_one();
  return QueueStatus::Accepted;
}

std::optional<Event> EventQueue::wait_pop() {
  std::unique_lock lock(mutex_);
  condition_.wait(lock, [this] { return closed_ || !events_.empty(); });
  if (events_.empty()) return std::nullopt;
  Event event = std::move(events_.front());
  events_.pop_front();
  return event;
}

std::optional<Event> EventQueue::try_pop() {
  std::lock_guard lock(mutex_);
  if (events_.empty()) return std::nullopt;
  Event event = std::move(events_.front());
  events_.pop_front();
  return event;
}

void EventQueue::close() {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }
  condition_.notify_all();
}

bool EventQueue::closed() const {
  std::lock_guard lock(mutex_);
  return closed_;
}

std::size_t EventQueue::size() const {
  std::lock_guard lock(mutex_);
  return events_.size();
}

std::size_t EventQueue::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

}  // namespace rime::core
