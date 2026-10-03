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

// Integer check shared by both payload contracts: JSON numbers are doubles,
// so exactness against trunc plus an int32 bound keeps the cast defined.
bool is_int32(const double value) {
  return value == std::trunc(value) && value >= -2147483648.0 && value <= 2147483647.0;
}

Result execute_send(InputService& service, const rime::action::Action& action,
                    rime::core::CancellationToken cancellation) {
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
    const rime::core::json::Value* unicode = step.find("unicode");
    if (unicode && !unicode->is_bool()) {
      return fail(action, Code::InvalidContract, "input.send unicode must be a boolean");
    }
    const bool is_unicode = unicode && unicode->as_bool();
    const double vk_number = vk->as_number();
    if (is_unicode) {
      if (!is_int32(vk_number) || vk_number < 0.0 || vk_number > 65535.0) {
        return fail(action, Code::InvalidContract,
                    "input.send unicode vk must be an integer in 0..65535");
      }
    } else if (!is_int32(vk_number) || vk_number < 1.0 || vk_number > 254.0) {
      return fail(action, Code::InvalidContract, "input.send vk must be an integer in 1..254");
    }
    keys.push_back({static_cast<std::uint32_t>(vk_number), down->as_bool(), is_unicode});
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  const auto sent = service.send(keys);
  if (!sent.ok()) return fail(action, sent.code, sent.message);

  rime::core::json::Value value = rime::core::json::Value::object();
  value.set("sent", rime::core::json::Value::number(static_cast<double>(keys.size())));
  return {action.id, true, false, "key steps injected", {}, std::move(value)};
}

Result execute_mouse(InputService& service, const rime::action::Action& action,
                     rime::core::CancellationToken cancellation) {
  if (action.target.kind != "input") {
    return fail(action, Code::InvalidContract,
                "input.mouse requires target kind 'input', got: " + action.target.kind);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  const auto payload = rime::core::json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract,
                "input.mouse payload must be an object with a steps array");
  }
  // speed is part of the payload contract (AHK MouseMove/MouseClick Speed)
  // but is deliberately not forwarded to the injector: SendInput moves are
  // instantaneous, exactly like AHK's aSpeed == 0 || SM_INPUT gate.
  if (const rime::core::json::Value* speed = payload.value->find("speed"); speed) {
    if (!speed->is_number()) {
      return fail(action, Code::InvalidContract, "input.mouse speed must be a number in 0..100");
    }
    const double speed_number = speed->as_number();
    if (!is_int32(speed_number) || speed_number < 0.0 || speed_number > 100.0) {
      return fail(action, Code::InvalidContract,
                  "input.mouse speed must be an integer in 0..100");
    }
  }
  const rime::core::json::Value* steps_value = payload.value->find("steps");
  if (!steps_value || !steps_value->is_array()) {
    return fail(action, Code::InvalidContract, "input.mouse steps must be an array");
  }
  const auto& steps = steps_value->as_array();
  if (steps.empty()) {
    return fail(action, Code::InvalidContract, "input.mouse payload must not be empty");
  }
  std::vector<SendMouseStep> mouse_steps;
  mouse_steps.reserve(steps.size());
  for (const auto& step : steps) {
    if (!step.is_object()) {
      return fail(action, Code::InvalidContract,
                  "input.mouse steps must be objects with an action");
    }
    const rime::core::json::Value* kind = step.find("action");
    if (!kind || !kind->is_string()) {
      return fail(action, Code::InvalidContract,
                  "input.mouse steps require a string action");
    }
    const std::string action_name = kind->as_string();
    SendMouseStep parsed;
    if (action_name == "move" || action_name == "relmove") {
      const rime::core::json::Value* x = step.find("x");
      const rime::core::json::Value* y = step.find("y");
      if (!x || !x->is_number() || !y || !y->is_number()) {
        return fail(action, Code::InvalidContract,
                    "input.mouse move steps require numeric x and y");
      }
      if (!is_int32(x->as_number()) || !is_int32(y->as_number())) {
        return fail(action, Code::InvalidContract,
                    "input.mouse move coordinates must be int32 integers");
      }
      parsed.action = action_name == "move" ? SendMouseAction::Move : SendMouseAction::RelMove;
      parsed.x = static_cast<std::int32_t>(x->as_number());
      parsed.y = static_cast<std::int32_t>(y->as_number());
    } else if (action_name == "down" || action_name == "up") {
      const rime::core::json::Value* button = step.find("button");
      if (!button || !button->is_number()) {
        return fail(action, Code::InvalidContract,
                    "input.mouse button steps require a numeric button");
      }
      const double button_number = button->as_number();
      if (!is_int32(button_number) || button_number < 1.0 || button_number > 3.0) {
        return fail(action, Code::InvalidContract,
                    "input.mouse buttons must be 1 (left), 2 (right) or 3 (middle)");
      }
      parsed.action = action_name == "down" ? SendMouseAction::Down : SendMouseAction::Up;
      parsed.button = static_cast<std::uint32_t>(button_number);
    } else {
      return fail(action, Code::InvalidContract,
                  "input.mouse step action must be move, relmove, down or up, got: " +
                      action_name);
    }
    mouse_steps.push_back(parsed);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  const auto sent = service.send_mouse(mouse_steps);
  if (!sent.ok()) return fail(action, sent.code, sent.message);

  rime::core::json::Value value = rime::core::json::Value::object();
  value.set("sent", rime::core::json::Value::number(static_cast<double>(mouse_steps.size())));
  return {action.id, true, false, "mouse steps injected", {}, std::move(value)};
}

}  // namespace

rime::action::Result InputExecutor::execute(const rime::action::Action& action,
                                            rime::core::CancellationToken cancellation) {
  if (const auto lane_error = rime::core::require_lane(rime::core::Lane::Worker);
      !lane_error.ok()) {
    return fail(action, lane_error.code, lane_error.message);
  }
  if (action.type == "input.send") {
    return execute_send(service_, action, cancellation);
  }
  if (action.type == "input.mouse") {
    return execute_mouse(service_, action, cancellation);
  }
  return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
}

}  // namespace rime::win32
