#pragma once

#include "rime/core/types.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace rime::core {

// EventDispatch* marks Runtime handler dispatch: events are not actions and
// never carry an action id. ActionStarted/ActionFinished come only from the
// Action Kernel executing an action. ActionAccepted/ActionRefused come from
// the action Dispatcher's queue decisions: Accepted when an action is
// queued, Refused when it never reaches an executor (queue full, dispatcher
// closed, superseded by a newer same-key action, dropped by queue policy or
// cancelled before execute). StateChanged covers component state flips
// (runtime start/stop, dispatcher close).
enum class TraceKind : std::uint8_t {
  EventAccepted,
  EventDispatchStarted,
  EventDispatchFinished,
  ActionStarted,
  ActionFinished,
  StateChanged,
  ActionAccepted,
  ActionRefused
};

// Draws the next process-wide trace sequence number. Every TraceSink draws
// from this single counter so entries emitted by different producers
// (runtime, kernel, shutdown) share one totally ordered sequence space.
inline Sequence next_trace_sequence() {
  static std::atomic<Sequence> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

// Producers leave `sequence` at 0: the sink assigns it while holding its own
// lock so `snapshot()` order equals emission order even when producers race
// on different threads. `action_id` is the Action identity on every
// Action* entry (Started/Finished/Accepted/Refused) and 0 for state/event
// entries.
//
// Action envelope (filled by the Action Kernel and Dispatcher on action
// entries; empty/0 on event/state entries): `capability` is the authority
// the action required; `subject` doubles as the executor registration key
// (executors register per action type); `result_code` is the contract
// error-code name of the outcome ("none" for success), empty on Started and
// Accepted because no result exists yet; `duration_ms` measures executor
// wall time (steady_clock) and stays 0 on Started, Accepted, Refused and on
// pre-dispatch failures that never reach an executor.
struct TraceEntry {
  Sequence sequence{0};
  TraceKind kind{TraceKind::EventAccepted};
  std::string subject;
  std::string detail;
  std::uint64_t action_id{0};
  std::string capability;
  std::string result_code;
  std::uint64_t duration_ms{0};
};

class TraceSink {
 public:
  virtual ~TraceSink() = default;
  virtual void record(TraceEntry entry) = 0;
};

class InMemoryTrace final : public TraceSink {
 public:
  void record(TraceEntry entry) override {
    std::lock_guard lock(mutex_);
    entry.sequence = next_trace_sequence();
    entries_.push_back(std::move(entry));
  }

  [[nodiscard]] std::vector<TraceEntry> snapshot() const {
    std::lock_guard lock(mutex_);
    return entries_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<TraceEntry> entries_;
};

}  // namespace rime::core
