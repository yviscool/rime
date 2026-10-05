#include "rime/win32/js_input.hpp"

#include "async_task.hpp"
#include "events_module.hpp"
#include "input_util.hpp"
#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/window.hpp"

#include "quickjs.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

// Shared helpers (host_of, binding_of, strict_int64, event_json, chord
// parsing, required_string, ...) live in input_util.hpp so the M2-C exports
// in events_module.cpp use the exact same validation and match rules.

// Reads one send step with TypeError on anything but an integer vk in
// 1..254 (0..65535 when unicode marks a UTF-16 code unit) plus a boolean
// down; the executor re-validates the payload as the contract enforcer, this
// only gives JS callers precise call-site errors. The optional `unicode`
// field must be a boolean when present.
bool parse_send_step(JSContext* context, JSValueConst step, SendKeyEvent& out) {
  JSValue unicode_value = JS_GetPropertyStr(context, step, "unicode");
  if (JS_IsException(unicode_value)) return false;
  bool is_unicode = false;
  if (!JS_IsUndefined(unicode_value)) {
    if (!JS_IsBool(unicode_value)) {
      JS_FreeValue(context, unicode_value);
      JS_ThrowTypeError(context, "send(steps): step.unicode must be a boolean");
      return false;
    }
    is_unicode = JS_ToBool(context, unicode_value) > 0;
  }
  JS_FreeValue(context, unicode_value);
  JSValue vk_value = JS_GetPropertyStr(context, step, "vk");
  if (JS_IsException(vk_value)) return false;
  double vk_number = 0;
  const bool vk_ok =
      JS_IsNumber(vk_value) && JS_ToFloat64(context, &vk_number, vk_value) == 0 &&
      std::isfinite(vk_number) && std::trunc(vk_number) == vk_number &&
      (is_unicode ? (vk_number >= 0.0 && vk_number <= 65535.0)
                  : (vk_number >= 1.0 && vk_number <= 254.0));
  JS_FreeValue(context, vk_value);
  if (!vk_ok) {
    JS_ThrowTypeError(context,
                      is_unicode ? "send(steps): step.unicode vk must be an integer in 0..65535"
                                 : "send(steps): step.vk must be an integer in 1..254");
    return false;
  }
  JSValue down_value = JS_GetPropertyStr(context, step, "down");
  if (JS_IsException(down_value)) return false;
  if (!JS_IsBool(down_value)) {
    JS_FreeValue(context, down_value);
    JS_ThrowTypeError(context, "send(steps): step.down must be a boolean");
    return false;
  }
  out.vk = static_cast<std::uint32_t>(vk_number);
  out.down = JS_ToBool(context, down_value) > 0;
  out.unicode = is_unicode;
  JS_FreeValue(context, down_value);
  return true;
}

