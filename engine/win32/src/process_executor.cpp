#include "rime/win32/process_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"

#include "utf.hpp"

#include <charconv>
#include <cmath>
#include <string>

namespace rime::win32 {
namespace {

using Result = rime::action::Result;
using Code = rime::core::Error::Code;
namespace json = rime::core::json;

Result fail(const rime::action::Action& action, const Code code, std::string message) {
  return {action.id, false, false, message, {code, message}, {}};
}

Result cancelled(const rime::action::Action& action, const std::string& message) {
  return {action.id, false, true, message, {Code::Cancelled, message}, {}};
}

bool parse_pid(const std::string& text, std::uint32_t& out) {
  if (text.empty()) return false;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, out);
  return parsed.ec == std::errc{} && parsed.ptr == end && out != 0;
}

// JSON numbers are doubles, so exactness against trunc plus a uint32 bound
// keeps the cast to a pid defined.
bool is_pid_number(const double value) {
  return value == std::trunc(value) && value >= 1.0 && value <= 4294967295.0;
}

Result run_launch(const rime::action::Action& action, ProcessService& service,
                  const rime::core::CancellationToken& cancellation) {
  if (action.target.id != "new") {
    return fail(action, Code::InvalidContract, "process.launch target id must be 'new'");
  }
  const auto payload = rime::core::json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract, "process.launch payload must be a JSON object");
  }
  const rime::core::json::Value* command = payload.value->find("command");
  if (!command || !command->is_string() || command->as_string().empty()) {
    return fail(action, Code::InvalidContract,
                "process.launch payload requires a non-empty string command");
  }
  const rime::core::json::Value* args = payload.value->find("args");
  if (args && !args->is_string()) {
    return fail(action, Code::InvalidContract, "process.launch payload args must be a string");
  }
  const rime::core::json::Value* working_dir = payload.value->find("workingDir");
  if (working_dir && !working_dir->is_string()) {
    return fail(action, Code::InvalidContract,
                "process.launch payload workingDir must be a string");
  }

  LaunchSpec spec;
  spec.executable = from_utf8(command->as_string());
  if (args && args->is_string()) spec.arguments = from_utf8(args->as_string());
  if (working_dir && working_dir->is_string()) spec.working_dir = from_utf8(working_dir->as_string());

  std::uint32_t pid = 0;
  if (const auto launch_error = service.launch(spec, pid); !launch_error.ok()) {
    return fail(action, launch_error.code, launch_error.message);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled after execution");
  }
  rime::core::json::Value value = rime::core::json::Value::object();
  value.set("pid", rime::core::json::Value::number(static_cast<double>(pid)));
  return {action.id, true, false, "launched process " + std::to_string(pid), {}, std::move(value)};
}

Result run_terminate(const rime::action::Action& action, ProcessService& service,
                     const rime::core::CancellationToken& cancellation) {
  std::uint32_t pid = 0;
  if (!parse_pid(action.target.id, pid)) {
    return fail(action, Code::InvalidContract,
                "process.terminate target id must be a positive integer");
  }
  if (const auto terminate_error = service.terminate(pid, 1); !terminate_error.ok()) {
    return fail(action, terminate_error.code, terminate_error.message);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled after execution");
  }
  rime::core::json::Value value = rime::core::json::Value::object();
  value.set("pid", rime::core::json::Value::number(static_cast<double>(pid)));
  return {action.id, true, false, "terminated process " + std::to_string(pid), {}, std::move(value)};
}

