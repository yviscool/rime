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
#include <chrono>
#include <cstdint>
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
// kinds are invoked here with { active, seq }. Any failure fails closed.
bool criterion_met(JSContext* context, EventsState* state, std::uint64_t id,
                   const std::shared_ptr<const ContextSnapshot>& snapshot) {
  if (id == 0) return true;
  const EventsState::Criterion* criterion = find_criterion(state, id);
  if (!criterion) return false;
  if (criterion->spec.kind != CriterionKind::Function) {
    if (!snapshot) return false;
    for (const auto& entry : snapshot->results) {
      if (entry.first == id) return entry.second;
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

void fire_hotkey(JSContext* context, EventsState* state, const EventsState::Hotkey& hotkey) {
  if (hotkey.via_action) {
    (void)submit_event_action(context, state, hotkey.action, "hotkey");
    return;
  }
  if (hotkey.observer == 0) return;
  json::Value payload = json::Value::object();
  payload.set("name", json::Value::string(hotkey.name));
  const std::string text = json::stringify(payload);
  if (const auto error = state->host->invoke_callback(hotkey.observer, text); !error.ok()) {
    record_failure(state->host, "rime:input.hotkey", error);
  }
}

// First-match-wins: registrations are scanned in order, a failing HotIf
// criterion falls through to the next candidate. Rows are snapshotted first
// so a HotIf function can add/remove hotkeys mid-evaluation.
bool hotkey_try_match(JSContext* context, EventsState* state, JSValueConst event) {
  struct Row {
    std::uint64_t sub;
    std::uint32_t vk;
    std::uint8_t mask;
    std::uint64_t criterion;
  };
  std::vector<Row> rows;
  rows.reserve(state->hotkeys.size());
  for (const auto& hotkey : state->hotkeys) {
    if (hotkey.enabled) rows.push_back({hotkey.sub, hotkey.vk, hotkey.mask, hotkey.criterion});
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
  state->scheduler_timer = host->timers().schedule(
      std::chrono::milliseconds(delay), [queue, channel, pending] {
        // Coalescing: while a tick is queued or running, later firings do
        // not pile up - the JS thread re-arms after it catches up.
        if (!pending->exchange(true, std::memory_order_acq_rel)) {
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
    // AHK's instance cap: while max_instances deliveries are in flight the
    // next message for this monitor is dropped, not queued.
    if (monitor->running >= monitor->max_instances) continue;
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
    if (argc >= 3) {
      bool have = false;
      bool enabled = existing->enabled;
      if (!parse_onoff_options(context, argv[2], have, enabled)) return JS_EXCEPTION;
      if (have) existing->enabled = enabled;
    }
    return make_subscription(context, binding, "hotkey", existing->sub);
  }

  const bool is_function = JS_IsFunction(context, argv[1]);
  if (!is_function && !JS_IsObject(argv[1])) {
    return JS_ThrowTypeError(context,
                             "hotkey(name, action, options?): action must be a function, an "
                             "action template, or 'on'|'off'|'toggle'");
  }
  bool have_options = false;
  bool options_enabled = true;
  if (argc >= 3 && !parse_onoff_options(context, argv[2], have_options, options_enabled)) {
    return JS_EXCEPTION;
  }

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

  // Settings forms above are plain state; registering a hotstring is what
  // installs a global hook stream, so only now is the capability required.
  if (!binding->kernel->allows(kHookCapability)) {
    return throw_capability_error(context, kHookCapability);
  }

  int third_control = -1;
  if (argc >= 3) {
    third_control = parse_ctrl_word(context, argv[2], "hotstring(spec, action, onOff)");
    if (third_control < 0 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) return JS_EXCEPTION;
  }
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
         add("onError", events_on_error, 1) && add("onExit", events_on_exit, 1);
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
  return hotkeys.size() + hotstrings.size() + timers.size() + monitors.size() +
         clipboard_listeners.size() + error_observers.size() + exit_observers.size();
}

}  // namespace rime::win32
