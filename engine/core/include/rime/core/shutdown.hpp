#pragma once

#include "rime/core/trace.hpp"
#include "rime/core/types.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rime::core {

// Ordered, observable shutdown sequence. Advancing backwards is rejected so
// the documented order (reject input -> cancel -> uninstall hooks -> stop
// workers -> destroy UI/COM -> close JS) cannot be violated.
enum class ShutdownPhase : std::uint8_t {
  Running = 0,
  RejectingNewWork,
  CancellingTasks,
  UninstallingCallbacks,
  StoppingWorkers,
  DestroyingUi,
  ClosingJs,
  Completed,
  Failed,
};

const char* shutdown_phase_name(ShutdownPhase phase);

class ShutdownSequence final {
 public:
  explicit ShutdownSequence(std::shared_ptr<TraceSink> trace = {});

  // Moves the sequence forward (or repeats the current phase). Moving
  // backwards, or advancing after Completed/Failed, fails.
  // NOTE: to enter Failed, call fail() instead of advance(Failed) so the
  // failure reason is recorded; advance(Failed) is still accepted but drops
  // the reason. Enum semantics are intentionally unchanged.
  [[nodiscard]] Error advance(ShutdownPhase next, std::string subject = "runtime");
  [[nodiscard]] Error fail(std::string reason);

  // Registers runtime-owned references (hook, window procedure, COM
  // reference, JS callback) that must be released before unload succeeds.
  Error retain(std::string id);
  Error release(const std::string& id);
  [[nodiscard]] std::vector<std::string> outstanding() const;
  [[nodiscard]] bool idle() const;

  [[nodiscard]] ShutdownPhase phase() const;
  [[nodiscard]] bool completed() const;
  [[nodiscard]] bool failed() const;

 private:
  void trace(ShutdownPhase phase, const std::string& subject, const std::string& detail);

  mutable std::mutex mutex_;
  ShutdownPhase phase_{ShutdownPhase::Running};
  std::vector<std::string> outstanding_;
  std::shared_ptr<TraceSink> trace_;
  Sequence sequence_{0};
};

}  // namespace rime::core
