#include "rime/win32/js_control.hpp"

#include "async_task.hpp"
#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "quickjs.h"

#include <windows.h>

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

// control.gettext(id[, options]) -> { text }
JSValue control_get_text(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_action(context, argc, argv, "getText(id[, options])", "control.gettext", "{}",
                        binding);
}

// ---- Phase 2 verbs --------------------------------------------------------
// Routes one control action: strict id + prebuilt JSON payload + options.
JSValue dispatch_control(JSContext* context, JSValueConst id_value, JSValueConst options_value,
                         bool has_options, const char* signature, const char* type,
                         json::Value payload, ControlModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:control is not wired");
  }
  std::uint64_t id = 0;
  if (!strict_control_id(context, id_value, id, signature)) return JS_EXCEPTION;
  if (!binding->kernel->allows("windows.automation.control")) {
    return throw_capability_error(context, "windows.automation.control");
  }
  ActionOptions options;
  if (has_options && !parse_action_options(context, options_value, options)) return JS_EXCEPTION;
  auto action = make_action(*binding->next_action_id, "rime:control", type,
                            "windows.automation.control",
                            {"control", std::to_string(id)}, json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// control.listAdd(id, text[, options]) -> { index }
JSValue control_list_add(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "listAdd(id, text[, options])");
  if (!JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context, "listAdd(id, text): text must be a string");
  }
  const char* text = JS_ToCString(context, argv[1]);
  if (!text) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  payload.set("text", json::Value::string(text));
  JS_FreeCString(context, text);
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          "listAdd(id, text)", "control.list.add", std::move(payload), binding);
}

// control.listDelete(id, index[, options]) -> { deleted: true }
JSValue control_list_delete(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "listDelete(id, index[, options])");
  double number = 0;
  if (!JS_IsNumber(argv[1]) || JS_ToFloat64(context, &number, argv[1]) ||
      !std::isfinite(number) || std::trunc(number) != number || number < 1.0 ||
      number > 1000000.0) {
    return JS_ThrowTypeError(context, "listDelete: index must be an integer >= 1");
  }
  json::Value payload = json::Value::object();
  payload.set("index", json::Value::number(number));
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          "listDelete(id, index)", "control.list.delete", std::move(payload),
                          binding);
}

// control.listChoose(id, {index}|{text}[, notifyParent][, options])
JSValue control_list_choose(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 4)
    return JS_ThrowTypeError(context, "listChoose(id, sel[, notifyParent[, options]])");
  if (!is_plain_object(argv[1])) {
    return JS_ThrowTypeError(context, "listChoose: sel must be {index} or {text}");
  }
  json::Value payload = json::Value::object();
  JSValue index_value = JS_GetPropertyStr(context, argv[1], "index");
  if (JS_IsException(index_value)) return JS_EXCEPTION;
  const bool has_index = !JS_IsUndefined(index_value) && !JS_IsNull(index_value);
  JSValue text_value = JS_GetPropertyStr(context, argv[1], "text");
  if (JS_IsException(text_value)) {
    JS_FreeValue(context, index_value);
    return JS_EXCEPTION;
  }
  const bool has_text = !JS_IsUndefined(text_value) && !JS_IsNull(text_value);
  if (has_index == has_text) {
    JS_FreeValue(context, index_value);
    JS_FreeValue(context, text_value);
    return JS_ThrowTypeError(context, "listChoose: sel needs exactly one of index or text");
  }
  if (has_index) {
    double number = 0;
    if (!JS_IsNumber(index_value) || JS_ToFloat64(context, &number, index_value) ||
        !std::isfinite(number) || std::trunc(number) != number || number < 0.0 ||
        number > 1000000.0) {
      JS_FreeValue(context, index_value);
      JS_FreeValue(context, text_value);
      return JS_ThrowTypeError(context, "listChoose: index must be an integer >= 0");
    }
    payload.set("index", json::Value::number(number));
  } else {
    if (!JS_IsString(text_value)) {
      JS_FreeValue(context, index_value);
      JS_FreeValue(context, text_value);
      return JS_ThrowTypeError(context, "listChoose: text must be a string");
    }
    const char* text = JS_ToCString(context, text_value);
    if (!text) {
      JS_FreeValue(context, index_value);
      JS_FreeValue(context, text_value);
      return JS_EXCEPTION;
    }
    payload.set("text", json::Value::string(text));
    JS_FreeCString(context, text);
  }
  JS_FreeValue(context, index_value);
  JS_FreeValue(context, text_value);
  bool notify = true;
  bool notify_present = false;
  int notify_arg = 2;
  if (argc >= 3 && (JS_IsBool(argv[2]) || JS_IsUndefined(argv[2]) || JS_IsNull(argv[2]))) {
    if (JS_IsBool(argv[2])) {
      notify = JS_ToBool(context, argv[2]) != 0;
      notify_present = true;
    }
    notify_arg = 3;
  }
  if (notify_present) payload.set("notifyParent", json::Value::boolean(notify));
  const int options_arg = notify_arg;
  const bool has_options = argc > options_arg;
  return dispatch_control(context, argv[0], has_options ? argv[options_arg] : JS_UNDEFINED,
                          has_options, "listChoose(id, sel[, notifyParent[, options]])",
                          "control.list.choose", std::move(payload), binding);
}

