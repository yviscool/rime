#include "rime/win32/js_control.hpp"

#include "async_task.hpp"
#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "quickjs.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

rime::js::Host* host_of(JSContext* context) {
  return static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
}

ControlModuleBinding* binding_of(JSContext* context) {
  auto* host = host_of(context);
  if (!host) return nullptr;
  return static_cast<ControlModuleBinding*>(host->module_data("rime:control"));
}

// Strict JS number -> uint64 control id: the stable ids from controls() fit
// in 2^53 - 1 (generation and sequence are small); anything else is a
// synchronous TypeError, mirroring strict_element_id.
bool strict_control_id(JSContext* context, JSValueConst value, std::uint64_t& out,
                       const char* what) {
  if (!JS_IsNumber(value)) {
    JS_ThrowTypeError(context, "%s: controlId must be a number", what);
    return false;
  }
  double number = 0;
  if (JS_ToFloat64(context, &number, value)) return false;
  if (!std::isfinite(number) || std::trunc(number) != number) {
    JS_ThrowTypeError(context, "%s: controlId must be an integer", what);
    return false;
  }
  constexpr double kMaxSafeInteger = 9007199254740991.0;  // 2^53 - 1
  if (number <= 0.0 || number > kMaxSafeInteger) {
    JS_ThrowTypeError(context, "%s: controlId must be a positive integer", what);
    return false;
  }
  out = static_cast<std::uint64_t>(number);
  return true;
}

JSValue throw_capability_error(JSContext* context, const char* capability) {
  const std::string text = std::string("required capability was not granted: ") + capability;
  JSValue error = JS_NewError(context);
  if (JS_IsException(error)) return JS_EXCEPTION;
  JSValue message = JS_NewString(context, text.c_str());
  if (JS_IsException(message)) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  if (JS_DefinePropertyValueStr(context, error, "message", message, JS_PROP_WRITABLE) < 0) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  return JS_Throw(context, error);
}

// Synchronous service errors surface as thrown Errors framed "code: message"
// (same framing the async lane uses through complete_async), so control.ts
// can map them without a second error taxonomy.
JSValue throw_service_error(JSContext* context, const rime::core::Error& error) {
  const std::string text = rime::core::error_code_name(error.code) + (": " + error.message);
  return JS_ThrowInternalError(context, "%s", text.c_str());
}

bool is_plain_object(JSValueConst value) {
  return JS_IsObject(value) && !JS_IsArray(value) && !JS_IsNull(value);
}

bool case_insensitive_equal(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const char l = left[i];
    const char r = right[i];
    const char ln = (l >= 'A' && l <= 'Z') ? static_cast<char>(l + ('a' - 'A')) : l;
    const char rn = (r >= 'A' && r <= 'Z') ? static_cast<char>(r + ('a' - 'A')) : r;
    if (ln != rn) return false;
  }
  return true;
}

// Reads an optional string field; missing stays absent, present non-string
// throws naming the call and field.
bool optional_spec_string(JSContext* context, JSValueConst object, const char* call,
                          const char* field, std::string& out, bool& present) {
  JSValue value = JS_GetPropertyStr(context, object, field);
  if (JS_IsException(value)) return false;
  if (JS_IsUndefined(value) || JS_IsNull(value)) {
    JS_FreeValue(context, value);
    return true;
  }
  if (!JS_IsString(value)) {
    JS_FreeValue(context, value);
    JS_ThrowTypeError(context, "%s: spec.%s must be a string", call, field);
    return false;
  }
  const char* text = JS_ToCString(context, value);
  JS_FreeValue(context, value);
  if (!text) return false;
  out = text;
  present = true;
  JS_FreeCString(context, text);
  return true;
}

