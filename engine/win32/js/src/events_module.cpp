#include "events_module.hpp"

#include "async_task.hpp"
#include "input_util.hpp"
#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/win32/clipboard.hpp"
#include "rime/win32/context_watcher.hpp"
#include "rime/win32/ui_thread.hpp"
#include "rime/win32/window.hpp"

#include "quickjs.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// M2-C event exports: Hotkey/Hotstring (+ HotIf criteria), InstallKeybdHook/
// InstallMouseHook, SetTimer, OnMessage, OnClipboardChange, OnError, OnExit.
// Every registration returns the same Subscription object shape
// ({ id, kind, close() }); every callback is delivered on the JS thread
// through the host event queue or a direct JS-thread invocation - the hook
// thread never runs script (see docs/api/hotkey-events.md for the per-API
// dispatch model and the documented AHK deviations).

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

constexpr const char* kHookCapability = "windows.hook.global";
constexpr const char* kInjectCapability = "windows.input.inject";
constexpr const char* kClipboardReadCapability = "windows.clipboard.read";
// WM_CLIPBOARDUPDATE: posted to the pump window once the OS listener is
// attached (AddClipboardFormatListener).
constexpr unsigned int kWmClipboardUpdate = 0x031D;
constexpr std::int64_t kDefaultTimerPeriod = 250;
constexpr std::int64_t kMaxTimerDelayMs = 0x7FFFFFFF;
constexpr std::int64_t kMaxMessageInstances = 255;
// AHK's default end chars: whitespace plus the common word delimiters.
constexpr const char* kDefaultEndChars = "-{}[]()';:'\"`\\,.?!\n\t ";

enum class SpecResult { Spec, OptionsOnly, Invalid };

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t timer_delay(std::int64_t period_ms) {
  const std::int64_t delay = period_ms < 0 ? -period_ms : period_ms;
  return delay > kMaxTimerDelayMs ? kMaxTimerDelayMs : delay;
}

// Strict integer read with a caller-named label (period, msgNumber, ...).
bool parse_i64(JSContext* context, JSValueConst value, std::int64_t& out, const char* label) {
  if (!JS_IsNumber(value)) {
    JS_ThrowTypeError(context, "%s: expected a number", label);
    return false;
  }
  double number = 0;
  if (JS_ToFloat64(context, &number, value)) return false;
  if (number != number || number > 9007199254740991.0 || number < -9007199254740991.0 ||
      static_cast<std::int64_t>(number) != number) {
    JS_ThrowTypeError(context, "%s: expected an integer", label);
    return false;
  }
  out = static_cast<std::int64_t>(number);
  return true;
}

// Reads "on"|"off"|"toggle" (case-insensitive). Returns -1 for
// undefined/null; throws a TypeError for anything else.
int parse_ctrl_word(JSContext* context, JSValueConst value, const char* label) {
  if (JS_IsUndefined(value) || JS_IsNull(value)) return -1;
  if (!JS_IsString(value)) {
    JS_ThrowTypeError(context, "%s: expected 'on', 'off' or 'toggle'", label);
    return -1;
  }
  const char* text = JS_ToCString(context, value);
  if (!text) return -1;
  const std::string word = ascii_lower(text);
  JS_FreeCString(context, text);
  if (word == "on") return 1;
  if (word == "off") return 0;
  if (word == "toggle") return 2;
  JS_ThrowTypeError(context, "%s: expected 'on', 'off' or 'toggle'", label);
  return -1;
}

// Options string for hotkey(): whitespace/comma separated On/Off tokens,
// later tokens win. Anything else is rejected (AHK's B/P/S/T/I letters need
// hook-side behavior this stage does not implement).
bool parse_onoff_options(JSContext* context, JSValueConst value, bool& have, bool& enabled) {
  have = false;
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  if (!JS_IsString(value)) {
    JS_ThrowTypeError(context, "hotkey(name, action, options): options must be a string");
    return false;
  }
  const char* text = JS_ToCString(context, value);
  if (!text) return false;
  const std::string options(text);
  JS_FreeCString(context, text);
  std::size_t start = 0;
  while (start <= options.size()) {
    const auto end = options.find_first_of(" \t,", start);
    const auto token = options.substr(start, end == std::string::npos ? end : end - start);
    start = end == std::string::npos ? options.size() + 1 : end + 1;
    if (token.empty()) continue;
    const std::string lowered = ascii_lower(token);
    if (lowered == "on") {
      have = true;
      enabled = true;
    } else if (lowered == "off") {
      have = true;
      enabled = false;
    } else {
      JS_ThrowTypeError(context,
                        "hotkey(name, action, options): unsupported option '%s' (only On and Off "
                        "are accepted)",
                        token.c_str());
      return false;
    }
  }
  return true;
}

// ---- M2-D registration options ---------------------------------------------

// Options accepted by hotkey(name, action, options) and
// hotstring(spec, action, onOff): the existing string forms, or an object
// { on?, suspendExempt?, inputLevel? }. Validation happens before any
// capability check and before a registration is touched.
struct RegOptions {
  int control{-1};  // -1 absent, 0 off, 1 on, 2 toggle (string form only)
  bool have_exempt{false};
  bool suspend_exempt{false};
  bool have_level{false};
  std::uint32_t input_level{0};
};

bool parse_reg_options(JSContext* context, JSValueConst value, const char* label,
                       const bool allow_toggle, RegOptions& out) {
  out = RegOptions{};
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  if (JS_IsString(value)) {
    if (allow_toggle) {
      out.control = parse_ctrl_word(context, value, label);
      return out.control >= 0;
    }
    bool have = false;
    bool enabled = true;
    if (!parse_onoff_options(context, value, have, enabled)) return false;
    if (have) out.control = enabled ? 1 : 0;
    return true;
  }
  if (!JS_IsObject(value)) {
    JS_ThrowTypeError(context, "%s: options must be 'on'|'off'|'toggle' or an object", label);
    return false;
  }
  JSPropertyEnum* names = nullptr;
  std::uint32_t count = 0;
  if (JS_GetOwnPropertyNames(context, &names, &count, value, JS_GPN_STRING_MASK) < 0) return false;
  for (std::uint32_t index = 0; index < count; ++index) {
    const char* key = JS_AtomToCString(context, names[index].atom);
    if (!key) {
      JS_FreePropertyEnum(context, names, count);
      return false;
    }
    const std::string name(key);
    JS_FreeCString(context, key);
    if (name != "on" && name != "suspendExempt" && name != "inputLevel") {
      JS_FreePropertyEnum(context, names, count);
      JS_ThrowTypeError(context, "%s: unsupported option '%s' (expected on, suspendExempt or "
                                 "inputLevel)",
                        label, name.c_str());
      return false;
    }
  }
  JS_FreePropertyEnum(context, names, count);
  JSValue entry = JS_GetPropertyStr(context, value, "on");
  if (JS_IsException(entry)) return false;
  if (!JS_IsUndefined(entry)) {
    if (!JS_IsBool(entry)) {
      JS_FreeValue(context, entry);
      JS_ThrowTypeError(context, "%s: options.on must be a boolean", label);
      return false;
    }
    out.control = JS_ToBool(context, entry) > 0 ? 1 : 0;
  }
  JS_FreeValue(context, entry);
  entry = JS_GetPropertyStr(context, value, "suspendExempt");
  if (JS_IsException(entry)) return false;
  if (!JS_IsUndefined(entry)) {
    if (!JS_IsBool(entry)) {
      JS_FreeValue(context, entry);
      JS_ThrowTypeError(context, "%s: options.suspendExempt must be a boolean", label);
      return false;
    }
    out.have_exempt = true;
    out.suspend_exempt = JS_ToBool(context, entry) > 0;
  }
  JS_FreeValue(context, entry);
  entry = JS_GetPropertyStr(context, value, "inputLevel");
  if (JS_IsException(entry)) return false;
  if (!JS_IsUndefined(entry)) {
    double number = 0;
    if (!JS_IsNumber(entry) || JS_ToFloat64(context, &number, entry) != 0 || number < 0 ||
        number > 4294967295.0 || number != static_cast<double>(static_cast<std::uint64_t>(number))) {
      JS_FreeValue(context, entry);
      JS_ThrowTypeError(context, "%s: options.inputLevel must be a non-negative integer", label);
      return false;
    }
    out.have_level = true;
    out.input_level = static_cast<std::uint32_t>(number);
  }
  JS_FreeValue(context, entry);
  return true;
}

// Accepts both the Rime chord grammar ("ctrl+shift+k") and AHK's symbol
// prefixes ("^!a"); left/right variants and pass-through prefixes are
// rejected because this stage does not implement hook-side suppression.
bool normalize_hotkey_name(const std::string& text, std::uint32_t& vk, std::uint8_t& mask,
                           std::string& error) {
  if (parse_chord(text, vk, mask, error)) return true;
  std::string translated;
  translated.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '^':
        translated += "ctrl+";
        break;
      case '!':
        translated += "alt+";
        break;
      case '#':
        translated += "super+";
        break;
      case '+':
        if (translated.empty()) translated += "shift+";
        else translated += '+';
        break;
      case '<':
      case '>':
        error = "left/right modifier prefixes (<^ >^ <# ...) are not supported";
        return false;
      case '~':
      case '*':
      case '$':
        error = "unsupported hotkey prefix character (hook pass-through is not implemented)";
        return false;
      default:
        translated += c;
        break;
    }
  }
  return parse_chord(translated, vk, mask, error);
}

// Hotstring option letters (Hotstring::ParseOptions): supported * ? B C O R
// T Z X, rejected K P S, everything else is ignored like AHK does. R/T are
// accepted as no-ops - Rime always injects the replacement as raw text.
bool parse_hotstring_options(const std::string& options, EventsState::Hotstring& out,
                             std::string& error) {
  error.clear();
  for (std::size_t index = 0; index < options.size(); ++index) {
    const char letter = options[index];
    const char next = index + 1 < options.size() ? options[index + 1] : '\0';
    switch (letter) {
      case '*':
        out.wildcard = next != '0';
        break;
      case '?':
        out.inside_word = next != '0';
        break;
      case 'B':
        out.do_backspace = next != '0';
        break;
      case 'C':
        if (next == '0') {
          out.conform_case = true;
          out.case_sensitive = false;
        } else if (next == '1') {
          out.conform_case = false;
          out.case_sensitive = false;
        } else {
          out.conform_case = false;
          out.case_sensitive = true;
        }
        break;
      case 'O':
        out.omit_end_char = next != '0';
        break;
      case 'Z':
        out.do_reset = next != '0';
        break;
      case 'X':
        // Function/action behavior comes from the second argument;
        // hotstring(spec, fn) is the supported X-equivalent form.
        out.execute = next != '0';
        break;
      case 'R':
      case 'T':
        break;  // accepted, always raw (documented deviation)
      case 'K':
      case 'P':
      case 'S':
        error = std::string("unsupported option letter: ") + letter;
        return false;
      default:
        // Digits and unknown letters are ignored, matching AHK.
        break;
    }
  }
  return true;
}

// ":options:trigger::replacement" (or the options-only ":options:").
SpecResult parse_hotstring_spec(const std::string& spec,
                                const EventsState::Hotstring& defaults,
                                EventsState::Hotstring& out, std::string& error) {
  error.clear();
  if (spec.empty() || spec[0] != ':') {
    error = "spec must start with ':' (form: :options:trigger::replacement)";
    return SpecResult::Invalid;
  }
  const auto options_end = spec.find(':', 1);
  if (options_end == std::string::npos) {
    error = "spec must contain ':options:trigger::replacement'";
    return SpecResult::Invalid;
  }
  out = EventsState::Hotstring{};
  out.do_backspace = defaults.do_backspace;
  out.case_sensitive = defaults.case_sensitive;
  out.conform_case = defaults.conform_case;
  out.omit_end_char = defaults.omit_end_char;
  out.do_reset = defaults.do_reset;
  out.wildcard = defaults.wildcard;
  out.inside_word = defaults.inside_word;
  const std::string options = spec.substr(1, options_end - 1);
  if (!parse_hotstring_options(options, out, error)) return SpecResult::Invalid;
  const std::string rest = spec.substr(options_end + 1);
  if (rest.empty()) return SpecResult::OptionsOnly;
  const auto trigger_end = rest.find("::");
  if (trigger_end == std::string::npos) {
    error = "spec must contain '::' after its options";
    return SpecResult::Invalid;
  }
  out.trigger = rest.substr(0, trigger_end);
  out.replacement = rest.substr(trigger_end + 2);
  if (out.trigger.empty()) {
    error = "trigger must not be empty";
    return SpecResult::Invalid;
  }
  return SpecResult::Spec;
}

std::string make_hotstring_key(std::uint64_t criterion, bool case_sensitive, bool inside_word,
                               const std::string& trigger) {
  std::string key = std::to_string(criterion);
  key += case_sensitive ? "|1" : "|0";
  key += inside_word ? "|1" : "|0";
  key += '|';
  key += case_sensitive ? trigger : ascii_lower(trigger);
  return key;
}

std::size_t utf8_char_count(const std::string& text) {
  std::size_t count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0u) != 0x80u) ++count;
  }
  return count;
}

// Encodes UTF-8 as input.send steps: one down/up pair per UTF-16 code unit
// with the unicode flag so units above 0xFF stay valid.
void append_utf16_steps(const std::string& text, json::Value& steps) {
  auto push_unit = [&steps](const std::uint32_t unit) {
    json::Value down = json::Value::object();
    down.set("vk", json::Value::number(static_cast<double>(unit)));
    down.set("down", json::Value::boolean(true));
    down.set("unicode", json::Value::boolean(true));
    steps.push(std::move(down));
    json::Value up = json::Value::object();
    up.set("vk", json::Value::number(static_cast<double>(unit)));
    up.set("down", json::Value::boolean(false));
    up.set("unicode", json::Value::boolean(true));
    steps.push(std::move(up));
  };
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::uint32_t code_point = 0;
    std::size_t length = 1;
    if (first < 0x80) {
      code_point = first;
    } else if ((first & 0xE0u) == 0xC0u) {
      code_point = first & 0x1Fu;
      length = 2;
    } else if ((first & 0xF0u) == 0xE0u) {
      code_point = first & 0x0Fu;
      length = 3;
    } else if ((first & 0xF8u) == 0xF0u) {
      code_point = first & 0x07u;
      length = 4;
    } else {
      ++index;
      continue;
    }
    if (index + length > text.size()) break;
    bool valid = true;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0u) != 0x80u) {
        valid = false;
        break;
      }
      code_point = (code_point << 6) | (continuation & 0x3Fu);
    }
    if (!valid) {
      ++index;
      continue;
    }
    index += length;
    if (code_point <= 0xFFFF) {
      push_unit(code_point);
    } else {
      code_point -= 0x10000;
      push_unit(0xD800u + (code_point >> 10));
      push_unit(0xDC00u + (code_point & 0x3FFu));
    }
  }
}

// Basic conformToCase rules: an all-caps trigger uppercases the output, a
// capitalized trigger capitalizes its first letter, anything else passes.
std::string conform_case_text(const std::string& out, const std::string& trigger) {
  bool has_alpha = false;
  bool has_lower = false;
  bool has_upper = false;
  std::size_t first_alpha = std::string::npos;
  bool first_alpha_upper = false;
  for (std::size_t index = 0; index < trigger.size(); ++index) {
    const char c = trigger[index];
    if (c >= 'A' && c <= 'Z') {
      has_upper = true;
      has_alpha = true;
      if (first_alpha == std::string::npos) {
        first_alpha = index;
        first_alpha_upper = true;
      }
    } else if (c >= 'a' && c <= 'z') {
      has_lower = true;
      has_alpha = true;
      if (first_alpha == std::string::npos) {
        first_alpha = index;
        first_alpha_upper = false;
      }
    }
  }
  if (!has_alpha || out.empty()) return out;
  std::string result = out;
  if (has_upper && !has_lower) {
    for (char& c : result) {
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return result;
  }
  if (first_alpha_upper && result[0] >= 'a' && result[0] <= 'z') {
    result[0] = static_cast<char>(result[0] - 'a' + 'A');
  }
  return result;
}

void record_failure(rime::js::Host* host, const char* where, const rime::core::Error& error) {
  if (host) (void)host->record(where, error.message);
}

void free_owned(JSContext* context, JSValue& value) {
  if (context && !JS_IsUndefined(value)) JS_FreeValue(context, value);
  value = JS_UNDEFINED;
}

// ---- M2-D shared key mapping ---------------------------------------------

// Maps one key press to the character it produces for text collection. Both
// the hotstring stream and InputHook's buffer use this US-layout table; keys
// outside it (navigation, function keys, non-US layouts) produce nothing.
bool key_char(const std::uint32_t vk, const bool shift, char& out) {
  if (vk >= 'A' && vk <= 'Z') {
    out = shift ? static_cast<char>(vk) : static_cast<char>(vk - 'A' + 'a');
    return true;
  }
  if (vk >= '0' && vk <= '9') {
    static constexpr char kShiftDigits[] = ")!@#$%^&*(";
    out = shift ? kShiftDigits[vk - '0'] : static_cast<char>(vk);
    return true;
  }
  if (vk == 0x20) {
    out = ' ';
    return true;
  }
  if (vk == 0x09) {
    out = '\t';
    return true;
  }
  if (vk == 0x0D) {
    out = '\n';
    return true;
  }
  static constexpr struct {
    int vk;
    char plain;
    char shifted;
  } kOemKeys[] = {
      {0xBA, ';', ':'}, {0xBB, '=', '+'}, {0xBC, ',', '<'}, {0xBD, '-', '_'},
      {0xBE, '.', '>'}, {0xBF, '/', '?'}, {0xC0, '`', '~'}, {0xDB, '[', '{'},
      {0xDC, '\\', '|'}, {0xDD, ']', '}'}, {0xDE, '\'', '"'},
  };
  for (const auto& entry : kOemKeys) {
    if (entry.vk == static_cast<int>(vk)) {
      out = shift ? entry.shifted : entry.plain;
      return true;
    }
  }
  return false;
}

// Printable text only: Enter/Tab stay non-text keys (they can still end the
// input through EndKeys), matching the stage's collection rule.
bool is_text_char(const char c) { return c >= 0x20 && c < 0x7F; }

// EndMods: the held modifiers rendered with AHK's MODLR_STRING tokens. This
// stage's event feed carries one flag per side, so held modifiers use the
// left-side glyph of each pair, in AHK's control/alt/shift/super order.
std::string end_mods_text(const bool control, const bool alt, const bool shift, const bool super) {
  std::string mods;
  if (control) mods += "<^";
  if (alt) mods += "<!";
  if (shift) mods += "<+";
  if (super) mods += "<#";
  return mods;
}

// Reverse of chord_key(): the canonical token spelling for one vk, used for
// the EndKey snapshot ("escape", "f24", "a"; "#NN" for keys the grammar
// does not name).
std::string chord_key_name(const std::uint32_t vk) {
  if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk - 'A' + 'a'));
  if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
  if (vk >= 0x70 && vk <= 0x87) return "f" + std::to_string(vk - 0x6Fu);
  struct NamedKey {
    const char* name;
    std::uint32_t vk;
  };
  static constexpr NamedKey kNamedKeys[] = {
      {"space", 0x20},   {"tab", 0x09},      {"enter", 0x0D},   {"escape", 0x1B},
      {"backspace", 0x08},{"delete", 0x2E},  {"insert", 0x2D},  {"home", 0x24},
      {"end", 0x23},     {"pageup", 0x21},   {"pagedown", 0x22}, {"left", 0x25},
      {"up", 0x26},      {"right", 0x27},    {"down", 0x28},    {"pause", 0x13},
      {"capslock", 0x14},{"numlock", 0x90},  {"scrolllock", 0x91},
  };
  for (const auto& named : kNamedKeys) {
    if (named.vk == vk) return named.name;
  }
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string text = "#";
  text += kHex[(vk >> 4) & 0xF];
  text += kHex[vk & 0xF];
  return text;
}