// Reads one mouse step into its payload JSON: action must be move/relmove
// (int32 x and y) or down/up (button 1..3). TypeError on anything else; the
// executor re-validates as the contract enforcer.
bool parse_mouse_step(JSContext* context, JSValueConst step, json::Value& entry) {
  JSValue action_value = JS_GetPropertyStr(context, step, "action");
  if (JS_IsException(action_value)) return false;
  if (!JS_IsString(action_value)) {
    JS_FreeValue(context, action_value);
    JS_ThrowTypeError(context, "mouse(payload): every step needs a string action");
    return false;
  }
  const char* action_text = JS_ToCString(context, action_value);
  JS_FreeValue(context, action_value);
  if (!action_text) return false;
  const std::string action(action_text);
  JS_FreeCString(context, action_text);
  entry = json::Value::object();
  entry.set("action", json::Value::string(action));
  if (action == "move" || action == "relmove") {
    for (const char* field : {"x", "y"}) {
      JSValue value = JS_GetPropertyStr(context, step, field);
      if (JS_IsException(value)) return false;
      double number = 0;
      const bool ok = JS_IsNumber(value) && JS_ToFloat64(context, &number, value) == 0 &&
                      std::isfinite(number) && std::trunc(number) == number &&
                      number >= -2147483648.0 && number <= 2147483647.0;
      JS_FreeValue(context, value);
      if (!ok) {
        JS_ThrowTypeError(context, "mouse(payload): move steps need int32 integer x and y");
        return false;
      }
      entry.set(field, json::Value::number(number));
    }
    return true;
  }
  if (action == "down" || action == "up") {
    JSValue button_value = JS_GetPropertyStr(context, step, "button");
    if (JS_IsException(button_value)) return false;
    double button = 0;
    const bool ok = JS_IsNumber(button_value) && JS_ToFloat64(context, &button, button_value) == 0 &&
                    std::isfinite(button) && std::trunc(button) == button && button >= 1.0 &&
                    button <= 3.0;
    JS_FreeValue(context, button_value);
    if (!ok) {
      JS_ThrowTypeError(context,
                        "mouse(payload): button steps need button 1 (left), 2 (right) or 3 (middle)");
      return false;
    }
    entry.set("button", json::Value::number(button));
    return true;
  }
  JS_ThrowTypeError(context,
                    "mouse(payload): step.action must be move, relmove, down or up");
  return false;
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
  // stalling idle()/settle. The worker lane runs the same shared pump the
  // promise path uses; with no promise to settle, a pump failure lands in
  // the host error log instead.
  const bool posted = host->post_worker([host, dispatcher_ptr] {
    try {
      run_queue_pump(host, *dispatcher_ptr);
    } catch (const std::exception& exception) {
      (void)host->record("rime:input.chord", exception.what());
    } catch (...) {
      (void)host->record("rime:input.chord", "queue pump failed");
    }
  });
  if (!posted) {
    (void)host->record("rime:input.chord", "worker service is stopping");
  }
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
  // build a contract-shaped action (the kernel re-checks shape anyway);
  // read_action_template is the shared validation used by the M2-C exports.
  EventAction parsed;
  if (!read_action_template(context, argv[1], parsed, "action")) return JS_EXCEPTION;
  chord->type = std::move(parsed.type);
  chord->capability = std::move(parsed.capability);
  chord->target_kind = std::move(parsed.target_kind);
  chord->target_id = std::move(parsed.target_id);
  chord->payload = std::move(parsed.payload);

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

// Queues the `input.send` action: steps validate up front (TypeError for
// malformed input, Error naming windows.input.inject when the capability is
// missing), the steps become the action payload, and the shared queue
// settles the returned promise with {sent: n}.
JSValue input_send(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                   void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1 || argc > 2 || !JS_IsArray(argv[0])) {
    return JS_ThrowTypeError(context, "send(steps[, options])");
  }
  if (!binding->kernel->allows("windows.input.inject")) {
    return throw_capability_error(context, "windows.input.inject");
  }
  JSValue steps = argv[0];
  JSValue length_value = JS_GetPropertyStr(context, steps, "length");
  if (JS_IsException(length_value)) return JS_EXCEPTION;
  std::int64_t length = 0;
  const bool length_ok = JS_ToInt64(context, &length, length_value) == 0;
  JS_FreeValue(context, length_value);
  if (!length_ok) return JS_EXCEPTION;
  if (length <= 0) {
    return JS_ThrowTypeError(context, "send(steps): steps must not be empty");
  }
  json::Value payload = json::Value::array();
  for (std::int64_t index = 0; index < length; ++index) {
    JSValue step = JS_GetPropertyInt64(context, steps, index);
    if (JS_IsException(step)) return JS_EXCEPTION;
    if (!JS_IsObject(step)) {
      JS_FreeValue(context, step);
      return JS_ThrowTypeError(context, "send(steps): every step must be an object");
    }
    SendKeyEvent key;
    const bool step_ok = parse_send_step(context, step, key);
    JS_FreeValue(context, step);
    if (!step_ok) return JS_EXCEPTION;
    json::Value entry = json::Value::object();
    entry.set("vk", json::Value::number(static_cast<double>(key.vk)));
    entry.set("down", json::Value::boolean(key.down));
    // Omitted when false so non-unicode payloads stay byte-identical with
    // earlier revisions; the executor treats a missing field as false.
    if (key.unicode) entry.set("unicode", json::Value::boolean(true));
    payload.push(std::move(entry));
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  auto action = make_action(*binding->next_action_id, "rime:input", "input.send",
                            "windows.input.inject", {"input", "keyboard"},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// Per-side modifier + CapsLock snapshot behind input.modifiers(). Reads are
// synchronous (keyboard.send needs the pre-state before it compiles the
// batch) and gated behind windows.input.inject like every other input read:
// the denial is a thrown Error naming the capability.
JSValue input_modifiers(JSContext* context, JSValueConst, int, JSValueConst*, int, void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (!binding->kernel->allows("windows.input.inject")) {
    return throw_capability_error(context, "windows.input.inject");
  }
  const ModifierState state = read_modifier_state();
  JSValue result = JS_NewObject(context);
  if (JS_IsException(result)) return JS_EXCEPTION;
  const struct {
    const char* name;
    bool value;
  } fields[] = {
      {"lcontrol", state.lcontrol}, {"rcontrol", state.rcontrol}, {"lshift", state.lshift},
      {"rshift", state.rshift},     {"lalt", state.lalt},         {"ralt", state.ralt},
      {"lwin", state.lwin},         {"rwin", state.rwin},         {"capsLock", state.caps_lock},
  };
  for (const auto& field : fields) {
    if (JS_SetPropertyStr(context, result, field.name, JS_NewBool(context, field.value)) < 0) {
      JS_FreeValue(context, result);
      return JS_EXCEPTION;
    }
  }
  return result;
}

// Queues the `input.mouse` action: the payload ({steps, speed?}) validates up
// front (TypeError for malformed input, Error naming windows.input.inject
// when the capability is missing), the steps become the action payload, and
// the shared queue settles the returned promise with {sent: n}.
JSValue input_mouse(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                    void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1 || argc > 2 || !JS_IsObject(argv[0]) || JS_IsArray(argv[0])) {
    return JS_ThrowTypeError(context, "mouse(payload[, options])");
  }
  if (!binding->kernel->allows("windows.input.inject")) {
    return throw_capability_error(context, "windows.input.inject");
  }
  JSValue payload_object = argv[0];
  JSValue steps = JS_GetPropertyStr(context, payload_object, "steps");
  if (JS_IsException(steps)) return JS_EXCEPTION;
  if (!JS_IsArray(steps)) {
    JS_FreeValue(context, steps);
    return JS_ThrowTypeError(context, "mouse(payload): payload.steps must be an array");
  }
  json::Value payload = json::Value::object();
  JSValue speed_value = JS_GetPropertyStr(context, payload_object, "speed");
  if (JS_IsException(speed_value)) {
    JS_FreeValue(context, steps);
    return JS_EXCEPTION;
  }
  if (!JS_IsUndefined(speed_value)) {
    double speed = 0;
    const bool speed_ok =
        JS_IsNumber(speed_value) && JS_ToFloat64(context, &speed, speed_value) == 0 &&
        std::isfinite(speed) && std::trunc(speed) == speed && speed >= 0.0 && speed <= 100.0;
    if (!speed_ok) {
      JS_FreeValue(context, speed_value);
      JS_FreeValue(context, steps);
      return JS_ThrowTypeError(context, "mouse(payload): speed must be an integer in 0..100");
    }
    payload.set("speed", json::Value::number(speed));
  }
  JS_FreeValue(context, speed_value);
  JSValue length_value = JS_GetPropertyStr(context, steps, "length");
  if (JS_IsException(length_value)) {
    JS_FreeValue(context, steps);
    return JS_EXCEPTION;
  }
  std::int64_t length = 0;
  const bool length_ok = JS_ToInt64(context, &length, length_value) == 0;
  JS_FreeValue(context, length_value);
  if (!length_ok) {
    JS_FreeValue(context, steps);
    return JS_EXCEPTION;
  }
  if (length <= 0) {
    JS_FreeValue(context, steps);
    return JS_ThrowTypeError(context, "mouse(payload): steps must not be empty");
  }
  json::Value step_array = json::Value::array();
  for (std::int64_t index = 0; index < length; ++index) {
    JSValue step = JS_GetPropertyInt64(context, steps, index);
    if (JS_IsException(step)) {
      JS_FreeValue(context, steps);
      return JS_EXCEPTION;
    }
    if (!JS_IsObject(step)) {
      JS_FreeValue(context, step);
      JS_FreeValue(context, steps);
      return JS_ThrowTypeError(context, "mouse(payload): every step must be an object");
    }
    json::Value entry = json::Value::object();
    const bool step_ok = parse_mouse_step(context, step, entry);
    JS_FreeValue(context, step);
    if (!step_ok) {
      JS_FreeValue(context, steps);
      return JS_EXCEPTION;
    }
    step_array.push(std::move(entry));
  }
  JS_FreeValue(context, steps);
  payload.set("steps", std::move(step_array));
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  auto action = make_action(*binding->next_action_id, "rime:input", "input.mouse",
                            "windows.input.inject", {"input", "mouse"},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// Cursor plus the window/control under it behind input.mouseGetPos(). The
// capability check runs inside the async body like every window read: a
// missing windows.input.read rejects with code capability_denied instead of
// throwing synchronously.
JSValue input_mouse_get_pos(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel || !binding->window_service) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "mouseGetPos([options])");
  ActionOptions options;
  if (argc == 1 && !parse_action_options(context, argv[0], options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  WindowService* window_service = binding->window_service;
  const auto timeout = std::chrono::milliseconds(options.deadline_ms);
  return start_async(
      context,
      [kernel, window_service, timeout]() -> AsyncOutcome {
        // Ownership: kernel/window_service (raw) outlive the host; the task
        // always settles its promise, so no outcome is dropped.
        if (!kernel->allows("windows.input.read")) {
          return async_failure("capability_denied",
                               "required capability was not granted: windows.input.read");
        }
        POINT point{};
        if (!GetCursorPos(&point)) {
          return async_failure("execution_failed", "GetCursorPos failed");
        }
        WindowAtInfo info;
        if (const auto error =
                window_service->window_at(point.x, point.y, info, timeout);
            !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("x", json::Value::number(static_cast<double>(point.x)));
        result.set("y", json::Value::number(static_cast<double>(point.y)));
        result.set("window",
                   info.window ? window_info_json(*info.window) : json::Value::null());
        json::Value control = json::Value::null();
        if (info.control) {
          control = json::Value::object();
          control.set("id", json::Value::number(static_cast<double>(info.control->id)));
          control.set("className", json::Value::string(info.control->class_name));
          control.set("classNN", json::Value::string(info.control->class_nn));
        }
        result.set("control", std::move(control));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

// Resolves one scNNN scan-code value to a virtual key. The plain scan-code
// range goes through MapVirtualKeyW, the 0x100 form carries AHK's extended
// bit (0x14D = Right, the mirror of getKeySC's 0xE04D), the 0xE000 form is
// getKeySC's own output fed back in, and the three fixed AHK specials
// (SC_PAUSE 0x045, SC_NUMLOCK 0x145, SC_RSHIFT 0x136 - keyboard_mouse.h)
// keep their documented meaning ahead of the range rules. Values Windows
// cannot map (0, gaps, 0x200..0xDFFF) fail instead of guessing a key.
bool sc_state_key(const std::uint32_t scan, std::uint32_t& vk) {
  const auto map = [&vk](const std::uint32_t code) {
    const UINT mapped = MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX);
    if (mapped == 0) return false;
    vk = static_cast<std::uint32_t>(mapped);
    return true;
  };
  if (scan == 0x136u) return map(0x36u);  // SC_RSHIFT -> right shift
  if (scan == 0x145u) return map(0x45u);  // SC_NUMLOCK -> numlock
  if (scan == 0x045u) {                   // SC_PAUSE -> pause
    vk = VK_PAUSE;
    return true;
  }
  if (scan >= 1u && scan <= 0xFFu) return map(scan);
  if (scan >= 0x100u && scan <= 0x1FFu) return map(0xE000u | (scan & 0xFFu));
  if (scan >= 0xE000u && scan <= 0xE0FFu) return map(scan);
  return false;
}

// Resolves a state-read key name (getKeyState/keyWait/keyHistory spellings)
// to a virtual key: chord tokens (letters, digits, f1..f24, navigation and
// lock names), modifier names, the mouse buttons, AHK's explicit vkXX hex
// form (TextToVK's aAllowExplicitVK spelling) and the scNNN scan-code form
// (this repo's extension of the shared grammar, in the spirit of AHK's
// TextToVKandSC, so getKeyState/keyWait/getKeyVK/getKeyName accept the
// names getKeySC hands back). Lowercased first (AHK key names are
// case-insensitive); unknown names fail with an error string instead of
// guessing a key.
bool state_key(const std::string& token, std::uint32_t& vk, std::string& error) {
  if (token.empty()) {
    error = "key name must not be empty";
    return false;
  }
  const std::string key = ascii_lower(token);
  if (chord_key(key, vk, error)) return true;
  if (key.size() >= 2 && key[0] == 'v' && key[1] == 'k') {
    // Explicit vk form:1..2 hex digits, value1..254 (same range send()
    // accepts for non-unicode steps).
    if (key.size() >= 3 && key.size() <= 4) {
      std::uint32_t value = 0;
      bool hex = true;
      for (std::size_t index = 2; index < key.size(); ++index) {
        const char c = key[index];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
          digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
          digit = static_cast<std::uint32_t>(c - 'a' + 10);
        } else {
          hex = false;
          break;
        }
        value = value * 16u + digit;
      }
      if (hex && value >= 1 && value <= 0xFE) {
        vk = value;
        return true;
      }
    }
    error = "invalid vk key name: " + token;
    return false;
  }
  if (key.size() >= 2 && key[0] == 's' && key[1] == 'c') {
    // Explicit sc form: "sc" plus 1..4 hex digits, all of them hex - a
    // suffix that is not fully hexadecimal fails the whole name (AHK's
    // TextToSC disallows any invalid suffix rather than parsing a prefix).
    std::uint32_t value = 0;
    bool hex = key.size() >= 3 && key.size() <= 6;
    if (hex) {
      for (std::size_t index = 2; index < key.size(); ++index) {
        const char c = key[index];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
          digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
          digit = static_cast<std::uint32_t>(c - 'a' + 10);
        } else {
          hex = false;
          break;
        }
        value = value * 16u + digit;
      }
    }
    if (hex && sc_state_key(value, vk)) return true;
    error = "invalid sc key name: " + token;
    return false;
  }
  struct StateKey {
    const char* name;
    std::uint32_t vk;
  };
  static constexpr StateKey kStateKeys[] = {
      {"ctrl", VK_CONTROL},    {"control", VK_CONTROL}, {"lctrl", VK_LCONTROL},
      {"lcontrol", VK_LCONTROL}, {"rctrl", VK_RCONTROL}, {"rcontrol", VK_RCONTROL},
      {"shift", VK_SHIFT},     {"lshift", VK_LSHIFT},   {"rshift", VK_RSHIFT},
      {"alt", VK_MENU},        {"lalt", VK_LMENU},      {"ralt", VK_RMENU},
      {"lwin", VK_LWIN},       {"rwin", VK_RWIN},
      {"lbutton", VK_LBUTTON}, {"rbutton", VK_RBUTTON}, {"mbutton", VK_MBUTTON},
      {"xbutton1", VK_XBUTTON1}, {"xbutton2", VK_XBUTTON2},
  };
  for (const auto& named : kStateKeys) {
    if (key == named.name) {
      vk = named.vk;
      return true;
    }
  }
  // chord_key's own message says "chord token"; a state read reports the
  // name in its own terms (its range hint for f99-style mistakes survives).
  if (error.rfind("unknown chord token", 0) == 0) error = "unknown key name: " + token;
  return false;
}

// Canonical lowercase token for one virtual key: exactly one spelling per
// VK out of the names chord_key and the modifier/mouse table accept, so
// aliases (control, return, esc, ins, del, pgup, ...) collapse onto it and
// the result reads back through state_key. A VK no token names returns ""
// - AHK falls back to vkNN and to the unshifted character there, two
// spellings this repo never accepts back, so getKeyName never emits a name
// getKeyState would reject.
std::string key_name_for_vk(const std::uint32_t vk) {
  struct NamedVK {
    std::uint32_t vk;
    const char* name;
  };
  static constexpr NamedVK kNamedVKs[] = {
      {VK_LBUTTON, "lbutton"},    {VK_RBUTTON, "rbutton"},   {VK_MBUTTON, "mbutton"},
      {VK_XBUTTON1, "xbutton1"},  {VK_XBUTTON2, "xbutton2"},
      {VK_BACK, "backspace"},     {VK_TAB, "tab"},           {VK_RETURN, "enter"},
      {VK_ESCAPE, "escape"},      {VK_SPACE, "space"},       {VK_PRIOR, "pageup"},
      {VK_NEXT, "pagedown"},      {VK_HOME, "home"},         {VK_END, "end"},
      {VK_LEFT, "left"},          {VK_UP, "up"},             {VK_RIGHT, "right"},
      {VK_DOWN, "down"},          {VK_INSERT, "insert"},     {VK_DELETE, "delete"},
      {VK_PAUSE, "pause"},        {VK_CAPITAL, "capslock"},  {VK_NUMLOCK, "numlock"},
      {VK_SCROLL, "scrolllock"},
      {VK_SHIFT, "shift"},        {VK_CONTROL, "ctrl"},      {VK_MENU, "alt"},
      {VK_LSHIFT, "lshift"},      {VK_RSHIFT, "rshift"},     {VK_LCONTROL, "lctrl"},
      {VK_RCONTROL, "rctrl"},     {VK_LMENU, "lalt"},        {VK_RMENU, "ralt"},
      {VK_LWIN, "lwin"},          {VK_RWIN, "rwin"},
  };
  for (const auto& named : kNamedVKs) {
    if (named.vk == vk) return named.name;
  }
  if (vk >= static_cast<std::uint32_t>('A') && vk <= static_cast<std::uint32_t>('Z')) {
    return std::string(1, static_cast<char>(vk - 'A' + 'a'));
  }
  if (vk >= static_cast<std::uint32_t>('0') && vk <= static_cast<std::uint32_t>('9')) {
    return std::string(1, static_cast<char>(vk));  // chord digits are ASCII
  }
  if (vk >= 0x70u && vk <= 0x87u) {  // f1..f24
    return "f" + std::to_string(vk - 0x70u + 1u);
  }
  return {};
}

// Reads the keyName argument shared by getKeyState/keyWait: a string that
// resolves through state_key; anything else is a TypeError naming the call.
bool parse_state_key(JSContext* context, JSValueConst value, const char* what,
                     std::uint32_t& vk) {
  if (!JS_IsString(value)) {
    JS_ThrowTypeError(context, "%s(keyName): keyName must be a string", what);
    return false;
  }
  const char* text = JS_ToCString(context, value);
  if (!text) return false;
  const std::string name(text);
  JS_FreeCString(context, text);
  std::string error;
  if (!state_key(name, vk, error)) {
    JS_ThrowTypeError(context, "%s(keyName): %s", what, error.c_str());
    return false;
  }
  return true;
}

// input.getKeyState(keyName[, mode]): synchronous state read behind
// windows.input.read. mode is AHK's first-character selection (default L):
// L logical, P physical, T toggle - only the first character counts, like
// AHK (script2.cpp:2264). Unknown keys/modes are TypeErrors; a missing
// capability is a thrown Error naming windows.input.read, mirroring
// input.modifiers() (validate arguments first, then the gate).
JSValue input_get_key_state(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                            int, void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "getKeyState(keyName[, mode])");
  std::uint32_t vk = 0;
  if (!parse_state_key(context, argv[0], "getKeyState", vk)) return JS_EXCEPTION;
  KeyStateType type = KeyStateType::Logical;
  if (argc == 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (!JS_IsString(argv[1])) {
      return JS_ThrowTypeError(context, "getKeyState(keyName, mode): mode must be a string");
    }
    const char* text = JS_ToCString(context, argv[1]);
    if (!text) return JS_EXCEPTION;
    const char first = text[0];  // '\0' (empty string) falls to default below
    JS_FreeCString(context, text);
    switch (first) {
      case 'L':
      case 'l':
        type = KeyStateType::Logical;
        break;
      case 'P':
      case 'p':
        type = KeyStateType::Physical;
        break;
      case 'T':
      case 't':
        type = KeyStateType::Toggle;
        break;
      default:
        return JS_ThrowTypeError(
            context, "getKeyState(keyName, mode): mode must start with L, P or T");
    }
  }
  if (!binding->kernel->allows("windows.input.read")) {
    return throw_capability_error(context, "windows.input.read");
  }
  const bool down = type == KeyStateType::Physical
                        ? binding->service->physical_key_down(vk)
                        : read_key_state(vk, type);
  return JS_NewBool(context, down ? 1 : 0);
}

constexpr std::chrono::milliseconds kKeyWaitPollInterval{25};

// input.getKeySC(keyName): AHK GetKeySC (script2.cpp:2303-2310) - the scan
// code a key name maps to, or 0 when it maps to none. Name -> vk goes through
// state_key like getKeyState/keyWait, then MapVirtualKeyW exactly as the
// injector does (engine/win32/src/input.cpp:712). It reads no input state and
// no service, so no capability gate: docs/api/coverage.json records it as a
// runtime-level call. Deviation from AHK: extended keys return the standard
// 0xE0-prefixed make code (Right = 0xE04D) instead of AHK's internal 0x100
// flag (0x14D) - the low byte and the extended bit both match the hook's
// sc04D/E0 prefix - and numpad Enter has no separate name here (enter/return
// both resolve to VK_RETURN, so 0x1C is returned for both).
JSValue input_get_key_sc(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "getKeySC(keyName)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "getKeySC(keyName): keyName must be a string");
  }
  const char* text = JS_ToCString(context, argv[0]);
  if (!text) return JS_EXCEPTION;
  const std::string name(text);
  JS_FreeCString(context, text);
  std::uint32_t vk = 0;
  std::string error;
  if (!state_key(name, vk, error)) {
    // AHK GetKeySC: an unparseable name has no scan code at all (0), it does
    // not throw - unlike getKeyState, which reports the name it rejected.
    return JS_NewUint32(context, 0);
  }
  const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
  if (sc == 0) return JS_NewUint32(context, 0);  // mouse buttons and friends
  // E0 is part of the make code the hook delivers for these keys; the
  // right-hand and navigation cluster is a fixed list, not a layout query
  // (MapVirtualKeyW's E0 high byte comes back layout-dependent).
  const bool extended = vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME ||
                        vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT ||
                        vk == VK_UP || vk == VK_RIGHT || vk == VK_DOWN ||
                        vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_LWIN ||
                        vk == VK_RWIN;
  return JS_NewUint32(context, extended ? (sc | 0xE000u) : sc);
}

// input.getKeyVK(keyName): AHK GetKeyVK (script2.cpp:2294-2302) - the
// virtual key a key name maps to, or 0 when it maps to none. Name -> vk goes
// through state_key exactly like getKeyState/keyWait/keySC, so every shared
// spelling resolves: chord tokens, modifier and mouse names, the vkXX hex
// form and the scNNN scan-code form (this repo's extension of the grammar,
// in the spirit of AHK's TextToVKandSC which feeds both GetKeyVK and
// GetKeySC). An unparseable name returns 0 instead of throwing - the contract
// AHK GetKeyVK and this repo's getKeySC already use - and only a non-string
// argument is a TypeError. Mouse buttons keep their VK values, like AHK. No
// capability gate: it reads no input state and no service (docs/api/
// coverage.json records it as a runtime-level call), just like getKeySC.
JSValue input_get_key_vk(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "getKeyVK(keyName)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "getKeyVK(keyName): keyName must be a string");
  }
  const char* text = JS_ToCString(context, argv[0]);
  if (!text) return JS_EXCEPTION;
  const std::string name(text);
  JS_FreeCString(context, text);
  std::uint32_t vk = 0;
  std::string error;
  if (!state_key(name, vk, error)) {
    // AHK GetKeyVK: a name that resolves to no key yields 0, it does not
    // throw - unlike getKeyState, which reports the name it rejected.
    return JS_NewUint32(context, 0);
  }
  return JS_NewUint32(context, vk);
}

// input.getKeyName(keyName): AHK GetKeyName (script2.cpp:2310-2317) - the
// canonical name a key resolves to, or "" when it has none. Any spelling the
// shared grammar accepts (chord tokens, modifier/mouse names, vkXX, scNNN)
// resolves through state_key, then the VK is looked up in this repo's
// canonical table, so the result is a lowercase input-grammar token that
// reads back through getKeyState/keyWait. Deviation from AHK (recorded by
// the hub): AHK returns its display-table spelling (Escape, LControl, ...),
// an unshifted character, or vkNN for an unnamed code - this repo offers
// only the one canonical token, so those two fallbacks become "" (forms it
// does not accept back). Unparseable names return "" too, matching AHK's
// default; a non-string argument is a TypeError. No capability gate: it
// reads no input state and no service, just like getKeySC/getKeyVK.
JSValue input_get_key_name(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "getKeyName(keyName)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "getKeyName(keyName): keyName must be a string");
  }
  const char* text = JS_ToCString(context, argv[0]);
  if (!text) return JS_EXCEPTION;
  const std::string name(text);
  JS_FreeCString(context, text);
  std::uint32_t vk = 0;
  std::string error;
  if (!state_key(name, vk, error)) {
    // AHK GetKeyName: a name that resolves to no key has no name either.
    return JS_NewString(context, "");
  }
  const std::string canonical = key_name_for_vk(vk);
  return JS_NewString(context, canonical.c_str());
}

// One keyWait loop: held by shared_ptr through the worker/timer closures,
// exactly like the window WaitLoop (the Host outlives every armed task).
// Exactly one terminal path runs - resolve, reject, or CancelById - and
// each erases this token's pending/timer bookkeeping, so unload cannot
// wedge on it.
struct KeyWaitLoop {
  rime::js::Host* host;
  InputService* service;
  rime::action::Kernel* kernel;
  std::uint32_t vk{0};
  bool want_down{false};
  KeyStateType type{KeyStateType::Physical};
  std::uint64_t token{0};
  std::uint64_t cancellation_id{0};
  std::int64_t deadline_unix_ms{0};  // absolute system ms since epoch
  std::uint64_t budget_ms{0};        // the requested deadlineMs (error text)
};

// One wait step: settles the promise or re-arms the 25ms poll. Runs on the
// worker lane; capability/cancellation are checked before every read. Like
// windows.wait this path is exempt from Action dispatch, so there is no
// Action Trace.
void key_wait_step(std::shared_ptr<KeyWaitLoop> loop) {
  rime::js::Host* host = loop->host;
  const std::uint64_t token = loop->token;
  std::optional<AsyncOutcome> outcome;
  try {
    if (!loop->kernel->allows("windows.input.read")) {
      outcome = async_failure("capability_denied",
                              "required capability was not granted: windows.input.read");
    } else if (loop->cancellation_id != 0 && host->is_cancelled(loop->cancellation_id)) {
      outcome = async_failure("cancelled", "wait cancelled");
    } else {
      const bool down = loop->type == KeyStateType::Physical
                            ? loop->service->physical_key_down(loop->vk)
                            : read_key_state(loop->vk, loop->type);
      if (down == loop->want_down) {
        outcome = async_success("true");
      } else {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (now_ms >= loop->deadline_unix_ms) {
          outcome = async_failure(
              "timeout", "key wait timed out after " + std::to_string(loop->budget_ms) + "ms");
        }
      }
    }
  } catch (const std::exception& exception) {
    outcome = async_failure("execution_failed", exception.what());
  } catch (...) {
    outcome = async_failure("execution_failed", "native key wait failed");
  }
  if (outcome.has_value()) {
    if (outcome->ok) {
      host->complete_async(token, true, std::move(outcome->payload));
    } else {
      host->complete_async(token, false, outcome->code + ":" + outcome->payload);
    }
    return;
  }
  // Not satisfied yet: sleep on the scheduler, then poll again on the
  // worker. A cancellation landing between checks settles the promise
  // through CancelById; the next step observes is_cancelled and drains the
  // timer bookkeeping through complete_async (dropped, but erasing the
  // token).
  host->schedule_task(token, kKeyWaitPollInterval, [loop] {
    loop->host->schedule_worker(loop->token, [loop] { key_wait_step(loop); });
  });
}

// input.keyWait(keyName[, options]): AHK KeyWait. Resolves true once the
// key reaches the requested state; rejects `timeout` after deadlineMs
// (default5000 like every entry - AHK waits forever, a bounded wait is
// the house rule), `cancelled` when the bound cancellation fires, or
// `capability_denied` on the first poll. Defaults mirror AHK (wait.cpp:111):
// wait for RELEASE (options.down flips to a press wait) in PHYSICAL mode
// (options.mode accepts 'physical'|'logical'). Unknown keys, options and
// modes throw synchronously.
JSValue input_key_wait(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "keyWait(keyName[, options])");
  std::uint32_t vk = 0;
  if (!parse_state_key(context, argv[0], "keyWait", vk)) return JS_EXCEPTION;
  ActionOptions options;
  bool want_down = false;                   // AHK default: wait for release
  KeyStateType type = KeyStateType::Physical;  // AHK default: physical
  if (argc == 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (!JS_IsObject(argv[1])) {
      return JS_ThrowTypeError(context, "keyWait(keyName, options): options must be an object");
    }
    if (!parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
    JSValue down = JS_GetPropertyStr(context, argv[1], "down");
    if (JS_IsException(down)) return JS_EXCEPTION;
    if (!JS_IsUndefined(down) && !JS_IsNull(down)) {
      if (!JS_IsBool(down)) {
        JS_FreeValue(context, down);
        return JS_ThrowTypeError(context, "keyWait options.down must be a boolean");
      }
      want_down = JS_ToBool(context, down) > 0;
    }
    JS_FreeValue(context, down);
    JSValue mode = JS_GetPropertyStr(context, argv[1], "mode");
    if (JS_IsException(mode)) return JS_EXCEPTION;
    if (!JS_IsUndefined(mode) && !JS_IsNull(mode)) {
      if (!JS_IsString(mode)) {
        JS_FreeValue(context, mode);
        return JS_ThrowTypeError(context, "keyWait options.mode must be a string");
      }
      const char* text = JS_ToCString(context, mode);
      JS_FreeValue(context, mode);
      if (!text) return JS_EXCEPTION;
      const std::string mode_text(text);
      JS_FreeCString(context, text);
      if (mode_text == "physical") {
        type = KeyStateType::Physical;
      } else if (mode_text == "logical") {
        type = KeyStateType::Logical;
      } else {
        return JS_ThrowTypeError(context, "keyWait options.mode must be 'physical' or 'logical'");
      }
    }
  }
  auto* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  JSValue promise = JS_UNDEFINED;
  std::uint64_t token = 0;
  if (const auto error = host->begin_async(context, promise, token, options.cancellation_id);
      !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  // deadline_ms is bounded by js_int64_strict (2^53), so now + budget stays
  // far inside int64 milliseconds since the epoch.
  const auto deadline_unix_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      static_cast<std::int64_t>(options.deadline_ms);
  auto loop = std::make_shared<KeyWaitLoop>(KeyWaitLoop{
      host, binding->service, binding->kernel, vk, want_down, type, token,
      options.cancellation_id, deadline_unix_ms, options.deadline_ms});
  host->schedule_worker(token, [loop = std::move(loop)] { key_wait_step(loop); });
  return promise;
}

constexpr std::chrono::milliseconds kBlockGuardPollInterval{50};

// Released-block guard state for blockInput's cancellation option. The
// chain runs on the timer thread only (is_cancelled and set_blocked are
// thread-safe; no JS executes here) and lives inside the armed callback:
// the host stops its timers before the input service can be destroyed
// (bootstrap/runtime destruction order), and a dropped timer entry
// releases the guard with it.
struct BlockGuard {
  rime::js::Host* host;
  InputService* service;
  std::uint64_t cancellation_id;
};

// One guard step: release the block when the bound cancellation fires or
// stop chasing an already released block; otherwise re-arm the 50ms poll.
// A 0 timer id means the timer service is stopping: the chain ends here.
void block_guard_step(std::shared_ptr<BlockGuard> guard) {
  if (guard->host->is_cancelled(guard->cancellation_id)) {
    guard->service->set_blocked(false);
    return;
  }
  if (!guard->service->blocked()) return;  // released by blockInput('off')
  const auto timer_id = guard->host->timers().schedule(
      kBlockGuardPollInterval, [guard] { block_guard_step(guard); });
  if (timer_id == 0) return;  // host stopping: no new callbacks
}

// input.blockInput(mode[, options]): synchronous BlockInput switch behind
// windows.input.inject. mode is 'on'/'off'; AHK's other modes (Send, Mouse,
// SendAndMouse, Default, MouseMove, MouseMoveOff and the1/0 spellings)
// are unimplemented TypeErrors. Returns whether input is blocked
// afterwards - false when the input service is not running, since without
// installed hooks there is nothing to swallow. options.cancellationId arms
// a timer-thread guard that releases the block as soon as that
// cancellation fires; an explicit 'off' stops the guard, and
// InputService::stop() clears the flag unconditionally, so no shutdown
// path can leave the desktop blocked.
JSValue input_block_input(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "blockInput(mode[, options])");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "blockInput(mode): mode must be a string");
  }
  const char* mode_text = JS_ToCString(context, argv[0]);
  if (!mode_text) return JS_EXCEPTION;
  const std::string mode(mode_text);
  JS_FreeCString(context, mode_text);
  if (mode != "on" && mode != "off") {
    return JS_ThrowTypeError(context,
                             "blockInput(mode): mode must be 'on' or 'off' "
                             "(AHK's Send/Mouse variants are not implemented)");
  }
  std::uint64_t cancellation_id = 0;
  if (argc == 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (!JS_IsObject(argv[1])) {
      return JS_ThrowTypeError(context, "blockInput(mode, options): options must be an object");
    }
    if (!optional_u64(context, argv[1], "cancellationId", cancellation_id)) return JS_EXCEPTION;
  }
  if (!binding->kernel->allows("windows.input.inject")) {
    return throw_capability_error(context, "windows.input.inject");
  }
  const bool on = mode == "on";
  if (on && binding->service->state() != InputServiceState::Running) {
    // No installed hooks: setting the flag would lie about the effect.
    return JS_NewBool(context, 0);
  }
  binding->service->set_blocked(on);
  if (on && cancellation_id != 0) {
    rime::js::Host* host = host_of(context);
    if (host) {
      auto guard =
          std::make_shared<BlockGuard>(BlockGuard{host, binding->service, cancellation_id});
      (void)host->timers().schedule(kBlockGuardPollInterval,
                                    [guard] { block_guard_step(guard); });
    }
  }
  return JS_NewBool(context, binding->service->blocked() ? 1 : 0);
}

