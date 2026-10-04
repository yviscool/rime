#pragma once

#include "rime/core/clock.hpp"
#include "rime/core/event.hpp"
#include "rime/core/scheduler_policy.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace rime::core {

enum class QueueStatus : std::uint8_t {
  Accepted,
  Full,
  Closed,
  // The new event replaced a pending event that shared its coalesce key.
  Coalesced,
  // The event was dropped because an equivalent key was accepted within
  // the policy dedupe window.
  Deduped,
};

class EventQueue {
 public:
  explicit EventQueue(std::size_t capacity, const Clock* clock = nullptr);
  explicit EventQueue(SchedulerPolicy policy, const Clock* clock = nullptr);

  [[nodiscard]] QueueStatus push(Event event);
  [[nodiscard]] std::optional<Event> wait_pop();
  [[nodiscard]] std::optional<Event> try_pop();
  void close();

  [[nodiscard]] bool closed() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t dropped() const;
  [[nodiscard]] const SchedulerPolicy& policy() const { return policy_; }

 private:
  SchedulerPolicy policy_;
  const Clock* clock_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Event> events_;
  std::unordered_map<std::string, std::uint64_t> last_accepted_ms_;
  std::size_t dropped_{0};
  bool closed_{false};
};

}  // namespace rime::core
