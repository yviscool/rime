#include "rime/win32/clipboard_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"
#include "rime/win32/action_contract.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace rime::win32 {
namespace {

using Code = rime::core::Error::Code;
namespace contract = rime::win32::contract;
using contract::fail;

// The types this executor dispatches. Both are clipboard mutations, both
// claim the default target and windows.clipboard.write, and both run on the
// worker lane: the clipboard swap is short and synchronous, exactly as the
// text write is.
const std::unordered_set<std::string>& clipboard_action_types() {
  static const std::unordered_set<std::string> types = {"clipboard.write",
                                                        "clipboard.restore"};
  return types;
}

}  // namespace

rime::action::Result ClipboardExecutor::execute(const rime::action::Action& action,
                                                rime::core::CancellationToken cancellation) {
  if (const auto bad = contract::lane(action, rime::core::Lane::Worker)) return *bad;
  if (!clipboard_action_types().contains(action.type)) return contract::unsupported(action);
  if (const auto bad = contract::target_kind(action, action.type.c_str(), "clipboard")) return *bad;
  if (action.target.id != "default") {
    return fail(action, Code::InvalidContract,
                action.type + " target id must be 'default'");
  }
  if (const auto bad = contract::cancel_before(action, cancellation)) return *bad;

  const auto payload = rime::core::json::parse(action.payload);
  if (const auto bad = contract::object_payload(action, payload, action.type + " payload ")) {
    return *bad;
  }

  if (action.type == "clipboard.write") {
    const rime::core::json::Value* text = payload.value->find("text");
    if (!text || !text->is_string()) {
      return fail(action, Code::InvalidContract,
                  "clipboard.write payload requires a string text");
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

  // clipboard.restore: an opaque ClipboardAll blob, validated byte by byte
  // here so a non-integer or out-of-range element is an invalid_contract
  // before the clipboard is emptied.
  const rime::core::json::Value* bytes = payload.value->find("bytes");
  if (!bytes || !bytes->is_array()) {
    return fail(action, Code::InvalidContract,
                "clipboard.restore payload requires a bytes array");
  }
  std::vector<std::uint8_t> blob;
  blob.reserve(bytes->size());
  for (const rime::core::json::Value& item : bytes->as_array()) {
    const double number = item.as_number();
    if (!item.is_number() || !contract::in_integral_range(number, 0.0, 255.0)) {
      return fail(action, Code::InvalidContract,
                  "clipboard.restore payload bytes must be integers in 0..255");
    }
    blob.push_back(static_cast<std::uint8_t>(number));
  }
  // Same no-timeout rationale as the write above.
  std::uint32_t restored = 0;
  if (const auto restore_error = service_.restore_all(blob, restored); !restore_error.ok()) {
    return fail(action, restore_error.code, restore_error.message);
  }
  if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;

  rime::core::json::Value result_value = rime::core::json::Value::object();
  result_value.set("formats", rime::core::json::Value::number(static_cast<double>(restored)));
  return {action.id, true, false, "clipboard restored", {}, std::move(result_value)};
}

}  // namespace rime::win32