// control.listFind(id, text[, options]) -> { index } (0 = miss, a result)
JSValue control_list_find(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "listFind(id, text[, options])");
  if (!JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context, "listFind(id, text): text must be a string");
  }
  const char* text = JS_ToCString(context, argv[1]);
  if (!text) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  payload.set("text", json::Value::string(text));
  JS_FreeCString(context, text);
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          "listFind(id, text)", "control.list.find", std::move(payload), binding);
}

// control.listIndex(id[, options]) -> { index }
JSValue control_list_index(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "listIndex(id[, options])");
  return dispatch_control(context, argv[0], argc == 2 ? argv[1] : JS_UNDEFINED, argc == 2,
                          "listIndex(id)", "control.list.index", json::Value::object(), binding);
}

// control.listChoice(id[, index[, options]]) -> { text }
JSValue control_list_choice(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 3) return JS_ThrowTypeError(context, "listChoice(id[, index[, options]])");
  json::Value payload = json::Value::object();
  JSValueConst options_value = JS_UNDEFINED;
  bool has_options = false;
  if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (is_plain_object(argv[1])) {
      if (argc > 2) {
        return JS_ThrowTypeError(context, "listChoice(id[, index[, options]]): too many arguments");
      }
      options_value = argv[1];
      has_options = true;
    } else {
      double number = 0;
      if (!JS_IsNumber(argv[1]) || JS_ToFloat64(context, &number, argv[1]) ||
          !std::isfinite(number) || std::trunc(number) != number || number < 0.0 ||
          number > 1000000.0) {
        return JS_ThrowTypeError(context, "listChoice: index must be an integer >= 0");
      }
      payload.set("index", json::Value::number(number));
      if (argc == 3) {
        options_value = argv[2];
        has_options = true;
      }
    }
  } else if (argc == 3) {
    options_value = argv[2];
    has_options = true;
  }
  return dispatch_control(context, argv[0], options_value, has_options,
                          "listChoice(id[, index[, options]])", "control.list.choice",
                          std::move(payload), binding);
}

// control.listItems(id[, limit[, options]]) -> { items[] }
JSValue control_list_items(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 3) return JS_ThrowTypeError(context, "listItems(id[, limit[, options]])");
  json::Value payload = json::Value::object();
  JSValueConst options_value = JS_UNDEFINED;
  bool has_options = false;
  if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    if (is_plain_object(argv[1])) {
      if (argc > 2) {
        return JS_ThrowTypeError(context, "listItems(id[, limit[, options]]): too many arguments");
      }
      options_value = argv[1];
      has_options = true;
    } else {
      double number = 0;
      if (!JS_IsNumber(argv[1]) || JS_ToFloat64(context, &number, argv[1]) ||
          !std::isfinite(number) || std::trunc(number) != number || number < 1.0 ||
          number > 10000.0) {
        return JS_ThrowTypeError(context, "listItems: limit must be an integer in 1..10000");
      }
      payload.set("limit", json::Value::number(number));
      if (argc == 3) {
        options_value = argv[2];
        has_options = true;
      }
    }
  } else if (argc == 3) {
    options_value = argv[2];
    has_options = true;
  }
  return dispatch_control(context, argv[0], options_value, has_options,
                          "listItems(id[, limit[, options]])", "control.list.items",
                          std::move(payload), binding);
}

