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
// Action Kernel and always carry the executing action's id.
enum class TraceKind : std::uint8_t {
  EventAccepted,
  EventDispatchStarted,
  EventDispatchFinished,
  ActionStarted,
  ActionFinished,
  StateChanged
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
// on different threads. `action_id` is the Action identity for
// ActionStarted/ActionFinished entries and 0 for state/event entries.
struct TraceEntry {
  Sequence sequence{0};
  TraceKind kind{TraceKind::EventAccepted};
  std::string subject;
  std::string detail;
  std::uint64_t action_id{0};
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