// control.resolve(windowId, spec[, options]) -> {id, className, classNN}
// A read (capability windows.window.read): enumerates controls() and matches
// exactly one explicit form. {auto} tries classNN, then text (AHK fallback).
JSValue control_resolve(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:control is not wired");
  }
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "resolve(windowId, spec[, options])");
  int64_t window_raw = 0;
  if (!js_int64_strict(context, argv[0], window_raw, "resolve(windowId)")) return JS_EXCEPTION;
  if (window_raw <= 0) {
    return JS_ThrowTypeError(context, "resolve(windowId): windowId must be a positive integer");
  }
  const auto window_id = static_cast<std::uint64_t>(window_raw);
  if (!is_plain_object(argv[1])) {
    return JS_ThrowTypeError(context, "resolve(windowId, spec): spec must be an object");
  }
  std::string class_nn;
  std::string text;
  std::string auto_text;
  bool has_class_nn = false;
  bool has_text = false;
  bool has_auto = false;
  if (!optional_spec_string(context, argv[1], "resolve", "classNN", class_nn, has_class_nn) ||
      !optional_spec_string(context, argv[1], "resolve", "text", text, has_text) ||
      !optional_spec_string(context, argv[1], "resolve", "auto", auto_text, has_auto)) {
    return JS_EXCEPTION;
  }
  // {hwnd} and {point} are read here (numbers only); matching runs in the
  // async body against controls()/id_for_hwnd/window_at.
  std::uint64_t hwnd = 0;
  bool has_hwnd = false;
  {
    JSValue value = JS_GetPropertyStr(context, argv[1], "hwnd");
    if (JS_IsException(value)) return JS_EXCEPTION;
    if (!JS_IsUndefined(value) && !JS_IsNull(value)) {
      double number = 0;
      if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) ||
          !std::isfinite(number) || std::trunc(number) != number || number <= 0.0 ||
          number > 9007199254740991.0) {
        JS_FreeValue(context, value);
        return JS_ThrowTypeError(context, "resolve: spec.hwnd must be a positive integer");
      }
      hwnd = static_cast<std::uint64_t>(number);
      has_hwnd = true;
    }
    JS_FreeValue(context, value);
  }
  int point_x = 0;
  int point_y = 0;
  std::string point_space = "window-client";
  bool has_point = false;
  {
    JSValue point = JS_GetPropertyStr(context, argv[1], "point");
    if (JS_IsException(point)) return JS_EXCEPTION;
    if (!JS_IsUndefined(point) && !JS_IsNull(point)) {
      if (!is_plain_object(point)) {
        JS_FreeValue(context, point);
        return JS_ThrowTypeError(context, "resolve: spec.point must be {x, y[, space]}");
      }
      int coords[2] = {0, 0};
      const char* keys[2] = {"x", "y"};
      for (int i = 0; i < 2; ++i) {
        JSValue field = JS_GetPropertyStr(context, point, keys[i]);
        if (JS_IsException(field)) {
          JS_FreeValue(context, point);
          return JS_EXCEPTION;
        }
        double number = 0;
        if (!JS_IsNumber(field) || JS_ToFloat64(context, &number, field) ||
            !std::isfinite(number) || std::trunc(number) != number || number < -2147483648.0 ||
            number > 2147483647.0) {
          JS_FreeValue(context, field);
          JS_FreeValue(context, point);
          return JS_ThrowTypeError(context, "resolve: spec.point.%s must be a 32-bit integer",
                                   keys[i]);
        }
        coords[i] = static_cast<int>(number);
        JS_FreeValue(context, field);
      }
      point_x = coords[0];
      point_y = coords[1];
      JSValue space = JS_GetPropertyStr(context, point, "space");
      if (JS_IsException(space)) {
        JS_FreeValue(context, point);
        return JS_EXCEPTION;
      }
      if (!JS_IsUndefined(space) && !JS_IsNull(space)) {
        const char* space_text = JS_ToCString(context, space);
        JS_FreeValue(context, space);
        if (!space_text) {
          JS_FreeValue(context, point);
          return JS_EXCEPTION;
        }
        point_space = space_text;
        JS_FreeCString(context, space_text);
        if (point_space != "window-client" && point_space != "screen") {
          JS_FreeValue(context, point);
          return JS_ThrowTypeError(
              context, "resolve: spec.point.space must be 'window-client' or 'screen'");
        }
      } else {
        JS_FreeValue(context, space);
      }
      JS_FreeValue(context, point);
      has_point = true;
    } else {
      JS_FreeValue(context, point);
    }
  }
  const int forms = (has_class_nn ? 1 : 0) + (has_text ? 1 : 0) + (has_hwnd ? 1 : 0) +
                    (has_point ? 1 : 0) + (has_auto ? 1 : 0);
  if (forms != 1) {
    return JS_ThrowTypeError(
        context, "resolve: spec needs exactly one of classNN, text, hwnd, point or auto");
  }
  if (!binding->kernel->allows("windows.window.read")) {
    return throw_capability_error(context, "windows.window.read");
  }
  ActionOptions options;
  if (argc == 3 && !parse_action_options(context, argv[2], options)) return JS_EXCEPTION;
  WindowService* service = binding->service;
  rime::action::Kernel* kernel = binding->kernel;
  return start_async(
      context,
      [service, kernel, window_id, class_nn, text, auto_text, hwnd, point_x, point_y, point_space,
       has_class_nn, has_text, has_auto, has_hwnd,
       has_point]() -> AsyncOutcome {
        if (!kernel->allows("windows.window.read")) {
          return capability_denied("windows.window.read");
        }
        std::vector<ControlInfo> controls;
        if (const auto error = service->controls(window_id, controls); !error.ok()) {
          return async_failure(error);
        }
        std::uint64_t matched = 0;
        std::string matched_class;
        std::string matched_nn;
        const auto take = [&](std::uint64_t id, const std::string& name,
                              const std::string& nn) {
          matched = id;
          matched_class = name;
          matched_nn = nn;
        };
        if (has_class_nn || has_auto) {
          const std::string& want = has_class_nn ? class_nn : auto_text;
          for (const auto& control : controls) {
            if (case_insensitive_equal(control.class_nn, want)) {
              take(control.id, control.class_name, control.class_nn);
              break;
            }
          }
        }
        if (matched == 0 && (has_text || has_auto)) {
          const std::string& want = has_text ? text : auto_text;
          for (const auto& control : controls) {
            std::string got;
            if (const auto error = service->control_get_text(control.id, got); !error.ok()) {
              if (error.code == rime::core::Error::Code::TargetGone) continue;
              return async_failure(error);
            }
            if (case_insensitive_equal(got, want)) {
              take(control.id, control.class_name, control.class_nn);
              break;
            }
          }
        }
        if (matched == 0 && has_hwnd) {
          void* raw = reinterpret_cast<void*>(static_cast<std::uintptr_t>(hwnd));
          std::uint64_t id = 0;
          if (const auto error = service->control_id_for_hwnd(window_id, raw, id); !error.ok()) {
            return async_failure(error);
          }
          for (const auto& control : controls) {
            if (control.id == id) {
              take(control.id, control.class_name, control.class_nn);
              break;
            }
          }
          if (matched == 0) {
            return async_failure("invalid_contract", "resolve: hwnd is not a child control here");
          }
        }
        if (matched == 0 && has_point) {
          int sx = point_x;
          int sy = point_y;
          if (point_space == "window-client") {
            WindowInfo info;
            if (const auto error = service->info(window_id, info); !error.ok()) {
              return async_failure(error);
            }
            sx = static_cast<int>(info.rect.left) + point_x;
            sy = static_cast<int>(info.rect.top) + point_y;
          }
          WindowAtInfo hit;
          if (const auto error = service->window_at(sx, sy, hit); !error.ok()) {
            return async_failure(error);
          }
          if (!hit.control.has_value() || !hit.window.has_value() ||
              hit.window->id != window_id) {
            return async_failure("invalid_contract",
                                 "resolve: point hits no control of this window");
          }
          take(hit.control->id, hit.control->class_name, hit.control->class_nn);
        }
        if (matched == 0) {
          return async_failure("invalid_contract", "resolve: no control matches the spec");
        }
        json::Value value = json::Value::object();
        value.set("id", json::Value::number(static_cast<double>(matched)));
        value.set("className", json::Value::string(matched_class));
        value.set("classNN", json::Value::string(matched_nn));
        return async_success(json::stringify(value));
      },
      options.cancellation_id);
}