// control.tabSelect(id, index[, options]) -> { selected: true }
JSValue control_tab_select(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "tabSelect(id, index[, options])");
  double number = 0;
  if (!JS_IsNumber(argv[1]) || JS_ToFloat64(context, &number, argv[1]) ||
      !std::isfinite(number) || std::trunc(number) != number || number < 1.0 ||
      number > 1000000.0) {
    return JS_ThrowTypeError(context, "tabSelect: index must be an integer >= 1");
  }
  json::Value payload = json::Value::object();
  payload.set("index", json::Value::number(number));
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          "tabSelect(id, index)", "control.tab.select", std::move(payload),
                          binding);
}

// control.editCount(id[, options]) -> { lines }
JSValue control_edit_count(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "editCount(id[, options])");
  return dispatch_control(context, argv[0], argc == 2 ? argv[1] : JS_UNDEFINED, argc == 2,
                          "editCount(id)", "control.edit.count", json::Value::object(), binding);
}

// control.editCaret(id[, options]) -> { line, col }
JSValue control_edit_caret(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "editCaret(id[, options])");
  return dispatch_control(context, argv[0], argc == 2 ? argv[1] : JS_UNDEFINED, argc == 2,
                          "editCaret(id)", "control.edit.caret", json::Value::object(), binding);
}

// control.editLine(id, line[, options]) -> { text }
JSValue control_edit_line(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "editLine(id, line[, options])");
  double number = 0;
  if (!JS_IsNumber(argv[1]) || JS_ToFloat64(context, &number, argv[1]) ||
      !std::isfinite(number) || std::trunc(number) != number || number < 1.0 ||
      number > 1000000.0) {
    return JS_ThrowTypeError(context, "editLine: line must be an integer >= 1");
  }
  json::Value payload = json::Value::object();
  payload.set("line", json::Value::number(number));
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          "editLine(id, line)", "control.edit.line", std::move(payload), binding);
}

// control.editSelected(id[, options]) -> { text }
JSValue control_edit_selected(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                              void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "editSelected(id[, options])");
  return dispatch_control(context, argv[0], argc == 2 ? argv[1] : JS_UNDEFINED, argc == 2,
                          "editSelected(id)", "control.edit.selected", json::Value::object(),
                          binding);
}

// control.editPaste(id, text[, options]) -> { text }
JSValue control_edit_paste(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "editPaste(id, text[, options])");
  if (!JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context, "editPaste(id, text): text must be a string");
  }
  const char* text = JS_ToCString(context, argv[1]);
  if (!text) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  payload.set("text", json::Value::string(text));
  JS_FreeCString(context, text);
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          "editPaste(id, text)", "control.edit.paste", std::move(payload),
                          binding);
}

// control.setChecked(id, checked[, ensureActive[, options]])
// checked: true/false/-1 (toggle). Resolves { checked }.
JSValue control_set_checked(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 4)
    return JS_ThrowTypeError(context, "setChecked(id, checked[, ensureActive[, options]])");
  json::Value payload = json::Value::object();
  if (JS_IsBool(argv[1])) {
    payload.set("checked", json::Value::number(JS_ToBool(context, argv[1]) != 0 ? 1.0 : 0.0));
  } else {
    double number = 0;
    if (!JS_IsNumber(argv[1]) || JS_ToFloat64(context, &number, argv[1]) ||
        !std::isfinite(number) || std::trunc(number) != number || number < -1.0 ||
        number > 1.0) {
      return JS_ThrowTypeError(context, "setChecked: checked must be a boolean or -1, 0, 1");
    }
    payload.set("checked", json::Value::number(number));
  }
  // argv[2]: ensureActive bool, or the options object when no flag given.
  JSValueConst options_value = JS_UNDEFINED;
  bool has_options = false;
  if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
    if (JS_IsBool(argv[2])) {
      payload.set("ensureActive", json::Value::boolean(JS_ToBool(context, argv[2]) != 0));
      if (argc == 4) {
        options_value = argv[3];
        has_options = true;
      }
    } else if (is_plain_object(argv[2])) {
      if (argc > 3) {
        return JS_ThrowTypeError(
            context, "setChecked(id, checked[, ensureActive[, options]]): too many arguments");
      }
      options_value = argv[2];
      has_options = true;
    } else {
      return JS_ThrowTypeError(context, "setChecked: ensureActive must be a boolean");
    }
  } else if (argc == 4) {
    options_value = argv[3];
    has_options = true;
  }
  return dispatch_control(context, argv[0], options_value, has_options,
                          "setChecked(id, checked[, ensureActive[, options]])",
                          "control.setchecked", std::move(payload), binding);
}

