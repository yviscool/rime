#include "rime/action/codec.hpp"

#include "rime/core/json.hpp"

#include <cmath>
#include <initializer_list>
#include <utility>

namespace rime::action {
namespace {

using rime::core::Error;
using rime::core::json::Value;

constexpr double k_max_safe_integer = 9007199254740991.0;
constexpr double k_min_safe_integer = 1.0;

Error invalid(std::string message) {
  return {Error::Code::InvalidContract, std::move(message)};
}

bool is_safe_integer(const Value& value) {
  if (!value.is_number()) return false;
  const double number = value.as_number();
  return number == std::floor(number) && number >= k_min_safe_integer && number <= k_max_safe_integer;
}

bool has_only_keys(const Value& object, std::initializer_list<std::string_view> allowed,
                   std::string_view what, std::string& error) {
  for (const auto& member : object.as_object()) {
    bool found = false;
    for (const std::string_view key : allowed) {
      if (member.first == key) {
        found = true;
        break;
      }
    }
    if (!found) {
      error = std::string(what) + ": unexpected field '" + member.first + "'";
      return false;
    }
  }
  return true;
}

bool require_object(const Value* value, std::string_view what, std::string& error) {
  if (!value || !value->is_object()) {
    error = std::string(what) + ": required object field is missing or not an object";
    return false;
  }
  return true;
}

bool require_string(const Value* value, bool allow_empty, std::string_view what, std::string& error) {
  if (!value || !value->is_string() || (!allow_empty && value->as_string().empty())) {
    error = std::string(what) + ": required string field is missing"
            + (allow_empty ? "" : " or empty");
    return false;
  }
  return true;
}

bool decode_identity(const Value& value, Identity& out, std::string_view what, std::string& error) {
  if (!value.is_object()) {
    error = std::string(what) + ": must be an object";
    return false;
  }
  if (!has_only_keys(value, {"kind", "id"}, what, error)) return false;
  const Value* kind = value.find("kind");
  const Value* id = value.find("id");
  if (!require_string(kind, false, std::string(what) + ".kind", error)) return false;
  if (!require_string(id, false, std::string(what) + ".id", error)) return false;
  out = {kind->as_string(), id->as_string()};
  return true;
}

bool decode_precondition(const Value& value, Precondition& out, std::string_view what,
                         std::string& error) {
  if (!value.is_object()) {
    error = std::string(what) + ": must be an object";
    return false;
  }
  if (!has_only_keys(value, {"type", "expected"}, what, error)) return false;
  const Value* type = value.find("type");
  const Value* expected = value.find("expected");
  if (!require_string(type, false, std::string(what) + ".type", error)) return false;
  if (!expected || !expected->is_string()) {
    error = std::string(what) + ".expected: required string field is missing";
    return false;
  }
  out = {type->as_string(), expected->as_string()};
  return true;
}

Value encode_identity(const Identity& identity) {
  Value object = Value::object();
  object.set("kind", Value::string(identity.kind));
  object.set("id", Value::string(identity.id));
  return object;
}

bool action_fields_valid(const Action& action) {
  const bool invalid_precondition = [&] {
    for (const Precondition& precondition : action.preconditions) {
      if (precondition.type.empty()) return true;
    }
    return false;
  }();
  return action.schema_version == 1 && action.id != 0 && action.id <= k_max_safe_integer &&
         action.source.kind.empty() == false && !action.source.id.empty() &&
         !action.type.empty() && !action.capability.empty() && !action.target.kind.empty() &&
         !action.target.id.empty() && action.deadline_unix_ms != 0 &&
         action.deadline_unix_ms <= k_max_safe_integer &&
         action.parent_action_id <= k_max_safe_integer && !invalid_precondition;
}

}  // namespace

DecodedAction decode_action(const std::string_view text) {
  auto parse_outcome = rime::core::json::parse(text);
  if (!parse_outcome.ok()) {
    return {std::nullopt, invalid("action-v1: malformed json: " + parse_outcome.error)};
  }
  const Value& root = *parse_outcome.value;
  std::string error;
  if (!root.is_object()) return {std::nullopt, invalid("action-v1: must be an object")};
  if (!has_only_keys(root, {"schemaVersion", "id", "source", "type", "capability", "target",
                            "preconditions", "deadlineUnixMs", "parentActionId", "payload",
                            "idempotencyKey"},
                     "action-v1", error)) {
    return {std::nullopt, invalid(std::move(error))};
  }

  const Value* schema_version = root.find("schemaVersion");
  if (!schema_version || !schema_version->is_number() || schema_version->as_number() != 1) {
    return {std::nullopt, invalid("action-v1: schemaVersion must be 1")};
  }
  const Value* id = root.find("id");
  if (!id || !is_safe_integer(*id)) {
    return {std::nullopt, invalid("action-v1: id must be a safe positive integer")};
  }
  const Value* source = root.find("source");
  if (!require_object(source, "action-v1.source", error)) return {std::nullopt, invalid(std::move(error))};
  const Value* type = root.find("type");
  if (!require_string(type, false, "action-v1.type", error)) return {std::nullopt, invalid(std::move(error))};
  const Value* capability = root.find("capability");
  if (!require_string(capability, false, "action-v1.capability", error)) return {std::nullopt, invalid(std::move(error))};
  const Value* target = root.find("target");
  if (!require_object(target, "action-v1.target", error)) return {std::nullopt, invalid(std::move(error))};
  const Value* preconditions = root.find("preconditions");
  if (!preconditions || !preconditions->is_array()) {
    return {std::nullopt, invalid("action-v1.preconditions: required array field is missing")};
  }
  const Value* deadline = root.find("deadlineUnixMs");
  if (!deadline || !is_safe_integer(*deadline)) {
    return {std::nullopt, invalid("action-v1: deadlineUnixMs must be a safe positive integer")};
  }
  const Value* payload = root.find("payload");
  if (!payload || !payload->is_object()) {
    return {std::nullopt, invalid("action-v1.payload: required object field is missing")};
  }

  Action action;
  action.schema_version = 1;
  action.id = static_cast<rime::core::ActionId>(id->as_number());
  if (!decode_identity(*source, action.source, "action-v1.source", error)) {
    return {std::nullopt, invalid(std::move(error))};
  }
  action.type = type->as_string();
  action.capability = capability->as_string();
  if (!decode_identity(*target, action.target, "action-v1.target", error)) {
    return {std::nullopt, invalid(std::move(error))};
  }
  for (std::size_t index = 0; index < preconditions->as_array().size(); ++index) {
    Precondition precondition;
    if (!decode_precondition(preconditions->as_array()[index], precondition,
                             "action-v1.preconditions[" + std::to_string(index) + "]", error)) {
      return {std::nullopt, invalid(std::move(error))};
    }
    action.preconditions.push_back(std::move(precondition));
  }
  action.deadline_unix_ms = static_cast<std::uint64_t>(deadline->as_number());
  if (const Value* parent = root.find("parentActionId"); parent && !parent->is_null()) {
    if (!is_safe_integer(*parent)) {
      return {std::nullopt, invalid("action-v1.parentActionId must be null or a safe positive integer")};
    }
    action.parent_action_id = static_cast<rime::core::ActionId>(parent->as_number());
  }
  action.payload = rime::core::json::stringify(*payload);
  if (const Value* key = root.find("idempotencyKey"); key) {
    if (!key->is_string() || key->as_string().empty()) {
      return {std::nullopt, invalid("action-v1.idempotencyKey must be a non-empty string")};
    }
    action.idempotency_key = key->as_string();
  }
  return {std::move(action), Error::none()};
}

std::optional<std::string> encode_action(const Action& action) {
  if (!action_fields_valid(action)) return std::nullopt;
  auto payload = rime::core::json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) return std::nullopt;

