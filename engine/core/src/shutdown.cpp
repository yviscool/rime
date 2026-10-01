#include "rime/core/shutdown.hpp"

#include <algorithm>
#include <utility>

namespace rime::core {

const char* shutdown_phase_name(const ShutdownPhase phase) {
  switch (phase) {
    case ShutdownPhase::Running:
      return "running";
    case ShutdownPhase::RejectingNewWork:
      return "rejecting_new_work";
    case ShutdownPhase::CancellingTasks:
      return "cancelling_tasks";
    case ShutdownPhase::UninstallingCallbacks:
      return "uninstalling_callbacks";
    case ShutdownPhase::StoppingWorkers:
      return "stopping_workers";
    case ShutdownPhase::DestroyingUi:
      return "destroying_ui";
    case ShutdownPhase::ClosingJs:
      return "closing_js";
    case ShutdownPhase::Completed:
      return "completed";
    case ShutdownPhase::Failed:
      return "failed";
  }
  return "unknown";
}

ShutdownSequence::ShutdownSequence(std::shared_ptr<TraceSink> trace)
    : trace_(std::move(trace)) {}

Error ShutdownSequence::advance(const ShutdownPhase next, std::string subject) {
  ShutdownPhase entered = next;
  {
    std::lock_guard lock(mutex_);
    if (phase_ == next) return Error::none();
    if (phase_ == ShutdownPhase::Failed) {
      return {Error::Code::InvalidState, "shutdown already failed"};
    }
    if (phase_ == ShutdownPhase::Completed) {
      return {Error::Code::InvalidState, "shutdown already completed"};
    }
    if (static_cast<std::uint8_t>(next) < static_cast<std::uint8_t>(phase_)) {
      return {Error::Code::InvalidState,
              std::string("shutdown cannot move backwards from ") + shutdown_phase_name(phase_) +
                  " to " + shutdown_phase_name(next)};
    }
    phase_ = next;
    entered = phase_;
  }
  // Trace outside the lock: TraceSink::record is user code and may throw or
  // re-enter; holding mutex_ across it risks deadlock.
  // NOTE: prefer fail() over advance(Failed) so the reason is preserved.
  try {
    trace(entered, subject, "phase entered");
  } catch (...) {
  }
  return Error::none();
}

Error ShutdownSequence::fail(std::string reason) {
  std::lock_guard lock(mutex_);
  if (phase_ == ShutdownPhase::Completed) {
    return {Error::Code::InvalidState, "shutdown already completed"};
  }
  const std::string detail = reason.empty() ? "shutdown failed" : std::move(reason);
  phase_ = ShutdownPhase::Failed;
  trace(phase_, "runtime", detail);
  return Error::none();
}

Error ShutdownSequence::retain(std::string id) {
  if (id.empty()) return {Error::Code::InvalidContract, "reference id must not be empty"};
  std::lock_guard lock(mutex_);
  if (phase_ == ShutdownPhase::Completed || phase_ == ShutdownPhase::Failed) {
    return {Error::Code::InvalidState, "cannot retain references after shutdown finished"};
  }
  if (std::find(outstanding_.begin(), outstanding_.end(), id) != outstanding_.end()) {
    return {Error::Code::InvalidState, "reference already retained: " + id};
  }
  outstanding_.push_back(std::move(id));
  return Error::none();
}

Error ShutdownSequence::release(const std::string& id) {
  std::lock_guard lock(mutex_);
  const auto found = std::find(outstanding_.begin(), outstanding_.end(), id);
  if (found == outstanding_.end()) {
    return {Error::Code::InvalidState, "reference was not retained: " + id};
  }
  outstanding_.erase(found);
  return Error::none();
}

std::vector<std::string> ShutdownSequence::outstanding() const {
  std::lock_guard lock(mutex_);
  return outstanding_;
}

bool ShutdownSequence::idle() const {
  std::lock_guard lock(mutex_);
  return outstanding_.empty();
}

ShutdownPhase ShutdownSequence::phase() const {
  std::lock_guard lock(mutex_);
  return phase_;
}

bool ShutdownSequence::completed() const {
  std::lock_guard lock(mutex_);
  return phase_ == ShutdownPhase::Completed;
}

bool ShutdownSequence::failed() const {
  std::lock_guard lock(mutex_);
  return phase_ == ShutdownPhase::Failed;
}

void ShutdownSequence::trace(const ShutdownPhase phase, const std::string& subject,
                             const std::string& detail) {
  if (!trace_) return;
  const std::string entry = std::string(shutdown_phase_name(phase)) + ": " + detail;
  trace_->record({0, TraceKind::StateChanged, subject, entry});
}

}  // namespace rime::core