// control.isChecked(id[, options]) -> { checked }
JSValue control_is_checked(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "isChecked(id[, options])");
  return dispatch_control(context, argv[0], argc == 2 ? argv[1] : JS_UNDEFINED, argc == 2,
                          "isChecked(id)", "control.ischecked", json::Value::object(), binding);
}

// control.showDropdown(id[, options]) / control.hideDropdown(id[, options])
JSValue control_show_dropdown(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                              void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "showDropdown(id[, options])");
  return dispatch_control(context, argv[0], argc == 2 ? argv[1] : JS_UNDEFINED, argc == 2,
                          "showDropdown(id)", "control.dropdown.show", json::Value::object(),
                          binding);
}

JSValue control_hide_dropdown(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                              void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "hideDropdown(id[, options])");
  return dispatch_control(context, argv[0], argc == 2 ? argv[1] : JS_UNDEFINED, argc == 2,
                          "hideDropdown(id)", "control.dropdown.hide", json::Value::object(),
                          binding);
}

// control.setStyle(id, {op, bits}[, options]) / setExStyle ditto.
// op is one of "+", "-", "^", "=" (AHK prefix rule).
JSValue control_set_style(JSContext* context, int argc, JSValueConst* argv, bool extended,
                          ControlModuleBinding* binding) {
  const char* signature =
      extended ? "setExStyle(id, {op, bits}[, options])" : "setStyle(id, {op, bits}[, options])";
  const char* type = extended ? "control.set.exstyle" : "control.set.style";
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "%s", signature);
  if (!is_plain_object(argv[1])) {
    return JS_ThrowTypeError(context, "%s: spec must be {op, bits}", signature);
  }
  JSValue op_value = JS_GetPropertyStr(context, argv[1], "op");
  if (JS_IsException(op_value)) return JS_EXCEPTION;
  bool op_ok = false;
  char op = '=';
  if (JS_IsString(op_value)) {
    const char* op_text = JS_ToCString(context, op_value);
    if (op_text) {
      if (op_text[0] != '\0' && op_text[1] == '\0' &&
          (op_text[0] == '+' || op_text[0] == '-' || op_text[0] == '^' ||
           op_text[0] == '=')) {
        op = op_text[0];
        op_ok = true;
      }
      JS_FreeCString(context, op_text);
    }
  }
  JS_FreeValue(context, op_value);
  if (!op_ok) {
    return JS_ThrowTypeError(context, "%s: op must be one of +, -, ^, =", signature);
  }
  JSValue bits_value = JS_GetPropertyStr(context, argv[1], "bits");
  if (JS_IsException(bits_value)) return JS_EXCEPTION;
  double bits_number = 0;
  if (!JS_IsNumber(bits_value) || JS_ToFloat64(context, &bits_number, bits_value) ||
      !std::isfinite(bits_number) || std::trunc(bits_number) != bits_number ||
      bits_number < 0.0 || bits_number > 4294967295.0) {
    JS_FreeValue(context, bits_value);
    return JS_ThrowTypeError(context, "%s: bits must be a uint32", signature);
  }
  JS_FreeValue(context, bits_value);
  json::Value payload = json::Value::object();
  payload.set("op", json::Value::string(std::string(1, op)));
  payload.set("bits", json::Value::number(bits_number));
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          signature, type, std::move(payload), binding);
}

