#include "rime/win32/process_executor.hpp"

#include "rime/core/json.hpp"

#include "utf.hpp"

#include <charconv>
#include <string>

namespace rime::win32 {
namespace {

using Result = rime::action::Result;
using Code = rime::core::Error::Code;

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

}  // namespace

rime::action::Result ProcessExecutor::execute(const rime::action::Action& action,
                                              rime::core::CancellationToken cancellation) {
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
  if (action.type == "process.launch") {
    return run_launch(action, service_, cancellation);
  }
  if (action.type == "process.terminate") {
    return run_terminate(action, service_, cancellation);
  }
  return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
}

}  // namespace rime::win32