// Single-character key token -> vk, used for end keys the chord grammar has
// no name for ("," "." "!" ...). Shifted forms map to their base key with
// `shift` set so the caller can require the shift state (AHK's
// END_KEY_WITH_SHIFT/OUT distinction for non-alpha characters).
bool char_key_vk(const std::string& token, std::uint32_t& vk, bool& shift, std::string& error) {
  if (token.size() != 1) {
    error = "unknown chord token: " + token;
    return false;
  }
  const char c = token[0];
  if (c >= 'a' && c <= 'z') {
    vk = static_cast<std::uint32_t>(c - 'a' + 'A');
    shift = false;
    return true;
  }
  if (c >= 'A' && c <= 'Z') {
    vk = static_cast<std::uint32_t>(c - 'A' + 'A');
    shift = false;
    return true;
  }
  if (c >= '0' && c <= '9') {
    vk = static_cast<std::uint32_t>(c);
    shift = false;
    return true;
  }
  struct CharKey {
    char plain;
    char shifted;
    std::uint32_t vk;
  };
  static constexpr CharKey kCharKeys[] = {
      {';', ':', 0xBA}, {'=', '+', 0xBB}, {',', '<', 0xBC}, {'-', '_', 0xBD},
      {'.', '>', 0xBE}, {'/', '?', 0xBF}, {'`', '~', 0xC0}, {'[', '{', 0xDB},
      {'\\', '|', 0xDC}, {']', '}', 0xDD}, {'\'', '"', 0xDE}, {'1', '!', 0x31},
      {'2', '@', 0x32}, {'3', '#', 0x33}, {'4', '$', 0x34}, {'5', '%', 0x35},
      {'6', '^', 0x36}, {'7', '&', 0x37}, {'8', '*', 0x38}, {'9', '(', 0x39},
      {'0', ')', 0x30}, {' ', ' ', 0x20},
  };
  for (const auto& entry : kCharKeys) {
    if (entry.plain == c) {
      vk = entry.vk;
      shift = false;
      return true;
    }
    if (entry.shifted == c && c != entry.plain) {
      vk = entry.vk;
      shift = true;
      return true;
    }
  }
  error = "unknown chord token: " + token;
  return false;
}

// Splits an EndKeys/KeyOpt key list into key names. Accepts the AHK brace
// form ("{Escape}{Enter}") and the comma/space separated form ("escape,
// enter"); every token resolves through the chord key grammar without
// modifiers.
bool split_key_tokens(const std::string& text, std::vector<std::string>& out, std::string& error) {
  out.clear();
  error.clear();
  std::size_t index = 0;
  while (index < text.size()) {
    const char c = text[index];
    if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++index;
      continue;
    }
    std::string token;
    if (c == '{') {
      const auto close = text.find('}', index + 1);
      if (close == std::string::npos) {
        error = "unterminated '{' in key list";
        return false;
      }
      token = text.substr(index + 1, close - index - 1);
      index = close + 1;
    } else {
      const auto next = text.find_first_of("{, \t\n\r", index);
      token = text.substr(index, next == std::string::npos ? next : next - index);
      index = next == std::string::npos ? text.size() : next;
    }
    if (token.empty()) continue;
    out.push_back(token);
  }
  return true;
}

// How a key list treats single-character tokens: EndKeys distinguishes the
// shift state of non-alpha characters (AHK's END_KEY_WITH_SHIFT/OUT pair),
// KeyOpt flags every key without a shift requirement.
enum class KeyListMode { EndKeys, KeyOpt };

// One key resolved from an EndKeys/KeyOpt key list.
struct KeyListEntry {
  std::uint8_t vk{0};
  bool require_shift{false};
  bool require_no_shift{false};
};

// Resolves an EndKeys/KeyOpt key list to virtual-key entries. Key lists are
// comma/space separated tokens ("escape,enter,f1") or brace names
// ("{Escape}{Enter}"); ValueError for unknown names, modifier chords or a
// text that yields no keys. The caller decides which flags to apply - the
// EndKeys path marks end keys, KeyOpt applies its E/I/N/Z options.
bool resolve_key_list(JSContext* context, const std::string& text, const KeyListMode mode,
                      std::vector<KeyListEntry>& out, const char* label) {
  out.clear();
  if (text.empty()) return true;
  std::vector<std::string> tokens;
  std::string error;
  if (!split_key_tokens(text, tokens, error)) {
    JS_ThrowTypeError(context, "%s: %s", label, error.c_str());
    return false;
  }
  if (tokens.empty()) {
    JS_ThrowTypeError(context, "%s: no keys were parsed", label);
    return false;
  }
  for (const std::string& token : tokens) {
    std::uint32_t vk = 0;
    std::uint8_t mask = 0;
    std::string chord_error;
    bool require_shift = false;
    bool require_no_shift = false;
    if (parse_chord(token, vk, mask, chord_error)) {
      if (mask != 0) {
        JS_ThrowTypeError(context, "%s: modifier chords are not supported (%s)", label,
                           token.c_str());
        return false;
      }
      const bool alpha = (token[0] >= 'a' && token[0] <= 'z') ||
                         (token[0] >= 'A' && token[0] <= 'Z');
      // Bare single non-alpha tokens (digits above all) end only in their
      // natural shift state, mirroring AHK's non-alpha end-key rule.
      if (mode == KeyListMode::EndKeys && token.size() == 1 && !alpha) require_no_shift = true;
    } else if (std::string char_error; char_key_vk(token, vk, require_shift, char_error)) {
      if (mode != KeyListMode::EndKeys) {
        require_shift = false;  // KeyOpt flags the key regardless of shift
      } else if (!require_shift) {
        require_no_shift = true;  // unshifted punctuation ends only unshifted
      }
    } else {
      JS_ThrowTypeError(context, "%s: %s", label, char_error.c_str());
      return false;
    }
    if (vk >= 256) {
      JS_ThrowTypeError(context, "%s: key out of range (%s)", label, token.c_str());
      return false;
    }
    out.push_back({static_cast<std::uint8_t>(vk), require_shift, require_no_shift});
  }
  return true;
}

// Marks every resolved key as an end key (the __New/endKeys path).
void apply_end_keys(const std::vector<KeyListEntry>& entries,
                    std::array<std::uint8_t, 256>& flags) {
  for (const KeyListEntry& entry : entries) {
    auto& slot = flags[entry.vk];
    slot = static_cast<std::uint8_t>(slot | EventsState::InputHook::kEndKey);
    if (entry.require_shift)
      slot = static_cast<std::uint8_t>(slot | EventsState::InputHook::kEndKeyShift);
    if (entry.require_no_shift)
      slot = static_cast<std::uint8_t>(slot | EventsState::InputHook::kEndKeyNoShift);
  }
}

// Comma separated MatchList with AHK's ",," escape for a literal comma;
// blank entries are dropped, an entry list that stays empty is a ValueError.
bool parse_match_list(JSContext* context, const std::string& text,
                      std::vector<std::string>& out) {
  out.clear();
  if (text.empty()) return true;
  std::string current;
  bool have_entry = false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != ',') {
      current += text[index];
      continue;
    }
    if (index + 1 < text.size() && text[index + 1] == ',') {
      current += ',';
      ++index;
      continue;
    }
    if (!current.empty()) {
      out.push_back(current);
      have_entry = true;
    }
    current.clear();
  }
  if (!current.empty()) {
    out.push_back(current);
    have_entry = true;
  }
  if (!have_entry) {
    JS_ThrowTypeError(context, "matchList: no match phrases were parsed");
    return false;
  }
  return true;
}

// InputHook option letters (input_type::ParseOptions): B C I L T V * are
// honored, H/M/E are accepted as no-ops (they need hook-side behavior this
// stage does not implement), anything else is a ValueError. Never throws;
// the caller raises the error with `error`.
bool parse_hook_options(const std::string& options, EventsState::InputHook& out,
                        std::string& error) {
  error.clear();
  for (std::size_t index = 0; index < options.size(); ++index) {
    const char letter = options[index];
    switch (letter) {
      case ' ':
      case '\t':
      case '\n':
      case '\r':
        break;
      case 'B':
        out.backspace_undo = false;
        break;
      case 'C':
        out.case_sensitive = true;
        break;
      case 'H':
      case 'M':
      case 'E':
        break;  // accepted, not implemented (documented deviation)
      case 'I': {
        const bool has_digits = index + 1 < options.size() && options[index + 1] >= '0' &&
                                options[index + 1] <= '9';
        std::int64_t level = has_digits ? 0 : 1;
        while (has_digits && index + 1 < options.size() && options[index + 1] >= '0' &&
               options[index + 1] <= '9') {
          level = level * 10 + (options[++index] - '0');
        }
        if (level < 0 || level > 101) {
          error = "option I: MinSendLevel must be 0..101";
          return false;
        }
        out.min_send_level = static_cast<std::uint32_t>(level);
        break;
      }
      case 'L': {
        const bool negative = index + 1 < options.size() && options[index + 1] == '-';
        const std::size_t digit_start = negative ? index + 2 : index + 1;
        if (digit_start >= options.size() || options[digit_start] < '0' ||
            options[digit_start] > '9') {
          error = "option L: expected a buffer length";
          return false;
        }
        std::int64_t length = 0;
        index = digit_start - 1;
        while (index + 1 < options.size() && options[index + 1] >= '0' &&
               options[index + 1] <= '9') {
          length = length * 10 + (options[++index] - '0');
        }
        // AHK clamps a negative length to zero (collect nothing).
        out.buffer_max = negative ? 0 : length;
        break;
      }
      case 'T': {
        if (index + 1 >= options.size() ||
            !(std::isdigit(static_cast<unsigned char>(options[index + 1])) ||
              options[index + 1] == '-' || options[index + 1] == '.')) {
          error = "option T: expected a timeout in seconds";
          return false;
        }
        const char* start = options.c_str() + index + 1;
        char* end = nullptr;
        const double seconds = std::strtod(start, &end);
        if (end == start) {
          error = "option T: expected a timeout in seconds";
          return false;
        }
        // A negative timeout simply never fires (AHK only runs the timer
        // when Timeout > 0).
        out.timeout_ms = static_cast<std::int64_t>(seconds * 1000.0);
        index = static_cast<std::size_t>(end - options.c_str()) - 1;
        break;
      }
      case 'V':
        out.visible_text = true;
        out.visible_non_text = true;
        break;
      case '*':
        out.find_anywhere = true;
        break;
      default:
        error = std::string("unsupported option letter: ") + letter;
        return false;
    }
  }
  return true;
}

// ---- InputHook bookkeeping ------------------------------------------------

// Defined further down with the shared dispatch; declared here because the
// hook activation path runs before those definitions.
bool ensure_dispatch(JSContext* context, InputModuleBinding* binding, EventsState* state);
void release_dispatch_if_idle(EventsState* state);
JSValue events_hook_channel(JSContext* context, JSValueConst this_val, int argc,
                            JSValueConst* argv, int magic, void* opaque);

EventsState::InputHook* find_input_hook(EventsState* state, const std::uint64_t id) {
  for (auto& hook : state->input_hooks) {
    if (hook.id == id) return &hook;
  }
  return nullptr;
}

// Resolves `this_val.id` to a live hook ref. objects.json: InvalidState when
// the runtime (and with it every hook) is closed.
EventsState::InputHook* hook_of(JSContext* context, JSValueConst this_val, EventsState* state) {
  JSValue id_value = JS_GetPropertyStr(context, this_val, "id");
  if (JS_IsException(id_value)) return nullptr;
  std::int64_t id = 0;
  const bool ok = JS_IsNumber(id_value) && JS_ToInt64(context, &id, id_value) == 0;
  JS_FreeValue(context, id_value);
  if (!ok) {
    JS_ThrowInternalError(context, "InputHook: missing object id");
    return nullptr;
  }
  if (state->closed) {
    JS_ThrowInternalError(context, "InputHook is closed");
    return nullptr;
  }
  EventsState::InputHook* hook = find_input_hook(state, static_cast<std::uint64_t>(id));
  if (!hook) JS_ThrowInternalError(context, "InputHook is closed");
  return hook;
}

void invoke_hook_callback(rime::js::Host* host, const std::uint64_t callback,
                          const std::string& where, const std::string& payload) {
  if (callback == 0) return;
  if (const auto error = host->invoke_callback(callback, payload); !error.ok()) {
    record_failure(host, where.c_str(), error);
  }
}

// Settle every pending Wait() with the final EndReason (a JSON string) and
// disarm the deadline timers so nothing keeps the host busy afterwards.
void settle_hook_waiters(EventsState* state, EventsState::InputHook& hook,
                         const std::string& end_reason) {
  rime::js::Host* host = state->host;
  const std::string value = json::stringify(json::Value::string(end_reason));
  for (const auto& waiter : hook.wait_tokens) host->complete_async(waiter, true, value);
  for (const auto& timer : hook.wait_timers)
    if (timer != 0) (void)host->timers().cancel(timer);
  hook.wait_tokens.clear();
  hook.wait_timers.clear();
}

void cancel_hook_timeout(EventsState* state, EventsState::InputHook& hook) {
  if (hook.timeout_timer != 0) {
    (void)state->host->timers().cancel(hook.timeout_timer);
    hook.timeout_timer = 0;
  }
}

// Releases the resources a hook holds only while input is in progress: the
// host subscription, the On* callback ids (captured first so a handler that
// restarts the input keeps its fresh registrations) and the shared dispatch
// user slot. `keep_callback` stays registered - the end path invokes OnEnd
// after this release and removes it then.
void release_hook_active(EventsState* state, EventsState::InputHook& hook,
                         const std::uint64_t keep_callback = 0) {
  rime::js::Host* host = state->host;
  const std::uint64_t on_char = hook.on_char_cb;
  const std::uint64_t on_end = hook.on_end_cb;
  const std::uint64_t on_down = hook.on_key_down_cb;
  const std::uint64_t on_up = hook.on_key_up_cb;
  hook.on_char_cb = 0;
  hook.on_end_cb = 0;
  hook.on_key_down_cb = 0;
  hook.on_key_up_cb = 0;
  if (hook.sub != 0) {
    (void)host->subscriptions().remove(hook.sub);
    hook.sub = 0;
    if (state->dispatch_users > 0) state->dispatch_users -= 1;
    release_dispatch_if_idle(state);
  }
  cancel_hook_timeout(state, hook);
  if (on_char != 0) (void)host->remove_callback(on_char);
  if (on_end != 0 && on_end != keep_callback) (void)host->remove_callback(on_end);
  if (on_down != 0) (void)host->remove_callback(on_down);
  if (on_up != 0) (void)host->remove_callback(on_up);
  // The lazy timeout/wait relay is a host callback too: drop it once no hook
  // needs it, so an idle InputHook keeps the host unloadable (the next
  // Start() recreates it).
  bool any_active = false;
  for (const auto& candidate : state->input_hooks) {
    if (candidate.in_progress) {
      any_active = true;
      break;
    }
  }
  if (!any_active && state->hook_channel_callback != 0) {
    (void)host->remove_callback(state->hook_channel_callback);
    state->hook_channel_callback = 0;
  }
}

// Ends an in-progress input: state first (so handlers and Wait() observe the
// final snapshot), then waiters, then OnEnd (the callback is kept registered
// through the release below and removed right after the invoke), then the
// active resources are gone.
void end_input_hook(JSContext* context, EventsState* state, EventsState::InputHook& hook,
                    const std::string& reason, const std::string& end_key = {},
                    const std::string& match = {}) {
  if (!hook.in_progress) return;
  hook.in_progress = false;
  hook.end_reason = reason;
  hook.end_key = end_key;
  hook.match = match;
  settle_hook_waiters(state, hook, reason);
  const std::uint64_t on_end = hook.on_end_cb;
  release_hook_active(state, hook, on_end);
  if (on_end != 0) {
    json::Value payload = json::Value::object();
    payload.set("reason", json::Value::string(reason));
    payload.set("input", json::Value::string(hook.buffer));
    payload.set("endKey", json::Value::string(hook.end_key));
    payload.set("endMods", json::Value::string(hook.end_mods));
    payload.set("match", json::Value::string(hook.match));
    invoke_hook_callback(state->host, on_end, "rime:input.inputHook",
                         json::stringify(payload));
    (void)state->host->remove_callback(on_end);
  }
  (void)context;
}

// Arms the Timeout property timer through the shared relay so the firing
// lands on the JS thread; a re-arm cancels the previous timer first (AHK's
// set_Timeout re-arms while input is in progress).
void arm_hook_timeout(EventsState* state, EventsState::InputHook& hook) {
  cancel_hook_timeout(state, hook);
  if (!hook.in_progress || hook.timeout_ms <= 0 || state->hook_channel_callback == 0) return;
  const auto queue = state->host->event_queue();
  const std::uint64_t channel = state->hook_channel_callback;
  const std::uint64_t id = hook.id;
  hook.timeout_timer = state->host->timers().schedule(
      std::chrono::milliseconds(hook.timeout_ms), [queue, channel, id] {
        (void)queue->push(channel, "{\"hook\":" + std::to_string(id) + ",\"timeout\":1}");
      });
}

// Registers the On* callbacks and the shared dispatch slot an in-progress
// hook needs; rollback on failure leaves the hook idle.
bool activate_input_hook(JSContext* context, InputModuleBinding* binding, EventsState* state,
                         EventsState::InputHook& hook) {
  rime::js::Host* host = state->host;
  const auto add_owned = [host, context](JSValue& owned, std::uint64_t& out) {
    if (JS_IsUndefined(owned) || JS_IsNull(owned)) return true;
    const auto error = host->add_callback(JS_DupValue(context, owned), out);
    if (!error.ok()) {
      JS_ThrowInternalError(context, "%s", error.message.c_str());
      return false;
    }
    return true;
  };
  if (!ensure_dispatch(context, binding, state)) return false;
  if (state->hook_channel_callback == 0) {
    JSValue closure = JS_NewCClosure(context, events_hook_channel, "inputHookChannel", nullptr, 1,
                                     0, binding);
    if (JS_IsException(closure)) return false;
    std::uint64_t channel = 0;
    if (const auto error = host->add_callback(closure, channel); !error.ok()) {
      JS_ThrowInternalError(context, "%s", error.message.c_str());
      return false;
    }
    state->hook_channel_callback = channel;
  }
  if (!add_owned(hook.on_char, hook.on_char_cb) || !add_owned(hook.on_end, hook.on_end_cb) ||
      !add_owned(hook.on_key_down, hook.on_key_down_cb) ||
      !add_owned(hook.on_key_up, hook.on_key_up_cb)) {
    release_hook_active(state, hook);
    return false;
  }
  const std::uint64_t sub = host->allocate_subscription_id();
  hook.sub = sub;
  if (const auto error = host->subscriptions().add("inputHook", sub); !error.ok()) {
    hook.sub = 0;
    release_hook_active(state, hook);
    JS_ThrowInternalError(context, "%s", error.message.c_str());
    return false;
  }
  state->dispatch_users += 1;
  hook.in_progress = true;
  hook.buffer.clear();
  hook.end_key.clear();
  hook.end_mods.clear();
  hook.match.clear();
  hook.end_reason = "";  // in progress (AHK GetEndReason default)
  arm_hook_timeout(state, hook);
  return true;
}