// input.setLockForce(keyName, force): the native half behind AHK Set*LockState
// (SetCapsLockState/SetNumLockState/SetScrollLockState) - the persistent
// direction keyboard.setLockState arms before it taps. force 'on'/'off'
// marks the key always-on/always-off (AHK g_ForceKeyLock): from then on the
// low-level keyboard hook swallows foreign presses and releases of that key,
// recorded first for history, subscriptions and the physical snapshot exactly
// like blockInput, so the toggle cannot move - while this process's own
// injected taps pass, which is the path that writes the state. 'neutral'
// releases the force and is ungated, mirroring installKeybdHook's un-gated
// removal. Establishing a direction needs windows.hook.global and re-installs
// the keyboard hook when something forced it away - a deviation from AHK,
// which has no capability concept and simply owns its hook. keyName accepts
// only capslock/numlock/scrolllock (state_key resolves the spelling first) and
// force only on/off/neutral; anything else is a synchronous TypeError. AHK
// anchors: script2.cpp:1768-1806 SetToggleState, hook.cpp:1904-1908
// (pForceToggle -> SuppressThisKey).
JSValue input_set_lock_force(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                             void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc != 2) return JS_ThrowTypeError(context, "setLockForce(keyName, force)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "setLockForce(keyName): keyName must be a string");
  }
  const char* key_text = JS_ToCString(context, argv[0]);
  if (!key_text) return JS_EXCEPTION;
  const std::string key_name(key_text);
  JS_FreeCString(context, key_text);
  std::uint32_t vk = 0;
  std::string error;
  if (!state_key(key_name, vk, error)) {
    return JS_ThrowTypeError(context, "setLockForce(keyName): %s", error.c_str());
  }
  if (vk != VK_CAPITAL && vk != VK_NUMLOCK && vk != VK_SCROLL) {
    return JS_ThrowTypeError(
        context, "setLockForce(keyName): keyName must be capslock, numlock or scrolllock");
  }
  if (!JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context, "setLockForce(force): force must be 'on', 'off' or 'neutral'");
  }
  const char* force_text = JS_ToCString(context, argv[1]);
  if (!force_text) return JS_EXCEPTION;
  const std::string force = ascii_lower(force_text);
  JS_FreeCString(context, force_text);
  if (force != "on" && force != "off" && force != "neutral") {
    return JS_ThrowTypeError(context, "setLockForce(force): force must be 'on', 'off' or 'neutral'");
  }
  if (force != "neutral") {
    // Arming is the privileged half (it keeps a global hook swallowing key
    // events); releasing is not, exactly like installKeybdHook(false).
    if (!binding->kernel->allows("windows.hook.global")) {
      return throw_capability_error(context, "windows.hook.global");
    }
    if (!binding->service->keyboard_hook_installed()) {
      (void)binding->service->set_keyboard_hook(true, false);
    }
  }
  binding->service->set_force_toggle(vk, force == "on" ? 1 : force == "off" ? -1 : 0);
  return JS_UNDEFINED;
}

