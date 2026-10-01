#include "rime/win32/clipboard_executor.hpp"

#include "rime/core/json.hpp"

#include <string>

namespace rime::win32 {
namespace {

using Result = rime::action::Result;
using Code = rime::core::Error::Code;

Result fail(const rime::action::Action& action, const Code code, std::string message) {
  return {action.id, false, false, message, {code, message}, {}};
}

}  // namespace

rime::action::Result ClipboardExecutor::execute(const rime::action::Action& action,
                                                rime::core::CancellationToken cancellation) {
  if (action.type != "clipboard.write") {
    return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
  }
  if (action.target.kind != "clipboard") {
    return fail(action, Code::InvalidContract,
                "clipboard.write requires target kind 'clipboard', got: " + action.target.kind);
  }
  if (action.target.id != "default") {
    return fail(action, Code::InvalidContract, "clipboard.write target id must be 'default'");
  }
  if (cancellation.cancelled()) {
    return {action.id, false, true, "action was cancelled before execution",
            {Code::Cancelled, "action was cancelled before execution"}, {}};
  }

  const auto payload = rime::core::json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract, "clipboard.write payload must be a JSON object");
  }
  const rime::core::json::Value* text = payload.value->find("text");
  if (!text || !text->is_string()) {
    return fail(action, Code::InvalidContract, "clipboard.write payload requires a string text");
  }

  // No per-call timeout here by design: the clipboard write is synchronous
  // and short (no UI queue wait), so the kernel's pre-dispatch and
  // post-commit deadline checks are the timeout enforcement for this action.
  const std::string value = text->as_string();
  if (const auto write_error = service_.write_text(value); !write_error.ok()) {
    return fail(action, write_error.code, write_error.message);
  }
  if (cancellation.cancelled()) {
    return {action.id, false, true, "action was cancelled after execution",
            {Code::Cancelled, "action was cancelled after execution"}, {}};
  }

  rime::core::json::Value result_value = rime::core::json::Value::object();
  result_value.set("text", rime::core::json::Value::string(value));
  return {action.id, true, false, "clipboard updated", {}, std::move(result_value)};
}

}  // namespace rime::win32
