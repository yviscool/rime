#include "rime/win32/clipboard_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"
#include "rime/win32/action_contract.hpp"

#include <string>

namespace rime::win32 {
namespace {

using Code = rime::core::Error::Code;
namespace contract = rime::win32::contract;
using contract::fail;

}  // namespace

rime::action::Result ClipboardExecutor::execute(const rime::action::Action& action,
                                                rime::core::CancellationToken cancellation) {
  if (const auto bad = contract::lane(action, rime::core::Lane::Worker)) return *bad;
  if (const auto bad = contract::action_type(action, "clipboard.write")) return *bad;
  if (const auto bad = contract::target_kind(action, "clipboard.write", "clipboard")) return *bad;
  if (action.target.id != "default") {
    return fail(action, Code::InvalidContract, "clipboard.write target id must be 'default'");
  }
  if (const auto bad = contract::cancel_before(action, cancellation)) return *bad;

  const auto payload = rime::core::json::parse(action.payload);
  if (const auto bad = contract::object_payload(action, payload, "clipboard.write payload ")) {
    return *bad;
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
  if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;

  rime::core::json::Value result_value = rime::core::json::Value::object();
  result_value.set("text", rime::core::json::Value::string(value));
  return {action.id, true, false, "clipboard updated", {}, std::move(result_value)};
}

}  // namespace rime::win32