// ---- InputHook capture -----------------------------------------------------

// The event's injection level for this stage: physical input and this
// runtime's own injections all carry level 0 (AHK's default), so a hook
// with MinSendLevel > 0 never collects anything yet.
constexpr std::uint32_t kEventLevel = 0;

// MatchList check: exact (default) or substring (FindAnywhere) against each
// phrase, case per CaseSensitive. Returns the phrase that matched.
bool hook_match(const EventsState::InputHook& hook, std::string& matched) {
  if (hook.match_list.empty()) return false;
  const std::string buffer = hook.case_sensitive ? hook.buffer : ascii_lower(hook.buffer);
  for (const std::string& phrase : hook.match_list) {
    const std::string needle = hook.case_sensitive ? phrase : ascii_lower(phrase);
    if (needle.empty()) continue;
    const bool ok = hook.find_anywhere ? buffer.find(needle) != std::string::npos
                                       : buffer == needle;
    if (ok) {
      matched = phrase;
      return true;
    }
  }
  return false;
}

// Mirrors AHK input_type::CollectChar: append while there is room, check the
// match list, then the buffer limit (reaching the limit ends the input as
// "Max"). For L0 nothing is collected and nothing ends. Returns true when
// the input ended - in that case OnEnd already fired and the caller must not
// deliver OnChar/OnKeyDown for this event (AHK drops those messages too,
// because the end message precedes them in the queue).
bool collect_input_char(JSContext* context, EventsState* state, EventsState::InputHook& hook,
                        const char character) {
  const std::int64_t limit = hook.buffer_max;
  if (static_cast<std::int64_t>(hook.buffer.size()) == limit) {
    if (limit <= 0) return false;  // L0: collect nothing, allow OnChar
  } else {
    hook.buffer.push_back(character);
  }
  std::string matched;
  if (hook_match(hook, matched)) {
    end_input_hook(context, state, hook, "Match", {}, matched);
    return true;
  }
  if (static_cast<std::int64_t>(hook.buffer.size()) >= limit) {
    end_input_hook(context, state, hook, "Max");
    return true;
  }
  return false;
}

std::string hook_key_payload(const std::uint32_t vk, const std::uint32_t scan, const bool down,
                             const int has_char, const char character) {
  json::Value payload = json::Value::object();
  payload.set("vk", json::Value::number(static_cast<double>(vk)));
  payload.set("scan", json::Value::number(static_cast<double>(scan)));
  payload.set("down", json::Value::boolean(down));
  if (has_char) payload.set("char", json::Value::string(std::string(1, character)));
  return json::stringify(payload);
}

// Feeds one key event (down or up, own injections included, level 0) into
// every in-progress InputHook, in AHK's CollectInputHook order: end keys
// first, then collection, backspace undo, notify flags, OnKeyDown and
// OnChar. Hook ids are snapshotted and re-resolved per hook because an
// On* handler may Stop()/Start() hooks (the vector may even reallocate).
void feed_input_hooks(JSContext* context, EventsState* state, JSValueConst event) {
  if (state->input_hooks.empty()) return;
  std::vector<std::uint64_t> active;
  active.reserve(state->input_hooks.size());
  for (const auto& hook : state->input_hooks) {
    if (hook.in_progress && hook.sub != 0) active.push_back(hook.id);
  }
  if (active.empty()) return;

  const auto read_number = [context, event](const char* name, double& out) {
    JSValue value = event_field(context, event, name);
    const bool ok = JS_IsNumber(value) && JS_ToFloat64(context, &out, value) == 0;
    JS_FreeValue(context, value);
    return ok;
  };
  const auto read_flag = [context, event](const char* name) {
    JSValue value = event_field(context, event, name);
    const bool flag = JS_IsBool(value) && JS_ToBool(context, value) > 0;
    JS_FreeValue(context, value);
    return flag;
  };
  double vk_number = 0;
  double scan_number = 0;
  if (!read_number("vk", vk_number)) return;
  (void)read_number("scan", scan_number);
  const std::uint32_t vk = static_cast<std::uint32_t>(vk_number);
  const std::uint32_t scan = static_cast<std::uint32_t>(scan_number);
  if (vk >= 256) return;
  const bool pressed = read_flag("down");
  const bool shift = read_flag("shift");
  const bool alt = read_flag("alt");
  const bool control = read_flag("control");
  const bool super = read_flag("super");

  for (const std::uint64_t id : active) {
    EventsState::InputHook* hook = find_input_hook(state, id);
    if (!hook || !hook->in_progress || hook->sub == 0) continue;
    if (!rime::core::SchedulerPolicy::level_allowed(hook->min_send_level, kEventLevel)) continue;
    std::uint8_t flags = hook->key_flags[vk];
    if (pressed) {
      // 1. End keys terminate on the key-down (before any collection).
      if (flags & EventsState::InputHook::kEndKey) {
        const bool require_shift = (flags & EventsState::InputHook::kEndKeyShift) != 0;
        const bool require_no_shift = (flags & EventsState::InputHook::kEndKeyNoShift) != 0;
        bool shift_ok = true;
        if (require_shift) shift_ok = shift;
        else if (require_no_shift) shift_ok = !shift;
        if (shift_ok) {
          hook->end_mods = end_mods_text(control, alt, shift, super);
          end_input_hook(context, state, *hook, "EndKey", chord_key_name(vk));
          continue;
        }
      }
      // 2. Classify and collect (may end the input).
      char character = 0;
      const bool mapped = key_char(vk, shift, character);
      const bool text = mapped && is_text_char(character);
      const bool treat_as_text = text && (flags & EventsState::InputHook::kIgnoreText) == 0;
      bool ended = false;
      if (treat_as_text) ended = collect_input_char(context, state, *hook, character);
      if (ended) continue;
      // 3. Backspace undo: only an unmodified Backspace erases (shift is
      //    allowed; ctrl/alt/super keep their native word-delete meaning).
      if (vk == 0x08 && hook->backspace_undo && !alt && !control && !super) {
        if (!hook->buffer.empty()) hook->buffer.pop_back();
      }
      // 4. Text/non-text classification for the matching key-up.
      if (hook->notify_non_text) {
        if (treat_as_text) {
          flags = static_cast<std::uint8_t>(flags | EventsState::InputHook::kHadText);
        } else {
          flags = static_cast<std::uint8_t>(flags & ~EventsState::InputHook::kHadText);
        }
        hook->key_flags[vk] = flags;
      }
      // 5. OnKeyDown, then OnChar - never for the terminating event
      //    (the early `continue` above took care of that).
      if ((flags & EventsState::InputHook::kNotify) ||
          (hook->notify_non_text && !treat_as_text)) {
        const std::uint64_t callback = hook->on_key_down_cb;
        invoke_hook_callback(state->host, callback, "rime:input.inputHook",
                             hook_key_payload(vk, scan, true, 0, 0));
        hook = find_input_hook(state, id);
        if (!hook || !hook->in_progress) continue;
      }
      if (treat_as_text) {
        const std::uint64_t callback = hook->on_char_cb;
        invoke_hook_callback(state->host, callback, "rime:input.inputHook",
                             hook_key_payload(vk, scan, true, 1, character));
      }
    } else {
      // Key-up: OnKeyUp only, and only while the input is still running
      // (a key released after the end was dropped by AHK the same way).
      const bool was_text = (flags & EventsState::InputHook::kHadText) != 0;
      if ((flags & EventsState::InputHook::kNotify) ||
          (hook->notify_non_text && !was_text)) {
        const std::uint64_t callback = hook->on_key_up_cb;
        invoke_hook_callback(state->host, callback, "rime:input.inputHook",
                             hook_key_payload(vk, scan, false, 0, 0));
      }
    }
  }
}

// ---- InputHook timeout/wait relay -----------------------------------------

// The timer thread posts {"hook":id,"timeout":1} when the Timeout property
// expires and {"hook":id,"wait":token} when a Wait() deadline runs out;
// both land here on the JS thread.
JSValue events_hook_channel(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                            int, void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
  EventsState* state = binding->events.get();
  if (!state || state->closed) return JS_UNDEFINED;
  const auto read_integer = [context, argv](const char* name, std::int64_t& out) {
    JSValue value = event_field(context, argv[0], name);
    const bool ok = JS_IsNumber(value) && JS_ToInt64(context, &out, value) == 0;
    JS_FreeValue(context, value);
    return ok;
  };
  std::int64_t id_number = 0;
  if (!read_integer("hook", id_number)) return JS_UNDEFINED;
  EventsState::InputHook* hook = find_input_hook(state, static_cast<std::uint64_t>(id_number));
  if (!hook) return JS_UNDEFINED;
  std::int64_t flag = 0;
  if (read_integer("timeout", flag) && flag != 0) {
    // The Timeout property fired: the input ends as "Timeout".
    if (hook->in_progress) end_input_hook(context, state, *hook, "Timeout");
    return JS_UNDEFINED;
  }
  std::int64_t token = 0;
  if (read_integer("wait", token) && token > 0) {
    // A Wait() deadline: resolve that waiter with "Timeout"; the input
    // itself keeps running (the Timeout property decides that separately).
    for (std::size_t index = 0; index < hook->wait_tokens.size(); ++index) {
      if (hook->wait_tokens[index] != static_cast<std::uint64_t>(token)) continue;
      const bool have_timer = index < hook->wait_timers.size();
      if (have_timer && hook->wait_timers[index] != 0)
        (void)state->host->timers().cancel(hook->wait_timers[index]);
      hook->wait_tokens.erase(hook->wait_tokens.begin() + static_cast<std::ptrdiff_t>(index));
      if (have_timer)
        hook->wait_timers.erase(hook->wait_timers.begin() + static_cast<std::ptrdiff_t>(index));
      (void)state->host->complete_async(static_cast<std::uint64_t>(token), true,
                                        json::stringify(json::Value::string("Timeout")));
      break;
    }
  }
  return JS_UNDEFINED;
}

JSValue build_window_object(JSContext* context, const WindowInfo& info) {
  JSValue object = JS_NewObject(context);
  if (JS_IsException(object)) return JS_EXCEPTION;
  JS_SetPropertyStr(context, object, "id", JS_NewInt64(context, static_cast<std::int64_t>(info.id)));
  JS_SetPropertyStr(context, object, "title", JS_NewString(context, info.title.c_str()));
  JS_SetPropertyStr(context, object, "className", JS_NewString(context, info.class_name.c_str()));
  JS_SetPropertyStr(context, object, "processName",
                    JS_NewString(context, info.process_name.c_str()));
  JS_SetPropertyStr(context, object, "pid",
                    JS_NewInt64(context, static_cast<std::int64_t>(info.process_id)));
  return object;
}

// ---- criterion bookkeeping -------------------------------------------------

EventsState::Criterion* find_criterion(EventsState* state, std::uint64_t id) {
  for (auto& criterion : state->criteria) {
    if (criterion.id == id) return &criterion;
  }
  return nullptr;
}

JSValue criterion_descriptor(JSContext* context, EventsState* state, std::uint64_t id) {
  const char* kind = "none";
  if (id != 0) {
    if (const EventsState::Criterion* criterion = find_criterion(state, id)) {
      switch (criterion->spec.kind) {
        case CriterionKind::Active:
          kind = "winActive";
          break;
        case CriterionKind::Exists:
          kind = "winExist";
          break;
        case CriterionKind::NotActive:
          kind = "winNotActive";
          break;
        case CriterionKind::NotExists:
          kind = "winNotExist";
          break;
        case CriterionKind::Function:
          kind = "function";
          break;
      }
    } else {
      kind = "gone";
    }
  }
  JSValue object = JS_NewObject(context);
  if (JS_IsException(object)) return object;
  JS_SetPropertyStr(context, object, "kind", JS_NewString(context, kind));
  JS_SetPropertyStr(context, object, "id", JS_NewInt64(context, static_cast<std::int64_t>(id)));
  return object;
}

// Drops criteria no registration references anymore, then publishes the
// remaining set to the watcher (which starts/stops its capture thread).
void sync_criteria(JSContext* context, EventsState* state) {
  if (state->closed) return;
  std::vector<std::uint64_t> referenced;
  referenced.reserve(state->criteria.size());
  referenced.push_back(state->current_criterion);
  for (const auto& hotkey : state->hotkeys) {
    if (hotkey.criterion != 0) referenced.push_back(hotkey.criterion);
  }
  for (const auto& hotstring : state->hotstrings) {
    if (hotstring.criterion != 0) referenced.push_back(hotstring.criterion);
  }
  const auto is_referenced = [&referenced](std::uint64_t id) {
    return std::find(referenced.begin(), referenced.end(), id) != referenced.end();
  };
  for (auto iterator = state->criteria.begin(); iterator != state->criteria.end();) {
    if (is_referenced(iterator->id)) {
      ++iterator;
      continue;
    }
    free_owned(context, iterator->fn);
    iterator = state->criteria.erase(iterator);
  }
  if (!state->window_service) return;
  auto specs = std::make_shared<std::vector<CriterionSpec>>();
  specs->reserve(state->criteria.size());
  for (const auto& criterion : state->criteria) specs->push_back(criterion.spec);
  if (!state->watcher && !specs->empty()) {
    state->watcher = std::make_shared<ContextWatcher>(*state->window_service);
  }
  if (state->watcher) state->watcher->set_criteria(specs);
}

// Evaluates one HotIf criterion against the snapshot captured for this
// event; window kinds were resolved on the UI lane by the watcher, Function
// kinds are invoked here with { active, seq }. Any failure fails closed, and
// so does an evaluation that used up the #HotIfTimeout budget (the policy's
// rule - the criterion itself cannot be preempted mid-call).
bool criterion_met(JSContext* context, EventsState* state, std::uint64_t id,
                   const std::shared_ptr<const ContextSnapshot>& snapshot) {
  if (id == 0) return true;
  const auto eval_start = std::chrono::steady_clock::now();
  const auto within_budget = [state, eval_start] {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - eval_start)
                             .count();
    return state->policy.hot_if_met(static_cast<std::uint64_t>(elapsed < 0 ? 0 : elapsed));
  };
  const EventsState::Criterion* criterion = find_criterion(state, id);
  if (!criterion) return false;
  if (criterion->spec.kind != CriterionKind::Function) {
    if (!snapshot) return false;
    for (const auto& entry : snapshot->results) {
      if (entry.first == id) return entry.second && within_budget();
    }
    return false;
  }
  JSValue payload = JS_NewObject(context);
  if (JS_IsException(payload)) return false;
  JSValue active = JS_NULL;
  if (snapshot && snapshot->has_foreground) {
    active = build_window_object(context, snapshot->foreground);
    if (JS_IsException(active)) {
      JS_FreeValue(context, payload);
      return false;
    }
  }
  JS_SetPropertyStr(context, payload, "active", active);
  JS_SetPropertyStr(context, payload, "seq",
                    JS_NewInt64(context, static_cast<std::int64_t>(
                                             snapshot ? snapshot->seq : 0)));
  JSValue result = JS_Call(context, criterion->fn, JS_UNDEFINED, 1, &payload);
  JS_FreeValue(context, payload);
  if (JS_IsException(result)) {
    JSValue exception = JS_GetException(context);
    const char* message = JS_ToCString(context, exception);
    JS_FreeValue(context, exception);
    record_failure(state->host, "rime:input.hotIf",
                   {rime::core::Error::Code::ExecutionFailed,
                    message ? message : "hotIf function failed"});
    if (message) JS_FreeCString(context, message);
    return false;
  }
  const bool boolean_result = JS_IsBool(result);
  const bool met = boolean_result && JS_ToBool(context, result) > 0;
  JS_FreeValue(context, result);
  if (!boolean_result) {
    record_failure(state->host, "rime:input.hotIf",
                   {rime::core::Error::Code::InvalidContract,
                    "hotIf function must return a boolean"});
    return false;
  }
  if (!within_budget()) {
    record_failure(state->host, "rime:input.hotIf",
                   {rime::core::Error::Code::Timeout,
                    "hotIf criterion exceeded its evaluation budget"});
    return false;
  }
  return met;
}

// ---- shared dispatch -------------------------------------------------------

bool ensure_dispatch(JSContext* context, InputModuleBinding* binding, EventsState* state);

JSValue events_dispatch(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                        int magic, void* opaque);
JSValue events_message_channel(JSContext* context, JSValueConst this_val, int argc,
                               JSValueConst* argv, int magic, void* opaque);
JSValue events_timer_tick(JSContext* context, JSValueConst this_val, int, JSValueConst*,
                          int magic, void* opaque);

// Queues one fire-and-forget Action exactly like input.bind does (the
// worker-lane pump drains it with the shared queue pump).
bool submit_event_action(JSContext*, EventsState* state, const EventAction& action,
                         const char* source_kind) {
  rime::action::Action built = make_action(*state->next_action_id, "rime:input", action.type,
                                           action.capability,
                                           {action.target_kind, action.target_id}, action.payload);
  built.source = {source_kind, "rime:input"};
  const auto status = state->dispatcher->submit(std::move(built));
  if (status != rime::action::DispatchStatus::Accepted &&
      status != rime::action::DispatchStatus::Coalesced) {
    return false;
  }
  rime::js::Host* host = state->host;
  const bool posted = host->post_worker([host, dispatcher = state->dispatcher] {
    try {
      run_queue_pump(host, *dispatcher);
    } catch (const std::exception& exception) {
      (void)host->record("rime:input.event", exception.what());
    } catch (...) {
      (void)host->record("rime:input.event", "queue pump failed");
    }
  });
  if (!posted) {
    record_failure(host, "rime:input.event",
                   {rime::core::Error::Code::InvalidState, "worker service is stopping"});
  }
  return true;
}

