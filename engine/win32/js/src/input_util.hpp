#pragma once

// Internal helpers shared by input_module.cpp (rime:input core exports) and
// events_module.cpp (Hotkey/Hotstring/timer/message/... exports). Not a
// public API: both modules live in rime::win32 and are compiled into
// rime_win32_js.

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/win32/input.hpp"
#include "rime/win32/js_input.hpp"

#include "quickjs.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace rime::win32 {

inline rime::js::Host* host_of(JSContext* context) {
  return static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
}

inline InputModuleBinding* binding_of(JSContext* context) {
  auto* host = host_of(context);
  if (!host) return nullptr;
  return static_cast<InputModuleBinding*>(host->module_data("rime:input"));
}

// Strict JS number -> int64: non-numbers, NaN/Infinity, fractions and
// out-of-range values raise a TypeError instead of truncating.
inline bool strict_int64(JSContext* context, JSValueConst value, std::int64_t& out,
                         const char* what) {
  if (!JS_IsNumber(value)) {
    JS_ThrowTypeError(context, "%s: id must be a number", what);
    return false;
  }
  double number = 0;
  if (JS_ToFloat64(context, &number, value)) return false;
  if (!std::isfinite(number) || std::trunc(number) != number) {
    JS_ThrowTypeError(context, "%s: id must be an integer", what);
    return false;
  }
  constexpr double kMaxSafeInteger = 9007199254740991.0;  // 2^53 - 1
  if (number < -kMaxSafeInteger || number > kMaxSafeInteger) {
    JS_ThrowTypeError(context, "%s: id is out of range", what);
    return false;
  }
  if (JS_ToInt64(context, &out, value)) return false;
  return true;
}

// Wire shape of one InputEvent as delivered through the host event queue.
inline std::string event_json(const InputEvent& event) {
  namespace json = rime::core::json;
  json::Value value = json::Value::object();
  value.set("sequence", json::Value::number(static_cast<double>(event.sequence)));
  value.set("timestamp", json::Value::number(static_cast<double>(event.timestamp_ms)));
  value.set("injected", json::Value::boolean(event.injected));
  value.set("selfInjected", json::Value::boolean(event.self_injected));
  if (event.kind == InputEventKind::Key) {
    value.set("kind", json::Value::string("key"));
    value.set("down", json::Value::boolean(event.key_down));
    value.set("vk", json::Value::number(static_cast<double>(event.vk)));
    value.set("scan", json::Value::number(static_cast<double>(event.scan)));
    value.set("alt", json::Value::boolean(event.alt));
    value.set("control", json::Value::boolean(event.control));
    value.set("shift", json::Value::boolean(event.shift));
    value.set("super", json::Value::boolean(event.super));
    return json::stringify(value);
  }
  value.set("kind", json::Value::string("mouse"));
  const char* action = "move";
  switch (event.mouse_action) {
    case MouseAction::Down:
      action = "down";
      break;
    case MouseAction::Up:
      action = "up";
      break;
    case MouseAction::Wheel:
      action = "wheel";
      break;
    case MouseAction::Move:
      break;
  }
  value.set("action", json::Value::string(action));
  value.set("x", json::Value::number(static_cast<double>(event.x)));
  value.set("y", json::Value::number(static_cast<double>(event.y)));
  value.set("button", json::Value::number(static_cast<double>(event.button)));
  value.set("wheelDelta", json::Value::number(static_cast<double>(event.wheel_delta)));
  return json::stringify(value);
}

