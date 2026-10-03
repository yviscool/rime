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

// What one dispatch decision did with an event. Callers report the outcome
// (deliver, buffer, drop) instead of inventing their own drop rules.
enum class Delivery : std::uint8_t {
  Deliver,  // run the delivery now
  Buffer,   // keep exactly one pending item, launched when a slot frees
  Drop,     // discard the event under the full-load policy
};

struct SchedulerPolicy {
  std::size_t capacity{64};
  OverflowPolicy overflow{OverflowPolicy::Reject};
  // Items with a non-empty coalesce key accepted within this window are
  // dropped as duplicates. 0 disables time-based deduplication.
  std::uint64_t dedupe_window_ms{0};

  // ---- dispatch decisions (M2-D) ------------------------------------------
  // The instruction-level scheduling knobs from
  // docs/api/directives-and-syntax.md live here so Hook, Timer and module
  // implementations consult one policy instead of each owning a rule.
  //
  // #MaxThreads: deliveries in flight across this scheduler; 0 = unlimited.
  std::uint32_t max_concurrency{0};
  // #MaxThreadsPerHotkey: in-flight deliveries allowed for one registration;
  // 0 = unlimited. The default of 1 makes a re-entrant delivery explicit
  // (dropped) instead of running the same callback recursively.
  std::uint32_t max_concurrency_per_subscription{1};
  // #InputLevel: level new registrations start at; an event whose injection
  // level is below a registration's level never reaches it (physical input
  // and this stage's own injections are level 0, matching AHK).
  std::uint32_t input_level{0};
  // #HotIfTimeout: budget in milliseconds for one condition evaluation; the
  // condition fails closed when it used up the budget. 0 = no budget.
  std::uint64_t hot_if_timeout_ms{0};

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

  // Event dispatch defaults (hotkey/hotstring/timer/onMessage/InputHook):
  // one in-flight item absorbs later repeats of the same key (timer ticks),
  // a re-entrant registration delivery is dropped, no global cap.
  [[nodiscard]] static SchedulerPolicy events() {
    SchedulerPolicy policy;
    policy.overflow = OverflowPolicy::CoalesceByKey;
    policy.max_concurrency = 0;
    policy.max_concurrency_per_subscription = 1;
    policy.input_level = 0;
    policy.hot_if_timeout_ms = 1000;  // AHK g_HotExprTimeout default.
    return policy;
  }

  // #MaxThreads: false while `running` deliveries are already in flight.
  [[nodiscard]] bool admits_total(const std::uint32_t running) const {
    return max_concurrency == 0 || running < max_concurrency;
  }

  // #MaxThreadsPerHotkey: at the registration's cap the event is dropped.
  // Queue-level coalescing (#MaxThreadsBuffer) already happened upstream in
  // the EventQueue under this policy's `overflow`.
  [[nodiscard]] Delivery admit_subscription(const std::uint32_t running,
                                            const std::uint32_t cap) const {
    if (cap == 0 || running < cap) return Delivery::Deliver;
    return Delivery::Drop;
  }

  // Repeat of an item that is already in flight: merged when the policy
  // coalesces (timer ticks), dropped otherwise.
  [[nodiscard]] Delivery admit_repeat(const bool in_flight) const {
    if (!in_flight) return Delivery::Deliver;
    return overflow == OverflowPolicy::CoalesceByKey ? Delivery::Buffer : Delivery::Drop;
  }

  // #Suspend / #SuspendExempt: a suspended dispatch delivers only to
  // registrations that opted out of suspension.
  [[nodiscard]] static bool dispatch_allowed(const bool suspended, const bool exempt) {
    return !suspended || exempt;
  }

  // #InputLevel: an event below the registration's level never reaches it.
  [[nodiscard]] static bool level_allowed(const std::uint32_t registration_level,
                                          const std::uint32_t event_level) {
    return event_level >= registration_level;
  }

  // #HotIfTimeout: a condition evaluation that used up its budget is not met
  // (AHK times the criterion out and refuses to fire the hotkey).
  [[nodiscard]] bool hot_if_met(const std::uint64_t elapsed_ms) const {
    return hot_if_timeout_ms == 0 || elapsed_ms <= hot_if_timeout_ms;
  }
};

}  // namespace rime::core
