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
#include <vector>

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
      "control.sendtext", "control.list.add", "control.list.delete", "control.list.choose",
      "control.list.find", "control.list.index", "control.list.choice", "control.list.items",
      "control.tab.select", "control.edit.count", "control.edit.caret", "control.edit.line",
      "control.edit.selected", "control.edit.paste", "control.setchecked", "control.ischecked"};
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

// Required string field (text payloads).
bool read_text_field(const rime::core::json::Value& payload, const char* key,
                     const std::string& type, std::string& out, std::string& error) {
  const rime::core::json::Value* field = payload.find(key);
  if (!field || !field->is_string()) {
    error = type + " payload requires a string " + key;
    return false;
  }
  out = field->as_string();
  return true;
}

// Required 1-based index field (0 allowed where AHK assigns it meaning).
bool read_index_field(const rime::core::json::Value& payload, const char* key,
                      const std::string& type, bool allow_zero, int& out, std::string& error) {
  const rime::core::json::Value* field = payload.find(key);
  if (!field || !field->is_number()) {
    error = type + " payload requires an integer " + key;
    return false;
  }
  const double raw = field->as_number();
  const double low = allow_zero ? 0.0 : 1.0;
  if (!std::isfinite(raw) || raw != std::trunc(raw) || raw < low || raw > 1'000'000.0) {
    error = type + " " + key + " is out of range";
    return false;
  }
  out = static_cast<int>(raw);
  return true;
}