// Shared shape for click/focus/setText/getText/sendText: strict id, arity,
// capability gate, then a routed Action with target {"control", "<id>"}.
JSValue control_action(JSContext* context, int argc, JSValueConst* argv,
                      const char* signature, const char* type, std::string payload_json,
                      ControlModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:control is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "%s", signature);
  std::uint64_t id = 0;
  if (!strict_control_id(context, argv[0], id, signature)) return JS_EXCEPTION;
  if (!binding->kernel->allows("windows.automation.control")) {
    return throw_capability_error(context, "windows.automation.control");
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  auto action = make_action(*binding->next_action_id, "rime:control", type,
                            "windows.automation.control",
                            {"control", std::to_string(id)}, std::move(payload_json), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

bool read_click_option_string(JSContext* context, JSValueConst object, const char* call,
                              const char* field, const char* const* names, std::size_t count,
                              std::string& out, bool& present) {
  JSValue value = JS_GetPropertyStr(context, object, field);
  if (JS_IsException(value)) return false;
  if (JS_IsUndefined(value) || JS_IsNull(value)) {
    JS_FreeValue(context, value);
    return true;
  }
  if (!JS_IsString(value)) {
    JS_FreeValue(context, value);
    JS_ThrowTypeError(context, "%s: %s must be a string", call, field);
    return false;
  }
  const char* text = JS_ToCString(context, value);
  JS_FreeValue(context, value);
  if (!text) return false;
  out = text;
  JS_FreeCString(context, text);
  present = true;
  for (std::size_t i = 0; i < count; ++i) {
    if (out == names[i]) return true;
  }
  JS_ThrowTypeError(context, "%s: unknown %s '%s'", call, field, out.c_str());
  return false;
}

// control.click(id[, opts[, options]]) -> { clicks }
JSValue control_click(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 3) return JS_ThrowTypeError(context, "click(id[, opts[, options]])");
  json::Value payload = json::Value::object();
  if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (!is_plain_object(argv[1])) {
      return JS_ThrowTypeError(context, "click: opts must be an object");
    }
    static constexpr const char* kButtons[] = {"left", "right", "middle", "x1", "x2"};
    static constexpr const char* kPhases[] = {"downUp", "down", "up"};
    std::string button;
    std::string phase;
    bool has_button = false;
    bool has_phase = false;
    if (!read_click_option_string(context, argv[1], "click", "button", kButtons, 5, button,
                                  has_button) ||
        !read_click_option_string(context, argv[1], "click", "phase", kPhases, 3, phase,
                                  has_phase)) {
      return JS_EXCEPTION;
    }
    if (has_button) payload.set("button", json::Value::string(button));
    if (has_phase) payload.set("phase", json::Value::string(phase));
    JSValue count_value = JS_GetPropertyStr(context, argv[1], "count");
    if (JS_IsException(count_value)) return JS_EXCEPTION;
    if (!JS_IsUndefined(count_value) && !JS_IsNull(count_value)) {
      int64_t count = 0;
      if (!js_int64_strict(context, count_value, count, "click.count") || count < 0 ||
          count > 1000000) {
        JS_FreeValue(context, count_value);
        return JS_ThrowTypeError(context, "click: count must be an integer in 0..1000000");
      }
      payload.set("count", json::Value::number(static_cast<double>(count)));
    }
    JS_FreeValue(context, count_value);
    JSValue activate_value = JS_GetPropertyStr(context, argv[1], "activate");
    if (JS_IsException(activate_value)) return JS_EXCEPTION;
    if (!JS_IsUndefined(activate_value) && !JS_IsNull(activate_value)) {
      if (!JS_IsBool(activate_value)) {
        JS_FreeValue(context, activate_value);
        return JS_ThrowTypeError(context, "click: activate must be a boolean");
      }
      payload.set("activate", json::Value::boolean(JS_ToBool(context, activate_value) != 0));
    }
    JS_FreeValue(context, activate_value);
    for (const char* key : {"x", "y"}) {
      JSValue field = JS_GetPropertyStr(context, argv[1], key);
      if (JS_IsException(field)) return JS_EXCEPTION;
      if (!JS_IsUndefined(field) && !JS_IsNull(field)) {
        int64_t coord = 0;
        if (!js_int64_strict(context, field, coord, "click.x/y") || coord < -2147483648ll ||
            coord > 2147483647ll) {
          JS_FreeValue(context, field);
          return JS_ThrowTypeError(context, "click: %s must be a 32-bit integer", key);
        }
        payload.set(key, json::Value::number(static_cast<double>(coord)));
      }
      JS_FreeValue(context, field);
    }
    std::uint64_t settle = 0;
    bool has_settle = false;
    {
      JSValue settle_value = JS_GetPropertyStr(context, argv[1], "settleMs");
      if (JS_IsException(settle_value)) return JS_EXCEPTION;
      has_settle = !JS_IsUndefined(settle_value) && !JS_IsNull(settle_value);
      JS_FreeValue(context, settle_value);
    }
    if (has_settle && !optional_u64(context, argv[1], "settleMs", settle)) return JS_EXCEPTION;
    if (has_settle) {
      payload.set("settleMs", json::Value::number(static_cast<double>(settle)));
    }
  }
  const int action_argc = argc == 3 ? 2 : argc;
  JSValueConst action_argv[2] = {argv[0], argc == 3 ? argv[2] : JS_UNDEFINED};
  return control_action(context, action_argc, const_cast<JSValueConst*>(action_argv),
                        "click(id[, opts[, options]])", "control.click", json::stringify(payload),
                        binding);
}

