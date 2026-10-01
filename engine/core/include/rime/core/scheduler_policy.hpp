#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rime::core {

// Central scheduling policy shared by the event queue, the action dispatcher
// and hook delivery. Module implementations must not invent their own
// capacity, drop or merge rules.
enum class OverflowPolicy : std::uint8_t {
  // Reject the new item and report QueueFull (default).
  Reject,
  // Drop the oldest pending item to make room for the new one.
  DropOldest,
  // Merge the new item into a pending item that shares its coalesce key.
  CoalesceByKey,
};

struct SchedulerPolicy {
  std::size_t capacity{64};
  OverflowPolicy overflow{OverflowPolicy::Reject};
  // Items with a non-empty coalesce key accepted within this window are
  // dropped as duplicates. 0 disables time-based deduplication.
  std::uint64_t dedupe_window_ms{0};

  [[nodiscard]] static SchedulerPolicy bounded(std::size_t capacity) {
    SchedulerPolicy policy;
    policy.capacity = capacity;
    return policy;
  }

  [[nodiscard]] static SchedulerPolicy coalescing(std::size_t capacity, std::uint64_t dedupe_window_ms = 0) {
    SchedulerPolicy policy;
    policy.capacity = capacity;
    policy.overflow = OverflowPolicy::CoalesceByKey;
    policy.dedupe_window_ms = dedupe_window_ms;
    return policy;
  }

  [[nodiscard]] static SchedulerPolicy dropping(std::size_t capacity) {
    SchedulerPolicy policy;
    policy.capacity = capacity;
    policy.overflow = OverflowPolicy::DropOldest;
    return policy;
  }
};

}  // namespace rime::core
