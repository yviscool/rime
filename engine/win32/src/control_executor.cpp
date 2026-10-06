#include "rime/win32/control_executor.hpp"

#include "rime/core/json.hpp"
#include "rime/core/lane.hpp"
#include "rime/win32/action_contract.hpp"

#include <windows.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

namespace rime::win32 {
namespace {

using Code = rime::core::Error::Code;
namespace contract = rime::win32::contract;
using contract::fail;

// The types this executor dispatches (mirrored literally in
// Bootstrap::register_executors, which matrix:check enforces).
const std::unordered_set<std::string>& control_action_types() {
  static const std::unordered_set<std::string> types = {
      "control.click", "control.focus", "control.settext", "control.gettext",
      "control.sendtext"};
  return types;
}

// Decimal stable id from controls()/window_at() (same from_chars rule as
// parse_window_id: rejects empty/non-digits/overflow and 0).
bool parse_control_id(const std::string& text, std::uint64_t& out) {
  if (text.empty()) return false;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, out);
  return parsed.ec == std::errc{} && parsed.ptr == end && out != 0;
}

bool remaining_timeout(const rime::action::Action& action, std::chrono::milliseconds& out) {
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const auto remaining =
      static_cast<std::int64_t>(action.deadline_unix_ms) - static_cast<std::int64_t>(now_ms);
  if (remaining <= 0) return false;
  out = std::chrono::milliseconds(remaining);
  return true;
}

// Interruptible settle wait on the worker lane (replaces AHK's global
// ControlDelay sleep): slices so cancellation lands promptly.
bool settle_wait(const std::uint64_t ms, rime::core::CancellationToken cancellation) {
  std::uint64_t waited = 0;
  while (waited < ms) {
    if (cancellation.cancelled()) return false;
    const std::uint64_t step = (std::min<std::uint64_t>)(ms - waited, 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(step));
    waited += step;
  }
  return !cancellation.cancelled();
}

bool read_optional_bool(const rime::core::json::Value& payload, const char* key, bool fallback,
                        bool& out) {
  const rime::core::json::Value* field = payload.find(key);
  if (!field) {
    out = fallback;
    return true;
  }
  if (!field->is_bool()) return false;
  out = field->as_bool();
  return true;
}

// Optional non-negative integer millisecond budget (doubles on the wire,
// like every other numeric payload field).
bool read_optional_ms(const rime::core::json::Value& payload, const char* key,
                      std::uint64_t fallback, std::uint64_t& out) {
  const rime::core::json::Value* field = payload.find(key);
  if (!field) {
    out = fallback;
    return true;
  }
  if (!field->is_number()) return false;
  const double raw = field->as_number();
  if (!std::isfinite(raw) || raw != std::trunc(raw) || raw < 0.0 ||
      raw > 3'600'000.0) {
    return false;
  }
  out = static_cast<std::uint64_t>(raw);
  return true;
}

bool read_optional_count(const rime::core::json::Value& payload, int& out) {
  const rime::core::json::Value* field = payload.find("count");
  if (!field) {
    out = 1;
    return true;
  }
  if (!field->is_number()) return false;
  const double raw = field->as_number();
  if (!std::isfinite(raw) || raw != std::trunc(raw) || raw < 0.0 || raw > 1'000'000.0) {
    return false;
  }
  out = static_cast<int>(raw);
  return true;
}

}  // namespace