JSValue control_set_style(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_set_style(context, argc, argv, false, binding);
}

JSValue control_set_ex_style(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                             void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_set_style(context, argc, argv, true, binding);
}

// SendKeys subset compiler: literal chars, ^!+# modifiers, and a fixed named
// key table. Everything else in braces ({Click}, {U+}, {Blind}, {ASC}, ...)
// is an explicit TypeError - the subset is documented, not guessed.
namespace send_keys {

void push_vk(json::Value& steps, std::uint32_t vk, bool down) {
  json::Value step = json::Value::object();
  step.set("vk", json::Value::number(static_cast<double>(vk)));
  step.set("down", json::Value::boolean(down));
  steps.push(std::move(step));
}

void push_char(json::Value& steps, std::uint32_t unit) {
  json::Value step = json::Value::object();
  step.set("char", json::Value::number(static_cast<double>(unit)));
  step.set("down", json::Value::boolean(true));
  steps.push(std::move(step));
}

bool named_vk(const std::string& name, std::uint32_t& vk) {
  if (name == "Enter") vk = VK_RETURN;
  else if (name == "Tab") vk = VK_TAB;
  else if (name == "Esc" || name == "Escape") vk = VK_ESCAPE;
  else if (name == "Backspace" || name == "BS") vk = VK_BACK;
  else if (name == "Delete" || name == "Del") vk = VK_DELETE;
  else if (name == "Home") vk = VK_HOME;
  else if (name == "End") vk = VK_END;
  else if (name == "Left") vk = VK_LEFT;
  else if (name == "Right") vk = VK_RIGHT;
  else if (name == "Up") vk = VK_UP;
  else if (name == "Down") vk = VK_DOWN;
  else if (name == "Space") vk = VK_SPACE;
  else if (name == "Insert" || name == "Ins") vk = VK_INSERT;
  else if (name == "PgUp") vk = VK_PRIOR;
  else if (name == "PgDn") vk = VK_NEXT;
  else return false;
  return true;
}

bool function_vk(const std::string& name, std::uint32_t& vk) {
  if (name.size() < 2 || name[0] != 'F') return false;
  int number = 0;
  for (std::size_t i = 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
    number = number * 10 + (name[i] - '0');
  }
  if (number < 1 || number > 24) return false;
  vk = static_cast<std::uint32_t>(VK_F1 + number - 1);
  return true;
}

bool modifier_vk(char mark, std::uint32_t& vk) {
  if (mark == '^') vk = VK_CONTROL;
  else if (mark == '!') vk = VK_MENU;
  else if (mark == '+') vk = VK_SHIFT;
  else if (mark == '#') vk = VK_LWIN;
  else return false;
  return true;
}

// Emits one logical unit (char or vk) wrapped in the pending modifiers.
void emit_unit(json::Value& steps, bool is_char, std::uint32_t code,
               const std::vector<std::uint32_t>& modifiers) {
  for (const auto mod : modifiers) push_vk(steps, mod, true);
  if (is_char) {
    push_char(steps, code);
  } else {
    push_vk(steps, code, true);
    push_vk(steps, code, false);
  }
  for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) push_vk(steps, *it, false);
}