bool ensure_dispatch(JSContext* context, InputModuleBinding* binding, EventsState* state) {
  if (state->dispatch_subscription != 0) return true;
  auto* host = host_of(context);
  const auto queue = host->event_queue();
  const auto callback_id = std::make_shared<std::atomic<std::uint64_t>>(0);
  const std::uint64_t subscription_id = state->service->subscribe(
      [queue, callback_id](const InputEvent& event) {
        const std::uint64_t id = callback_id->load(std::memory_order_acquire);
        if (id != 0) (void)queue->push(id, event_json(event));
      });
  if (subscription_id == 0) {
    JS_ThrowInternalError(context, "input service is not running");
    return false;
  }
  JSValue closure =
      JS_NewCClosure(context, events_dispatch, "inputEvents", nullptr, 1, 0, binding);
  if (JS_IsException(closure)) {
    (void)state->service->unsubscribe(subscription_id);
    return false;
  }
  std::uint64_t host_callback = 0;
  if (const auto error = host->add_callback(closure, host_callback); !error.ok()) {
    (void)state->service->unsubscribe(subscription_id);
    JS_ThrowInternalError(context, "%s", error.message.c_str());
    return false;
  }
  callback_id->store(host_callback, std::memory_order_release);
  state->dispatch_callback = host_callback;
  state->dispatch_subscription = subscription_id;
  return true;
}

void release_dispatch_if_idle(EventsState* state) {
  if (state->dispatch_users != 0 || state->dispatch_subscription == 0) return;
  (void)state->service->unsubscribe(state->dispatch_subscription);
  if (state->dispatch_callback != 0) {
    (void)state->host->remove_callback(state->dispatch_callback);
  }
  state->dispatch_subscription = 0;
  state->dispatch_callback = 0;
}

// ---- hotkey matching -------------------------------------------------------

// Keeps the in-flight delivery counters exact across re-entrant pumps: a
// handler that pumps messages can nest another delivery of the same
// registration, and both the global (#MaxThreads) and per-registration
// (#MaxThreadsPerHotkey) caps read these counters.
// Tracks one in-flight delivery across the JS invoke. The registration may
// be closed from inside its own callback (erase/realloc of the storage), so
// the guard never holds a reference into the vector: it keys on the stable
// subscription id and re-finds the entry. A registration closed mid-delivery
// is gone - its counter died with it - while the global in-flight count stays
// exact.
struct DeliveryGuard {
  enum class Kind : std::uint8_t { Hotkey, Hotstring };
  EventsState* state;
  Kind kind;
  std::uint64_t sub;

  DeliveryGuard(EventsState& state_ref, const Kind kind_ref, const std::uint64_t sub_ref)
      : state(&state_ref), kind(kind_ref), sub(sub_ref) {
    state->running_deliveries += 1;
    if (std::uint32_t* running = find_running()) *running += 1;
  }
  DeliveryGuard(const DeliveryGuard&) = delete;
  DeliveryGuard& operator=(const DeliveryGuard&) = delete;
  ~DeliveryGuard() {
    if (std::uint32_t* running = find_running()) {
      if (*running > 0) *running -= 1;
    }
    state->running_deliveries -= 1;
  }

  [[nodiscard]] std::uint32_t* find_running() const {
    if (kind == Kind::Hotkey) {
      for (auto& hotkey : state->hotkeys) {
        if (hotkey.sub == sub) return &hotkey.running;
      }
    } else {
      for (auto& hotstring : state->hotstrings) {
        if (hotstring.sub == sub) return &hotstring.running;
      }
    }
    return nullptr;
  }
};

