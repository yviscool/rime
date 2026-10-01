#pragma once

#include "rime/action/action.hpp"
#include "rime/core/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace rime::action {

// Contract codec: strict JSON Schema Draft 2020-12 subset validation for
// action-v1 and result-v1, including additionalProperties rejection and
// JavaScript safe-integer bounds.
struct DecodedAction {
  std::optional<Action> action;
  rime::core::Error error;
  [[nodiscard]] bool ok() const noexcept { return action.has_value(); }
};

DecodedAction decode_action(std::string_view text);
std::optional<std::string> encode_action(const Action& action);

struct DecodedResult {
  std::optional<Result> result;
  rime::core::Error error;
  [[nodiscard]] bool ok() const noexcept { return result.has_value(); }
};

DecodedResult decode_result(std::string_view text);
std::optional<std::string> encode_result(const Result& result);

}  // namespace rime::action