// control.focus(id[, options]) -> { focused: true }
JSValue control_focus(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_action(context, argc, argv, "focus(id[, options])", "control.focus", "{}",
                        binding);
}

// control.setText(id, text[, options]) / control.sendText(id, text[, options])
JSValue control_text(JSContext* context, int argc, JSValueConst* argv, const char* signature,
                     const char* type, ControlModuleBinding* binding) {
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "%s", signature);
  if (!JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context, "%s: text must be a string", signature);
  }
  const char* text = JS_ToCString(context, argv[1]);
  if (!text) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  payload.set("text", json::Value::string(text));
  JS_FreeCString(context, text);
  JSValueConst action_argv[2] = {argv[0], argc == 3 ? argv[2] : JS_UNDEFINED};
  return control_action(context, argc == 3 ? 2 : 1, const_cast<JSValueConst*>(action_argv),
                        signature, type, json::stringify(payload), binding);
}

JSValue control_set_text(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_text(context, argc, argv, "setText(id, text[, options])", "control.settext",
                      binding);
}

JSValue control_send_text(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_text(context, argc, argv, "sendText(id, text[, options])", "control.sendtext",
                      binding);
}

// control.getText(id[, options]) -> { text }
JSValue control_get_text(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_action(context, argc, argv, "getText(id[, options])", "control.gettext", "{}",
                        binding);
}