// Capacity half of the central policy: false when #MaxThreads or
// #MaxThreadsPerHotkey refuses the delivery; the rejection is counted so
// tests can observe drops without parsing logs. (Suspend and #InputLevel
// are matching rules handled in the matchers, not load rejections.)
bool admit_observer_delivery(EventsState* state, const std::uint32_t running) {
  if (!state->policy.admits_total(state->running_deliveries)) {
    state->dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (state->policy.admit_subscription(running,
                                       state->policy.max_concurrency_per_subscription) !=
      rime::core::Delivery::Deliver) {
    state->dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void fire_hotkey(JSContext* context, EventsState* state, EventsState::Hotkey& hotkey) {
  if (hotkey.via_action) {
    (void)submit_event_action(context, state, hotkey.action, "hotkey");
    return;
  }
  if (hotkey.observer == 0) return;
  if (!admit_observer_delivery(state, hotkey.running)) return;
  DeliveryGuard guard(*state, DeliveryGuard::Kind::Hotkey, hotkey.sub);
  json::Value payload = json::Value::object();
  payload.set("name", json::Value::string(hotkey.name));
  const std::string text = json::stringify(payload);
  if (const auto error = state->host->invoke_callback(hotkey.observer, text); !error.ok()) {
    record_failure(state->host, "rime:input.hotkey", error);
  }
}

// First-match-wins: registrations are scanned in order, a failing HotIf
// criterion falls through to the next candidate. Rows are snapshotted first
// so a HotIf function can add/remove hotkeys mid-evaluation. Suspend and
// #InputLevel filter the scan (they are matching rules), the capacity caps
// apply at delivery time.
bool hotkey_try_match(JSContext* context, EventsState* state, JSValueConst event) {
  struct Row {
    std::uint64_t sub;
    std::uint32_t vk;
    std::uint8_t mask;
    std::uint64_t criterion;
    bool suspend_exempt;
    std::uint32_t input_level;
  };
  std::vector<Row> rows;
  rows.reserve(state->hotkeys.size());
  for (const auto& hotkey : state->hotkeys) {
    if (!hotkey.enabled) continue;
    if (!rime::core::SchedulerPolicy::dispatch_allowed(state->suspended, hotkey.suspend_exempt)) {
      continue;
    }
    if (!rime::core::SchedulerPolicy::level_allowed(hotkey.input_level, kEventLevel)) continue;
    rows.push_back({hotkey.sub, hotkey.vk, hotkey.mask, hotkey.criterion, hotkey.suspend_exempt,
                    hotkey.input_level});
  }
  if (rows.empty()) return false;
  const auto snapshot = state->watcher ? state->watcher->current() : nullptr;
  for (const auto& row : rows) {
    ChordBinding probe{};
    probe.vk = row.vk;
    probe.mask = row.mask;
    if (!matches_chord(context, event, probe)) continue;
    if (!criterion_met(context, state, row.criterion, snapshot)) continue;
    EventsState::Hotkey* hotkey = nullptr;
    for (auto& candidate : state->hotkeys) {
      if (candidate.sub == row.sub) {
        hotkey = &candidate;
        break;
      }
    }
    if (!hotkey || !hotkey->enabled) continue;
    // The HotIf evaluation ran script; suspend/level may have changed.
    if (!rime::core::SchedulerPolicy::dispatch_allowed(state->suspended, hotkey->suspend_exempt)) {
      continue;
    }
    if (!rime::core::SchedulerPolicy::level_allowed(hotkey->input_level, kEventLevel)) continue;
    fire_hotkey(context, state, *hotkey);
    return true;
  }
  return false;
}

// ---- hotstring matching ----------------------------------------------------

bool hotstring_match(const EventsState::Hotstring& hotstring, const std::string& typed,
                     const bool allow_suffix) {
  const std::string& trigger = hotstring.trigger;
  if (typed.size() < trigger.size()) return false;
  if (!allow_suffix && typed.size() != trigger.size()) return false;
  const std::string_view tail(typed.data() + typed.size() - trigger.size(), trigger.size());
  if (hotstring.case_sensitive) return tail == std::string_view(trigger);
  return ascii_lower(tail) == ascii_lower(trigger);
}

void finish_hotstring(JSContext* context, InputModuleBinding*, EventsState* state,
                      const std::uint64_t sub, const bool end_char_typed, const char end_char) {
  EventsState::Hotstring* hotstring = nullptr;
  for (auto& candidate : state->hotstrings) {
    if (candidate.sub == sub) {
      hotstring = &candidate;
      break;
    }
  }
  if (!hotstring || !hotstring->enabled) return;
  const bool do_reset = hotstring->do_reset;
  const bool omit_end_char = hotstring->omit_end_char;
  const bool do_backspace = hotstring->do_backspace;
  const bool conform = hotstring->conform_case;
  const std::string trigger = hotstring->trigger;
  const std::string replacement = hotstring->replacement;
  const bool via_action = hotstring->via_action;
  const bool has_observer = hotstring->observer != 0;
  EventAction action = hotstring->action;

  if (has_observer) {
    if (!admit_observer_delivery(state, hotstring->running)) {
      // The trigger was still consumed (the end char matched); only the
      // delivery is refused, exactly like a cap refusal in AHK.
      if (do_reset) state->typed.clear();
      return;
    }
    DeliveryGuard guard(*state, DeliveryGuard::Kind::Hotstring, hotstring->sub);
    if (const auto error = state->host->invoke_callback(hotstring->observer, "{}"); !error.ok()) {
      record_failure(state->host, "rime:input.hotstring", error);
    }
  } else if (via_action) {
    (void)submit_event_action(context, state, action, "hotstring");
  } else {
    // Replacement form: remove the trigger (and the end char that was
    // already delivered - this stage does not suppress at the hook), then
    // inject the replacement plus the end char unless O omitted it.
    std::string output = replacement;
    if (conform) output = conform_case_text(output, trigger);
    std::size_t backspaces = 0;
    if (do_backspace) {
      backspaces = utf8_char_count(trigger);
      if (end_char_typed) backspaces += 1;
    }
    json::Value steps = json::Value::array();
    for (std::size_t index = 0; index < backspaces; ++index) {
      json::Value down = json::Value::object();
      down.set("vk", json::Value::number(8));
      down.set("down", json::Value::boolean(true));
      steps.push(std::move(down));
      json::Value up = json::Value::object();
      up.set("vk", json::Value::number(8));
      up.set("down", json::Value::boolean(false));
      steps.push(std::move(up));
    }
    append_utf16_steps(output, steps);
    if (end_char_typed && do_backspace && !omit_end_char) {
      append_utf16_steps(std::string(1, end_char), steps);
    }
    if (steps.size() > 0) {
      EventAction send{};
      send.type = "input.send";
      send.capability = kInjectCapability;
      send.target_kind = "input";
      send.target_id = "keyboard";
      send.payload = json::stringify(steps);
      (void)submit_event_action(context, state, send, "hotstring");
    }
  }
  // Read before the handler ran: the observer may have removed the entry.
  if (do_reset) state->typed.clear();
}

void hotstring_on_key(JSContext* context, InputModuleBinding* binding, EventsState* state,
                      JSValueConst event) {
  JSValue vk_value = event_field(context, event, "vk");
  double vk_number = -1;
  const bool vk_ok = JS_IsNumber(vk_value) && JS_ToFloat64(context, &vk_number, vk_value) == 0;
  JS_FreeValue(context, vk_value);
  if (!vk_ok) return;
  const int vk = static_cast<int>(vk_number);
  const auto read_flag = [context, event](const char* name) {
    JSValue value = event_field(context, event, name);
    const bool flag = JS_IsBool(value) && JS_ToBool(context, value) > 0;
    JS_FreeValue(context, value);
    return flag;
  };
  if (read_flag("control") || read_flag("alt") || read_flag("super")) {
    state->typed.clear();
    return;
  }
  const bool shift = read_flag("shift");
  if (vk == 0x08) {  // VK_BACK
    if (!state->typed.empty()) state->typed.pop_back();
    return;
  }
  char character = 0;
  bool word_char = false;
  if (vk >= 'A' && vk <= 'Z') {
    character = shift ? static_cast<char>(vk) : static_cast<char>(vk - 'A' + 'a');
    word_char = true;
  } else if (vk >= '0' && vk <= '9') {
    static constexpr char kShiftDigits[] = ")!@#$%^&*(";
    character = shift ? kShiftDigits[vk - '0'] : static_cast<char>(vk);
    word_char = true;
  } else if (vk == 0x20) {
    character = ' ';
  } else if (vk == 0x09) {
    character = '\t';
  } else if (vk == 0x0D) {
    character = '\n';
  } else {
    static constexpr struct {
      int vk;
      char plain;
      char shifted;
    } kOemKeys[] = {
        {0xBA, ';', ':'}, {0xBB, '=', '+'}, {0xBC, ',', '<'}, {0xBD, '-', '_'},
        {0xBE, '.', '>'}, {0xBF, '/', '?'}, {0xC0, '`', '~'}, {0xDB, '[', '{'},
        {0xDC, '\\', '|'}, {0xDD, ']', '}'}, {0xDE, '\'', '"'},
    };
    for (const auto& entry : kOemKeys) {
      if (entry.vk == vk) {
        character = shift ? entry.shifted : entry.plain;
        break;
      }
    }
    if (character == 0) {
      state->typed.clear();  // navigation/function key: reset the stream
      return;
    }
  }

  if (word_char) {
    state->typed.push_back(character);
    // Wildcard hotstrings fire as soon as the trigger is completed; the
    // end-char pass below handles the rest.
    std::uint64_t matched = 0;
    for (const auto& hotstring : state->hotstrings) {
      if (!hotstring.enabled || !hotstring.wildcard) continue;
      if (!rime::core::SchedulerPolicy::dispatch_allowed(state->suspended, hotstring.suspend_exempt)) {
        continue;
      }
      if (!rime::core::SchedulerPolicy::level_allowed(hotstring.input_level, kEventLevel)) continue;
      if (!hotstring_match(hotstring, state->typed, true)) continue;
      matched = hotstring.sub;
      break;
    }
    if (matched != 0) finish_hotstring(context, binding, state, matched, false, '\0');
    return;
  }
  if (state->end_chars.find(character) == std::string::npos) {
    state->typed.clear();
    return;
  }
  std::uint64_t matched = 0;
  for (const auto& hotstring : state->hotstrings) {
    if (!hotstring.enabled || hotstring.wildcard) continue;
    if (!rime::core::SchedulerPolicy::dispatch_allowed(state->suspended, hotstring.suspend_exempt)) {
      continue;
    }
    if (!rime::core::SchedulerPolicy::level_allowed(hotstring.input_level, kEventLevel)) continue;
    if (!hotstring_match(hotstring, state->typed, hotstring.inside_word)) continue;
    matched = hotstring.sub;
    break;
  }
  if (matched == 0) {
    state->typed.clear();
    return;
  }
  finish_hotstring(context, binding, state, matched, true, character);
}

// ---- dispatch entry --------------------------------------------------------

JSValue events_dispatch(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_UNDEFINED;
  EventsState* state = binding->events.get();
  if (!state || state->closed || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
  JSValue event = argv[0];
  state->dispatched.fetch_add(1, std::memory_order_relaxed);

  JSValue kind_value = event_field(context, event, "kind");
  const char* kind_text = JS_IsString(kind_value) ? JS_ToCString(context, kind_value) : nullptr;
  JS_FreeValue(context, kind_value);
  if (!kind_text) return JS_UNDEFINED;
  const bool is_key = std::string_view(kind_text) == "key";
  const bool is_mouse = std::string_view(kind_text) == "mouse";
  JS_FreeCString(context, kind_text);

  if (!is_key) {
    // Mouse movement resets the hotstring stream while MouseReset is on.
    if (is_mouse && state->mouse_reset && !state->typed.empty()) {
      JSValue action_value = event_field(context, event, "action");
      const char* action_text = JS_IsString(action_value) ? JS_ToCString(context, action_value) : nullptr;
      JS_FreeValue(context, action_value);
      const bool move = action_text != nullptr && std::string_view(action_text) == "move";
      if (action_text) JS_FreeCString(context, action_text);
      if (move) state->typed.clear();
    }
    return JS_UNDEFINED;
  }

  // M2-D: InputHook capture sees every key event first - including input
  // this process injected (AHK's hook collects own injections too), and key
  // ups (OnKeyUp). Only the matching paths below filter self-injected input.
  feed_input_hooks(context, state, event);

  // Input this process injected through input.send never feeds matching -
  // the same loop-prevention rule input.bind uses.
  JSValue self_value = event_field(context, event, "selfInjected");
  const bool self_input = JS_IsBool(self_value) && JS_ToBool(context, self_value) > 0;
  JS_FreeValue(context, self_value);
  if (self_input) return JS_UNDEFINED;
  JSValue down_value = event_field(context, event, "down");
  const bool pressed = JS_IsBool(down_value) && JS_ToBool(context, down_value) > 0;
  JS_FreeValue(context, down_value);
  if (!pressed) return JS_UNDEFINED;

  if (hotkey_try_match(context, state, event)) return JS_UNDEFINED;
  hotstring_on_key(context, binding, state, event);
  return JS_UNDEFINED;
}

// ---- timers ----------------------------------------------------------------

void arm_scheduler(EventsState* state) {
  rime::js::Host* host = state->host;
  if (!host) return;
  if (state->scheduler_timer != 0) {
    (void)host->timers().cancel(state->scheduler_timer);
    state->scheduler_timer = 0;
  }
  if (state->timers.empty() || state->scheduler_callback == 0) return;
  std::int64_t earliest = state->timers.front().next_due_ms;
  for (const auto& timer : state->timers) {
    earliest = std::min(earliest, timer.next_due_ms);
  }
  const std::int64_t now = now_ms();
  std::int64_t delay = earliest > now ? earliest - now : 0;
  if (delay > kMaxTimerDelayMs) delay = kMaxTimerDelayMs;
  const auto queue = host->event_queue();
  const std::uint64_t channel = state->scheduler_callback;
  const auto pending = state->tick_pending;
  // Captured by value: the timer thread must never read state->policy.
  const rime::core::SchedulerPolicy policy = state->policy;
  state->scheduler_timer = host->timers().schedule(
      std::chrono::milliseconds(delay), [queue, channel, pending, policy] {
        // Coalescing (#MaxThreadsBuffer): while a tick is queued or
        // running, later firings do not pile up - the JS thread re-arms
        // after it catches up. The central policy decides the repeat;
        // the pending flag is the exact in-flight state.
        const bool in_flight = pending->load(std::memory_order_acquire);
        if (policy.admit_repeat(in_flight) == rime::core::Delivery::Deliver &&
            !pending->exchange(true, std::memory_order_acq_rel)) {
          (void)queue->push(channel, "{\"tick\":1}");
        }
      });
}

bool remove_timer_entry(JSContext* context, EventsState* state, std::uint64_t sub) {
  for (auto iterator = state->timers.begin(); iterator != state->timers.end(); ++iterator) {
    if (iterator->sub != sub) continue;
    if (iterator->callback != 0) (void)state->host->remove_callback(iterator->callback);
    free_owned(context, iterator->fn);
    (void)state->host->subscriptions().remove(sub);
    state->timers.erase(iterator);
    return true;
  }
  return false;
}

JSValue events_timer_tick(JSContext* context, JSValueConst, int, JSValueConst*, int,
                          void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_UNDEFINED;
  EventsState* state = binding->events.get();
  if (!state || state->closed) return JS_UNDEFINED;
  state->tick_pending->store(false, std::memory_order_release);
  const std::int64_t now = now_ms();
  struct Due {
    std::uint64_t sub;
    std::int64_t priority;
    std::uint64_t sequence;
  };
  std::vector<Due> due;
  due.reserve(state->timers.size());
  for (const auto& timer : state->timers) {
    if (timer.next_due_ms <= now) due.push_back({timer.sub, timer.priority, timer.sequence});
  }
  std::sort(due.begin(), due.end(), [](const Due& left, const Due& right) {
    if (left.priority != right.priority) return left.priority > right.priority;
    return left.sequence < right.sequence;
  });
  for (const auto& row : due) {
    EventsState::Timer* timer = nullptr;
    for (auto& candidate : state->timers) {
      if (candidate.sub == row.sub) {
        timer = &candidate;
        break;
      }
    }
    if (!timer) continue;
    const bool once = timer->once;
    const std::uint64_t callback = timer->callback;
    timer->next_due_ms = now + timer_delay(timer->period_ms);
    if (const auto error = state->host->invoke_callback(callback, "{}"); !error.ok()) {
      record_failure(state->host, "rime:input.setTimer", error);
    }
    if (once) (void)remove_timer_entry(context, state, row.sub);
  }
  arm_scheduler(state);
  return JS_UNDEFINED;
}

// ---- onMessage -------------------------------------------------------------

JSValue events_message_channel(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                               int, void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
  EventsState* state = binding->events.get();
  if (!state || state->closed) return JS_UNDEFINED;
  JSValue message_value = event_field(context, argv[0], "msg");
  double message_number = 0;
  const bool message_ok =
      JS_IsNumber(message_value) && JS_ToFloat64(context, &message_number, message_value) == 0;
  JS_FreeValue(context, message_value);
  if (!message_ok) return JS_UNDEFINED;
  const std::uint32_t message = static_cast<std::uint32_t>(message_number);

  json::Value payload = json::Value::object();
  payload.set("msg", json::Value::number(static_cast<double>(message)));
  const auto copy_number = [context, &payload](JSValueConst object, const char* name) {
    JSValue value = event_field(context, object, name);
    double number = 0;
    if (JS_IsNumber(value) && JS_ToFloat64(context, &number, value) == 0) {
      payload.set(name, json::Value::number(number));
    }
    JS_FreeValue(context, value);
  };
  copy_number(argv[0], "wParam");
  copy_number(argv[0], "lParam");
  copy_number(argv[0], "hwnd");
  const std::string text = json::stringify(payload);

  std::vector<std::uint64_t> matches;
  matches.reserve(state->monitors.size());
  for (const auto& monitor : state->monitors) {
    if (monitor.msg == message) matches.push_back(monitor.sub);
  }
  for (const std::uint64_t sub : matches) {
    EventsState::Monitor* monitor = nullptr;
    for (auto& candidate : state->monitors) {
      if (candidate.sub == sub) {
        monitor = &candidate;
        break;
      }
    }
    if (!monitor || monitor->msg != message) continue;
    // AHK's instance cap via the central policy: while max_instances (its
    // MaxThreads bound for this monitor) deliveries are in flight the next
    // message is dropped, not queued.
    if (state->policy.admit_subscription(static_cast<std::uint32_t>(monitor->running),
                                         static_cast<std::uint32_t>(monitor->max_instances)) !=
        rime::core::Delivery::Deliver) {
      state->dropped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    monitor->running += 1;
    if (const auto error = state->host->invoke_callback(monitor->callback, text); !error.ok()) {
      record_failure(state->host, "rime:input.onMessage", error);
    }
    EventsState::Monitor* again = nullptr;
    for (auto& candidate : state->monitors) {
      if (candidate.sub == sub) {
        again = &candidate;
        break;
      }
    }
    if (again && again->msg == message && again->running > 0) again->running -= 1;
  }
  return JS_UNDEFINED;
}

// ---- Subscription objects --------------------------------------------------

bool close_registration(JSContext* context, InputModuleBinding* binding, const std::string& kind,
                        std::uint64_t sub);

JSValue subscription_close(JSContext* context, JSValueConst this_val, int, JSValueConst*, int,
                           void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!binding || !host || !binding->events) return JS_NewBool(context, 0);
  JSValue kind_value = JS_GetPropertyStr(context, this_val, "kind");
  JSValue id_value = JS_GetPropertyStr(context, this_val, "id");
  if (JS_IsException(kind_value) || JS_IsException(id_value)) {
    JS_FreeValue(context, kind_value);
    JS_FreeValue(context, id_value);
    return JS_EXCEPTION;
  }
  std::string kind;
  bool ok = JS_IsString(kind_value);
  if (ok) {
    if (const char* text = JS_ToCString(context, kind_value)) {
      kind.assign(text);
      JS_FreeCString(context, text);
    } else {
      ok = false;
    }
  }
  std::int64_t id = 0;
  ok = ok && strict_int64(context, id_value, id, "subscription.close()");
  JS_FreeValue(context, kind_value);
  JS_FreeValue(context, id_value);
  if (!ok) return JS_EXCEPTION;
  const bool removed =
      id > 0 && close_registration(context, binding, kind, static_cast<std::uint64_t>(id));
  return JS_NewBool(context, removed ? 1 : 0);
}

JSValue make_subscription(JSContext* context, InputModuleBinding* binding, const char* kind,
                          std::uint64_t sub) {
  JSValue object = JS_NewObject(context);
  if (JS_IsException(object)) return object;
  JS_SetPropertyStr(context, object, "id", JS_NewInt64(context, static_cast<std::int64_t>(sub)));
  JS_SetPropertyStr(context, object, "kind", JS_NewString(context, kind));
  JSValue close = JS_NewCClosure(context, subscription_close, "close", nullptr, 0, 0, binding);
  if (JS_IsException(close)) {
    JS_FreeValue(context, object);
    return close;
  }
  JS_SetPropertyStr(context, object, "close", close);
  return object;
}

// The single close path behind Subscription.close(): true the first time an
// open registration is released, false once it is gone (including after
// teardown, so close() stays idempotent).
bool close_registration(JSContext* context, InputModuleBinding* binding, const std::string& kind,
                        const std::uint64_t sub) {
  auto* host = host_of(context);
  if (!binding || !host || !binding->events) return false;
  EventsState* state = binding->events.get();
  if (!state || state->closed) return false;

  if (kind == "hotkey") {
    for (auto iterator = state->hotkeys.begin(); iterator != state->hotkeys.end(); ++iterator) {
      if (iterator->sub != sub) continue;
      if (!iterator->via_action && iterator->observer != 0) {
        (void)host->remove_callback(iterator->observer);
      }
      state->hotkeys.erase(iterator);
      if (state->dispatch_users > 0) state->dispatch_users -= 1;
      release_dispatch_if_idle(state);
      sync_criteria(context, state);
      (void)host->subscriptions().remove(sub);
      return true;
    }
    return false;
  }
  if (kind == "hotstring") {
    for (auto iterator = state->hotstrings.begin(); iterator != state->hotstrings.end();
         ++iterator) {
      if (iterator->sub != sub) continue;
      if (!iterator->via_action && iterator->observer != 0) {
        (void)host->remove_callback(iterator->observer);
      }
      state->hotstrings.erase(iterator);
      if (state->dispatch_users > 0) state->dispatch_users -= 1;
      release_dispatch_if_idle(state);
      sync_criteria(context, state);
      (void)host->subscriptions().remove(sub);
      return true;
    }
    return false;
  }
  if (kind == "timer") {
    const bool removed = remove_timer_entry(context, state, sub);
    if (removed) arm_scheduler(state);
    return removed;
  }
  if (kind == "message") {
    for (auto iterator = state->monitors.begin(); iterator != state->monitors.end(); ++iterator) {
      if (iterator->sub != sub) continue;
      if (iterator->callback != 0) (void)host->remove_callback(iterator->callback);
      free_owned(context, iterator->fn);
      state->monitors.erase(iterator);
      (void)host->subscriptions().remove(sub);
      if (state->monitors.empty() && state->ui_observer != 0 && state->window_service) {
        (void)state->window_service->ui().remove_message_observer(state->ui_observer);
        state->ui_observer = 0;
      }
      if (state->monitors.empty() && state->message_channel_callback != 0) {
        (void)host->remove_callback(state->message_channel_callback);
        state->message_channel_callback = 0;
      }
      return true;
    }
    return false;
  }
  if (kind == "clipboard") {
    for (auto iterator = state->clipboard_listeners.begin();
         iterator != state->clipboard_listeners.end(); ++iterator) {
      if (iterator->sub != sub) continue;
      if (state->clipboard_service && iterator->native_id != 0) {
        (void)state->clipboard_service->remove_change_listener(iterator->native_id);
      }
      if (iterator->callback != 0) (void)host->remove_callback(iterator->callback);
      state->clipboard_listeners.erase(iterator);
      (void)host->subscriptions().remove(sub);
      if (state->clipboard_listeners.empty() && state->window_service) {
        auto& ui = state->window_service->ui();
        if (state->clipboard_ui_observer != 0) {
          (void)ui.remove_message_observer(state->clipboard_ui_observer);
          state->clipboard_ui_observer = 0;
        }
        if (state->clipboard_os_listener) {
          const std::uintptr_t window = ui.message_window();
          if (window != 0) {
            (void)ui.call(
                [window] {
                  (void)RemoveClipboardFormatListener(reinterpret_cast<HWND>(window));
                },
                std::chrono::seconds(1));
          }
          state->clipboard_os_listener = false;
        }
      }
      return true;
    }
    return false;
  }
  if (kind == "error") {
    for (auto iterator = state->error_observers.begin();
         iterator != state->error_observers.end(); ++iterator) {
      if (iterator->sub != sub) continue;
      host->remove_error_observer(iterator->callback);
      if (iterator->callback != 0) (void)host->remove_callback(iterator->callback);
      state->error_observers.erase(iterator);
      (void)host->subscriptions().remove(sub);
      return true;
    }
    return false;
  }
  if (kind == "exit") {
    for (auto iterator = state->exit_observers.begin(); iterator != state->exit_observers.end();
         ++iterator) {
      if (iterator->sub != sub) continue;
      (void)host->remove_exit_handler(sub);  // also releases the callback
      state->exit_observers.erase(iterator);
      return true;
    }
    return false;
  }
  return false;
}

// ---- exports ---------------------------------------------------------------

JSValue events_hotkey(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!binding || !host || !binding->events || !binding->service || !binding->kernel ||
      !binding->dispatcher || !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  EventsState* state = binding->events.get();
  if (state->closed) return JS_ThrowInternalError(context, "rime:input events are shut down");
  if (argc < 2 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "hotkey(name, action, options?)");
  }
  // Argument validation first (M2-B/M2-C order): the options are parsed
  // before the capability gate is consulted.
  RegOptions options;
  if (argc >= 3 &&
      !parse_reg_options(context, argv[2], "hotkey(name, action, options)", false, options)) {
    return JS_EXCEPTION;
  }
  if (!binding->kernel->allows(kHookCapability)) {
    return throw_capability_error(context, kHookCapability);
  }
  const char* name_text = JS_ToCString(context, argv[0]);
  if (!name_text) return JS_EXCEPTION;
  const std::string name(name_text);
  JS_FreeCString(context, name_text);
  std::uint32_t vk = 0;
  std::uint8_t mask = 0;
  std::string parse_error;
  if (!normalize_hotkey_name(name, vk, mask, parse_error)) {
    return JS_ThrowTypeError(context, "hotkey(name, action): %s", parse_error.c_str());
  }
  const auto find_existing = [state, vk, mask]() -> EventsState::Hotkey* {
    for (auto& hotkey : state->hotkeys) {
      if (hotkey.vk == vk && hotkey.mask == mask && hotkey.criterion == state->current_criterion) {
        return &hotkey;
      }
    }
    return nullptr;
  };

  // Control form: hotkey(name, "on"|"off"|"toggle") toggles an existing
  // registration under the current criterion; there is nothing to control
  // otherwise (AHK's nonexistent-hotkey error).
  if (JS_IsString(argv[1])) {
    const int control = parse_ctrl_word(context, argv[1], "hotkey(name, action)");
    if (control < 0) return JS_EXCEPTION;
    EventsState::Hotkey* existing = find_existing();
    if (!existing) {
      return JS_ThrowTypeError(
          context, "hotkey(name, action): no hotkey is registered for this name under the "
                   "current criterion");
    }
    if (control == 2) existing->enabled = !existing->enabled;
    else existing->enabled = control == 1;
    if (options.control == 1) existing->enabled = true;
    else if (options.control == 0) existing->enabled = false;
    else if (options.control == 2) existing->enabled = !existing->enabled;
    if (options.have_exempt) existing->suspend_exempt = options.suspend_exempt;
    if (options.have_level) existing->input_level = options.input_level;
    return make_subscription(context, binding, "hotkey", existing->sub);
  }

  const bool is_function = JS_IsFunction(context, argv[1]);
  if (!is_function && !JS_IsObject(argv[1])) {
    return JS_ThrowTypeError(context,
                             "hotkey(name, action, options?): action must be a function, an "
                             "action template, or 'on'|'off'|'toggle'");
  }
  const bool have_options = options.control >= 0;
  const bool options_enabled = options.control != 0;

  // Validate the action before touching any registration.
  std::uint64_t incoming_observer = 0;
  const bool incoming_via_action = !is_function;
  EventAction incoming_action{};
  if (is_function) {
    if (const auto error = host->add_callback(JS_DupValue(context, argv[1]), incoming_observer);
        !error.ok()) {
      return JS_ThrowInternalError(context, "%s", error.message.c_str());
    }
  } else if (!read_action_template(context, argv[1], incoming_action, "action")) {
    return JS_EXCEPTION;
  }

  if (EventsState::Hotkey* existing = find_existing()) {
    if (!existing->via_action && existing->observer != 0) {
      (void)host->remove_callback(existing->observer);
    }
    existing->via_action = incoming_via_action;
    existing->observer = incoming_observer;
    existing->action = std::move(incoming_action);
    if (have_options) existing->enabled = options_enabled;
    if (options.have_exempt) existing->suspend_exempt = options.suspend_exempt;
    if (options.have_level) existing->input_level = options.input_level;
    return make_subscription(context, binding, "hotkey", existing->sub);
  }

  if (!ensure_dispatch(context, binding, state)) return JS_EXCEPTION;
  state->dispatch_users += 1;
  const std::uint64_t sub = host->allocate_subscription_id();
  EventsState::Hotkey entry{};
  entry.sub = sub;
  entry.vk = vk;
  entry.mask = mask;
  entry.criterion = state->current_criterion;
  entry.enabled = have_options ? options_enabled : true;
  entry.via_action = incoming_via_action;
  entry.observer = incoming_observer;
  entry.action = std::move(incoming_action);
  entry.name = name;
  if (options.have_exempt) entry.suspend_exempt = options.suspend_exempt;
  entry.input_level = options.have_level ? options.input_level : state->policy.input_level;
  state->hotkeys.push_back(std::move(entry));
  if (const auto error = host->subscriptions().add("hotkey", sub); !error.ok()) {
    state->hotkeys.pop_back();
    if (state->dispatch_users > 0) state->dispatch_users -= 1;
    release_dispatch_if_idle(state);
    if (!incoming_via_action && incoming_observer != 0) {
      (void)host->remove_callback(incoming_observer);
    }
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  sync_criteria(context, state);
  return make_subscription(context, binding, "hotkey", sub);
}

JSValue events_hotstring(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!binding || !host || !binding->events || !binding->service || !binding->kernel ||
      !binding->dispatcher || !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  EventsState* state = binding->events.get();
  if (state->closed) return JS_ThrowInternalError(context, "rime:input events are shut down");
  if (argc < 1 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "hotstring(spec, action?, onOff?)");
  }
  const char* spec_text = JS_ToCString(context, argv[0]);
  if (!spec_text) return JS_EXCEPTION;
  const std::string spec(spec_text);
  JS_FreeCString(context, spec_text);

  // Special forms: global settings and a buffer reset.
  if (spec == "Reset") {
    state->typed.clear();
    return JS_NULL;
  }
  if (spec == "EndChars") {
    if (argc >= 2) {
      if (!JS_IsString(argv[1])) {
        return JS_ThrowTypeError(context, "hotstring('EndChars', value): value must be a string");
      }
      const char* value = JS_ToCString(context, argv[1]);
      if (!value) return JS_EXCEPTION;
      state->end_chars.assign(value);
      JS_FreeCString(context, value);
    }
    return JS_NewString(context, state->end_chars.c_str());
  }
  if (spec == "MouseReset") {
    if (argc >= 2) {
      if (!JS_IsBool(argv[1])) {
        return JS_ThrowTypeError(context, "hotstring('MouseReset', value): value must be a boolean");
      }
      state->mouse_reset = JS_ToBool(context, argv[1]) > 0;
    }
    return JS_NewBool(context, state->mouse_reset ? 1 : 0);
  }

  EventsState::Hotstring parsed{};
  std::string parse_error;
  const SpecResult result = parse_hotstring_spec(spec, state->option_defaults, parsed, parse_error);
  if (result == SpecResult::Invalid) {
    return JS_ThrowTypeError(context, "hotstring(spec): %s", parse_error.c_str());
  }
  if (result == SpecResult::OptionsOnly) {
    // Options-only form updates the defaults future registrations inherit.
    state->option_defaults = parsed;
    return JS_NULL;
  }

  // M2-D options: the onOff argument also accepts { on?, suspendExempt?,
  // inputLevel? }; validation (and the capability check) happen before any
  // registration is touched.
  RegOptions options;
  if (argc >= 3 &&
      !parse_reg_options(context, argv[2], "hotstring(spec, action, onOff)", true, options)) {
    return JS_EXCEPTION;
  }

  // Settings forms above are plain state; registering a hotstring is what
  // installs a global hook stream, so only now is the capability required.
  if (!binding->kernel->allows(kHookCapability)) {
    return throw_capability_error(context, kHookCapability);
  }

  const int third_control = options.control;
  const auto apply_control = [](EventsState::Hotstring& target, const int control) {
    if (control == 0) target.enabled = false;
    else if (control == 1) target.enabled = true;
    else if (control == 2) target.enabled = !target.enabled;
  };

  const std::string key = make_hotstring_key(state->current_criterion, parsed.case_sensitive,
                                             parsed.inside_word, parsed.trigger);
  const auto find_existing = [state, &key]() -> EventsState::Hotstring* {
    for (auto& hotstring : state->hotstrings) {
      if (hotstring.key == key) return &hotstring;
    }
    return nullptr;
  };

  // Control form: hotstring(spec, "on"|"off"|"toggle").
  if (JS_IsString(argv[1])) {
    const int control = parse_ctrl_word(context, argv[1], "hotstring(spec, action)");
    if (control < 0) return JS_EXCEPTION;
    EventsState::Hotstring* existing = find_existing();
    if (!existing) {
      return JS_ThrowTypeError(
          context, "hotstring(spec, action): no hotstring is registered for this spec under the "
                   "current criterion");
    }
    apply_control(*existing, control);
    if (third_control >= 0) apply_control(*existing, third_control);
    if (options.have_exempt) existing->suspend_exempt = options.suspend_exempt;
    if (options.have_level) existing->input_level = options.input_level;
    return make_subscription(context, binding, "hotstring", existing->sub);
  }

  const bool is_function = JS_IsFunction(context, argv[1]);
  const bool is_object = JS_IsObject(argv[1]);
  const bool omitted = argc < 2 || JS_IsUndefined(argv[1]) || JS_IsNull(argv[1]);
  if (!omitted && !is_function && !is_object) {
    return JS_ThrowTypeError(context,
                             "hotstring(spec, action?, onOff?): action must be a function, an "
                             "action template, 'on'|'off'|'toggle', or omitted");
  }
  // The replacement form injects text; refuse at registration instead of
  // failing later inside the dispatcher.
  if (omitted && !binding->kernel->allows(kInjectCapability)) {
    return throw_capability_error(context, kInjectCapability);
  }

  std::uint64_t incoming_observer = 0;
  const bool incoming_via_action = is_object;
  EventAction incoming_action{};
  if (is_function) {
    if (const auto error = host->add_callback(JS_DupValue(context, argv[1]), incoming_observer);
        !error.ok()) {
      return JS_ThrowInternalError(context, "%s", error.message.c_str());
    }
  } else if (is_object && !read_action_template(context, argv[1], incoming_action, "action")) {
    return JS_EXCEPTION;
  }

  if (EventsState::Hotstring* existing = find_existing()) {
    if (!existing->via_action && existing->observer != 0) {
      (void)host->remove_callback(existing->observer);
    }
    const bool was_replacement = existing->observer == 0 && !existing->via_action;
    existing->trigger = std::move(parsed.trigger);
    existing->replacement = std::move(parsed.replacement);
    existing->wildcard = parsed.wildcard;
    existing->inside_word = parsed.inside_word;
    existing->do_backspace = parsed.do_backspace;
    existing->case_sensitive = parsed.case_sensitive;
    existing->conform_case = parsed.conform_case;
    existing->omit_end_char = parsed.omit_end_char;
    existing->do_reset = parsed.do_reset;
    existing->execute = parsed.execute;
    if (!omitted) {
      existing->via_action = incoming_via_action;
      existing->observer = incoming_observer;
      existing->action = std::move(incoming_action);
    } else {
      existing->via_action = false;
      existing->observer = 0;
      existing->action = EventAction{};
    }
    if (third_control >= 0) apply_control(*existing, third_control);
    if (options.have_exempt) existing->suspend_exempt = options.suspend_exempt;
    if (options.have_level) existing->input_level = options.input_level;
    (void)was_replacement;
    return make_subscription(context, binding, "hotstring", existing->sub);
  }

  if (!ensure_dispatch(context, binding, state)) return JS_EXCEPTION;
  state->dispatch_users += 1;
  const std::uint64_t sub = host->allocate_subscription_id();
  parsed.sub = sub;
  parsed.criterion = state->current_criterion;
  parsed.key = key;
  parsed.enabled = third_control == 0 ? false : true;
  if (options.have_exempt) parsed.suspend_exempt = options.suspend_exempt;
  parsed.input_level = options.have_level ? options.input_level : state->policy.input_level;
  parsed.via_action = incoming_via_action;
  parsed.observer = incoming_observer;
  parsed.action = std::move(incoming_action);
  state->hotstrings.push_back(std::move(parsed));
  if (const auto error = host->subscriptions().add("hotstring", sub); !error.ok()) {
    state->hotstrings.pop_back();
    if (state->dispatch_users > 0) state->dispatch_users -= 1;
    release_dispatch_if_idle(state);
    if (!incoming_via_action && incoming_observer != 0) {
      (void)host->remove_callback(incoming_observer);
    }
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  sync_criteria(context, state);
  return make_subscription(context, binding, "hotstring", sub);
}

JSValue hot_if_common(JSContext* context, InputModuleBinding*, EventsState* state,
                      const int argc, JSValueConst* argv, const CriterionKind kind,
                      const bool window_form) {
  JSValue previous = criterion_descriptor(context, state, state->current_criterion);
  if (JS_IsException(previous)) return previous;
  if (!window_form) {
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
      state->current_criterion = 0;
      sync_criteria(context, state);
      return previous;
    }
    if (!JS_IsFunction(context, argv[0])) {
      JS_FreeValue(context, previous);
      return JS_ThrowTypeError(context, "hotIf(fn|null)");
    }
    for (auto& criterion : state->criteria) {
      if (criterion.spec.kind == CriterionKind::Function &&
          JS_IsStrictEqual(context, criterion.fn, argv[0])) {
        state->current_criterion = criterion.id;
        sync_criteria(context, state);
        return previous;
      }
    }
    EventsState::Criterion entry{};
    entry.id = state->next_criterion_id++;
    entry.spec.id = entry.id;
    entry.spec.kind = CriterionKind::Function;
    entry.fn = JS_DupValue(context, argv[0]);
    state->criteria.push_back(std::move(entry));
    state->current_criterion = state->criteria.back().id;
    sync_criteria(context, state);
    return previous;
  }
  if (!state->window_service) {
    JS_FreeValue(context, previous);
    return JS_ThrowInternalError(context, "rime:input.hotIfWin* requires a window service");
  }
  WindowQuery query{};
  for (int index = 0; index < 3; ++index) {
    if (argc <= index || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) continue;
    if (!JS_IsString(argv[index])) {
      JS_FreeValue(context, previous);
      return JS_ThrowTypeError(
          context, "hotIfWin*(title?, className?, processName?): arguments must be strings");
    }
    const char* text = JS_ToCString(context, argv[index]);
    if (!text) {
      JS_FreeValue(context, previous);
      return JS_EXCEPTION;
    }
    if (index == 0) query.title.assign(text);
    else if (index == 1) query.class_name.assign(text);
    else query.process_name.assign(text);
    JS_FreeCString(context, text);
  }
  for (auto& criterion : state->criteria) {
    if (criterion.spec.kind == kind && criterion.spec.query == query) {
      state->current_criterion = criterion.id;
      sync_criteria(context, state);
      return previous;
    }
  }
  EventsState::Criterion entry{};
  entry.id = state->next_criterion_id++;
  entry.spec.id = entry.id;
  entry.spec.kind = kind;
  entry.spec.query = query;
  state->criteria.push_back(std::move(entry));
  state->current_criterion = state->criteria.back().id;
  sync_criteria(context, state);
  return previous;
}

JSValue events_hot_if(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  if (state->closed) return JS_ThrowInternalError(context, "rime:input events are shut down");
  return hot_if_common(context, binding, state, argc, argv, CriterionKind::Function, false);
}

JSValue events_hot_if_win(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque, const CriterionKind kind) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  if (state->closed) return JS_ThrowInternalError(context, "rime:input events are shut down");
  return hot_if_common(context, binding, state, argc, argv, kind, true);
}

JSValue events_hot_if_win_active(JSContext* context, JSValueConst this_val, int argc,
                                 JSValueConst* argv, int magic, void* opaque) {
  return events_hot_if_win(context, this_val, argc, argv, magic, opaque, CriterionKind::Active);
}

JSValue events_hot_if_win_exist(JSContext* context, JSValueConst this_val, int argc,
                                JSValueConst* argv, int magic, void* opaque) {
  return events_hot_if_win(context, this_val, argc, argv, magic, opaque, CriterionKind::Exists);
}

JSValue events_hot_if_win_not_active(JSContext* context, JSValueConst this_val, int argc,
                                     JSValueConst* argv, int magic, void* opaque) {
  return events_hot_if_win(context, this_val, argc, argv, magic, opaque,
                           CriterionKind::NotActive);
}

JSValue events_hot_if_win_not_exist(JSContext* context, JSValueConst this_val, int argc,
                                    JSValueConst* argv, int magic, void* opaque) {
  return events_hot_if_win(context, this_val, argc, argv, magic, opaque,
                           CriterionKind::NotExists);
}

JSValue events_install_hook(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void* opaque, const bool keyboard) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  bool install = true;
  bool force = false;
  if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    if (!JS_IsBool(argv[0])) {
      return JS_ThrowTypeError(context,
                               "install*Hook(install?, force?): install must be a boolean");
    }
    install = JS_ToBool(context, argv[0]) > 0;
  }
  if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (!JS_IsBool(argv[1])) {
      return JS_ThrowTypeError(context,
                               "install*Hook(install?, force?): force must be a boolean");
    }
    force = JS_ToBool(context, argv[1]) > 0;
  }
  if (install && !binding->kernel->allows(kHookCapability)) {
    return throw_capability_error(context, kHookCapability);
  }
  // Deviation from AHK (void): returns the effective installed state, so a
  // forced uninstall that a still-subscribed stream would otherwise undo is
  // observable.
  const bool effective = keyboard ? binding->service->set_keyboard_hook(install, force)
                                  : binding->service->set_mouse_hook(install, force);
  return JS_NewBool(context, effective ? 1 : 0);
}