// process.set.priority (capability process.manage): target id and payload.pid
// are the same pid, spelled twice on the wire so a queued action stays
// self-describing; a mismatch is a contract failure, not a silent preference.
Result run_set_priority(const rime::action::Action& action, ProcessService& service,
                        const rime::core::CancellationToken& cancellation) {
  std::uint32_t pid = 0;
  if (!parse_pid(action.target.id, pid)) {
    return fail(action, Code::InvalidContract,
                "process.set.priority target id must be a positive integer");
  }
  const auto payload = json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract,
                "process.set.priority payload must be a JSON object");
  }
  const json::Value* payload_pid = payload.value->find("pid");
  if (!payload_pid || !payload_pid->is_number() || !is_pid_number(payload_pid->as_number())) {
    return fail(action, Code::InvalidContract,
                "process.set.priority payload requires a positive integer pid");
  }
  if (static_cast<std::uint32_t>(payload_pid->as_number()) != pid) {
    return fail(action, Code::InvalidContract,
                "process.set.priority payload pid must match the target id");
  }
  const json::Value* priority = payload.value->find("priority");
  if (!priority || !priority->is_string() || priority->as_string().empty()) {
    return fail(action, Code::InvalidContract,
                "process.set.priority payload requires a non-empty string priority");
  }
  // Enum membership is checked here (and again by the service) so an unknown
  // spelling fails as a contract error before the process is opened.
  if (!is_valid_priority(priority->as_string())) {
    return fail(action, Code::InvalidContract,
                "process.set.priority payload priority must be one of idle, belowNormal, normal, "
                "aboveNormal, high, realtime");
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }
  if (const auto priority_error = service.set_priority(pid, priority->as_string());
      !priority_error.ok()) {
    return fail(action, priority_error.code, priority_error.message);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled after execution");
  }
  json::Value value = json::Value::object();
  value.set("pid", json::Value::number(static_cast<double>(pid)));
  return {action.id, true, false, "priority updated", {}, std::move(value)};
}

// process.runas (capability process.runas): every field is type-checked and
// the password is only ever required to be a string - no branch below, and no
// error message, can interpolate its value (see ProcessService::run_as for the
// Trace basis).
Result run_run_as(const rime::action::Action& action, ProcessService& service,
                  const rime::core::CancellationToken& cancellation) {
  if (action.target.id != "new") {
    return fail(action, Code::InvalidContract, "process.runas target id must be 'new'");
  }
  const auto payload = json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract, "process.runas payload must be a JSON object");
  }
  const json::Value* user = payload.value->find("user");
  if (!user || !user->is_string() || user->as_string().empty()) {
    return fail(action, Code::InvalidContract,
                "process.runas payload requires a non-empty string user");
  }
  const json::Value* password = payload.value->find("password");
  if (!password || !password->is_string()) {
    return fail(action, Code::InvalidContract, "process.runas payload requires a string password");
  }
  const json::Value* executable = payload.value->find("executable");
  if (!executable || !executable->is_string() || executable->as_string().empty()) {
    return fail(action, Code::InvalidContract,
                "process.runas payload requires a non-empty string executable");
  }
  const json::Value* domain = payload.value->find("domain");
  if (domain && !domain->is_string()) {
    return fail(action, Code::InvalidContract, "process.runas payload domain must be a string");
  }
  const json::Value* arguments = payload.value->find("arguments");
  if (arguments && !arguments->is_string()) {
    return fail(action, Code::InvalidContract, "process.runas payload arguments must be a string");
  }
  const json::Value* working_dir = payload.value->find("workingDir");
  if (working_dir && !working_dir->is_string()) {
    return fail(action, Code::InvalidContract,
                "process.runas payload workingDir must be a string");
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  RunAsSpec spec;
  spec.user = from_utf8(user->as_string());
  spec.password = from_utf8(password->as_string());
  if (domain && domain->is_string()) spec.domain = from_utf8(domain->as_string());
  spec.executable = from_utf8(executable->as_string());
  if (arguments && arguments->is_string()) spec.arguments = from_utf8(arguments->as_string());
  if (working_dir && working_dir->is_string()) spec.working_dir = from_utf8(working_dir->as_string());

  std::uint32_t pid = 0;
  if (const auto run_error = service.run_as(spec, pid); !run_error.ok()) {
    return fail(action, run_error.code, run_error.message);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled after execution");
  }
  json::Value value = json::Value::object();
  value.set("pid", json::Value::number(static_cast<double>(pid)));
  return {action.id, true, false, "process created", {}, std::move(value)};
}