// Strict UTF-8 -> wide (MB_ERR_INVALID_CHARS): garbage in is a contract
// error, never a silent replacement character.
bool utf8_to_wide_strict(const std::string& utf8, std::wstring& out, std::string& error,
                         const std::string& what) {
  out.clear();
  if (utf8.empty()) return true;
  if (utf8.size() > 1'000'000) {
    error = what + " text is too long";
    return false;
  }
  std::wstring wide(utf8.size() + 1, L'\0');
  const int converted =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                          static_cast<int>(utf8.size()), wide.data(), static_cast<int>(wide.size()));
  if (converted <= 0) {
    error = what + " text is not valid UTF-8";
    return false;
  }
  wide.resize(static_cast<std::size_t>(converted));
  out = std::move(wide);
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
    std::string utf8;
    std::string field_error;
    if (!read_text_field(*payload.value, "text", action.type, utf8, field_error)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    // Service takes wide text; the UTF-8 -> wide step rejects garbage loudly
    // (MB_ERR_INVALID_CHARS) instead of silently clearing the control.
    std::wstring wide;
    if (!utf8_to_wide_strict(utf8, wide, field_error, action.type)) {
      return fail(action, Code::InvalidContract, field_error);
    }
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

  // ---- List family ------------------------------------------------------
  // All list verbs resolve through the same class-substring table (AHK
  // GetIndexControlType rule) inside the service; the executor only shapes
  // payloads and results. Indexes are 1-based on the wire.
  if (action.type == "control.list.add") {
    std::string utf8;
    std::string field_error;
    if (!read_text_field(*payload.value, "text", action.type, utf8, field_error)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    std::wstring wide;
    if (!utf8_to_wide_strict(utf8, wide, field_error, action.type)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    int index1 = 0;
    if (const auto op = service_.control_list_add(id, wide, index1, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("index", rime::core::json::Value::number(static_cast<double>(index1)));
    return {action.id, true, false, "control item added", {}, std::move(result_value)};
  }
  if (action.type == "control.list.delete") {
    int index1 = 0;
    std::string field_error;
    if (!read_index_field(*payload.value, "index", action.type, false, index1, field_error)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    if (const auto op = service_.control_list_delete(id, index1, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("deleted", rime::core::json::Value::boolean(true));
    return {action.id, true, false, "control item deleted", {}, std::move(result_value)};
  }
  if (action.type == "control.list.choose") {
    const rime::core::json::Value* index_field = payload.value->find("index");
    const rime::core::json::Value* text_field = payload.value->find("text");
    const bool has_index = index_field != nullptr;
    const bool has_text = text_field != nullptr;
    if (has_index == has_text) {
      return fail(action, Code::InvalidContract,
                  "control.list.choose payload needs exactly one of index or text");
    }
    bool notify = true;
    if (const rime::core::json::Value* notify_field = payload.value->find("notifyParent")) {
      if (!notify_field->is_bool()) {
        return fail(action, Code::InvalidContract,
                    "control.list.choose notifyParent must be a boolean");
      }
      notify = notify_field->as_bool();
    }
    std::string field_error;
    if (has_index) {
      int index1 = 0;
      if (!read_index_field(*payload.value, "index", action.type, true, index1, field_error)) {
        return fail(action, Code::InvalidContract, field_error);
      }
      if (const auto op = service_.control_list_choose_index(id, index1, notify, timeout);
          !op.ok()) {
        return fail(action, op.code, op.message);
      }
    } else {
      std::string utf8;
      if (!read_text_field(*payload.value, "text", action.type, utf8, field_error)) {
        return fail(action, Code::InvalidContract, field_error);
      }
      std::wstring wide;
      if (!utf8_to_wide_strict(utf8, wide, field_error, action.type)) {
        return fail(action, Code::InvalidContract, field_error);
      }
      if (const auto op = service_.control_list_choose_text(id, wide, notify, timeout);
          !op.ok()) {
        return fail(action, op.code, op.message);
      }
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("chosen", rime::core::json::Value::boolean(true));
    return {action.id, true, false, "control item chosen", {}, std::move(result_value)};
  }
  if (action.type == "control.list.find") {
    std::string utf8;
    std::string field_error;
    if (!read_text_field(*payload.value, "text", action.type, utf8, field_error)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    std::wstring wide;
    if (!utf8_to_wide_strict(utf8, wide, field_error, action.type)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    int index1 = 0;
    if (const auto op = service_.control_list_find(id, wide, index1, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("index", rime::core::json::Value::number(static_cast<double>(index1)));
    return {action.id, true, false, "control item found", {}, std::move(result_value)};
  }
  if (action.type == "control.list.index") {
    int index1 = 0;
    if (const auto op = service_.control_list_index(id, index1, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("index", rime::core::json::Value::number(static_cast<double>(index1)));
    return {action.id, true, false, "control index read", {}, std::move(result_value)};
  }
  if (action.type == "control.list.choice") {
    int index1 = 0;
    std::string field_error;
    if (payload.value->find("index") != nullptr) {
      if (!read_index_field(*payload.value, "index", action.type, true, index1, field_error)) {
        return fail(action, Code::InvalidContract, field_error);
      }
    }
    std::string out;
    if (const auto op = service_.control_list_choice(id, index1, out, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("text", rime::core::json::Value::string(out));
    return {action.id, true, false, "control choice read", {}, std::move(result_value)};
  }
  if (action.type == "control.list.items") {
    std::uint64_t limit = 100;
    std::string field_error;
    if (payload.value->find("limit") != nullptr) {
      if (!read_optional_ms(*payload.value, "limit", 100, limit) || limit == 0 ||
          limit > 10000) {
        return fail(action, Code::InvalidContract,
                    "control.list.items limit must be an integer in 1..10000");
      }
    }
    std::vector<std::string> items;
    if (const auto op =
            service_.control_list_items(id, static_cast<std::size_t>(limit), items, timeout);
        !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value array = rime::core::json::Value::array();
    for (auto& item : items) array.push(rime::core::json::Value::string(item));
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("items", std::move(array));
    return {action.id, true, false, "control items read", {}, std::move(result_value)};
  }

  // ---- Tab ---------------------------------------------------------------
  if (action.type == "control.tab.select") {
    int index1 = 0;
    std::string field_error;
    if (!read_index_field(*payload.value, "index", action.type, false, index1, field_error)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    if (const auto op = service_.control_tab_select(id, index1, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("selected", rime::core::json::Value::boolean(true));
    return {action.id, true, false, "control tab selected", {}, std::move(result_value)};
  }

  // ---- Edit ---------------------------------------------------------------
  if (action.type == "control.edit.count") {
    int lines = 0;
    if (const auto op = service_.control_edit_count(id, lines, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("lines", rime::core::json::Value::number(static_cast<double>(lines)));
    return {action.id, true, false, "control lines counted", {}, std::move(result_value)};
  }
  if (action.type == "control.edit.caret") {
    int line1 = 0;
    int col1 = 0;
    if (const auto op = service_.control_edit_caret(id, line1, col1, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("line", rime::core::json::Value::number(static_cast<double>(line1)));
    result_value.set("col", rime::core::json::Value::number(static_cast<double>(col1)));
    return {action.id, true, false, "control caret read", {}, std::move(result_value)};
  }
  if (action.type == "control.edit.line") {
    int line1 = 0;
    std::string field_error;
    if (!read_index_field(*payload.value, "line", action.type, false, line1, field_error)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    std::string out;
    if (const auto op = service_.control_edit_line(id, line1, out, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("text", rime::core::json::Value::string(out));
    return {action.id, true, false, "control line read", {}, std::move(result_value)};
  }
  if (action.type == "control.edit.selected") {
    std::string out;
    if (const auto op = service_.control_edit_selected(id, out, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("text", rime::core::json::Value::string(out));
    return {action.id, true, false, "control selection read", {}, std::move(result_value)};
  }
  if (action.type == "control.edit.paste") {
    std::string utf8;
    std::string field_error;
    if (!read_text_field(*payload.value, "text", action.type, utf8, field_error)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    std::wstring wide;
    if (!utf8_to_wide_strict(utf8, wide, field_error, action.type)) {
      return fail(action, Code::InvalidContract, field_error);
    }
    if (const auto op = service_.control_edit_paste(id, wide, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("text", rime::core::json::Value::string(utf8));
    return {action.id, true, false, "control text pasted", {}, std::move(result_value)};
  }

  // ---- Checkboxes ----------------------------------------------------------
  if (action.type == "control.setchecked") {
    const rime::core::json::Value* field = payload.value->find("checked");
    if (!field || !field->is_number()) {
      return fail(action, Code::InvalidContract,
                  "control.setchecked payload requires an integer checked");
    }
    const double raw = field->as_number();
    if (!std::isfinite(raw) || raw != std::trunc(raw) || raw < -1.0 || raw > 1.0) {
      return fail(action, Code::InvalidContract,
                  "control.setchecked checked must be -1, 0 or 1");
    }
    bool ensure_active = false;
    if (const rime::core::json::Value* active = payload.value->find("ensureActive")) {
      if (!active->is_bool()) {
        return fail(action, Code::InvalidContract,
                    "control.setchecked ensureActive must be a boolean");
      }
      ensure_active = active->as_bool();
    }
    if (const auto op =
            service_.control_set_checked(id, static_cast<int>(raw), ensure_active, timeout);
        !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("checked", rime::core::json::Value::boolean(raw != 0.0));
    return {action.id, true, false, "control check set", {}, std::move(result_value)};
  }
  if (action.type == "control.ischecked") {
    bool checked = false;
    if (const auto op = service_.control_is_checked(id, checked, timeout); !op.ok()) {
      return fail(action, op.code, op.message);
    }
    if (const auto bad = contract::cancel_after(action, cancellation)) return *bad;
    rime::core::json::Value result_value = rime::core::json::Value::object();
    result_value.set("checked", rime::core::json::Value::boolean(checked));
    return {action.id, true, false, "control check read", {}, std::move(result_value)};
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