JSValue events_install_keybd_hook(JSContext* context, JSValueConst this_val, int argc,
                                  JSValueConst* argv, int magic, void* opaque) {
  return events_install_hook(context, this_val, argc, argv, magic, opaque, true);
}

JSValue events_install_mouse_hook(JSContext* context, JSValueConst this_val, int argc,
                                  JSValueConst* argv, int magic, void* opaque) {
  return events_install_hook(context, this_val, argc, argv, magic, opaque, false);
}

JSValue events_set_timer(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!binding || !host || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  if (state->closed) return JS_ThrowInternalError(context, "rime:input events are shut down");
  if (argc < 1 || !JS_IsFunction(context, argv[0])) {
    return JS_ThrowTypeError(context, "setTimer(fn, period?, priority?)");
  }
  EventsState::Timer* existing = nullptr;
  for (auto& timer : state->timers) {
    if (JS_IsStrictEqual(context, timer.fn, argv[0])) {
      existing = &timer;
      break;
    }
  }
  std::int64_t period = 0;
  bool have_period = false;
  if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (!parse_i64(context, argv[1], period, "setTimer(fn, period)")) return JS_EXCEPTION;
    have_period = true;
  }
  if (have_period && period == 0) {
    if (existing) (void)remove_timer_entry(context, state, existing->sub);
    arm_scheduler(state);
    return JS_NULL;
  }
  std::int64_t priority = existing ? existing->priority : 0;
  if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
    if (!parse_i64(context, argv[2], priority, "setTimer(fn, priority)")) return JS_EXCEPTION;
  }
  const std::int64_t effective_period =
      have_period ? period : (existing ? existing->period_ms : kDefaultTimerPeriod);
  if (existing) {
    existing->period_ms = effective_period;
    existing->once = effective_period < 0;
    existing->priority = priority;
    existing->next_due_ms = now_ms() + timer_delay(effective_period);
    arm_scheduler(state);
    return make_subscription(context, binding, "timer", existing->sub);
  }

  std::uint64_t callback = 0;
  if (const auto error = host->add_callback(JS_DupValue(context, argv[0]), callback);
      !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  if (state->scheduler_callback == 0) {
    JSValue closure =
        JS_NewCClosure(context, events_timer_tick, "inputTimerTick", nullptr, 0, 0, binding);
    if (JS_IsException(closure)) {
      (void)host->remove_callback(callback);
      return JS_EXCEPTION;
    }
    std::uint64_t channel = 0;
    if (const auto error = host->add_callback(closure, channel); !error.ok()) {
      (void)host->remove_callback(callback);
      return JS_ThrowInternalError(context, "%s", error.message.c_str());
    }
    state->scheduler_callback = channel;
  }
  const std::uint64_t sub = host->allocate_subscription_id();
  EventsState::Timer entry{};
  entry.sub = sub;
  entry.callback = callback;
  entry.fn = JS_DupValue(context, argv[0]);
  entry.period_ms = effective_period;
  entry.once = effective_period < 0;
  entry.priority = priority;
  entry.sequence = state->next_sequence++;
  entry.next_due_ms = now_ms() + timer_delay(effective_period);
  state->timers.push_back(std::move(entry));
  if (const auto error = host->subscriptions().add("timer", sub); !error.ok()) {
    free_owned(context, state->timers.back().fn);
    state->timers.pop_back();
    (void)host->remove_callback(callback);
    if (state->timers.empty() && state->scheduler_callback != 0) {
      (void)host->remove_callback(state->scheduler_callback);
      state->scheduler_callback = 0;
    }
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  arm_scheduler(state);
  return make_subscription(context, binding, "timer", sub);
}

JSValue events_on_message(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!binding || !host || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  if (state->closed) return JS_ThrowInternalError(context, "rime:input events are shut down");
  if (argc < 2 || !JS_IsFunction(context, argv[1])) {
    return JS_ThrowTypeError(context, "onMessage(msgNumber, fn, maxInstances?)");
  }
  if (!binding->window_service) {
    return JS_ThrowInternalError(context, "rime:input.onMessage requires a window service");
  }
  std::int64_t raw_message = 0;
  if (!parse_i64(context, argv[0], raw_message, "onMessage(msgNumber, fn)")) return JS_EXCEPTION;
  if (raw_message < 0 || raw_message > 0xFFFFFFFFll) {
    return JS_ThrowTypeError(context, "onMessage(msgNumber, fn): msgNumber must fit in uint32");
  }
  const std::uint32_t message = static_cast<std::uint32_t>(raw_message);
  std::int64_t max_instances = 1;
  bool have_max = false;
  if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
    if (!parse_i64(context, argv[2], max_instances, "onMessage(msgNumber, fn, maxInstances)")) {
      return JS_EXCEPTION;
    }
    if (max_instances < 0) {
      return JS_ThrowTypeError(
          context, "onMessage(msgNumber, fn, maxInstances): maxInstances must not be negative");
    }
    if (max_instances > kMaxMessageInstances) max_instances = kMaxMessageInstances;
    have_max = true;
  }

  // maxInstances 0 is AHK's delete signal: drop the monitor with the same
  // (msg, fn) pair and return null; an unknown pair stays a no-op.
  if (have_max && max_instances == 0) {
    for (auto iterator = state->monitors.begin(); iterator != state->monitors.end(); ++iterator) {
      if (iterator->msg != message || !JS_IsStrictEqual(context, iterator->fn, argv[1])) continue;
      const std::uint64_t sub = iterator->sub;
      (void)close_registration(context, binding, "message", sub);
      break;
    }
    return JS_NULL;
  }

  // Same (msg, fn): update the instance cap in place (AHK updates the item).
  for (auto& monitor : state->monitors) {
    if (monitor.msg == message && JS_IsStrictEqual(context, monitor.fn, argv[1])) {
      if (have_max) monitor.max_instances = max_instances;
      return make_subscription(context, binding, "message", monitor.sub);
    }
  }

  std::uint64_t callback = 0;
  if (const auto error = host->add_callback(JS_DupValue(context, argv[1]), callback);
      !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  if (state->message_channel_callback == 0) {
    JSValue closure =
        JS_NewCClosure(context, events_message_channel, "inputMessageChannel", nullptr, 1, 0,
                       binding);
    if (JS_IsException(closure)) {
      (void)host->remove_callback(callback);
      return JS_EXCEPTION;
    }
    std::uint64_t channel = 0;
    if (const auto error = host->add_callback(closure, channel); !error.ok()) {
      (void)host->remove_callback(callback);
      return JS_ThrowInternalError(context, "%s", error.message.c_str());
    }
    state->message_channel_callback = channel;
  }
  const std::uint64_t sub = host->allocate_subscription_id();
  EventsState::Monitor entry{};
  entry.sub = sub;
  entry.callback = callback;
  entry.msg = message;
  entry.max_instances = have_max ? max_instances : 1;
  entry.fn = JS_DupValue(context, argv[1]);
  state->monitors.push_back(std::move(entry));
  if (state->ui_observer == 0) {
    const auto queue = host->event_queue();
    const std::uint64_t channel = state->message_channel_callback;
    state->ui_observer = binding->window_service->ui().add_message_observer(
        [queue, channel](unsigned int msg, std::uintptr_t wparam, std::uintptr_t lparam,
                         std::uintptr_t hwnd) {
          json::Value payload = json::Value::object();
          payload.set("msg", json::Value::number(static_cast<double>(msg)));
          payload.set("wParam", json::Value::number(static_cast<double>(wparam)));
          payload.set("lParam", json::Value::number(static_cast<double>(lparam)));
          payload.set("hwnd", json::Value::number(static_cast<double>(hwnd)));
          (void)queue->push(channel, json::stringify(payload));
        });
    if (state->ui_observer == 0) {
      // No message pump: roll the half-registered monitor back completely.
      (void)close_registration(context, binding, "message", sub);
      return JS_ThrowInternalError(
          context, "rime:input.onMessage requires a running message pump");
    }
  }
  if (const auto error = host->subscriptions().add("message", sub); !error.ok()) {
    (void)close_registration(context, binding, "message", sub);
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  return make_subscription(context, binding, "message", sub);
}

// ---- onClipboardChange -----------------------------------------------------

JSValue events_on_clipboard_change(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                                   int, void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!binding || !host || !binding->events || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  EventsState* state = binding->events.get();
  if (state->closed) return JS_ThrowInternalError(context, "rime:input events are shut down");
  if (argc < 1 || !JS_IsFunction(context, argv[0])) {
    return JS_ThrowTypeError(context, "onClipboardChange(fn)");
  }
  if (!binding->clipboard_service) {
    return JS_ThrowInternalError(context,
                                 "rime:input.onClipboardChange requires a clipboard service");
  }
  if (!binding->window_service) {
    return JS_ThrowInternalError(context,
                                 "rime:input.onClipboardChange requires a window service");
  }
  if (!binding->kernel->allows(kClipboardReadCapability)) {
    return throw_capability_error(context, kClipboardReadCapability);
  }

  std::uint64_t callback = 0;
  if (const auto error = host->add_callback(JS_DupValue(context, argv[0]), callback);
      !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  ClipboardService* service = binding->clipboard_service;
  const auto queue = host->event_queue();
  const std::uint64_t relay = callback;
  // Each listener pushes for itself: the payload reaches exactly the
  // callback that registered it (no fan-out to foreign observers).
  const std::uint64_t native_id = service->add_change_listener([queue, relay](bool from_self) {
    (void)queue->push(relay, from_self ? "{\"type\":1}" : "{\"type\":0}");
  });
  if (native_id == 0) {
    (void)host->remove_callback(callback);
    return JS_ThrowInternalError(context, "clipboard service is stopping");
  }

  auto& ui = binding->window_service->ui();
  if (state->clipboard_listeners.empty()) {
    if (state->clipboard_ui_observer == 0) {
      state->clipboard_ui_observer = ui.add_message_observer(
          [service](unsigned int msg, std::uintptr_t, std::uintptr_t, std::uintptr_t) {
            if (msg == kWmClipboardUpdate) service->notify_change();
          });
      if (state->clipboard_ui_observer == 0) {
        (void)service->remove_change_listener(native_id);
        (void)host->remove_callback(callback);
        return JS_ThrowInternalError(
            context, "rime:input.onClipboardChange requires a running message pump");
      }
    }
    if (!state->clipboard_os_listener) {
      const std::uintptr_t window = ui.message_window();
      const auto attached = std::make_shared<std::atomic<bool>>(false);
      const auto attach_error = ui.call(
          [window, attached] {
            attached->store(window != 0 &&
                                AddClipboardFormatListener(reinterpret_cast<HWND>(window)) !=
                                    FALSE,
                            std::memory_order_release);
          },
          std::chrono::seconds(1));
      if (!attach_error.ok() || !attached->load(std::memory_order_acquire)) {
        (void)ui.remove_message_observer(state->clipboard_ui_observer);
        state->clipboard_ui_observer = 0;
        (void)service->remove_change_listener(native_id);
        (void)host->remove_callback(callback);
        return JS_ThrowInternalError(
            context, "AddClipboardFormatListener failed: %s",
            attach_error.ok() ? "message pump unavailable" : attach_error.message.c_str());
      }
      state->clipboard_os_listener = true;
    }
  }

  const std::uint64_t sub = host->allocate_subscription_id();
  EventsState::Observer entry{};
  entry.sub = sub;
  entry.callback = callback;
  entry.native_id = native_id;
  state->clipboard_listeners.push_back(entry);
  if (const auto error = host->subscriptions().add("clipboard", sub); !error.ok()) {
    (void)close_registration(context, binding, "clipboard", sub);
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  return make_subscription(context, binding, "clipboard", sub);
}

// ---- onError / onExit ------------------------------------------------------

JSValue events_on_error(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!binding || !host || !binding->events) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  EventsState* state = binding->events.get();
  if (state->closed) return JS_ThrowInternalError(context, "rime:input events are shut down");
  if (argc < 1 || !JS_IsFunction(context, argv[0])) {
    return JS_ThrowTypeError(context, "onError(fn)");
  }
  std::uint64_t callback = 0;
  if (const auto error = host->add_callback(JS_DupValue(context, argv[0]), callback);
      !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  const std::uint64_t sub = host->allocate_subscription_id();
  state->error_observers.push_back({sub, callback, 0});
  host->add_error_observer(callback);
  if (const auto error = host->subscriptions().add("error", sub); !error.ok()) {
    host->remove_error_observer(callback);
    state->error_observers.pop_back();
    (void)host->remove_callback(callback);
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  return make_subscription(context, binding, "error", sub);
}

JSValue events_on_exit(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!binding || !host || !binding->events) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  EventsState* state = binding->events.get();
  if (state->closed) return JS_ThrowInternalError(context, "rime:input events are shut down");
  if (argc < 1 || !JS_IsFunction(context, argv[0])) {
    return JS_ThrowTypeError(context, "onExit(fn)");
  }
  std::uint64_t callback = 0;
  if (const auto error = host->add_callback(JS_DupValue(context, argv[0]), callback);
      !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  const std::uint64_t sub = host->allocate_subscription_id();
  if (const auto error = host->add_exit_handler(callback, sub); !error.ok()) {
    (void)host->remove_callback(callback);
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  state->exit_observers.push_back({sub, callback, 0});
  // Deliberately NOT registered in the SubscriptionRegistry: a waiting
  // onExit handler must not block unload - HostAbi::unload excludes exit
  // handlers from its busy check and runs them as its first clean step.
  return make_subscription(context, binding, "exit", sub);
}

// ---- InputHook JS object ---------------------------------------------------

// Property magic for the accessor closures: the 17 accessor properties
// from docs/api/objects.json in member order (6 settable bools, 6 readonly
// snapshots, MinSendLevel, the 4 On* callbacks, Timeout). The `id` data
// property is defined separately and stays non-enumerable, so Object.keys()
// shows exactly the 23 documented members.
enum InputHookProperty : int {
  kHookPropBackspaceIsUndo = 0,
  kHookPropCaseSensitive,
  kHookPropFindAnywhere,
  kHookPropNotifyNonText,
  kHookPropVisibleNonText,
  kHookPropVisibleText,
  kHookPropEndKey,
  kHookPropEndMods,
  kHookPropEndReason,
  kHookPropInProgress,
  kHookPropInput,
  kHookPropMatch,
  kHookPropMinSendLevel,
  kHookPropOnChar,
  kHookPropOnEnd,
  kHookPropOnKeyDown,
  kHookPropOnKeyUp,
  kHookPropTimeout,
  kHookPropCount,
};

constexpr const char* kInputHookPropertyNames[kHookPropCount] = {
    "BackspaceIsUndo", "CaseSensitive",    "FindAnywhere",  "NotifyNonText",
    "VisibleNonText",  "VisibleText",      "EndKey",        "EndMods",
    "EndReason",       "InProgress",       "Input",         "Match",
    "MinSendLevel",    "OnChar",           "OnEnd",         "OnKeyDown",
    "OnKeyUp",         "Timeout",
};

// __New(options?, endKeys?, matchList?): validates every argument into a
// scratch copy first, then commits onto the idle hook - ValueError for bad
// options/end keys/match list, InvalidState while input is in progress
// (docs/api/objects.json). The factory and the __New method share this.
bool input_hook_init(JSContext* context, EventsState* state, const std::uint64_t id,
                     const int argc, JSValueConst* argv) {
  EventsState::InputHook* hook = find_input_hook(state, id);
  if (!hook) {
    JS_ThrowInternalError(context, "InputHook is closed");
    return false;
  }
  if (hook->in_progress) {
    JS_ThrowInternalError(context, "InputHook: cannot re-initialize while input is in progress");
    return false;
  }
  const auto read_arg = [context, argc, argv](const int index, std::string& out) -> int {
    // 0 = ok/absent, -1 = error (exception pending), 1 = wrong type.
    if (index >= argc || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) return 0;
    if (!JS_IsString(argv[index])) return 1;
    const char* text = JS_ToCString(context, argv[index]);
    if (!text) return -1;
    out.assign(text);
    JS_FreeCString(context, text);
    return 0;
  };
  EventsState::InputHook scratch;  // default option values, untouched state
  std::string text;
  const int options_status = read_arg(0, text);
  if (options_status < 0) return false;
  if (options_status > 0) {
    JS_ThrowTypeError(context, "__New(options): options must be a string");
    return false;
  }
  if (argc >= 1 && JS_IsString(argv[0])) {
      std::string error;
      if (!parse_hook_options(text, scratch, error)) {
      JS_ThrowTypeError(context, "__New(options): %s", error.c_str());
      return false;
    }
  }
  text.clear();
  const int end_status = read_arg(1, text);
  if (end_status < 0) return false;
  if (end_status > 0) {
    JS_ThrowTypeError(context, "__New(endKeys): endKeys must be a string");
    return false;
  }
  if (argc >= 2 && JS_IsString(argv[1])) {
    std::vector<KeyListEntry> entries;
    if (!resolve_key_list(context, text, KeyListMode::EndKeys, entries, "__New(endKeys)")) {
      return false;
    }
    apply_end_keys(entries, scratch.key_flags);
  }
  text.clear();
  const int match_status = read_arg(2, text);
  if (match_status < 0) return false;
  if (match_status > 0) {
    JS_ThrowTypeError(context, "__New(matchList): matchList must be a string");
    return false;
  }
  if (argc >= 3 && JS_IsString(argv[2])) {
    if (!parse_match_list(context, text, scratch.match_list)) return false;
  }
  // Commit: option fields plus a fresh collection state (AHK Start also
  // clears the buffer; __New on an idle hook resets everything observable).
  hook->backspace_undo = scratch.backspace_undo;
  hook->case_sensitive = scratch.case_sensitive;
  hook->find_anywhere = scratch.find_anywhere;
  hook->notify_non_text = scratch.notify_non_text;
  hook->visible_non_text = scratch.visible_non_text;
  hook->visible_text = scratch.visible_text;
  hook->min_send_level = scratch.min_send_level;
  hook->timeout_ms = scratch.timeout_ms;
  hook->buffer_max = scratch.buffer_max;
  hook->key_flags = scratch.key_flags;
  hook->match_list = scratch.match_list;
  hook->buffer.clear();
  hook->end_key.clear();
  hook->end_mods.clear();
  hook->match.clear();
  hook->end_reason = "Stopped";
  return true;
}

JSValue input_hook_new(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                       int, void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  EventsState::InputHook* hook = hook_of(context, this_val, state);
  if (!hook) return JS_EXCEPTION;
  const std::uint64_t id = hook->id;
  if (!input_hook_init(context, state, id, argc, argv)) return JS_EXCEPTION;
  return JS_UNDEFINED;
}

// KeyOpt(keys, keyOptions): AHK SetKeyFlags option letters - '+' add, '-'
// remove, E end key, I ignore text, N notify, Z zero; S/V are accepted
// without hook-side effect; "{All}" applies to every key. ValueError on
// invalid keys or options (docs/api/objects.json).
JSValue input_hook_key_opt(JSContext* context, JSValueConst this_val, int argc,
                           JSValueConst* argv, int, void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  EventsState::InputHook* hook = hook_of(context, this_val, state);
  if (!hook) return JS_EXCEPTION;
  if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context, "KeyOpt(keys, keyOptions)");
  }
  const char* keys_text = JS_ToCString(context, argv[0]);
  if (!keys_text) return JS_EXCEPTION;
  const std::string keys(keys_text);
  JS_FreeCString(context, keys_text);
  const char* options_text = JS_ToCString(context, argv[1]);
  if (!options_text) return JS_EXCEPTION;
  const std::string options(options_text);
  JS_FreeCString(context, options_text);
  bool adding = true;
  std::uint8_t add_flags = 0;
  std::uint8_t remove_flags = 0;
  for (const char raw : options) {
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
    if (letter == '+') {
      adding = true;
      continue;
    }
    if (letter == '-') {
      adding = false;
      continue;
    }
    if (letter == ' ' || letter == '\t') continue;
    std::uint8_t flag = 0;
    if (letter == 'E') {
      flag = EventsState::InputHook::kEndKey;
    } else if (letter == 'I') {
      flag = EventsState::InputHook::kIgnoreText;
    } else if (letter == 'N') {
      flag = EventsState::InputHook::kNotify;
    } else if (letter == 'S' || letter == 'V') {
      continue;  // accepted, not implemented (documented deviation)
    } else if (letter == 'Z') {
      add_flags = 0;
      remove_flags = EventsState::InputHook::kKeyOptionMask;
      continue;
    } else {
      JS_ThrowTypeError(context, "KeyOpt: unsupported option '%c'", letter);
      return JS_EXCEPTION;
    }
    if (adding) {
      add_flags = static_cast<std::uint8_t>(add_flags | flag);
    } else {
      remove_flags = static_cast<std::uint8_t>(remove_flags | flag);
      add_flags = static_cast<std::uint8_t>(add_flags & ~flag);
    }
  }
  if (ascii_lower(keys) == "{all}") {
    for (std::uint8_t& slot : hook->key_flags) {
      slot = static_cast<std::uint8_t>((slot & ~remove_flags) | add_flags);
    }
    return JS_UNDEFINED;
  }
  std::vector<KeyListEntry> entries;
  if (!resolve_key_list(context, keys, KeyListMode::KeyOpt, entries, "KeyOpt(keys)")) {
    return JS_EXCEPTION;
  }
  for (const KeyListEntry& entry : entries) {
    std::uint8_t& slot = hook->key_flags[entry.vk];
    slot = static_cast<std::uint8_t>((slot & ~remove_flags) | add_flags);
  }
  return JS_UNDEFINED;
}

// Start(): idempotent while input is in progress; capability-checked here
// (construction stays ungated - docs/api/objects.json pins no capability
// error on the factory).
JSValue input_hook_start(JSContext* context, JSValueConst this_val, int, JSValueConst*, int,
                         void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  EventsState* state = binding->events.get();
  EventsState::InputHook* hook = hook_of(context, this_val, state);
  if (!hook) return JS_EXCEPTION;
  if (hook->in_progress) return JS_UNDEFINED;  // AHK: Start is idempotent
  if (!binding->kernel->allows(kHookCapability)) {
    return throw_capability_error(context, kHookCapability);
  }
  if (!activate_input_hook(context, binding, state, *hook)) return JS_EXCEPTION;
  return JS_UNDEFINED;
}

// Stop(): no-op when input is not in progress (AHK has no error path).
JSValue input_hook_stop(JSContext* context, JSValueConst this_val, int, JSValueConst*, int,
                        void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  EventsState::InputHook* hook = hook_of(context, this_val, state);
  if (!hook) return JS_EXCEPTION;
  if (hook->in_progress) end_input_hook(context, state, *hook, "Stopped");
  return JS_UNDEFINED;
}

// Wait(maxTime?): promise resolving to the EndReason string. While idle it
// resolves immediately with the last EndReason; otherwise it waits for the
// input to end, or for the (optional, seconds) budget to expire - then the
// promise resolves with "Timeout" while the input itself keeps running.
JSValue input_hook_wait(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                        int, void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  EventsState::InputHook* hook = hook_of(context, this_val, state);
  if (!hook) return JS_EXCEPTION;
  double seconds = 0;
  if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    if (!JS_IsNumber(argv[0]) || JS_ToFloat64(context, &seconds, argv[0]) != 0 ||
        !std::isfinite(seconds)) {
      return JS_ThrowTypeError(context, "Wait(maxTime): maxTime must be a number (seconds)");
    }
    if (seconds < 0) seconds = 0;  // AHK's negative cast is UB; we clamp
  }
  rime::js::Host* host = state->host;
  JSValue promise = JS_UNDEFINED;
  std::uint64_t token = 0;
  if (const auto error = host->begin_async(context, promise, token, 0); !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  if (!hook->in_progress) {
    host->complete_async(token, true, json::stringify(json::Value::string(hook->end_reason)));
    return promise;
  }
  const std::uint64_t id = hook->id;
  hook->wait_tokens.push_back(token);
  if (argc >= 1 && seconds > 0) {
    const auto queue = host->event_queue();
    const std::uint64_t channel = state->hook_channel_callback;
    const std::uint64_t timer = host->timers().schedule(
        std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000.0)),
        [queue, channel, id, token] {
          (void)queue->push(channel, "{\"hook\":" + std::to_string(id) +
                                         ",\"wait\":" + std::to_string(token) + "}");
        });
    hook->wait_timers.push_back(timer);
  } else {
    hook->wait_timers.push_back(0);
  }
  return promise;
}

JSValue input_hook_get(JSContext* context, JSValueConst this_val, int, JSValueConst*, int magic,
                       void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_UNDEFINED;
  EventsState* state = binding->events.get();
  EventsState::InputHook* hook = hook_of(context, this_val, state);
  if (!hook) return JS_EXCEPTION;
  switch (magic) {
    case kHookPropBackspaceIsUndo:
      return JS_NewBool(context, hook->backspace_undo ? 1 : 0);
    case kHookPropCaseSensitive:
      return JS_NewBool(context, hook->case_sensitive ? 1 : 0);
    case kHookPropFindAnywhere:
      return JS_NewBool(context, hook->find_anywhere ? 1 : 0);
    case kHookPropNotifyNonText:
      return JS_NewBool(context, hook->notify_non_text ? 1 : 0);
    case kHookPropVisibleNonText:
      return JS_NewBool(context, hook->visible_non_text ? 1 : 0);
    case kHookPropVisibleText:
      return JS_NewBool(context, hook->visible_text ? 1 : 0);
    case kHookPropEndKey:
      return JS_NewString(context, hook->end_key.c_str());
    case kHookPropEndMods:
      return JS_NewString(context, hook->end_mods.c_str());
    case kHookPropEndReason:
      return JS_NewString(context, hook->end_reason.c_str());
    case kHookPropInProgress:
      return JS_NewBool(context, hook->in_progress ? 1 : 0);
    case kHookPropInput:
      return JS_NewString(context, hook->buffer.c_str());
    case kHookPropMatch:
      return JS_NewString(context, hook->match.c_str());
    case kHookPropMinSendLevel:
      return JS_NewUint32(context, hook->min_send_level);
    case kHookPropOnChar:
      return JS_IsUndefined(hook->on_char) ? JS_NULL : JS_DupValue(context, hook->on_char);
    case kHookPropOnEnd:
      return JS_IsUndefined(hook->on_end) ? JS_NULL : JS_DupValue(context, hook->on_end);
    case kHookPropOnKeyDown:
      return JS_IsUndefined(hook->on_key_down) ? JS_NULL
                                               : JS_DupValue(context, hook->on_key_down);
    case kHookPropOnKeyUp:
      return JS_IsUndefined(hook->on_key_up) ? JS_NULL : JS_DupValue(context, hook->on_key_up);
    case kHookPropTimeout:
      return JS_NewFloat64(context, static_cast<double>(hook->timeout_ms) / 1000.0);
    default:
      return JS_UNDEFINED;
  }
}

JSValue input_hook_set(JSContext* context, JSValueConst this_val, int argc, JSValueConst* argv,
                       int magic, void* opaque) {
  if (argc < 1) return JS_ThrowTypeError(context, "InputHook: missing property value");
  JSValueConst value = argv[0];
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  EventsState::InputHook* hook = hook_of(context, this_val, state);
  if (!hook) return JS_EXCEPTION;
  const char* name = (magic >= 0 && magic < kHookPropCount) ? kInputHookPropertyNames[magic]
                                                           : "property";
  if (magic >= kHookPropEndKey && magic <= kHookPropMatch) {
    return JS_ThrowTypeError(context, "InputHook.%s is read-only", name);
  }
  if (magic == kHookPropMinSendLevel) {
    double number = 0;
    if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) != 0 ||
        !std::isfinite(number)) {
      return JS_ThrowTypeError(context, "InputHook.%s must be numeric", name);
    }
    std::int32_t level = 0;
    if (JS_ToInt32(context, &level, value) != 0) return JS_EXCEPTION;
    hook->min_send_level = level < 0 ? 0 : static_cast<std::uint32_t>(level);
    return JS_UNDEFINED;
  }
  if (magic == kHookPropTimeout) {
    double number = 0;
    if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) != 0 ||
        !std::isfinite(number)) {
      return JS_ThrowTypeError(context, "InputHook.%s must be numeric", name);
    }
    if (number < 0) return JS_ThrowRangeError(context, "InputHook.%s cannot be negative", name);
    hook->timeout_ms = static_cast<std::int64_t>(number * 1000.0);
    arm_hook_timeout(state, *hook);  // re-arms while in progress
    return JS_UNDEFINED;
  }
  if (magic >= kHookPropOnChar && magic <= kHookPropOnKeyUp) {
    if (!JS_IsUndefined(value) && !JS_IsNull(value) && !JS_IsFunction(context, value)) {
      return JS_ThrowTypeError(context, "InputHook.%s must be a callable object", name);
    }
    JSValue* owned = &hook->on_char;
    std::uint64_t* callback = &hook->on_char_cb;
    if (magic == kHookPropOnEnd) {
      owned = &hook->on_end;
      callback = &hook->on_end_cb;
    } else if (magic == kHookPropOnKeyDown) {
      owned = &hook->on_key_down;
      callback = &hook->on_key_down_cb;
    } else if (magic == kHookPropOnKeyUp) {
      owned = &hook->on_key_up;
      callback = &hook->on_key_up_cb;
    }
    JS_FreeValue(context, *owned);
    *owned = (JS_IsUndefined(value) || JS_IsNull(value)) ? JS_UNDEFINED
                                                         : JS_DupValue(context, value);
    // While input is in progress the registered host callback must follow
    // the assignment - a plain JSValue swap would never fire.
    if (hook->in_progress) {
      if (*callback != 0) {
        (void)state->host->remove_callback(*callback);
        *callback = 0;
      }
      if (!JS_IsUndefined(*owned)) {
        if (const auto error = state->host->add_callback(JS_DupValue(context, *owned), *callback);
            !error.ok()) {
          return JS_ThrowInternalError(context, "%s", error.message.c_str());
        }
      }
    }
    return JS_UNDEFINED;
  }
  if (!JS_IsBool(value)) {
    return JS_ThrowTypeError(context, "InputHook.%s must be a boolean", name);
  }
  const bool flag = JS_ToBool(context, value) > 0;
  switch (magic) {
    case kHookPropBackspaceIsUndo:
      hook->backspace_undo = flag;
      break;
    case kHookPropCaseSensitive:
      hook->case_sensitive = flag;
      break;
    case kHookPropFindAnywhere:
      hook->find_anywhere = flag;
      break;
    case kHookPropNotifyNonText:
      hook->notify_non_text = flag;
      break;
    case kHookPropVisibleNonText:
      hook->visible_non_text = flag;
      break;
    case kHookPropVisibleText:
      hook->visible_text = flag;
      break;
    default:
      return JS_ThrowTypeError(context, "InputHook: unknown property");
  }
  return JS_UNDEFINED;
}