bool compile(JSContext* context, const std::string& keys, json::Value& steps,
             std::string& error) {
  std::vector<std::uint32_t> modifiers;
  bool raw = false;
  std::size_t i = 0;
  const auto fail = [&](const std::string& message) {
    error = message;
    return false;
  };
  while (i < keys.size()) {
    const char c = keys[i];
    if (!raw && (c == '^' || c == '!' || c == '+' || c == '#')) {
      std::uint32_t vk = 0;
      modifier_vk(c, vk);
      modifiers.push_back(vk);
      ++i;
      continue;
    }
    if (!raw && c == '{') {
      const std::size_t end = keys.find('}', i + 1);
      if (end == std::string::npos) return fail("send: unterminated {…}");
      const std::string name = keys.substr(i + 1, end - i - 1);
      i = end + 1;
      if (name == "Text") {
        raw = true;
        continue;
      }
      std::uint32_t vk = 0;
      if (named_vk(name, vk) || function_vk(name, vk)) {
        emit_unit(steps, false, vk, modifiers);
        modifiers.clear();
        continue;
      }
      return fail("send: unsupported {" + name +
                  "} (supported: Enter Tab Esc Backspace Delete Home End "
                  "Left Right Up Down Space Insert PgUp PgDn F1-F24 Text)");
    }
    if (!raw && c == '}') {
      // A lone } is literal (AHK rule).
      emit_unit(steps, true, static_cast<std::uint32_t>(static_cast<unsigned char>(c)),
                modifiers);
      modifiers.clear();
      ++i;
      continue;
    }
    if (!raw && c >= 'A' && c <= 'Z') {
      // Uppercase implies Shift unless shift is already held.
      bool has_shift = false;
      for (const auto mod : modifiers) {
        if (mod == VK_SHIFT) has_shift = true;
      }
      if (!has_shift) modifiers.push_back(VK_SHIFT);
      emit_unit(steps, false, static_cast<std::uint32_t>(c), modifiers);
      modifiers.clear();
      ++i;
      continue;
    }
    if (!raw && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ')) {
      std::uint32_t vk = (c == ' ') ? VK_SPACE : static_cast<std::uint32_t>(c - (c >= 'a' ? 0 : 0));
      if (c >= 'a' && c <= 'z') vk = static_cast<std::uint32_t>(c - 'a' + 'A');
      emit_unit(steps, false, vk, modifiers);
      modifiers.clear();
      ++i;
      continue;
    }
    // Raw character (or {Text} mode): UTF-8 decoded to units below.
    emit_unit(steps, true, static_cast<std::uint32_t>(static_cast<unsigned char>(c)), modifiers);
    modifiers.clear();
    ++i;
  }
  if (!modifiers.empty()) return fail("send: trailing modifier with no key");
  (void)context;
  return true;
}

}  // namespace send_keys

// control.send(id, keys[, options]) -> { steps }
JSValue control_send(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "send(id, keys[, options])");
  if (!JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context, "send(id, keys): keys must be a string");
  }
  const char* keys_text = JS_ToCString(context, argv[1]);
  if (!keys_text) return JS_EXCEPTION;
  const std::string keys(keys_text);
  JS_FreeCString(context, keys_text);
  if (keys.empty() || keys.size() > 100000) {
    return JS_ThrowTypeError(context, "send: keys must be 1..100000 UTF-8 bytes");
  }
  // Decode UTF-8 to code points first so multibyte characters survive as one
  // unit; lone surrogates/invalid bytes are a contract error, not mojibake.
  std::vector<std::uint32_t> units;
  for (std::size_t i = 0; i < keys.size();) {
    const unsigned char lead = static_cast<unsigned char>(keys[i]);
    std::uint32_t unit = 0;
    std::size_t length = 0;
    if (lead < 0x80) {
      unit = lead;
      length = 1;
    } else if ((lead >> 5) == 0x6 && i + 1 < keys.size()) {
      unit = ((lead & 0x1F) << 6) | (keys[i + 1] & 0x3F);
      length = 2;
    } else if ((lead >> 4) == 0xE && i + 2 < keys.size()) {
      unit = ((lead & 0x0F) << 12) | ((keys[i + 1] & 0x3F) << 6) | (keys[i + 2] & 0x3F);
      length = 3;
    } else {
      return JS_ThrowTypeError(context, "send: keys must be valid UTF-8 (no astral planes)");
    }
    if (unit == 0 || unit > 0xFFFF) {
      return JS_ThrowTypeError(context, "send: keys must be valid UTF-8 (no astral planes)");
    }
    units.push_back(unit);
    i += length;
  }
  json::Value steps = json::Value::array();
  {
    // ASCII strings go through the Send-grammar compiler; non-ASCII strings
    // take raw char steps with no syntax (documented subset boundary).
    std::string error;
    auto fail = [&](const std::string& message) -> JSValue {
      JS_ThrowTypeError(context, "send: %s", message.c_str());
      return JS_EXCEPTION;
    };
    // ASCII fast path when the whole string is ASCII (the common case).
    bool ascii_only = true;
    for (const auto u : units) {
      if (u >= 0x80) ascii_only = false;
    }
    if (ascii_only) {
      std::string narrow;
      for (const auto u : units) narrow.push_back(static_cast<char>(u));
      if (!send_keys::compile(context, narrow, steps, error)) {
        return fail(error);
      }
    } else {
      // Non-ASCII: no syntax, every unit is a raw char step (modifiers and
      // braces have no meaning outside ASCII by design).
      for (const auto u : units) {
        if (u == '{' || u == '}' || u == '^' || u == '!' || u == '+' || u == '#') {
          return fail("non-ASCII keys support plain text only (no Send grammar)");
        }
      }
      for (const auto u : units) {
        send_keys::push_char(steps, u);
      }
    }
  }
  json::Value payload = json::Value::object();
  payload.set("steps", std::move(steps));
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          "send(id, keys)", "control.send", std::move(payload), binding);
}