rime::action::Result ControlExecutor::execute(const rime::action::Action& action,
                                              rime::core::CancellationToken cancellation) {
  if (const auto bad = contract::lane(action, rime::core::Lane::Worker)) return *bad;
  if (!control_action_types().contains(action.type)) return contract::unsupported(action);
  if (const auto bad = contract::target_kind(action, action.type.c_str(), "control")) return *bad;
  std::uint64_t id = 0;
  if (!parse_control_id(action.target.id, id)) {
    return fail(action, Code::InvalidContract, "control target id must be a positive integer");
  }
  if (const auto bad = contract::cancel_before(action, cancellation)) return *bad;

  const auto payload = rime::core::json::parse(action.payload);
  if (const auto bad = contract::object_payload(action, payload, action.type + " payload ")) {
    return *bad;
  }
  std::chrono::milliseconds timeout{0};
  if (!remaining_timeout(action, timeout)) {
    return fail(action, Code::Timeout, action.type + " deadline already passed");
  }

  if (action.type == "control.click") {
    int vk = VK_LBUTTON;
    if (const rime::core::json::Value* button = payload.value->find("button")) {
      if (!button->is_string()) {
        return fail(action, Code::InvalidContract, "control.click button must be a string");
      }
      const std::string name = button->as_string();
      if (name == "left") {
        vk = VK_LBUTTON;
      } else if (name == "right") {
        vk = VK_RBUTTON;
      } else if (name == "middle") {
        vk = VK_MBUTTON;
      } else if (name == "x1") {
        vk = VK_XBUTTON1;
      } else if (name == "x2") {
        vk = VK_XBUTTON2;
      } else {
        return fail(action, Code::InvalidContract,
                    "control.click button must be left, right, middle, x1 or x2");
      }
    }
    int count = 1;
    if (!read_optional_count(*payload.value, count)) {
      return fail(action, Code::InvalidContract,
                  "control.click count must be an integer in 0..1000000");
    }
    WindowService::ControlClick click;
    click.vk = vk;
    click.count = count;
    if (const rime::core::json::Value* phase = payload.value->find("phase")) {
      if (!phase->is_string()) {
        return fail(action, Code::InvalidContract, "control.click phase must be a string");
      }
      const std::string name = phase->as_string();
      if (name == "downUp") {
        click.phase = WindowService::ControlClick::Phase::DownUp;
      } else if (name == "down") {
        click.phase = WindowService::ControlClick::Phase::Down;
      } else if (name == "up") {
        click.phase = WindowService::ControlClick::Phase::Up;
      } else {
        return fail(action, Code::InvalidContract,
                    "control.click phase must be downUp, down or up");
      }
    }
    if (!read_optional_bool(*payload.value, "activate", false, click.activate)) {
      return fail(action, Code::InvalidContract, "control.click activate must be a boolean");
    }
    int x = 0;
    int y = 0;
    for (const char* key : {"x", "y"}) {
      if (const rime::core::json::Value* field = payload.value->find(key)) {
        if (!field->is_number()) {
          return fail(action, Code::InvalidContract,
                      std::string("control.click ") + key + " must be a number");
        }
        const double raw = field->as_number();
        if (!std::isfinite(raw) || raw != std::trunc(raw) || raw < -2147483648.0 ||
            raw > 2147483647.0) {
          return fail(action, Code::InvalidContract,
                      std::string("control.click ") + key + " must be a 32-bit integer");
        }
        (std::string(key) == "x" ? x : y) = static_cast<int>(raw);
      }
    }
    click.x = x;
    click.y = y;
    std::uint64_t settle_ms = 0;
    if (!read_optional_ms(*payload.value, "settleMs", 0, settle_ms)) {
      return fail(action, Code::InvalidContract,
                  "control.click settleMs must be an integer in 0..3600000");
    }
    if (const auto click_error = service_.control_click(id, click, timeout); !click_error.ok()) {
      return fail(action, click_error.code, click_error.message);
    }
    if (settle_ms > 0 && !settle_wait(settle_ms, cancellation)) {
      return {action.id, false, true, "control.click cancelled during settle",
              {Code::Cancelled, "cancelled"}, {}};
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("clicks", rime::core::json::Value::number(static_cast<double>(count)));
    return {action.id, true, false, "control clicked", {}, std::move(result_value)};
  }

  if (action.type == "control.focus") {
    if (const auto focus_error = service_.control_focus(id, timeout); !focus_error.ok()) {
      return fail(action, focus_error.code, focus_error.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("focused", rime::core::json::Value::boolean(true));
    return {action.id, true, false, "control focused", {}, std::move(result_value)};
  }

  if (action.type == "control.settext" || action.type == "control.sendtext") {
    const rime::core::json::Value* text = payload.value->find("text");
    if (!text || !text->is_string()) {
      return fail(action, Code::InvalidContract, action.type + " payload requires a string text");
    }
    const std::string utf8 = text->as_string();
    if (utf8.size() > 1'000'000) {
      return fail(action, Code::InvalidContract, action.type + " text is too long");
    }
    // Service takes wide text; the UTF-8 -> wide step cannot fail loudly
    // here (best-effort conversion, like the clipboard path) - an empty
    // conversion of a non-empty input is rejected instead of silently
    // clearing the control.
    std::wstring wide;
    wide.resize(utf8.size() + 1, L'\0');
    const int converted =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                            static_cast<int>(utf8.size()), wide.data(), static_cast<int>(wide.size()));
    if (converted <= 0 && !utf8.empty()) {
      return fail(action, Code::InvalidContract, action.type + " text is not valid UTF-8");
    }
    wide.resize(converted > 0 ? static_cast<std::size_t>(converted) : 0);
    rime::core::Error op_error = rime::core::Error::none();
    if (action.type == "control.settext") {
      op_error = service_.control_set_text(id, wide, timeout);
    } else {
      op_error = service_.control_send_text(id, wide, timeout);
    }
    if (!op_error.ok()) {
      return fail(action, op_error.code, op_error.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("text", rime::core::json::Value::string(utf8));
    return {action.id, true, false,
            action.type == "control.settext" ? "control text set" : "control text sent", {},
            std::move(result_value)};
  }

  // control.getText: no payload fields.
  std::string out;
  if (const auto read_error = service_.control_get_text(id, out, timeout); !read_error.ok()) {
    return fail(action, read_error.code, read_error.message);
  }
  if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
  rime::core::json::Value result_value = rime::core::json::Value::object();
  result_value.set("text", rime::core::json::Value::string(out));
  return {action.id, true, false, "control text read", {}, std::move(result_value)};
}

}  // namespace rime::win32
