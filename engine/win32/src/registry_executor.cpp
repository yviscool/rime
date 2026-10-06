#include "rime/win32/registry_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace rime::win32 {
namespace {

using Result = rime::action::Result;
using Error = rime::core::Error;
using Code = rime::core::Error::Code;
namespace json = rime::core::json;

// Fixed English payload texts. The Action result is what the trace, the JS
// promise and docs/api/registry.md all read, so nothing here interpolates
// localised Win32 text; only Win32 failures carry a status number.
constexpr const char* kPayloadPrefix = "registry.write payload ";

Result fail(const rime::action::Action& action, const Code code, std::string message) {
  return {action.id, false, false, message, {code, message}, {}};
}

Result cancelled(const rime::action::Action& action, const char* message) {
  return {action.id, false, true, message, {Code::Cancelled, message}, {}};
}

// JSON carries integers as doubles, so the contract's 0..2^63-1 range is
// exactly the doubles that are integral and below 2^63. Values above 2^53
// are accepted but already rounded by JSON parsing (documented in
// docs/api/registry.md).
bool json_uint64(const json::Value& value, std::uint64_t& out) {
  if (!value.is_number()) return false;
  const double number = value.as_number();
  if (!std::isfinite(number) || std::trunc(number) != number) return false;
  // 9223372036854775808.0 is 2^63, exactly representable as a double; at or
  // above it the value no longer fits the payload contract's range.
  if (!(number >= 0.0) || number >= 9223372036854775808.0) return false;
  out = static_cast<std::uint64_t>(number);
  return true;
}

// Validates `value` against `type` and fills `out` without touching the OS.
// Every rejection names the field and the shape it needs, so a script never
// has to guess which part of the payload was wrong.
Error parse_value(const std::string& type, const json::Value& value, RegValue& out) {
  out = RegValue{};
  out.type = type;
  const std::string where = std::string(kPayloadPrefix) + "value ";

  if (type == "sz" || type == "expand_sz") {
    if (!value.is_string()) {
      return {Code::InvalidContract, where + "must be a string for type " + type};
    }
    out.value_sz = value.as_string();
    return Error::none();
  }

  if (type == "dword" || type == "qword") {
    if (!value.is_number()) {
      return {Code::InvalidContract, where + "must be a number for type " + type};
    }
    std::uint64_t integer = 0;
    if (!json_uint64(value, integer)) {
      return {Code::InvalidContract, where +
                                          "must be an integer in 0..9223372036854775807 for type " +
                                          type};
    }
    out.value_int = integer;
    return Error::none();
  }

  if (type == "multi_sz") {
    if (!value.is_array()) {
      return {Code::InvalidContract, where + "must be an array for type multi_sz"};
    }
    for (const json::Value& item : value.as_array()) {
      if (!item.is_string()) {
        return {Code::InvalidContract, where + "elements must be strings for type multi_sz"};
      }
      out.value_multi.push_back(item.as_string());
    }
    return Error::none();
  }

  if (type == "binary") {
    if (!value.is_array()) {
      return {Code::InvalidContract, where + "must be an array for type binary"};
    }
    for (const json::Value& item : value.as_array()) {
      std::uint64_t byte = 0;
      if (!json_uint64(item, byte) || byte > 255) {
        return {Code::InvalidContract, where + "elements must be integers 0..255 for type binary"};
      }
      out.value_bin.push_back(static_cast<std::uint8_t>(byte));
    }
    return Error::none();
  }

  return {Code::InvalidContract, std::string(kPayloadPrefix) + "does not support type: " + type};
}

}  // namespace

rime::action::Result RegistryExecutor::execute(const rime::action::Action& action,
                                                rime::core::CancellationToken cancellation) {
  if (const auto lane_error = rime::core::require_lane(rime::core::Lane::Worker);
      !lane_error.ok()) {
    return fail(action, lane_error.code, lane_error.message);
  }
  if (action.type != "registry.write") {
    return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
  }
  if (action.target.kind != "registry") {
    return fail(action, Code::InvalidContract,
                "registry.write requires target kind 'registry', got: " + action.target.kind);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  const auto payload = json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract, std::string(kPayloadPrefix) + "must be a JSON object");
  }
  const json::Value* op_value = payload.value->find("op");
  if (!op_value || !op_value->is_string()) {
    return fail(action, Code::InvalidContract, std::string(kPayloadPrefix) + "requires a string op");
  }
  const std::string op = op_value->as_string();
  const json::Value* key_value = payload.value->find("key");
  if (!key_value || !key_value->is_string() || key_value->as_string().empty()) {
    return fail(action, Code::InvalidContract,
                std::string(kPayloadPrefix) + "requires a non-empty string key");
  }
  const std::string key = key_value->as_string();
  // The target id is checked against the payload rather than trusted from it:
  // a queued Action is inspected by target alone, so the two must agree.
  if (action.target.id != key) {
    return fail(action, Code::InvalidContract,
                std::string(kPayloadPrefix) + "target id must match the payload key");
  }
  const json::Value* name_value = payload.value->find("name");
  if (name_value && !name_value->is_string()) {
    return fail(action, Code::InvalidContract, std::string(kPayloadPrefix) + "name must be a string");
  }
  const std::string name = name_value ? name_value->as_string() : std::string();
  const json::Value* type_value = payload.value->find("type");
  const json::Value* value_value = payload.value->find("value");

  rime::core::Error service_error = rime::core::Error::none();
  if (op == "set") {
    if (!type_value || !type_value->is_string()) {
      return fail(action, Code::InvalidContract, std::string(kPayloadPrefix) + "requires a string type");
    }
    if (!value_value) {
      return fail(action, Code::InvalidContract,
                  std::string(kPayloadPrefix) + "requires a value for op set");
    }
    RegValue reg_value;
    if (const auto error = parse_value(type_value->as_string(), *value_value, reg_value);
        !error.ok()) {
      return fail(action, error.code, error.message);
    }
    service_error = service_.write_value(key, name, reg_value);
  } else if (op == "createKey") {
    if (name_value || value_value || type_value) {
      return fail(action, Code::InvalidContract,
                  std::string(kPayloadPrefix) +
                      "op createKey does not accept name, value or type");
    }
    service_error = service_.create_key(key);
  } else if (op == "delete") {
    if (value_value || type_value) {
      return fail(action, Code::InvalidContract,
                  std::string(kPayloadPrefix) + "op delete does not accept value or type");
    }
    service_error = service_.delete_value(key, name);
  } else if (op == "deleteKey") {
    if (name_value || value_value || type_value) {
      return fail(action, Code::InvalidContract,
                  std::string(kPayloadPrefix) +
                      "op deleteKey does not accept name, value or type");
    }
    service_error = service_.delete_key(key);
  } else {
    return fail(action, Code::InvalidContract,
                std::string(kPayloadPrefix) + "does not support op: " + op);
  }

  if (!service_error.ok()) return fail(action, service_error.code, service_error.message);
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled after execution");
  }

  json::Value result = json::Value::object();
  result.set("key", json::Value::string(key));
  result.set("op", json::Value::string(op));
  return {action.id, true, false, "registry updated", {}, std::move(result)};
}

}  // namespace rime::win32