// control.show(id[, options]) / control.hide(id[, options])
JSValue control_show_hide(JSContext* context, int argc, JSValueConst* argv, bool show,
                          ControlModuleBinding* binding) {
  const char* signature = show ? "show(id[, options])" : "hide(id[, options])";
  const char* type = show ? "control.show" : "control.hide";
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "%s", signature);
  return dispatch_control(context, argv[0], argc == 2 ? argv[1] : JS_UNDEFINED, argc == 2,
                          signature, type, json::Value::object(), binding);
}

JSValue control_show(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_show_hide(context, argc, argv, true, binding);
}

JSValue control_hide(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_show_hide(context, argc, argv, false, binding);
}

// control.move(id, {x?, y?, w?, h?}[, options]) - top-level-client coords.
JSValue control_move(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "move(id, rect[, options])");
  if (!is_plain_object(argv[1])) {
    return JS_ThrowTypeError(context, "move: rect must be an object with x, y, w, h");
  }
  json::Value payload = json::Value::object();
  bool any = false;
  for (const char* key : {"x", "y", "w", "h"}) {
    JSValue field = JS_GetPropertyStr(context, argv[1], key);
    if (JS_IsException(field)) return JS_EXCEPTION;
    if (!JS_IsUndefined(field) && !JS_IsNull(field)) {
      double number = 0;
      const double low = (key[0] == 'w' || key[0] == 'h') ? 1.0 : -2147483648.0;
      if (!JS_IsNumber(field) || JS_ToFloat64(context, &number, field) ||
          !std::isfinite(number) || std::trunc(number) != number || number < low ||
          number > 2147483647.0) {
        JS_FreeValue(context, field);
        return JS_ThrowTypeError(context, "move: rect.%s is out of range", key);
      }
      payload.set(key, json::Value::number(number));
      any = true;
    }
    JS_FreeValue(context, field);
  }
  if (!any) {
    return JS_ThrowTypeError(context, "move: rect needs at least one of x, y, w, h");
  }
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          "move(id, rect)", "control.move", std::move(payload), binding);
}

// control.setEnabled(id, enabled[, options])
JSValue control_set_enabled(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                            void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "setEnabled(id, enabled[, options])");
  if (!JS_IsBool(argv[1])) {
    return JS_ThrowTypeError(context, "setEnabled: enabled must be a boolean");
  }
  json::Value payload = json::Value::object();
  payload.set("enabled", json::Value::boolean(JS_ToBool(context, argv[1]) != 0));
  return dispatch_control(context, argv[0], argc == 3 ? argv[2] : JS_UNDEFINED, argc == 3,
                          "setEnabled(id, enabled)", "control.setenabled", std::move(payload),
                          binding);
}

// control.tabIndex(id[, options]) -> { index }
JSValue control_tab_index(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "tabIndex(id[, options])");
  return dispatch_control(context, argv[0], argc == 2 ? argv[1] : JS_UNDEFINED, argc == 2,
                          "tabIndex(id)", "control.tab.index", json::Value::object(), binding);
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