// Synchronous queries: single Win32 reads, no Action, capability
// windows.window.read (same gate as windows.controls).
JSValue control_query_bool(JSContext* context, int argc, JSValueConst* argv, const char* signature,
                           rime::core::Error (WindowService::*method)(std::uint64_t, bool&,
                                                                      std::chrono::milliseconds),
                           ControlModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:control is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "%s", signature);
  std::uint64_t id = 0;
  if (!strict_control_id(context, argv[0], id, signature)) return JS_EXCEPTION;
  if (!binding->kernel->allows("windows.window.read")) {
    return throw_capability_error(context, "windows.window.read");
  }
  bool out = false;
  if (const auto error =
          (binding->service->*method)(id, out, std::chrono::seconds(5));
      !error.ok()) {
    return throw_service_error(context, error);
  }
  return JS_NewBool(context, out ? 1 : 0);
}

JSValue control_is_visible(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_query_bool(context, argc, argv, "isVisible(id)", &WindowService::control_is_visible,
                            binding);
}

JSValue control_is_enabled(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_query_bool(context, argc, argv, "isEnabled(id)", &WindowService::control_is_enabled,
                            binding);
}

// control.rect(id) -> {x, y, width, height} (screen coordinates, AHK rule)
JSValue control_rect(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:control is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "rect(id)");
  std::uint64_t id = 0;
  if (!strict_control_id(context, argv[0], id, "rect(id)")) return JS_EXCEPTION;
  if (!binding->kernel->allows("windows.window.read")) {
    return throw_capability_error(context, "windows.window.read");
  }
  Rect rect{};
  if (const auto error = binding->service->control_rect(id, rect); !error.ok()) {
    return throw_service_error(context, error);
  }
  JSValue result = JS_NewObject(context);
  if (JS_IsException(result)) return JS_EXCEPTION;
  auto set_number = [&](const char* name, double value) -> bool {
    JSValue field = JS_NewFloat64(context, value);
    if (JS_IsException(field)) {
      JS_FreeValue(context, result);
      return false;
    }
    if (JS_SetPropertyStr(context, result, name, field) < 0) {
      JS_FreeValue(context, field);
      JS_FreeValue(context, result);
      return false;
    }
    return true;
  };
  if (!set_number("x", static_cast<double>(rect.left)) ||
      !set_number("y", static_cast<double>(rect.top)) ||
      !set_number("width", static_cast<double>(rect.width())) ||
      !set_number("height", static_cast<double>(rect.height()))) {
    return JS_EXCEPTION;
  }
  return result;
}

