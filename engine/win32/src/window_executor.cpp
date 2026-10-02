#include "rime/win32/window_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
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
  static const std::unordered_set<std::string> types = {
      "window.move",       "window.focus",      "window.close",
      "window.hide",       "window.show",       "window.minimize",
      "window.maximize",   "window.restore",    "window.zorder",
      "window.kill",       "window.redraw",     "window.group.add",
      "window.group.activate", "window.group.deactivate", "window.group.close",
      "window.minimizeall", "window.minimizeall.undo"};
  return types;
}

// The group subset dispatches against a named group (target kind "group",
// target id = the group name) instead of a window identity.
bool is_group_action(const std::string& type) {
  static const std::unordered_set<std::string> types = {
      "window.group.add", "window.group.activate", "window.group.deactivate",
      "window.group.close"};
  return types.contains(type);
}

// The desktop subset dispatches against the whole desktop (target kind
// "desktop", target id "all") instead of a window or group identity.
bool is_desktop_action(const std::string& type) {
  static const std::unordered_set<std::string> types = {"window.minimizeall",
                                                        "window.minimizeall.undo"};
  return types.contains(type);
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

// Parses the window.group.add payload query - the wire twin of the JS
// query object ({title, matchMode, ahkClass, ahkExe, ahkId, includeHidden,
// active}). Fields are all optional but at least one selector is required;
// Rime refuses the empty spec AHK tolerates as a placeholder. Regex fields
// are only compiled when the group is evaluated, so a bad pattern surfaces
// from activate/deactivate/close (the module validates them up front for
// JS callers).
rime::core::Error parse_group_query(const rime::core::json::Value& value, WindowQuery& out) {
  const auto string_field = [&](const char* name, std::string& target) -> rime::core::Error {
    const rime::core::json::Value* text = value.find(name);
    if (!text) return rime::core::Error::none();
    if (!text->is_string()) {
      return {Code::InvalidContract, std::string("group query: ") + name + " must be a string"};
    }
    target = text->as_string();
    return rime::core::Error::none();
  };
  const auto flag_field = [&](const char* name, std::optional<bool>& target) -> rime::core::Error {
    const rime::core::json::Value* flag = value.find(name);
    if (!flag) return rime::core::Error::none();
    if (!flag->is_bool()) {
      return {Code::InvalidContract, std::string("group query: ") + name + " must be a boolean"};
    }
    target = flag->as_bool();
    return rime::core::Error::none();
  };
  if (const auto error = string_field("title", out.title); !error.ok()) return error;
  if (const auto error = string_field("ahkClass", out.class_name); !error.ok()) return error;
  if (const auto error = string_field("ahkExe", out.process_name); !error.ok()) return error;
  if (const auto error = flag_field("includeHidden", out.include_hidden); !error.ok()) return error;
  std::optional<bool> active;
  if (const auto error = flag_field("active", active); !error.ok()) return error;
  out.active = active.value_or(false);
  if (const rime::core::json::Value* mode = value.find("matchMode"); mode) {
    if (!mode->is_string()) {
      return {Code::InvalidContract, "group query: matchMode must be a string"};
    }
    const std::string text = mode->as_string();
    if (text == "exact") {
      out.title_match_mode = TitleMatchMode::Exact;
    } else if (text == "contains") {
      out.title_match_mode = TitleMatchMode::Contains;
    } else if (text == "startswith") {
      out.title_match_mode = TitleMatchMode::StartsWith;
    } else if (text == "regex") {
      out.title_match_mode = TitleMatchMode::Regex;
    } else {
      return {Code::InvalidContract,
              "group query: matchMode must be 'startswith', 'contains', 'exact' or 'regex'"};
    }
  }
  if (const rime::core::json::Value* id = value.find("ahkId"); id) {
    // Same 2^53 bound as js_int64_strict: the module writes integers that
    // survived that check, and everything above is lossy in a double.
    constexpr double kMaxExactInteger = 9007199254740991.0;
    if (!id->is_number()) {
      return {Code::InvalidContract, "group query: ahkId must be a positive window id"};
    }
    const double raw = id->as_number();
    if (!(raw > 0.0) || raw != std::floor(raw) || raw > kMaxExactInteger) {
      return {Code::InvalidContract, "group query: ahkId must be a positive window id"};
    }
    out.id = static_cast<std::uint64_t>(raw);
  }
  if (!out.active && out.title.empty() && out.class_name.empty() && out.process_name.empty() &&
      out.id == 0) {
    return {Code::InvalidContract,
            "window.group.add query must select at least one window field"};
  }
  return rime::core::Error::none();
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
  const bool group_action = is_group_action(action.type);
  const bool desktop_action = !group_action && is_desktop_action(action.type);
  if (group_action) {
    if (action.target.kind != "group") {
      return fail(action, Code::InvalidContract,
                  "window group actions require target kind 'group', got: " + action.target.kind);
    }
  } else if (desktop_action) {
    if (action.target.kind != "desktop") {
      return fail(action, Code::InvalidContract,
                  "window desktop actions require target kind 'desktop', got: " +
                      action.target.kind);
    }
  } else if (action.target.kind != "window") {
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

  if (desktop_action) {
    // Desktop actions carry no window identity; the fixed {"desktop", "all"}
    // target selects the whole desktop and the deadline bounds the queued
    // UI phase like every other action.
    if (action.target.id != "all") {
      return fail(action, Code::InvalidContract, "desktop target id must be 'all'");
    }
    if (cancellation.cancelled()) {
      return cancelled(action, "action was cancelled before execution");
    }
    if (!remaining_timeout(action, timeout)) {
      return fail(action, Code::Timeout, "action deadline exceeded");
    }
    const bool undo = action.type == "window.minimizeall.undo";
    if (const auto op_error = service_.minimize_all(undo, timeout); !op_error.ok()) {
      return fail(action, op_error.code, op_error.message);
    }
    if (cancellation.cancelled()) {
      return cancelled(action, "action was cancelled after execution");
    }
    // Fire-and-forget: the shell applies the change asynchronously, so the
    // value stays empty and the caller observes the effect through info().
    rime::core::json::Value value = rime::core::json::Value::object();
    const std::string detail = undo ? "all windows restored" : "all windows minimized";
    return {action.id, true, false, detail, {}, std::move(value)};
  }

  if (group_action) {
    // Group actions carry the group name as the target id and never resolve
    // a window identity; the deadline bounds each queued UI call the same
    // way as the window path.
    const std::string& name = action.target.id;
    if (name.empty()) {
      return fail(action, Code::InvalidContract, "group target id must not be empty");
    }
    if (action.type == "window.group.add") {
      WindowQuery spec;
      if (const auto parsed = parse_group_query(*payload.value, spec); !parsed.ok()) {
        return fail(action, parsed.code, parsed.message);
      }
      std::size_t spec_count = 0;
      const auto op_error = service_.group_add(name, spec, spec_count, timeout);
      if (!op_error.ok()) return fail(action, op_error.code, op_error.message);
      if (cancellation.cancelled()) {
        return cancelled(action, "action was cancelled after execution");
      }
      rime::core::json::Value value = rime::core::json::Value::object();
      value.set("count", rime::core::json::Value::number(static_cast<double>(spec_count)));
      return {action.id, true, false, "group spec added", {}, std::move(value)};
    }
    if (action.type == "window.group.activate" || action.type == "window.group.deactivate") {
      bool reverse = false;
      if (const rime::core::json::Value* flag = payload.value->find("reverse"); flag) {
        if (!flag->is_bool()) {
          return fail(action, Code::InvalidContract,
                      action.type + " payload: reverse must be a boolean");
        }
        reverse = flag->as_bool();
      }
      std::optional<WindowInfo> out;
      rime::core::Error op_error = rime::core::Error::none();
      if (action.type == "window.group.activate") {
        op_error = service_.group_activate(name, reverse, out, timeout);
      } else {
        op_error = service_.group_deactivate(name, reverse, out, timeout);
      }
      if (!op_error.ok()) return fail(action, op_error.code, op_error.message);
      if (cancellation.cancelled()) {
        return cancelled(action, "action was cancelled after execution");
      }
      const char* detail =
          action.type == "window.group.activate" ? "group window activated"
                                                  : "non-member activated";
      // Nothing matched (empty group, nothing eligible): resolve null like
      // active()/wait() instead of failing the operation.
      return {action.id, true, false, detail, {},
              out ? window_info_json(*out) : rime::core::json::Value::null()};
    }
    std::string mode;
    if (const rime::core::json::Value* text = payload.value->find("mode"); text) {
      if (!text->is_string()) {
        return fail(action, Code::InvalidContract,
                    "window.group.close payload: mode must be a string");
      }
      mode = text->as_string();
    }
    std::uint64_t closed = 0;
    std::optional<WindowInfo> activated;
    const auto op_error = service_.group_close(name, mode, closed, activated, timeout);
    if (!op_error.ok()) return fail(action, op_error.code, op_error.message);
    if (cancellation.cancelled()) {
      return cancelled(action, "action was cancelled after execution");
    }
    rime::core::json::Value value = rime::core::json::Value::object();
    value.set("closed", rime::core::json::Value::number(static_cast<double>(closed)));
    value.set("activated", activated ? window_info_json(*activated)
                                     : rime::core::json::Value::null());
    const std::string detail = std::to_string(closed) + " window(s) closed";
    return {action.id, true, false, detail, {}, std::move(value)};
  }

  std::string placement;
  if (action.type == "window.move") {
    const rime::core::json::Value* position = payload.value->find("position");
    if (!position || !position->is_string() || position->as_string().empty()) {
      return fail(action, Code::InvalidContract,
                  "window.move payload requires a string position");
    }
    placement = position->as_string();
  } else if (action.type == "window.zorder") {
    const rime::core::json::Value* where = payload.value->find("placement");
    if (!where || !where->is_string()) {
      return fail(action, Code::InvalidContract,
                  "window.zorder payload requires a string placement");
    }
    placement = where->as_string();
    if (placement != "top" && placement != "bottom") {
      return fail(action, Code::InvalidContract,
                  "window.zorder placement must be 'top' or 'bottom'");
    }
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
  // `window.close` and `window.kill` destroy the window, so the result value
  // is the snapshot taken before instead of a post-read that would fail.
  WindowInfo snapshot;
  const bool snapshot_before =
      action.type == "window.close" || action.type == "window.kill";
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
  } else if (action.type == "window.zorder") {
    op_error = service_.zorder(window_id, placement == "bottom", timeout);
  } else if (action.type == "window.focus") {
    op_error = service_.focus(window_id, timeout);
  } else if (action.type == "window.close") {
    op_error = service_.close(window_id, timeout);
  } else if (action.type == "window.kill") {
    op_error = service_.kill(window_id, timeout);
  } else if (action.type == "window.redraw") {
    op_error = service_.redraw(window_id, timeout);
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
  const std::string detail = action.type == "window.move"
                                 ? "window moved to " + placement
                                 : action.type == "window.zorder"
                                       ? "window z-order set to " + placement
                                       : action.type + " applied";
  return {action.id, true, false, detail, {}, window_info_json(snapshot)};
}

}  // namespace rime::win32
