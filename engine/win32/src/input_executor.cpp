#include "rime/win32/input_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

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

}  // namespace

rime::action::Result InputExecutor::execute(const rime::action::Action& action,
                                            rime::core::CancellationToken cancellation) {
  if (const auto lane_error = rime::core::require_lane(rime::core::Lane::Worker);
      !lane_error.ok()) {
    return fail(action, lane_error.code, lane_error.message);
  }
  if (action.type != "input.send") {
    return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
  }
  if (action.target.kind != "input") {
    return fail(action, Code::InvalidContract,
                "input.send requires target kind 'input', got: " + action.target.kind);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  const auto payload = rime::core::json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_array()) {
    return fail(action, Code::InvalidContract,
                "input.send payload must be a JSON array of key steps");
  }
  const auto& steps = payload.value->as_array();
  if (steps.empty()) {
    return fail(action, Code::InvalidContract, "input.send payload must not be empty");
  }
  std::vector<SendKeyEvent> keys;
  keys.reserve(steps.size());
  for (const auto& step : steps) {
    if (!step.is_object()) {
      return fail(action, Code::InvalidContract,
                  "input.send steps must be objects with vk and down");
    }
    const rime::core::json::Value* vk = step.find("vk");
    const rime::core::json::Value* down = step.find("down");
    if (!vk || !vk->is_number() || !down || !down->is_bool()) {
      return fail(action, Code::InvalidContract,
                  "input.send steps require a numeric vk and a boolean down");
    }
    const double vk_number = vk->as_number();
    if (vk_number != std::trunc(vk_number) || vk_number < 1.0 || vk_number > 254.0) {
      return fail(action, Code::InvalidContract, "input.send vk must be an integer in 1..254");
    }
    keys.push_back({static_cast<std::uint32_t>(vk_number), down->as_bool()});
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  const auto sent = service_.send(keys);
  if (!sent.ok()) return fail(action, sent.code, sent.message);

  rime::core::json::Value value = rime::core::json::Value::object();
  value.set("sent", rime::core::json::Value::number(static_cast<double>(keys.size())));
  return {action.id, true, false, "key steps injected", {}, std::move(value)};
}

}  // namespace rime::win32