// control.dispose(id) -> boolean (liveness probe; ids are never recycled,
// so dispose validates instead of freeing).
JSValue control_dispose(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:control is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "dispose(id)");
  std::uint64_t id = 0;
  if (!strict_control_id(context, argv[0], id, "dispose(id)")) return JS_EXCEPTION;
  bool alive = false;
  if (const auto error = binding->service->control_alive(id, alive); !error.ok()) {
    return throw_service_error(context, error);
  }
  return JS_NewBool(context, alive ? 1 : 0);
}

int control_module_init(JSContext* context, JSModuleDef* module) {
  ControlModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:control requires a control module binding");
    return -1;
  }
  JSValue control = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(value)) {
      JS_FreeValue(context, control);
      return false;
    }
    if (JS_SetPropertyStr(context, control, name, value) < 0) {
      JS_FreeValue(context, control);
      return false;
    }
    return true;
  };
  if (!add("resolve", control_resolve, 2) || !add("click", control_click, 1) ||
      !add("focus", control_focus, 1) || !add("setText", control_set_text, 2) ||
      !add("getText", control_get_text, 1) || !add("sendText", control_send_text, 2) ||
      !add("isVisible", control_is_visible, 1) || !add("isEnabled", control_is_enabled, 1) ||
      !add("rect", control_rect, 1) || !add("dispose", control_dispose, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "control", control);
}

JSModuleDef* create_control_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:control", control_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "control") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const ControlModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:control requires a window service, kernel and dispatcher"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_control_module(rime::js::Host& host, ControlModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:control", binding);
  if (const auto error = host.modules().add_native(
          "rime:control", [](JSContext* context) { return create_control_module(context); });
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error register_control_module(rime::js::Runtime& runtime,
                                           ControlModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:control", [](JSContext* context) { return create_control_module(context); }, binding);
}

}  // namespace rime::win32