// process.shutdown (capability process.shutdown): mode enum, force type and
// the 0..600 timeoutSec bound are all rejected here, before the service is
// called, so no invalid request can ever reach a Win32 shutdown API.
Result run_shutdown(const rime::action::Action& action, ProcessService& service,
                    const rime::core::CancellationToken& cancellation) {
  if (action.target.id != "system") {
    return fail(action, Code::InvalidContract, "process.shutdown target id must be 'system'");
  }
  const auto payload = json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract, "process.shutdown payload must be a JSON object");
  }
  const json::Value* mode = payload.value->find("mode");
  if (!mode || !mode->is_string()) {
    return fail(action, Code::InvalidContract, "process.shutdown payload requires a string mode");
  }
  const std::string& mode_text = mode->as_string();
  if (mode_text != "logoff" && mode_text != "shutdown" && mode_text != "reboot" &&
      mode_text != "poweroff" && mode_text != "hibernate") {
    return fail(action, Code::InvalidContract,
                "process.shutdown payload mode must be one of logoff, shutdown, reboot, poweroff, "
                "hibernate");
  }
  const json::Value* force = payload.value->find("force");
  if (force && !force->is_bool()) {
    return fail(action, Code::InvalidContract, "process.shutdown payload force must be a boolean");
  }
  const json::Value* timeout = payload.value->find("timeoutSec");
  if (timeout) {
    if (!timeout->is_number()) {
      return fail(action, Code::InvalidContract,
                  "process.shutdown payload timeoutSec must be an integer between 0 and 600");
    }
    const double seconds = timeout->as_number();
    if (seconds != std::trunc(seconds) || seconds < 0.0 || seconds > 600.0) {
      return fail(action, Code::InvalidContract,
                  "process.shutdown payload timeoutSec must be an integer between 0 and 600");
    }
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  ShutdownSpec spec;
  spec.mode = mode_text;
  spec.force = force && force->as_bool();
  spec.timeout_sec =
      timeout ? static_cast<std::uint32_t>(timeout->as_number()) : 0u;

  if (const auto shutdown_error = service.shutdown_system(spec); !shutdown_error.ok()) {
    return fail(action, shutdown_error.code, shutdown_error.message);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled after execution");
  }
  json::Value value = json::Value::object();
  value.set("mode", json::Value::string(mode_text));
  return {action.id, true, false, "shutdown requested", {}, std::move(value)};
}

}  // namespace

rime::action::Result ProcessExecutor::execute(const rime::action::Action& action,
                                              rime::core::CancellationToken cancellation) {
  if (const auto lane_error = rime::core::require_lane(rime::core::Lane::Worker);
      !lane_error.ok()) {
    return fail(action, lane_error.code, lane_error.message);
  }
  if (action.target.kind != "process") {
    return fail(action, Code::InvalidContract,
                "process actions require target kind 'process', got: " + action.target.kind);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }
  // No per-call timeout here by design: ProcessService calls are synchronous
  // and short (no UI queue wait), so the kernel's pre-dispatch and
  // post-commit deadline checks are the timeout enforcement for these actions.
  // run_as with LOGON_WITH_PROFILE is the documented exception: loading a
  // user profile can hold the worker lane for seconds.
  if (action.type == "process.launch") {
    return run_launch(action, service_, cancellation);
  }
  if (action.type == "process.terminate") {
    return run_terminate(action, service_, cancellation);
  }
  if (action.type == "process.set.priority") {
    return run_set_priority(action, service_, cancellation);
  }
  if (action.type == "process.runas") {
    return run_run_as(action, service_, cancellation);
  }
  if (action.type == "process.shutdown") {
    return run_shutdown(action, service_, cancellation);
  }
  return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
}

}  // namespace rime::win32