// input.createInputHook(options?, endKeys?, matchList?): builds one
// runtime-owned hook object - the native side of the InputHook member list
// in docs/api/objects.json. Construction is capability-free; Start() gates
// on windows.hook.global.
JSValue events_create_input_hook(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                                 int, void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  if (!state || state->closed) {
    return JS_ThrowInternalError(context, "rime:input events are shut down");
  }
  EventsState::InputHook entry{};
  entry.id = state->next_input_hook_id++;
  const std::uint64_t id = entry.id;
  state->input_hooks.push_back(entry);
  const auto discard = [state, id] {
    for (auto iterator = state->input_hooks.begin(); iterator != state->input_hooks.end();
         ++iterator) {
      if (iterator->id == id) {
        state->input_hooks.erase(iterator);
        break;
      }
    }
  };
  JSValue object = JS_NewObject(context);
  if (JS_IsException(object)) {
    discard();
    return JS_EXCEPTION;
  }
  // `id`: non-enumerable, non-writable data property (hook_of reads it).
  const JSAtom id_atom = JS_NewAtom(context, "id");
  if (id_atom == JS_ATOM_NULL ||
      JS_DefinePropertyValue(context, object, id_atom,
                             JS_NewInt64(context, static_cast<std::int64_t>(id)),
                             JS_PROP_CONFIGURABLE) < 0) {
    if (id_atom != JS_ATOM_NULL) JS_FreeAtom(context, id_atom);
    JS_FreeValue(context, object);
    discard();
    return JS_EXCEPTION;
  }
  JS_FreeAtom(context, id_atom);
  const auto define_method = [context, object, binding](const char* name, JSCClosure* function,
                                                        const int length) -> bool {
    JSValue closure = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(closure)) return false;
    return JS_SetPropertyStr(context, object, name, closure) >= 0;
  };
  const auto define_property = [context, object, binding](const int magic) -> bool {
    const char* name = kInputHookPropertyNames[magic];
    const bool readonly = magic >= kHookPropEndKey && magic <= kHookPropMatch;
    JSValue getter = JS_NewCClosure(context, input_hook_get, name, nullptr, 0, magic, binding);
    if (JS_IsException(getter)) return false;
    JSValue setter = JS_UNDEFINED;
    if (!readonly) {
      setter = JS_NewCClosure(context, input_hook_set, name, nullptr, 1, magic, binding);
      if (JS_IsException(setter)) {
        JS_FreeValue(context, getter);
        return false;
      }
    }
    const JSAtom atom = JS_NewAtom(context, name);
    if (atom == JS_ATOM_NULL) {
      JS_FreeValue(context, getter);
      JS_FreeValue(context, setter);
      return false;
    }
    // Consumes getter and setter on both success and failure.
    const int defined =
        JS_DefinePropertyGetSet(context, object, atom, getter, setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(context, atom);
    return defined >= 0;
  };
  bool built = define_method("__New", input_hook_new, 3) &&
               define_method("KeyOpt", input_hook_key_opt, 2) &&
               define_method("Start", input_hook_start, 0) &&
               define_method("Stop", input_hook_stop, 0) &&
               define_method("Wait", input_hook_wait, 1);
  for (int magic = 0; built && magic < kHookPropCount; ++magic) built = define_property(magic);
  if (!built) {
    JS_FreeValue(context, object);
    discard();
    return JS_EXCEPTION;
  }
  if (!input_hook_init(context, state, id, argc, argv)) {
    JS_FreeValue(context, object);
    discard();
    return JS_EXCEPTION;
  }
  return object;
}