  Value root = Value::object();
  root.set("schemaVersion", Value::number(1));
  root.set("id", Value::number(static_cast<double>(action.id)));
  root.set("source", encode_identity(action.source));
  root.set("type", Value::string(action.type));
  root.set("capability", Value::string(action.capability));
  root.set("target", encode_identity(action.target));
  Value preconditions = Value::array();
  for (const Precondition& precondition : action.preconditions) {
    Value entry = Value::object();
    entry.set("type", Value::string(precondition.type));
    entry.set("expected", Value::string(precondition.expected));
    preconditions.push(std::move(entry));
  }
  root.set("preconditions", std::move(preconditions));
  root.set("deadlineUnixMs", Value::number(static_cast<double>(action.deadline_unix_ms)));
  root.set("parentActionId", action.parent_action_id == 0
                                 ? Value::null()
                                 : Value::number(static_cast<double>(action.parent_action_id)));
  root.set("payload", std::move(*payload.value));
  if (!action.idempotency_key.empty()) {
    root.set("idempotencyKey", Value::string(action.idempotency_key));
  }
  return rime::core::json::stringify(root);
}

DecodedResult decode_result(const std::string_view text) {
  auto parse_outcome = rime::core::json::parse(text);
  if (!parse_outcome.ok()) {
    return {std::nullopt, invalid("result-v1: malformed json: " + parse_outcome.error)};
  }
  const Value& root = *parse_outcome.value;
  std::string error;
  if (!root.is_object()) return {std::nullopt, invalid("result-v1: must be an object")};
  if (!has_only_keys(root, {"schemaVersion", "actionId", "status", "value", "error"},
                     "result-v1", error)) {
    return {std::nullopt, invalid(std::move(error))};
  }
  const Value* schema_version = root.find("schemaVersion");
  if (!schema_version || !schema_version->is_number() || schema_version->as_number() != 1) {
    return {std::nullopt, invalid("result-v1: schemaVersion must be 1")};
  }
  const Value* action_id = root.find("actionId");
  if (!action_id || !is_safe_integer(*action_id)) {
    return {std::nullopt, invalid("result-v1: actionId must be a safe positive integer")};
  }
  const Value* status = root.find("status");
  if (!status || !status->is_string()) {
    return {std::nullopt, invalid("result-v1.status: required string field is missing")};
  }
  const std::string status_name = status->as_string();
  if (status_name != "succeeded" && status_name != "failed" && status_name != "cancelled") {
    return {std::nullopt, invalid("result-v1.status: must be succeeded, failed or cancelled")};
  }
  const Value* error_value = root.find("error");
  if (!error_value || (!error_value->is_null() && !error_value->is_object())) {
    return {std::nullopt, invalid("result-v1.error: required null or object field is missing")};
  }

  Result result;
  result.id = static_cast<rime::core::ActionId>(action_id->as_number());
  result.succeeded = status_name == "succeeded";
  result.cancelled = status_name == "cancelled";
  if (const Value* value = root.find("value"); value && !value->is_null()) {
    if (!value->is_object()) {
      return {std::nullopt, invalid("result-v1.value must be an object")};
    }
    result.value = *value;
  }
  if (error_value->is_object()) {
    if (!has_only_keys(*error_value, {"code", "message", "retryable"}, "result-v1.error", error)) {
      return {std::nullopt, invalid(std::move(error))};
    }
    const Value* code = error_value->find("code");
    const Value* message = error_value->find("message");
    const Value* retryable = error_value->find("retryable");
    if (!code || !code->is_string()) {
      return {std::nullopt, invalid("result-v1.error.code: required string field is missing")};
    }
    if (!message || !message->is_string()) {
      return {std::nullopt, invalid("result-v1.error.message: required string field is missing")};
    }
    if (!retryable || !retryable->is_bool()) {
      return {std::nullopt, invalid("result-v1.error.retryable: required boolean field is missing")};
    }
    Error::Code parsed_code = Error::Code::ExecutionFailed;
    if (!rime::core::error_code_from_name(code->as_string(), parsed_code)) {
      return {std::nullopt, invalid("result-v1.error.code: unknown error code")};
    }
    result.error = {parsed_code, message->as_string()};
    result.detail = message->as_string();
  } else if (!result.succeeded) {
    return {std::nullopt, invalid("result-v1.error: failed results require an error object")};
  }
  return {std::move(result), Error::none()};
}

std::optional<std::string> encode_result(const Result& result) {
  if (result.id == 0 || result.id > k_max_safe_integer) return std::nullopt;

  Value root = Value::object();
  root.set("schemaVersion", Value::number(1));
  root.set("actionId", Value::number(static_cast<double>(result.id)));
  const char* status = result.succeeded ? "succeeded" : (result.cancelled ? "cancelled" : "failed");
  root.set("status", Value::string(status));
  if (result.value.is_object()) root.set("value", result.value);

  if (result.succeeded && result.error.ok()) {
    root.set("error", Value::null());
  } else {
    rime::core::Error error = result.error;
    if (error.ok()) {
      error = {rime::core::Error::Code::ExecutionFailed,
               result.detail.empty() ? "action failed" : result.detail};
    }
    Value entry = Value::object();
    entry.set("code", Value::string(rime::core::error_code_name(error.code)));
    entry.set("message", Value::string(error.message));
    entry.set("retryable", Value::boolean(error.code == rime::core::Error::Code::QueueFull));
    root.set("error", std::move(entry));
  }
  return rime::core::json::stringify(root);
}

}  // namespace rime::action