// control.classNN(id) -> ClassNN string (agrees with controls() by construction)
JSValue control_class_nn(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:control is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "classNN(id)");
  std::uint64_t id = 0;
  if (!strict_control_id(context, argv[0], id, "classNN(id)")) return JS_EXCEPTION;
  if (!binding->kernel->allows("windows.window.read")) {
    return throw_capability_error(context, "windows.window.read");
  }
  std::string out;
  if (const auto error = binding->service->control_class_nn(id, out); !error.ok()) {
    return throw_service_error(context, error);
  }
  JSValue result = JS_NewString(context, out.c_str());
  if (JS_IsException(result)) return JS_EXCEPTION;
  return result;
}

// control.getStyle(id) / control.getExStyle(id) -> uint32 style bits
JSValue control_style(JSContext* context, int argc, JSValueConst* argv, bool extended,
                      ControlModuleBinding* binding) {
  const char* signature = extended ? "getExStyle(id)" : "getStyle(id)";
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:control is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "%s", signature);
  std::uint64_t id = 0;
  if (!strict_control_id(context, argv[0], id, signature)) return JS_EXCEPTION;
  if (!binding->kernel->allows("windows.window.read")) {
    return throw_capability_error(context, "windows.window.read");
  }
  std::uint32_t bits = 0;
  rime::core::Error error =
      extended ? binding->service->control_get_ex_style(id, bits)
               : binding->service->control_get_style(id, bits);
  if (!error.ok()) {
    return throw_service_error(context, error);
  }
  return JS_NewFloat64(context, static_cast<double>(bits));
}

JSValue control_get_style(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_style(context, argc, argv, false, binding);
}

JSValue control_get_ex_style(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                             void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  return control_style(context, argc, argv, true, binding);
}

// control.focusedChild(windowId) -> control id or 0 (nothing focused inside)
JSValue control_focused_child(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                              void* opaque) {
  auto* binding = static_cast<ControlModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:control is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "focusedChild(windowId)");
  int64_t window_raw = 0;
  if (!js_int64_strict(context, argv[0], window_raw, "focusedChild(windowId)")) {
    return JS_EXCEPTION;
  }
  if (window_raw <= 0) {
    return JS_ThrowTypeError(context, "focusedChild(windowId): windowId must be positive");
  }
  if (!binding->kernel->allows("windows.window.read")) {
    return throw_capability_error(context, "windows.window.read");
  }
  std::uint64_t out = 0;
  if (const auto error = binding->service->control_focused_child(
          static_cast<std::uint64_t>(window_raw), out);
      !error.ok()) {
    return throw_service_error(context, error);
  }
  return JS_NewFloat64(context, static_cast<double>(out));
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
      !add("rect", control_rect, 1) || !add("dispose", control_dispose, 1) ||
      !add("listAdd", control_list_add, 2) || !add("listDelete", control_list_delete, 2) ||
      !add("listChoose", control_list_choose, 2) || !add("listFind", control_list_find, 2) ||
      !add("listIndex", control_list_index, 1) || !add("listChoice", control_list_choice, 1) ||
      !add("listItems", control_list_items, 1) || !add("tabSelect", control_tab_select, 2) ||
      !add("editCount", control_edit_count, 1) || !add("editCaret", control_edit_caret, 1) ||
      !add("editLine", control_edit_line, 2) || !add("editSelected", control_edit_selected, 1) ||
      !add("editPaste", control_edit_paste, 2) || !add("setChecked", control_set_checked, 2) ||
      !add("isChecked", control_is_checked, 1) || !add("show", control_show, 1) ||
      !add("hide", control_hide, 1) || !add("move", control_move, 2) ||
      !add("setEnabled", control_set_enabled, 2) || !add("tabIndex", control_tab_index, 1) ||
      !add("showDropdown", control_show_dropdown, 1) ||
      !add("hideDropdown", control_hide_dropdown, 1) ||
      !add("setStyle", control_set_style, 2) || !add("setExStyle", control_set_ex_style, 2) ||
      !add("send", control_send, 2) || !add("classNN", control_class_nn, 1) ||
      !add("getStyle", control_get_style, 1) || !add("getExStyle", control_get_ex_style, 1) ||
      !add("focusedChild", control_focused_child, 1)) {
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