// Installing or binding a global hook is a privileged operation; the same
// policy the kernel enforces denies it here (registration returns
// synchronously, so the denial is a thrown Error naming the capability).
inline JSValue throw_capability_error(JSContext* context, const char* capability) {
  const std::string text = std::string("required capability was not granted: ") + capability;
  JSValue error = JS_NewError(context);
  if (JS_IsException(error)) return JS_EXCEPTION;
  JSValue message = JS_NewString(context, text.c_str());
  if (JS_IsException(message)) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  // JS_DefinePropertyValueStr consumes `message` on both success and failure.
  if (JS_DefinePropertyValueStr(context, error, "message", message,
                                JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  return JS_Throw(context, error);
}

inline std::string ascii_lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// Resolves one non-modifier chord token to its virtual-key code: letters and
// digits map directly (k -> 'K', 5 -> '5'), f1..f24 map to VK_F1..VK_F24,
// and the named table covers common navigation and lock keys. Unknown or
// out-of-range tokens fail with an error message instead of guessing a key.
inline bool chord_key(const std::string& token, std::uint32_t& vk, std::string& error) {
  if (token.size() == 1) {
    const char c = token[0];
    if (c >= 'a' && c <= 'z') {
      vk = static_cast<std::uint32_t>(c - 'a' + 'A');
      return true;
    }
    if (c >= '0' && c <= '9') {
      vk = static_cast<std::uint32_t>(c);
      return true;
    }
    error = "unknown chord token: " + token;
    return false;
  }
  if (token[0] == 'f' && token.size() <= 3) {
    int number = 0;
    for (std::size_t index = 1; index < token.size(); ++index) {
      if (token[index] < '0' || token[index] > '9') {
        error = "unknown chord token: " + token;
        return false;
      }
      number = number * 10 + (token[index] - '0');
    }
    if (number < 1 || number > 24) {
      error = "function key out of range (f1..f24): " + token;
      return false;
    }
    vk = 0x70u + static_cast<std::uint32_t>(number) - 1u;
    return true;
  }
  struct NamedKey {
    const char* name;
    std::uint32_t vk;
  };
  static constexpr NamedKey kNamedKeys[] = {
      {"space", 0x20},   {"tab", 0x09},      {"enter", 0x0D},   {"return", 0x0D},
      {"escape", 0x1B},  {"esc", 0x1B},      {"backspace", 0x08},
      {"delete", 0x2E},  {"del", 0x2E},      {"insert", 0x2D},  {"ins", 0x2D},
      {"home", 0x24},    {"end", 0x23},      {"pageup", 0x21},  {"pgup", 0x21},
      {"pagedown", 0x22},{"pgdn", 0x22},     {"left", 0x25},    {"up", 0x26},
      {"right", 0x27},   {"down", 0x28},     {"pause", 0x13},   {"capslock", 0x14},
      {"numlock", 0x90}, {"scrolllock", 0x91},
  };
  for (const auto& named : kNamedKeys) {
    if (token == named.name) {
      vk = named.vk;
      return true;
    }
  }
  error = "unknown chord token: " + token;
  return false;
}

// Parses a chord such as "ctrl+shift+k" into a virtual-key code plus an
// exact modifier mask. Modifier aliases: ctrl|control, alt, shift,
// super|win|logo. A chord carries exactly one key token and may not repeat a
// modifier; empty tokens ("" or "ctrl+") and unknown spellings are errors.
inline bool parse_chord(const std::string& text, std::uint32_t& vk, std::uint8_t& mask,
                        std::string& error) {
  constexpr std::uint8_t kModAlt = 1u << 0;
  constexpr std::uint8_t kModControl = 1u << 1;
  constexpr std::uint8_t kModShift = 1u << 2;
  constexpr std::uint8_t kModSuper = 1u << 3;
  vk = 0;
  mask = 0;
  error.clear();
  bool have_key = false;
  std::size_t start = 0;
  for (;;) {
    const auto plus = text.find('+', start);
    const auto end = plus == std::string::npos ? text.size() : plus;
    if (end == start) {
      error = "empty chord token";
      return false;
    }
    const std::string token = ascii_lower(std::string_view(text).substr(start, end - start));
    const auto repeat = [&error](std::string_view alias) {
      error = "duplicate modifier: " + std::string(alias);
      return false;
    };
    if (token == "ctrl" || token == "control") {
      if (mask & kModControl) return repeat(token);
      mask |= kModControl;
    } else if (token == "alt") {
      if (mask & kModAlt) return repeat(token);
      mask |= kModAlt;
    } else if (token == "shift") {
      if (mask & kModShift) return repeat(token);
      mask |= kModShift;
    } else if (token == "super" || token == "win" || token == "logo") {
      if (mask & kModSuper) return repeat(token);
      mask |= kModSuper;
    } else {
      std::uint32_t key_vk = 0;
      if (!chord_key(token, key_vk, error)) return false;
      if (have_key) {
        error = "chord takes exactly one key token: " + token;
        return false;
      }
      vk = key_vk;
      have_key = true;
    }
    if (plus == std::string::npos) break;
    start = end + 1;
  }
  if (!have_key) {
    error = "chord needs one key token alongside its modifiers";
    return false;
  }
  return true;
}

// Reads one property from the delivered event object. Event objects come
// from JSON parsing so property access cannot realistically throw; if it
// ever does, the exception is cleared and the read fails the match rather
// than stranding a pending exception inside the drain loop.
inline JSValue event_field(JSContext* context, JSValueConst event, const char* name) {
  JSValue value = JS_GetPropertyStr(context, event, name);
  if (JS_IsException(value)) {
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
    return JS_UNDEFINED;
  }
  return value;
}

// True when the event's four modifier flags are exactly the chord mask:
// ctrl+shift+k must not fire while win is also held.
inline bool modifiers_match(JSContext* context, JSValueConst event, const std::uint8_t mask) {
  constexpr std::uint8_t kModAlt = 1u << 0;
  constexpr std::uint8_t kModControl = 1u << 1;
  constexpr std::uint8_t kModShift = 1u << 2;
  constexpr std::uint8_t kModSuper = 1u << 3;
  struct Modifier {
    const char* name;
    std::uint8_t bit;
  };
  static constexpr Modifier kModifiers[] = {
      {"alt", kModAlt}, {"control", kModControl}, {"shift", kModShift}, {"super", kModSuper}};
  std::uint8_t seen = 0;
  for (const auto& modifier : kModifiers) {
    JSValue value = event_field(context, event, modifier.name);
    if (!JS_IsBool(value)) {
      JS_FreeValue(context, value);
      return false;
    }
    if (JS_ToBool(context, value)) seen |= modifier.bit;
    JS_FreeValue(context, value);
  }
  return seen == mask;
}

// Matches the delivered InputEvent against the binding: a key-down whose vk
// equals the chord key and whose modifiers form exactly the chord mask.
// Foreign injected input matches (external automation keeps working); input
// this process sent through send() never does, so an action that injects
// keys cannot feed its own chord - the loop-prevention rule at send level.
inline bool matches_chord(JSContext* context, JSValueConst event, const ChordBinding& chord) {
  JSValue self_value = event_field(context, event, "selfInjected");
  const bool self_input = JS_IsBool(self_value) && JS_ToBool(context, self_value) > 0;
  JS_FreeValue(context, self_value);
  if (self_input) return false;

  JSValue kind = event_field(context, event, "kind");
  if (!JS_IsString(kind)) {
    JS_FreeValue(context, kind);
    return false;
  }
  bool key_event = false;
  if (const char* text = JS_ToCString(context, kind)) {
    key_event = std::string_view(text) == "key";
    JS_FreeCString(context, text);
  }
  JS_FreeValue(context, kind);
  if (!key_event) return false;

  JSValue down = event_field(context, event, "down");
  const bool pressed = JS_IsBool(down) && JS_ToBool(context, down) > 0;
  JS_FreeValue(context, down);
  if (!pressed) return false;

  JSValue vk_value = event_field(context, event, "vk");
  double vk_number = -1;
  const bool vk_is_number =
      JS_IsNumber(vk_value) && JS_ToFloat64(context, &vk_number, vk_value) == 0;
  JS_FreeValue(context, vk_value);
  if (!vk_is_number || vk_number != static_cast<double>(chord.vk)) return false;

  return modifiers_match(context, event, chord.mask);
}

// Reads one required non-empty string field from an action template.
// Missing or ill-typed values throw a TypeError naming the field (`path` is
// the dotted label used in messages) so a malformed template can never
// reach the queue.
inline bool required_string(JSContext* context, JSValueConst object, const char* property,
                            const char* path, std::string& out) {
  JSValue value = JS_GetPropertyStr(context, object, property);
  if (JS_IsException(value)) return false;
  if (!JS_IsString(value)) {
    JS_FreeValue(context, value);
    JS_ThrowTypeError(context, "%s must be a non-empty string", path);
    return false;
  }
  const char* text = JS_ToCString(context, value);
  JS_FreeValue(context, value);
  if (!text) return false;
  out.assign(text);
  JS_FreeCString(context, text);
  if (out.empty()) {
    JS_ThrowTypeError(context, "%s must be a non-empty string", path);
    return false;
  }
  return true;
}

// Reads an action template (type/capability/target/payload) from an object,
// shared by input.bind and the M2-C exports so both validate identically.
// Validation is done once at registration: a matching trigger can then only
// ever build a contract-shaped action (the kernel re-checks shape anyway).
// `path` prefixes every TypeError message (usually "action").
inline bool read_action_template(JSContext* context, JSValueConst object, EventAction& out,
                                 const char* path) {
  const std::string type_path = std::string(path) + ".type";
  const std::string capability_path = std::string(path) + ".capability";
  const std::string target_path = std::string(path) + ".target";
  const std::string target_kind_path = target_path + ".kind";
  const std::string target_id_path = target_path + ".id";
  const std::string payload_path = std::string(path) + ".payload";
  if (!required_string(context, object, "type", type_path.c_str(), out.type) ||
      !required_string(context, object, "capability", capability_path.c_str(), out.capability)) {
    return false;
  }
  JSValue target = JS_GetPropertyStr(context, object, "target");
  if (JS_IsException(target)) return false;
  bool target_ok = JS_IsObject(target);
  if (target_ok) {
    target_ok = required_string(context, target, "kind", target_kind_path.c_str(),
                                out.target_kind) &&
                required_string(context, target, "id", target_id_path.c_str(), out.target_id);
  } else {
    JS_ThrowTypeError(context, "%s must be an object with kind and id", target_path.c_str());
  }
  JS_FreeValue(context, target);
  if (!target_ok) return false;

  // payload is stringified once at registration time so the trigger path is
  // just a copy; JSON.stringify semantics mean circular values throw now,
  // not on some later trigger.
  JSValue payload = JS_GetPropertyStr(context, object, "payload");
  if (JS_IsException(payload)) return false;
  if (JS_IsUndefined(payload) || JS_IsNull(payload)) {
    JS_FreeValue(context, payload);
    out.payload = "{}";
    return true;
  }
  if (!JS_IsObject(payload)) {
    JS_FreeValue(context, payload);
    JS_ThrowTypeError(context, "%s must be an object", payload_path.c_str());
    return false;
  }
  JSValue global = JS_GetGlobalObject(context);
  JSValue json = JS_GetPropertyStr(context, global, "JSON");
  JS_FreeValue(context, global);
  if (JS_IsException(json)) {
    JS_FreeValue(context, payload);
    return false;
  }
  JSValue stringify = JS_GetPropertyStr(context, json, "stringify");
  if (JS_IsException(stringify)) {
    JS_FreeValue(context, json);
    JS_FreeValue(context, payload);
    return false;
  }
  JSValue text = JS_Call(context, stringify, json, 1, &payload);
  JS_FreeValue(context, stringify);
  JS_FreeValue(context, json);
  if (JS_IsException(text)) {
    JS_FreeValue(context, payload);
    return false;
  }
  const bool is_string = JS_IsString(text);
  const char* utf8 = is_string ? JS_ToCString(context, text) : nullptr;
  JS_FreeValue(context, text);
  if (!is_string || !utf8) {
    JS_FreeValue(context, payload);
    if (!is_string) {
      JS_ThrowTypeError(context, "%s must serialize to a JSON object", payload_path.c_str());
      return false;
    }
    return false;
  }
  // The action contract requires a JSON object: a payload whose toJSON
  // collapsed it to a primitive is rejected at registration, not at execution.
  const bool object_text = utf8[0] == '{';
  if (object_text) out.payload.assign(utf8);
  JS_FreeCString(context, utf8);
  JS_FreeValue(context, payload);
  if (!object_text) {
    JS_ThrowTypeError(context, "%s must serialize to a JSON object", payload_path.c_str());
    return false;
  }
  return true;
}

}  // namespace rime::win32
