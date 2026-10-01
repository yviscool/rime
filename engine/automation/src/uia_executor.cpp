#include "rime/automation/uia_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rime::automation {
namespace {

using Result = rime::action::Result;
using Code = rime::core::Error::Code;

Result fail(const rime::action::Action& action, const Code code, std::string message) {
  return {action.id, false, false, message, {code, message}, {}};
}

Result cancelled(const rime::action::Action& action, const std::string& message) {
  return {action.id, false, true, message, {Code::Cancelled, message}, {}};
}

Result from_error(const rime::action::Action& action, const rime::core::Error& error) {
  return {action.id, false, false, error.message, error, {}};
}

rime::core::json::Value element_json(const ElementSnapshot& element) {
  rime::core::json::Value value = rime::core::json::Value::object();
  value.set("id", rime::core::json::Value::number(static_cast<double>(element.id)));
  value.set("name", rime::core::json::Value::string(element.name));
  value.set("controlType", rime::core::json::Value::string(element.control_type));
  value.set("automationId", rime::core::json::Value::string(element.automation_id));
  value.set("enabled", rime::core::json::Value::boolean(element.enabled));
  value.set("x", rime::core::json::Value::number(static_cast<double>(element.x)));
  value.set("y", rime::core::json::Value::number(static_cast<double>(element.y)));
  value.set("width", rime::core::json::Value::number(static_cast<double>(element.width)));
  value.set("height", rime::core::json::Value::number(static_cast<double>(element.height)));
  return value;
}

// Element actions name their target in target.id as a decimal string; the
// id must be a positive integer that survives the round trip.
bool parse_target_id(const rime::action::Action& action, std::uint64_t& out) {
  if (action.target.kind != "element") return false;
  const std::string& text = action.target.id;
  if (text.empty() || text.size() > 20) return false;
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') return false;
    value = value * 10 + static_cast<std::uint64_t>(digit - '0');
  }
  if (value == 0) return false;
  out = value;
  return true;
}

bool read_optional_string(const rime::core::json::Value& payload, const char* key,
                          std::string& out) {
  const rime::core::json::Value* field = payload.find(key);
  if (!field) return true;
  if (!field->is_string()) return false;
  out = field->as_string();
  return true;
}

bool read_optional_u64(const rime::core::json::Value& payload, const char* key,
                       std::uint64_t max, std::uint64_t& out) {
  const rime::core::json::Value* field = payload.find(key);
  if (!field) return true;
  if (!field->is_number()) return false;
  const double raw = field->as_number();
  if (raw != std::trunc(raw) || raw < 0.0 || raw > static_cast<double>(max)) return false;
  out = static_cast<std::uint64_t>(raw);
  return true;
}

}  // namespace

rime::action::Result UiaExecutor::execute(const rime::action::Action& action,
                                          rime::core::CancellationToken cancellation) {
  if (const auto lane_error = rime::core::require_lane(rime::core::Lane::Worker);
      !lane_error.ok()) {
    return fail(action, lane_error.code, lane_error.message);
  }
  const char* expected_type = op_ == Op::Find   ? "automation.find"
                              : op_ == Op::Read ? "automation.read"
                                                : "automation.invoke";
  if (action.type != expected_type) {
    return fail(action, Code::InvalidContract,
                std::string("unsupported action type: ") + action.type);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  if (op_ == Op::Find) {
    if (action.target.kind != "automation") {
      return fail(action, Code::InvalidContract,
                  "automation.find requires target kind 'automation', got: " +
                      action.target.kind);
    }
    const auto payload = rime::core::json::parse(action.payload);
    if (!payload.ok() || !payload.value->is_object()) {
      return fail(action, Code::InvalidContract,
                  "automation.find payload must be a JSON object");
    }
    const rime::core::json::Value& query = *payload.value;
    FindQuery parsed;
    parsed.max_results = 8;
    if (!read_optional_string(query, "name", parsed.name) ||
        !read_optional_string(query, "controlType", parsed.control_type) ||
        !read_optional_string(query, "automationId", parsed.automation_id)) {
      return fail(action, Code::InvalidContract,
                  "automation.find name, controlType and automationId must be strings");
    }
    std::uint64_t max_results = 8;
    if (!read_optional_u64(query, "fromId", 9007199254740991ull, parsed.from_id) ||
        !read_optional_u64(query, "maxResults", 64, max_results)) {
      return fail(action, Code::InvalidContract,
                  "automation.find fromId and maxResults must be integers in range");
    }
    if (max_results == 0) {
      return fail(action, Code::InvalidContract, "automation.find maxResults must be positive");
    }
    parsed.max_results = static_cast<std::uint32_t>(max_results);
    if (parsed.name.empty() && parsed.control_type.empty() && parsed.automation_id.empty()) {
      return fail(action, Code::InvalidContract,
                  "automation.find requires at least one of name, controlType or automationId");
    }
    if (cancellation.cancelled()) {
      return cancelled(action, "action was cancelled before execution");
    }

    std::vector<ElementSnapshot> elements;
    const rime::core::Error error = service_.find(parsed, elements);
    if (!error.ok()) return from_error(action, error);

    rime::core::json::Value list = rime::core::json::Value::array();
    for (const auto& element : elements) list.push(element_json(element));
    rime::core::json::Value value = rime::core::json::Value::object();
    value.set("elements", std::move(list));
    return {action.id, true, false, "elements found", {}, std::move(value)};
  }

  std::uint64_t element_id = 0;
  if (!parse_target_id(action, element_id)) {
    return fail(action, Code::InvalidContract,
                "element actions require target {kind: \"element\", id: \"<positive id>\"}");
  }

  if (op_ == Op::Read) {
    if (cancellation.cancelled()) {
      return cancelled(action, "action was cancelled before execution");
    }
    ElementSnapshot element;
    const rime::core::Error error = service_.read(element_id, element);
    if (!error.ok()) return from_error(action, error);
    return {action.id, true, false, "element read", {}, element_json(element)};
  }

  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }
  const rime::core::Error error = service_.invoke(element_id);
  if (!error.ok()) return from_error(action, error);
  rime::core::json::Value value = rime::core::json::Value::object();
  value.set("invoked", rime::core::json::Value::boolean(true));
  return {action.id, true, false, "element invoked", {}, std::move(value)};
}

}  // namespace rime::automation
