#include "rime/win32/window_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"

#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace rime::win32 {
namespace {

using Result = rime::action::Result;
using Code = rime::core::Error::Code;

// Every action type this executor dispatches; capability for all of them is
// `windows.window.write` (the kernel checks it before dispatch).
const std::unordered_set<std::string>& window_action_types() {
  static const std::unordered_set<std::string> types = {"window.move", "window.focus",
                                                        "window.close", "window.hide",
                                                        "window.show",  "window.minimize",
                                                        "window.maximize", "window.restore"};
  return types;
}

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

// Remaining time until the action deadline; non-positive yields Timeout so a
// deadline that expires between kernel dispatch and the UI queue still fails
// as Timeout instead of running late. The <=now boundary matches the kernel
// pre-dispatch and post-commit checks.
bool remaining_timeout(const rime::action::Action& action, std::chrono::milliseconds& out) {
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const auto deadline_ms = static_cast<std::int64_t>(action.deadline_unix_ms);
  const auto remaining = deadline_ms - now_ms;
  if (remaining <= 0) return false;
  out = std::chrono::milliseconds(remaining);
  return true;
}

// Resolves the target identity ("active" or a numeric id) into a live id.
// The caller passes the current remaining time so the active-window query
// is bounded by the action deadline like every other UI round-trip.
bool resolve_target(WindowService& service, const rime::action::Action& action,
                    std::uint64_t& out, rime::core::Error& error,
                    std::chrono::milliseconds timeout) {
  if (action.target.id == "active") {
    std::optional<WindowInfo> active;
    if (const auto query = service.active(active, timeout); !query.ok()) {
      error = query;
      return false;
    }
    if (!active.has_value()) {
      error = {Code::ExecutionFailed, "there is no active window"};
      return false;
    }
    out = active->id;
    return true;
  }
  if (!parse_window_id(action.target.id, out)) {
    error = {Code::InvalidContract,
             "window target id must be 'active' or a positive integer"};
    return false;
  }
  return true;
}

}  // namespace

rime::action::Result WindowExecutor::execute(const rime::action::Action& action,
                                             rime::core::CancellationToken cancellation) {
  if (const auto lane_error = rime::core::require_lane(rime::core::Lane::Worker);
      !lane_error.ok()) {
    return fail(action, lane_error.code, lane_error.message);
  }
  if (!window_action_types().contains(action.type)) {
    return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
  }
  if (action.target.kind != "window") {
    return fail(action, Code::InvalidContract,
                "window actions require target kind 'window', got: " + action.target.kind);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  // Entry check; the remaining time is recomputed before every UI
  // round-trip below (resolve/info/op/info) so a late queue wait fails fast.
  std::chrono::milliseconds timeout{};
  if (!remaining_timeout(action, timeout)) {
    return fail(action, Code::Timeout, "action deadline exceeded");
  }

  const auto payload = rime::core::json::parse(action.payload);
  if (!payload.ok() || !payload.value->is_object()) {
    return fail(action, Code::InvalidContract,
                action.type + " payload must be a JSON object");
  }

  std::string placement;
  if (action.type == "window.move") {
    const rime::core::json::Value* position = payload.value->find("position");
    if (!position || !position->is_string() || position->as_string().empty()) {
      return fail(action, Code::InvalidContract,
                  "window.move payload requires a string position");
    }
    placement = position->as_string();
  }

  std::uint64_t window_id = 0;
  rime::core::Error target_error = rime::core::Error::none();
  if (!remaining_timeout(action, timeout)) {
    return fail(action, Code::Timeout, "action deadline exceeded");
  }
  if (!resolve_target(service_, action, window_id, target_error, timeout)) {
    return fail(action, target_error.code, target_error.message);
  }
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled before execution");
  }

  rime::core::Error op_error = rime::core::Error::none();
  // `window.close` destroys the window, so the result value is the snapshot
  // taken before the close instead of a post-read that would fail.
  WindowInfo snapshot;
  const bool snapshot_before = action.type == "window.close";
  if (snapshot_before) {
    if (!remaining_timeout(action, timeout)) {
      return fail(action, Code::Timeout, "action deadline exceeded");
    }
    if (const auto before = service_.info(window_id, snapshot, timeout); !before.ok()) {
      return fail(action, before.code, before.message);
    }
  }
  if (!remaining_timeout(action, timeout)) {
    return fail(action, Code::Timeout, "action deadline exceeded");
  }
  if (action.type == "window.move") {
    op_error = service_.move(window_id, placement, timeout);
  } else if (action.type == "window.focus") {
    op_error = service_.focus(window_id, timeout);
  } else if (action.type == "window.close") {
    op_error = service_.close(window_id, timeout);
  } else if (action.type == "window.hide") {
    op_error = service_.hide(window_id, timeout);
  } else if (action.type == "window.show") {
    op_error = service_.show(window_id, timeout);
  } else if (action.type == "window.minimize") {
    op_error = service_.minimize(window_id, timeout);
  } else if (action.type == "window.maximize") {
    op_error = service_.maximize(window_id, timeout);
  } else if (action.type == "window.restore") {
    op_error = service_.restore(window_id, timeout);
  } else {
    return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
  }
  if (!op_error.ok()) return fail(action, op_error.code, op_error.message);
  if (cancellation.cancelled()) {
    return cancelled(action, "action was cancelled after execution");
  }

  // Every other action re-reads the window so the value reflects the
  // committed state.
  if (!snapshot_before) {
    if (!remaining_timeout(action, timeout)) {
      return fail(action, Code::Timeout, "action deadline exceeded");
    }
    if (const auto after = service_.info(window_id, snapshot, timeout); !after.ok()) {
      return fail(action, after.code, after.message);
    }
  }
  const std::string detail = action.type == "window.move" ? "window moved to " + placement
                                                          : action.type + " applied";
  return {action.id, true, false, detail, {}, window_info_json(snapshot)};
}

}  // namespace rime::win32
