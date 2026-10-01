#include "rime/action/kernel.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace rime::action {
namespace {

// Wall-clock semantics: deadlines are absolute Unix-epoch milliseconds and
// are read from system_clock (not steady_clock) so they stay comparable
// with the Action contract's deadlineUnixMs across processes.
std::int64_t now_unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Expired means deadline <= now: an action whose deadline equals the current
// time is already out of budget (no test pins the ==now boundary; kernel
// and executors share this <=now rule).
bool deadline_expired(const Action& action) {
  return static_cast<std::int64_t>(action.deadline_unix_ms) <= now_unix_ms();
}

// Migration hint for the pre-alignment capability namespace (commit
// a449766). Only renames with direct historical evidence are mapped
// exactly; other dotted names get a heuristic hint so no phantom old name
// is invented.
std::string denied_message(const std::string& capability) {
  std::string message = "required capability was not granted: " + capability;
  if (capability == "window.write") {
    message += " (renamed to windows.window.write)";
  } else if (capability == "clipboard.write") {
    message += " (renamed to windows.clipboard.write)";
  } else if (capability.starts_with("window.")) {
    message += " (did you mean windows.window.*?)";
  } else if (capability.starts_with("clipboard.")) {
    message += " (did you mean windows.clipboard.*?)";
  }
  return message;
}

}  // namespace

Kernel::Kernel(std::shared_ptr<const CapabilityPolicy> policy,
               std::shared_ptr<rime::core::TraceSink> trace)
    : policy_(std::move(policy)), trace_(std::move(trace)) {}

rime::core::Error Kernel::register_executor(std::string action_type,
                                            std::shared_ptr<Executor> executor) {
  if (action_type.empty() || !executor) {
    return {rime::core::Error::Code::InvalidContract,
            "executor registration requires an action type and executor"};
  }
  std::lock_guard lock(mutex_);
  if (executors_.contains(action_type)) {
    return {rime::core::Error::Code::InvalidContract,
            "an executor is already registered for this action type"};
  }
  executors_.emplace(std::move(action_type), std::move(executor));
  return rime::core::Error::none();
}

Result Kernel::execute(const Action& action, rime::core::CancellationToken cancellation) {
  using Code = rime::core::Error::Code;
  constexpr rime::core::ActionId max_safe_integer = 9'007'199'254'740'991ULL;
  const bool invalid_precondition = std::any_of(
      action.preconditions.begin(), action.preconditions.end(),
      [](const Precondition& precondition) { return precondition.type.empty(); });
  if (action.schema_version != 1 || action.id == 0 || action.source.kind.empty() ||
      action.source.id.empty() || action.type.empty() || action.capability.empty() ||
      action.target.kind.empty() || action.target.id.empty() || action.deadline_unix_ms == 0 ||
      action.id > max_safe_integer || action.deadline_unix_ms > max_safe_integer ||
      action.parent_action_id > max_safe_integer || invalid_precondition) {
    return fail(action, Code::InvalidContract,
                "action requires v1, identity, source, type, capability, target and deadline");
  }
  if (cancellation.cancelled()) {
    return fail(action, Code::Cancelled, "action was cancelled before execution");
  }
  // Deadline is absolute and enforced at dispatch: an expired action never
  // reaches an executor and reports Timeout (not ExecutionFailed).
  if (deadline_expired(action)) {
    return fail(action, Code::Timeout, "action deadline exceeded");
  }
  if (!policy_ || !policy_->allows(action.capability)) {
    return fail(action, Code::CapabilityDenied, denied_message(action.capability));
  }

  std::shared_ptr<Executor> executor;
  {
    std::lock_guard lock(mutex_);
    const auto found = executors_.find(action.type);
    if (found == executors_.end()) {
      return fail(action, Code::Unsupported, "no executor registered for action type");
    }
    executor = found->second;
  }

  record(action, rime::core::TraceKind::ActionStarted, "execution started");
  Result result;
  try {
    result = executor->execute(action, cancellation);
  } catch (const std::exception& exception) {
    result = {action.id, false, false, exception.what(),
              {Code::ExecutionFailed, exception.what()}};
  } catch (...) {
    result = {action.id, false, false, "executor threw an unknown exception",
              {Code::ExecutionFailed, "executor threw an unknown exception"}};
  }

  result.id = action.id;
  // Fixed priority: cancellation wins over deadline expiry so a
  // cancel-vs-timeout race reports Cancelled deterministically instead of
  // flapping between Cancelled and Timeout across runs.
  if (cancellation.cancelled()) {
    result.succeeded = false;
    result.cancelled = true;
    result.error = {Code::Cancelled,
                    "action cancelled during execution (after commit; side effects may have "
                    "occurred)"};
    result.detail = result.error.message;
  } else if (result.succeeded && deadline_expired(action)) {
    // Post-commit deadline recheck: the executor already ran, so a success
    // that overran the deadline is rewritten to Timeout. The side effects
    // cannot be undone, hence the note below.
    result.succeeded = false;
    result.error = {Code::Timeout,
                    "action deadline exceeded after commit; side effects may have occurred"};
    result.detail = result.error.message;
  } else if (!result.succeeded && result.error.ok()) {
    result.error = {Code::ExecutionFailed, result.detail.empty() ? "action failed" : result.detail};
  }
  record(action, rime::core::TraceKind::ActionFinished,
         result.succeeded ? "succeeded" : (result.cancelled ? "cancelled" : result.error.message));
  return result;
}

bool Kernel::allows(const std::string& capability) const {
  return policy_ && policy_->allows(capability);
}

Result Kernel::fail(const Action& action, const rime::core::Error::Code code,
                    std::string message) {
  Result result{action.id, false, code == rime::core::Error::Code::Cancelled, message,
                {code, std::move(message)}};
  record(action, rime::core::TraceKind::ActionFinished, result.error.message);
  return result;
}

void Kernel::record(const Action& action, const rime::core::TraceKind kind, std::string detail) {
  // TraceSink::record may throw (user-provided sink); tracing must never
  // propagate out of the action pipeline, so failures are swallowed and every
  // action still yields exactly one Result instead of throwing.
  try {
    if (trace_) {
      trace_->record({action.id, kind, action.type, std::move(detail)});
    }
  } catch (...) {
  }
}

}  // namespace rime::action
