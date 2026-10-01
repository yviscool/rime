#pragma once

#include "rime/core/types.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace rime::core {

enum class TraceKind : std::uint8_t { EventAccepted, ActionStarted, ActionFinished, StateChanged };

struct TraceEntry {
  Sequence sequence{0};
  TraceKind kind{TraceKind::EventAccepted};
  std::string subject;
  std::string detail;
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