// input.keyHistory([options]): synchronous KeyHistory report behind
// windows.input.read. options.maxEvents (integer0..500) resizes the
// recording ring like AHK's KeyHistory argument - a documented side effect
// of this query - and the report returns { capacity, count, events } with
// events oldest-first. Deviations from AHK: no GUI window (the argument-less
// KeyHistory opens one) and no target-window column. Out-of-range values
// are TypeErrors (AHK FR_E_ARG), a missing capability a thrown Error;
// arguments validate before the gate, and the resize only happens after it,
// so a denied call never mutates the ring.
JSValue input_key_history(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "keyHistory([options])");
  bool resize = false;
  std::size_t capacity = 0;
  if (argc == 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
    if (!JS_IsObject(argv[0])) {
      return JS_ThrowTypeError(context, "keyHistory(options): options must be an object");
    }
    JSValue max_events = JS_GetPropertyStr(context, argv[0], "maxEvents");
    if (JS_IsException(max_events)) return JS_EXCEPTION;
    if (!JS_IsUndefined(max_events) && !JS_IsNull(max_events)) {
      std::int64_t raw = 0;
      if (!js_int64_strict(context, max_events, raw, "options.maxEvents")) {
        JS_FreeValue(context, max_events);
        return JS_EXCEPTION;
      }
      if (raw < 0 || raw > 500) {
        JS_FreeValue(context, max_events);
        return JS_ThrowTypeError(context, "keyHistory options.maxEvents must be in 0..500");
      }
      capacity = static_cast<std::size_t>(raw);
      resize = true;
    }
    JS_FreeValue(context, max_events);
  }
  if (!binding->kernel->allows("windows.input.read")) {
    return throw_capability_error(context, "windows.input.read");
  }
  if (resize) binding->service->set_key_history_capacity(capacity);
  const std::vector<KeyHistoryEntry> entries = binding->service->key_history();
  json::Value events = json::Value::array();
  for (const auto& entry : entries) {
    json::Value row = json::Value::object();
    row.set("vk", json::Value::number(static_cast<double>(entry.vk)));
    row.set("scan", json::Value::number(static_cast<double>(entry.scan)));
    row.set("down", json::Value::boolean(entry.down));
    row.set("injected", json::Value::boolean(entry.injected));
    row.set("selfInjected", json::Value::boolean(entry.self_injected));
    row.set("timestamp", json::Value::number(static_cast<double>(entry.timestamp_ms)));
    row.set("elapsed", json::Value::number(static_cast<double>(entry.elapsed_ms)));
    events.push(std::move(row));
  }
  json::Value report = json::Value::object();
  report.set("capacity",
             json::Value::number(
                 static_cast<double>(binding->service->key_history_capacity())));
  report.set("count", json::Value::number(static_cast<double>(entries.size())));
  report.set("events", std::move(events));
  const std::string text = json::stringify(report);
  return JS_ParseJSON(context, text.c_str(), text.size(), "<keyHistory>");
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
      !add("bind", input_bind, 2) || !add("unbind", input_unbind, 1) ||
      !add("send", input_send, 1) || !add("modifiers", input_modifiers, 0) ||
      !add("mouse", input_mouse, 1) || !add("mouseGetPos", input_mouse_get_pos, 1) ||
      !add("getKeyState", input_get_key_state, 2) || !add("getKeySC", input_get_key_sc, 1) ||
      !add("getKeyVK", input_get_key_vk, 1) || !add("getKeyName", input_get_key_name, 1) ||
      !add("keyWait", input_key_wait, 2) ||
       !add("blockInput", input_block_input, 2) ||
       !add("setLockForce", input_set_lock_force, 2) || !add("keyHistory", input_key_history, 1)) {
    return -1;
  }
  // M2-C event exports (hotkey/hotstring/hotIf/setTimer/onMessage/...) share
  // the same `input` facade and the same module binding; they also create the
  // EventsState on first load.
  if (!add_events_exports(context, input)) {
    JS_FreeValue(context, input);
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
  if (const auto error = host.modules().add_native(
          "rime:input", [](JSContext* context) { return create_input_module(context); });
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error register_input_module(rime::js::Runtime& runtime,
                                        InputModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:input", [](JSContext* context) { return create_input_module(context); }, binding);
}

}  // namespace rime::win32