// ---- input.suspend / input.policy (M2-D) ----------------------------------

// input.suspend(on?): boolean or "on"|"off"|"toggle" sets the flag,
// undefined/null (or no argument) toggles; returns the resulting state.
// Hotkey and hotstring matching consult it through the central policy;
// timers, onMessage and InputHook capture keep running (AHK's Suspend only
// disables hotkey recognition).
JSValue input_suspend(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  if (!state || state->closed) {
    return JS_ThrowInternalError(context, "rime:input events are shut down");
  }
  if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    if (JS_IsBool(argv[0])) {
      state->suspended = JS_ToBool(context, argv[0]) > 0;
    } else if (JS_IsString(argv[0])) {
      const char* text = JS_ToCString(context, argv[0]);
      if (!text) return JS_EXCEPTION;
      const std::string word = ascii_lower(text);
      JS_FreeCString(context, text);
      if (word == "on") {
        state->suspended = true;
      } else if (word == "off") {
        state->suspended = false;
      } else if (word == "toggle") {
        state->suspended = !state->suspended;
      } else {
        return JS_ThrowTypeError(context, "suspend(on?): expected 'on', 'off' or 'toggle'");
      }
    } else {
      return JS_ThrowTypeError(context,
                               "suspend(on?): expected a boolean or 'on'|'off'|'toggle'");
    }
  } else {
    state->suspended = !state->suspended;
  }
  return JS_NewBool(context, state->suspended ? 1 : 0);
}

const char* overflow_policy_name(const rime::core::OverflowPolicy policy) {
  switch (policy) {
    case rime::core::OverflowPolicy::Reject:
      return "reject";
    case rime::core::OverflowPolicy::DropOldest:
      return "dropOldest";
    case rime::core::OverflowPolicy::CoalesceByKey:
      return "coalesce";
  }
  return "reject";
}

// input.policy(snapshot?): without arguments returns the central dispatch
// policy as { maxConcurrency, maxConcurrencyPerHotkey, inputLevel,
// hotIfTimeout, overflow }; with a snapshot object every field is validated
// first (unknown key => TypeError) and only then committed. This is the one
// place the directive knobs live - docs/api/directives-and-syntax.md.
JSValue input_policy(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->events) return JS_ThrowInternalError(context, "rime:input is not wired");
  EventsState* state = binding->events.get();
  if (!state || state->closed) {
    return JS_ThrowInternalError(context, "rime:input events are shut down");
  }
  if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    if (!JS_IsObject(argv[0])) {
      return JS_ThrowTypeError(context, "policy(snapshot): snapshot must be an object");
    }
    JSPropertyEnum* names = nullptr;
    std::uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &names, &count, argv[0], JS_GPN_STRING_MASK) < 0) {
      return JS_EXCEPTION;
    }
    bool known = true;
    for (std::uint32_t index = 0; index < count && known; ++index) {
      const char* key = JS_AtomToCString(context, names[index].atom);
      if (!key) {
        known = false;
        break;
      }
      const std::string name(key);
      JS_FreeCString(context, key);
      if (name != "maxConcurrency" && name != "maxConcurrencyPerHotkey" &&
          name != "inputLevel" && name != "hotIfTimeout" && name != "overflow") {
        JS_ThrowTypeError(context, "policy(snapshot): unsupported field '%s'", name.c_str());
        known = false;
      }
    }
    JS_FreePropertyEnum(context, names, count);
    if (!known) return JS_EXCEPTION;
    rime::core::SchedulerPolicy next = state->policy;
    // Reads one field; absent keys leave `present` false so the caller keeps
    // the current value (the shared `field` variable stays untouched).
    const auto read_uint = [context, argv](const char* key, std::uint64_t& out, bool& present) {
      present = false;
      JSValue entry = JS_GetPropertyStr(context, argv[0], key);
      if (JS_IsException(entry)) return false;
      if (JS_IsUndefined(entry)) {
        JS_FreeValue(context, entry);
        return true;
      }
      double number = 0;
      const bool numeric = JS_IsNumber(entry) && JS_ToFloat64(context, &number, entry) == 0;
      JS_FreeValue(context, entry);
      if (!numeric || !std::isfinite(number) || number < 0 ||
          number != std::trunc(number) || number > 4294967295.0) {
        JS_ThrowTypeError(context, "policy.%s must be a non-negative integer", key);
        return false;
      }
      out = static_cast<std::uint64_t>(number);
      present = true;
      return true;
    };
    std::uint64_t field = 0;
    bool present = false;
    if (!read_uint("maxConcurrency", field, present)) return JS_EXCEPTION;
    if (present) next.max_concurrency = static_cast<std::uint32_t>(field);
    if (!read_uint("maxConcurrencyPerHotkey", field, present)) return JS_EXCEPTION;
    if (present) next.max_concurrency_per_subscription = static_cast<std::uint32_t>(field);
    if (!read_uint("inputLevel", field, present)) return JS_EXCEPTION;
    if (present) next.input_level = static_cast<std::uint32_t>(field);
    if (!read_uint("hotIfTimeout", field, present)) return JS_EXCEPTION;
    if (present) next.hot_if_timeout_ms = field;
    JSValue overflow = JS_GetPropertyStr(context, argv[0], "overflow");
    if (JS_IsException(overflow)) return JS_EXCEPTION;
    if (!JS_IsUndefined(overflow)) {
      if (!JS_IsString(overflow)) {
        JS_FreeValue(context, overflow);
        return JS_ThrowTypeError(context, "policy.overflow must be a string");
      }
      const char* text = JS_ToCString(context, overflow);
      JS_FreeValue(context, overflow);
      if (!text) return JS_EXCEPTION;
      const std::string name(text);
      JS_FreeCString(context, text);
      if (name == "reject") {
        next.overflow = rime::core::OverflowPolicy::Reject;
      } else if (name == "dropOldest") {
        next.overflow = rime::core::OverflowPolicy::DropOldest;
      } else if (name == "coalesce") {
        next.overflow = rime::core::OverflowPolicy::CoalesceByKey;
      } else {
        return JS_ThrowTypeError(
            context, "policy.overflow must be 'reject', 'dropOldest' or 'coalesce'");
      }
    }
    state->policy = next;
  }
  JSValue snapshot = JS_NewObject(context);
  if (JS_IsException(snapshot)) return snapshot;
  JS_SetPropertyStr(context, snapshot, "maxConcurrency",
                    JS_NewUint32(context, state->policy.max_concurrency));
  JS_SetPropertyStr(context, snapshot, "maxConcurrencyPerHotkey",
                    JS_NewUint32(context, state->policy.max_concurrency_per_subscription));
  JS_SetPropertyStr(context, snapshot, "inputLevel",
                    JS_NewUint32(context, state->policy.input_level));
  JS_SetPropertyStr(context, snapshot, "hotIfTimeout",
                    JS_NewInt64(context, static_cast<std::int64_t>(state->policy.hot_if_timeout_ms)));
  JS_SetPropertyStr(context, snapshot, "overflow",
                    JS_NewString(context, overflow_policy_name(state->policy.overflow)));
  return snapshot;
}

// ---- module wiring ---------------------------------------------------------

// One EventsState per binding, created the first time rime:input loads and
// kept for the host's lifetime. The host teardown registered here closes
// every registration exactly once and runs before the JS context dies.
bool ensure_events_state(JSContext* context, InputModuleBinding* binding) {
  if (binding->events) return true;
  auto* host = host_of(context);
  if (!host) return false;
  auto state = std::make_shared<EventsState>();
  state->host = host;
  state->service = binding->service;
  state->window_service = binding->window_service;
  state->clipboard_service = binding->clipboard_service;
  state->kernel = binding->kernel;
  state->dispatcher = binding->dispatcher;
  state->next_action_id = binding->next_action_id;
  state->js_context = context;
  state->end_chars = kDefaultEndChars;
  const std::weak_ptr<EventsState> weak = state;
  binding->events = std::move(state);
  host->add_teardown([weak] {
    if (const auto locked = weak.lock()) locked->teardown();
  });
  return true;
}

}  // namespace

bool add_events_exports(JSContext* context, JSValue input) {
  auto* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:input requires an input module binding");
    return false;
  }
  if (!ensure_events_state(context, binding)) {
    JS_ThrowInternalError(context, "rime:input host is not available");
    return false;
  }
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(value)) return false;
    // JS_SetPropertyStr consumes `value` on both success and failure; it
    // returns >0 on success, so only a negative result is an error.
    return JS_SetPropertyStr(context, input, name, value) >= 0;
  };
  return add("hotkey", events_hotkey, 2) && add("hotstring", events_hotstring, 3) &&
         add("hotIf", events_hot_if, 1) && add("hotIfWinActive", events_hot_if_win_active, 3) &&
         add("hotIfWinExist", events_hot_if_win_exist, 3) &&
         add("hotIfWinNotActive", events_hot_if_win_not_active, 3) &&
         add("hotIfWinNotExist", events_hot_if_win_not_exist, 3) &&
         add("installKeybdHook", events_install_keybd_hook, 2) &&
         add("installMouseHook", events_install_mouse_hook, 2) &&
         add("setTimer", events_set_timer, 3) && add("onMessage", events_on_message, 3) &&
         add("onClipboardChange", events_on_clipboard_change, 1) &&
         add("onError", events_on_error, 1) && add("onExit", events_on_exit, 1) &&
         add("createInputHook", events_create_input_hook, 3) &&
         add("suspend", input_suspend, 1) && add("policy", input_policy, 1);
}

// ---- teardown --------------------------------------------------------------

void EventsState::teardown() {
  if (closed) return;
  closed = true;
  JSContext* context = js_context;

  // 1. Shared dispatch: stop delivering new hook events first, then release
  //    its callback and the InputService subscription.
  if (dispatch_subscription != 0 && service) {
    (void)service->unsubscribe(dispatch_subscription);
    dispatch_subscription = 0;
  }
  if (dispatch_callback != 0) {
    (void)host->remove_callback(dispatch_callback);
    dispatch_callback = 0;
  }
  dispatch_users = 0;

  // 1b. InputHooks: end in-progress inputs WITHOUT invoking OnEnd (no
  //     script runs during teardown), settle every Wait() with "Stopped",
  //     release the active resources and free the owned On* values. The
  //     vector itself stays (ids remain resolvable - to a closed state).
  for (auto& hook : input_hooks) {
    if (hook.in_progress) {
      hook.in_progress = false;
      hook.end_reason = "Stopped";
      settle_hook_waiters(this, hook, "Stopped");
    }
    release_hook_active(this, hook);
    free_owned(context, hook.on_char);
    free_owned(context, hook.on_end);
    free_owned(context, hook.on_key_down);
    free_owned(context, hook.on_key_up);
  }
  if (hook_channel_callback != 0) {
    (void)host->remove_callback(hook_channel_callback);
    hook_channel_callback = 0;
  }

  // 2. Hotkeys and hotstrings (observer callbacks plus their subscription
  //    ids); the criteria are released after them in step 7.
  for (auto& hotkey : hotkeys) {
    if (!hotkey.via_action && hotkey.observer != 0) {
      (void)host->remove_callback(hotkey.observer);
      hotkey.observer = 0;
    }
    (void)host->subscriptions().remove(hotkey.sub);
  }
  hotkeys.clear();
  for (auto& hotstring : hotstrings) {
    if (!hotstring.via_action && hotstring.observer != 0) {
      (void)host->remove_callback(hotstring.observer);
      hotstring.observer = 0;
    }
    (void)host->subscriptions().remove(hotstring.sub);
  }
  hotstrings.clear();
  typed.clear();

  // 3. Timers: cancel the scheduler first so no tick can land mid-teardown.
  if (scheduler_timer != 0) {
    (void)host->timers().cancel(scheduler_timer);
    scheduler_timer = 0;
  }
  for (auto& timer : timers) {
    if (timer.callback != 0) (void)host->remove_callback(timer.callback);
    free_owned(context, timer.fn);
    (void)host->subscriptions().remove(timer.sub);
  }
  timers.clear();
  if (scheduler_callback != 0) {
    (void)host->remove_callback(scheduler_callback);
    scheduler_callback = 0;
  }

  // 4. onMessage: shared UI observer, then every monitor and the channel.
  if (ui_observer != 0 && window_service) {
    (void)window_service->ui().remove_message_observer(ui_observer);
  }
  ui_observer = 0;
  for (auto& monitor : monitors) {
    if (monitor.callback != 0) (void)host->remove_callback(monitor.callback);
    free_owned(context, monitor.fn);
    (void)host->subscriptions().remove(monitor.sub);
  }
  monitors.clear();
  if (message_channel_callback != 0) {
    (void)host->remove_callback(message_channel_callback);
    message_channel_callback = 0;
  }

  // 5. Clipboard: per-listener services, the shared relay and the OS
  //    AddClipboardFormatListener pairing.
  for (auto& listener : clipboard_listeners) {
    if (clipboard_service && listener.native_id != 0) {
      (void)clipboard_service->remove_change_listener(listener.native_id);
      listener.native_id = 0;
    }
    if (listener.callback != 0) (void)host->remove_callback(listener.callback);
    (void)host->subscriptions().remove(listener.sub);
  }
  clipboard_listeners.clear();
  if (window_service) {
    auto& ui = window_service->ui();
    if (clipboard_ui_observer != 0) (void)ui.remove_message_observer(clipboard_ui_observer);
    clipboard_ui_observer = 0;
    if (clipboard_os_listener) {
      const std::uintptr_t window = ui.message_window();
      if (window != 0) {
        (void)ui.call(
            [window] {
              (void)RemoveClipboardFormatListener(reinterpret_cast<HWND>(window));
            },
            std::chrono::seconds(1));
      }
      clipboard_os_listener = false;
    }
  }

  // 6. Error observers and exit handlers (the latter release their own
  //    callback through remove_exit_handler).
  for (auto& observer : error_observers) {
    host->remove_error_observer(observer.callback);
    if (observer.callback != 0) (void)host->remove_callback(observer.callback);
    (void)host->subscriptions().remove(observer.sub);
  }
  error_observers.clear();
  for (auto& observer : exit_observers) {
    (void)host->remove_exit_handler(observer.sub);
  }
  exit_observers.clear();

  // 7. HotIf criteria last: free the function references and stop the
  //    watcher thread (no UI-lane call can still be in flight afterwards).
  for (auto& criterion : criteria) free_owned(context, criterion.fn);
  criteria.clear();
  current_criterion = 0;
  if (watcher) {
    watcher->stop();
    watcher.reset();
  }
}

std::size_t EventsState::open_count() const {
  std::size_t in_progress_hooks = 0;
  for (const auto& hook : input_hooks) {
    if (hook.in_progress) in_progress_hooks += 1;
  }
  return hotkeys.size() + hotstrings.size() + timers.size() + monitors.size() +
         clipboard_listeners.size() + error_observers.size() + exit_observers.size() +
         in_progress_hooks;
}

}  // namespace rime::win32
