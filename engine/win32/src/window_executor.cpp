#include "rime/win32/window_executor.hpp"

#include "rime/core/json.hpp"

#include <charconv>
#include <optional>
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

bool parse_window_id(const std::string& text, std::uint64_t& out) {
  if (text.empty()) return false;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, out);
  return parsed.ec == std::errc{} && parsed.ptr == end && out != 0;
}

}  // namespace

rime::action::Result WindowExecutor::execute(const rime::action::Action& action,
                                             rime::core::CancellationToken cancellation) {
  if (action.type != "window.move") {
    return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
  }
  if (action.target.kind != "window") {
    return fail(action, Code::InvalidContract,
                "window.move requires target kind 'window', got: " + action.target.kind);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  const auto payload = rime::core::json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract, "window.move payload must be a JSON object");
  }
  const rime::core::json::Value* position = payload.value->find("position");
  if (!position || !position->is_string() || position->as_string().empty()) {
    return fail(action, Code::InvalidContract,
                "window.move payload requires a string position");
  }
  const std::string placement = position->as_string();

  std::uint64_t window_id = 0;
  if (action.target.id == "active") {
    std::optional<WindowInfo> active;
    if (const auto query = service_.active(active); !query.ok()) {
      return fail(action, query.code, query.message);
    }
    if (!active.has_value()) {
      return fail(action, Code::ExecutionFailed, "there is no active window");
    }
    window_id = active->id;
  } else if (!parse_window_id(action.target.id, window_id)) {
    return fail(action, Code::InvalidContract,
                "window.move target id must be 'active' or a positive integer");
  }

  if (const auto move_error = service_.move(window_id, placement); !move_error.ok()) {
    return fail(action, move_error.code, move_error.message);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled after execution");
  }

  WindowInfo moved;
  if (const auto info_error = service_.info(window_id, moved); !info_error.ok()) {
    return fail(action, info_error.code, info_error.message);
  }
  return {action.id, true, false, "window moved to " + placement, {},
          window_info_json(moved)};
}

}  // namespace rime::win32
