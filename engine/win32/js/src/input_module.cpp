#include "rime/win32/js_input.hpp"

#include "async_task.hpp"
#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "quickjs.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

rime::js::Host* host_of(JSContext* context) {
  return static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
}

InputModuleBinding* binding_of(JSContext* context) {
  auto* host = host_of(context);
  if (!host) return nullptr;
  return static_cast<InputModuleBinding*>(host->module_data("rime:input"));
}

// Strict JS number -> int64: non-numbers, NaN/Infinity, fractions and
// out-of-range values raise a TypeError instead of truncating.
bool strict_int64(JSContext* context, JSValueConst value, std::int64_t& out, const char* what) {
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

std::string event_json(const InputEvent& event) {
  json::Value value = json::Value::object();
  value.set("sequence", json::Value::number(static_cast<double>(event.sequence)));
  value.set("timestamp", json::Value::number(static_cast<double>(event.timestamp_ms)));
  value.set("injected", json::Value::boolean(event.injected));
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
// policy the kernel enforces denies it here (bind/subscribe return
// synchronously, so the denial is a thrown Error naming the capability).
JSValue throw_capability_error(JSContext* context, const char* capability) {
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

std::string ascii_lower(std::string_view text) {
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
bool chord_key(const std::string& token, std::uint32_t& vk, std::string& error) {
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
bool parse_chord(const std::string& text, std::uint32_t& vk, std::uint8_t& mask,
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
JSValue event_field(JSContext* context, JSValueConst event, const char* name) {
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
bool modifiers_match(JSContext* context, JSValueConst event, const std::uint8_t mask) {
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
// Injected input matches too - no action can inject input today, so a
// binding cannot be retriggered by its own dispatch; an input-injection
// executor must add send-level suppression before that assumption weakens.
bool matches_chord(JSContext* context, JSValueConst event, const ChordBinding& chord) {
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

JSValue input_subscribe(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!host || !binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1 || !JS_IsFunction(context, argv[0])) {
    return JS_ThrowTypeError(context, "subscribe(handler)");
  }
  // Installing a global hook is a privileged operation; the same policy the
  // kernel enforces decides here (subscribe returns synchronously, so the
  // denial is a thrown Error).
  if (!binding->kernel->allows("windows.hook.global")) {
    return throw_capability_error(context, "windows.hook.global");
  }

  const auto queue = host->event_queue();
  auto callback_id = std::make_shared<std::atomic<std::uint64_t>>(0);
  // TODO: stringify once per event and fan out to every subscriber instead of
  // running json::stringify on the hook thread once per subscription; kept
  // per-subscriber for now to avoid restructuring the delivery chain.
  const std::uint64_t subscription_id = binding->service->subscribe(
      [queue, callback_id](const InputEvent& event) {
        const std::uint64_t id = callback_id->load(std::memory_order_acquire);
        // queue->push is bounded and can drop under pressure: the window
        // between the hook thread and the JS thread is expected loss, visible
        // via dropped_events(), not an error.
        if (id != 0) (void)queue->push(id, event_json(event));
      });
  if (subscription_id == 0) {
    return JS_ThrowInternalError(context, "input service is not running");
  }

  std::uint64_t host_callback_id = 0;
  if (const auto error = host->add_callback(JS_DupValue(context, argv[0]), host_callback_id);
      !error.ok()) {
    (void)binding->service->unsubscribe(subscription_id);
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  callback_id->store(host_callback_id, std::memory_order_release);
  binding->callbacks[subscription_id] = host_callback_id;
  return JS_NewInt64(context, static_cast<std::int64_t>(subscription_id));
}

JSValue input_unsubscribe(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!host || !binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "unsubscribe(subscriptionId)");
  std::int64_t raw_id = 0;
  if (!strict_int64(context, argv[0], raw_id, "unsubscribe(subscriptionId)")) return JS_EXCEPTION;
  if (raw_id <= 0) return JS_NewBool(context, 0);
  const std::uint64_t subscription_id = static_cast<std::uint64_t>(raw_id);

  bool removed = binding->service->unsubscribe(subscription_id);
  const auto found = binding->callbacks.find(subscription_id);
  if (found != binding->callbacks.end()) {
    (void)host->remove_callback(found->second);
    binding->callbacks.erase(found);
    removed = true;
  }
  return JS_NewBool(context, removed ? 1 : 0);
}

// Reads one required non-empty string field from the bind template. Missing
// or ill-typed values throw a TypeError naming the field (`path` is the
// dotted label used in messages) so a malformed template can never reach the
// queue.
bool required_string(JSContext* context, JSValueConst object, const char* property,
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

// Host event-queue entry for one chord binding: the drain loop invokes it on
// the JS thread with the parsed InputEvent object. A matching key-down builds
// a fresh Action from the template and submits it to the shared queue; a 0ms
// timer task then pumps the queue exactly like a promise-backed call.
JSValue chord_dispatch(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void* opaque) {
  auto* chord = static_cast<ChordBinding*>(opaque);
  if (!chord || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
  auto* binding = binding_of(context);
  if (!binding || !binding->dispatcher || !binding->next_action_id) return JS_UNDEFINED;
  if (!matches_chord(context, argv[0], *chord)) return JS_UNDEFINED;

  rime::action::Action action =
      make_action(*binding->next_action_id, "rime:input", chord->type, chord->capability,
                  {chord->target_kind, chord->target_id}, chord->payload);
  // Chord dispatch is not a direct JS call: report the true origin so trace
  // and inspect can tell bindings apart from module methods.
  action.source = {"chord", "rime:input"};

  const auto status = binding->dispatcher->submit(std::move(action));
  if (status != rime::action::DispatchStatus::Accepted &&
      status != rime::action::DispatchStatus::Coalesced) {
    // Closed/queue-full refusals are already traced by the dispatcher, and a
    // chord binding owns no promise to settle.
    return JS_UNDEFINED;
  }
  auto* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  rime::action::Dispatcher* dispatcher_ptr = binding->dispatcher;
  // No completion token exists for a chord dispatch: schedule_task's
  // delay-timer bookkeeping would leak a token no completion ever erases,
  // stalling idle()/settle. The raw timer runs the same shared pump the
  // promise path uses; with no promise to settle, a pump failure lands in
  // the host error log instead.
  host->timers().schedule(std::chrono::milliseconds(0), [host, dispatcher_ptr] {
    try {
      run_queue_pump(host, *dispatcher_ptr);
    } catch (const std::exception& exception) {
      (void)host->record("rime:input.chord", exception.what());
    } catch (...) {
      (void)host->record("rime:input.chord", "queue pump failed");
    }
  });
  return JS_UNDEFINED;
}

JSValue input_bind(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                   void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!host || !binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsObject(argv[1])) {
    return JS_ThrowTypeError(context, "bind(chord, action)");
  }
  if (!binding->kernel->allows("windows.hook.global")) {
    return throw_capability_error(context, "windows.hook.global");
  }

  const char* chord_text = JS_ToCString(context, argv[0]);
  if (!chord_text) return JS_EXCEPTION;
  const std::string chord_name(chord_text);
  JS_FreeCString(context, chord_text);

  auto chord = std::make_unique<ChordBinding>();
  std::string chord_error;
  if (!parse_chord(chord_name, chord->vk, chord->mask, chord_error)) {
    return JS_ThrowTypeError(context, "bind(chord, action): %s", chord_error.c_str());
  }

  // The template is validated once so a matching key-down can only ever
  // build a contract-shaped action (the kernel re-checks shape anyway).
  if (!required_string(context, argv[1], "type", "action.type", chord->type) ||
      !required_string(context, argv[1], "capability", "action.capability", chord->capability)) {
    return JS_EXCEPTION;
  }
  JSValue target = JS_GetPropertyStr(context, argv[1], "target");
  if (JS_IsException(target)) return JS_EXCEPTION;
  bool target_ok = JS_IsObject(target);
  if (target_ok) {
    target_ok = required_string(context, target, "kind", "action.target.kind",
                                chord->target_kind) &&
                required_string(context, target, "id", "action.target.id", chord->target_id);
  } else {
    JS_ThrowTypeError(context, "action.target must be an object with kind and id");
  }
  JS_FreeValue(context, target);
  if (!target_ok) return JS_EXCEPTION;

  // payload is stringified once at bind time so the trigger path is just a
  // copy; JSON.stringify semantics mean circular values throw now, not on
  // some later key-down.
  JSValue payload = JS_GetPropertyStr(context, argv[1], "payload");
  if (JS_IsException(payload)) return JS_EXCEPTION;
  if (JS_IsUndefined(payload) || JS_IsNull(payload)) {
    JS_FreeValue(context, payload);
    chord->payload = "{}";
  } else if (!JS_IsObject(payload)) {
    JS_FreeValue(context, payload);
    return JS_ThrowTypeError(context, "bind(chord, action): action.payload must be an object");
  } else {
    JSValue global = JS_GetGlobalObject(context);
    JSValue json = JS_GetPropertyStr(context, global, "JSON");
    JS_FreeValue(context, global);
    if (JS_IsException(json)) {
      JS_FreeValue(context, payload);
      return JS_EXCEPTION;
    }
    JSValue stringify = JS_GetPropertyStr(context, json, "stringify");
    if (JS_IsException(stringify)) {
      JS_FreeValue(context, json);
      JS_FreeValue(context, payload);
      return JS_EXCEPTION;
    }
    JSValue text = JS_Call(context, stringify, json, 1, &payload);
    JS_FreeValue(context, stringify);
    JS_FreeValue(context, json);
    if (JS_IsException(text)) {
      JS_FreeValue(context, payload);
      return JS_EXCEPTION;
    }
    const bool is_string = JS_IsString(text);
    const char* utf8 = is_string ? JS_ToCString(context, text) : nullptr;
    JS_FreeValue(context, text);
    if (!is_string || !utf8) {
      JS_FreeValue(context, payload);
      if (!is_string) {
        return JS_ThrowTypeError(
            context, "bind(chord, action): action.payload must serialize to a JSON object");
      }
      return JS_EXCEPTION;
    }
    // The action contract requires a JSON object: a payload whose toJSON
    // collapsed it to a primitive is rejected at bind, not at execution.
    const bool object_text = utf8[0] == '{';
    if (object_text) chord->payload.assign(utf8);
    JS_FreeCString(context, utf8);
    JS_FreeValue(context, payload);
    if (!object_text) {
      return JS_ThrowTypeError(
          context, "bind(chord, action): action.payload must serialize to a JSON object");
    }
  }

  // The closure borrows `chord`; the registration below owns it, so the
  // erase order in unbind can never race a finalizer.
  JSValue closure =
      JS_NewCClosure(context, chord_dispatch, "inputChord", nullptr, 1, 0, chord.get());
  if (JS_IsException(closure)) return JS_EXCEPTION;
  std::uint64_t host_callback_id = 0;
  if (const auto error = host->add_callback(closure, host_callback_id); !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  const auto queue = host->event_queue();
  const std::uint64_t subscription_id = binding->service->subscribe(
      [queue, host_callback_id](const InputEvent& event) {
        (void)queue->push(host_callback_id, event_json(event));
      });
  if (subscription_id == 0) {
    (void)host->remove_callback(host_callback_id);
    return JS_ThrowInternalError(context, "input service is not running");
  }
  const auto chord_id = binding->next_chord_id++;
  binding->chords.emplace(
      chord_id,
      InputModuleBinding::ChordRegistration{subscription_id, host_callback_id, std::move(chord)});
  return JS_NewInt64(context, static_cast<std::int64_t>(chord_id));
}

JSValue input_unbind(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!host || !binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "unbind(bindingId)");
  std::int64_t raw_id = 0;
  if (!strict_int64(context, argv[0], raw_id, "unbind(bindingId)")) return JS_EXCEPTION;
  if (raw_id <= 0) return JS_NewBool(context, 0);
  const auto found = binding->chords.find(static_cast<std::uint64_t>(raw_id));
  if (found == binding->chords.end()) return JS_NewBool(context, 0);
  // Close the delivery chain first so no queued event can reach a callback
  // whose registration - and owned context - is about to be erased.
  (void)binding->service->unsubscribe(found->second.subscription_id);
  (void)host->remove_callback(found->second.host_callback_id);
  binding->chords.erase(found);
  return JS_NewBool(context, 1);
}

int input_module_init(JSContext* context, JSModuleDef* module) {
  auto* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:input requires an input module binding");
    return -1;
  }
  JSValue input = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(value)) {
      JS_FreeValue(context, input);
      return false;
    }
    // JS_SetPropertyStr consumes `value` on both success and failure.
    if (JS_SetPropertyStr(context, input, name, value) < 0) {
      JS_FreeValue(context, input);
      return false;
    }
    return true;
  };
  if (!add("subscribe", input_subscribe, 1) || !add("unsubscribe", input_unsubscribe, 1) ||
      !add("bind", input_bind, 2) || !add("unbind", input_unbind, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "input", input);
}

JSModuleDef* create_input_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:input", input_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "input") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const InputModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:input requires an input service, kernel and dispatcher"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_input_module(rime::js::Host& host, InputModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:input", binding);
  host.modules().add_native("rime:input",
                            [](JSContext* context) { return create_input_module(context); });
  return rime::core::Error::none();
}

rime::core::Error register_input_module(rime::js::Runtime& runtime,
                                        InputModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:input", [](JSContext* context) { return create_input_module(context); }, binding);
}

}  // namespace rime::win32
