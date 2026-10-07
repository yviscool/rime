#include "rime/win32/js_ui.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

GuiModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<GuiModuleBinding*>(host->module_data("rime:ui"));
}

// Capability name checked by this module. Pointed at from
// contracts/registry/actions.json (capabilities.ui.create), so the literal
// lives here at the top of the file rather than inline in the bodies.
constexpr const char* kUiCreateCapability = "ui.create";

// Optional integer option (buttons, icon, width, ...): missing leaves the
// spec default, a fraction/non-finite/out-of-range value throws the same
// no-silent-truncation TypeError every numeric parameter in these modules
// follows. Range semantics (0..6 button sets, 1..20 tooltip index) stay with
// GuiService so there is exactly one validator for each rule.
bool optional_int_option(JSContext* context, JSValueConst object, const char* name,
                         std::optional<int>& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    std::int64_t raw = 0;
    const std::string label = std::string("options.") + name;
    if (!js_int64_strict(context, property, raw, label.c_str())) {
      JS_FreeValue(context, property);
      return false;
    }
    if (raw < static_cast<std::int64_t>((std::numeric_limits<int>::min)()) ||
        raw > static_cast<std::int64_t>((std::numeric_limits<int>::max)())) {
      JS_FreeValue(context, property);
      JS_ThrowRangeError(context, "options.%s is out of range", name);
      return false;
    }
    out = static_cast<int>(raw);
  }
  JS_FreeValue(context, property);
  return true;
}

// Optional number option (the AHK T timeout in seconds): any finite number,
// negative included, because window.cpp:1063-1066 clamps negatives to 0.1s
// rather than rejecting them.
bool optional_double_option(JSContext* context, JSValueConst object, const char* name,
                            double& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    if (!JS_IsNumber(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "options.%s must be a number", name);
      return false;
    }
    double number = 0;
    if (JS_ToFloat64(context, &number, property)) {
      JS_FreeValue(context, property);
      return false;
    }
    if (!std::isfinite(number)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "options.%s must be finite", name);
      return false;
    }
    out = number;
  }
  JS_FreeValue(context, property);
  return true;
}

// Optional boolean option (password, freeze, mute): a present non-boolean
// throws instead of being coerced, so `freeze: 0` cannot smuggle a number
// through as `false`.
bool optional_bool_option(JSContext* context, JSValueConst object, const char* name, bool& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    if (!JS_IsBool(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "options.%s must be a boolean", name);
      return false;
    }
    out = JS_ToBool(context, property);
  }
  JS_FreeValue(context, property);
  return true;
}

// Optional string option (title, ...): missing leaves the default; a present
// non-string throws rather than being stringified, matching how every string
// parameter in these modules fails early.
bool optional_string_option(JSContext* context, JSValueConst object, const char* name,
                            std::string& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    if (!JS_IsString(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "options.%s must be a string", name);
      return false;
    }
    const char* text = JS_ToCString(context, property);
    if (!text) {
      JS_FreeValue(context, property);
      return false;
    }
    out = text;
    JS_FreeCString(context, text);
  }
  JS_FreeValue(context, property);
  return true;
}

// Reads a positional string argument (or the default when absent).
bool positional_string_arg(JSContext* context, int argc, JSValueConst* argv, int index,
                           const char* name, const char* signature, std::string& out) {
  if (index >= argc) return true;
  if (!JS_IsString(argv[index])) {
    JS_ThrowTypeError(context, "%s: %s must be a string", signature, name);
    return false;
  }
  const char* text = JS_ToCString(context, argv[index]);
  if (!text) return false;
  out = text;
  JS_FreeCString(context, text);
  return true;
}

bool parse_options_object(JSContext* context, JSValueConst value, ActionOptions& action) {
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  if (!JS_IsObject(value)) {
    JS_ThrowTypeError(context, "options must be an object");
    return false;
  }
  return parse_action_options(context, value, action);
}

// The trailing options object plus its feature keys: present only when the
// caller passed one, so feature parsers can read props off it.
JSValueConst options_at(JSContext* context, int argc, JSValueConst* argv, int index,
                        const char* signature, bool& present) {
  present = index < argc;
  if (!present) return JS_UNDEFINED;
  if (JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) {
    present = false;
    return JS_UNDEFINED;
  }
  if (!JS_IsObject(argv[index])) {
    JS_ThrowTypeError(context, "%s: options must be an object", signature);
    return JS_EXCEPTION;
  }
  return argv[index];
}

// AHK MsgBox: msgBox(text?, options?) -> Promise<"OK" | "Yes" | ... | "Timeout">.
// title/buttons/icon/defaultIndex/timeout are AHK's Options string pieces
// spelled as object keys; the button set and the returned word are
// script2.cpp:1008-1045's. Capability ui.create is read inside the worker
// body, so a denied call never touches the dialog; no Action is built.
JSValue ui_msg_box(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  GuiModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:ui is not wired");
  }
  if (argc > 2) return JS_ThrowTypeError(context, "msgBox(text?, options?)");
  GuiService::MsgBoxSpec spec;
  if (!positional_string_arg(context, argc, argv, 0, "text", "msgBox(text?, options?)",
                             spec.text)) {
    return JS_EXCEPTION;
  }
  bool has_options = false;
  JSValueConst options = options_at(context, argc, argv, 1, "msgBox(text?, options?)",
                                    has_options);
  if (JS_IsException(options)) return JS_EXCEPTION;
  ActionOptions action;
  if (has_options) {
    std::optional<int> buttons;
    std::optional<int> icon;
    std::optional<int> default_index;
    if (!optional_string_option(context, options, "title", spec.title) ||
        !optional_int_option(context, options, "buttons", buttons) ||
        !optional_int_option(context, options, "icon", icon) ||
        !optional_int_option(context, options, "defaultIndex", default_index) ||
        !optional_double_option(context, options, "timeout", spec.timeout_seconds) ||
        !parse_action_options(context, options, action)) {
      return JS_EXCEPTION;
    }
    if (buttons) spec.buttons = *buttons;
    if (icon) spec.icon = static_cast<unsigned int>(*icon);
    if (default_index) spec.default_index = *default_index;
  }
  GuiService* service = binding->service;
  rime::action::Kernel* kernel = binding->kernel;
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  const std::uint64_t cancellation_id = action.cancellation_id;
  const std::int64_t deadline = wait_deadline_unix_ms(action.deadline_ms);
  return start_async(
      context,
      [service, kernel, host, cancellation_id, deadline, spec]() -> AsyncOutcome {
        if (!kernel->allows(kUiCreateCapability)) {
          return capability_denied(kUiCreateCapability);
        }
        rime::core::CancellationToken cancel;
        if (cancellation_id != 0) cancel = host->cancellation_token(cancellation_id);
        std::string result;
        if (const auto error = service->msg_box(spec, result, deadline, cancel); !error.ok()) {
          return async_failure(error);
        }
        return async_success(json::stringify(json::Value::string(result)));
      },
      cancellation_id);
}

// AHK InputBox: inputBox(prompt?, options?) -> Promise<{value, result}>.
// The edit contents come back even on Cancel/Timeout (InputBox.cpp:148-173);
// result is "OK", "Cancel" or "Timeout".
JSValue ui_input_box(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  GuiModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:ui is not wired");
  }
  if (argc > 2) return JS_ThrowTypeError(context, "inputBox(prompt?, options?)");
  GuiService::InputBoxSpec spec;
  if (!positional_string_arg(context, argc, argv, 0, "prompt", "inputBox(prompt?, options?)",
                             spec.prompt)) {
    return JS_EXCEPTION;
  }
  bool has_options = false;
  JSValueConst options = options_at(context, argc, argv, 1, "inputBox(prompt?, options?)",
                                    has_options);
  if (JS_IsException(options)) return JS_EXCEPTION;
  ActionOptions action;
  if (has_options) {
    std::optional<int> width;
    std::optional<int> height;
    std::optional<int> x;
    std::optional<int> y;
    if (!optional_string_option(context, options, "title", spec.title) ||
        !optional_string_option(context, options, "value", spec.default_value) ||
        !optional_bool_option(context, options, "password", spec.password) ||
        !optional_int_option(context, options, "width", width) ||
        !optional_int_option(context, options, "height", height) ||
        !optional_int_option(context, options, "x", x) ||
        !optional_int_option(context, options, "y", y) ||
        !optional_double_option(context, options, "timeout", spec.timeout_seconds) ||
        !parse_action_options(context, options, action)) {
      return JS_EXCEPTION;
    }
    if (width) spec.width = *width;
    if (height) spec.height = *height;
    if (x) spec.x = *x;
    if (y) spec.y = *y;
  }
  GuiService* service = binding->service;
  rime::action::Kernel* kernel = binding->kernel;
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  const std::uint64_t cancellation_id = action.cancellation_id;
  const std::int64_t deadline = wait_deadline_unix_ms(action.deadline_ms);
  return start_async(
      context,
      [service, kernel, host, cancellation_id, deadline, spec]() -> AsyncOutcome {
        if (!kernel->allows(kUiCreateCapability)) {
          return capability_denied(kUiCreateCapability);
        }
        rime::core::CancellationToken cancel;
        if (cancellation_id != 0) cancel = host->cancellation_token(cancellation_id);
        GuiService::InputBoxResult out;
        if (const auto error = service->input_box(spec, out, deadline, cancel); !error.ok()) {
          return async_failure(error);
        }
        json::Value payload = json::Value::object();
        payload.set("value", json::Value::string(out.value));
        payload.set("result", json::Value::string(out.result));
        return async_success(json::stringify(payload));
      },
      cancellation_id);
}

// AHK ToolTip: toolTip(text, options?) -> Promise<null>. `index` is AHK's
// WhichTip (1..20); x/y default to cursor+16 when omitted. Blank text
// destroys the tooltip. No HWND return (gui-menu.md §0.4).
JSValue ui_tool_tip(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  GuiModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:ui is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "toolTip(text, options?)");
  std::string text;
  if (!positional_string_arg(context, argc, argv, 0, "text", "toolTip(text, options?)", text)) {
    return JS_EXCEPTION;
  }
  bool has_options = false;
  JSValueConst options =
      options_at(context, argc, argv, 1, "toolTip(text, options?)", has_options);
  if (JS_IsException(options)) return JS_EXCEPTION;
  ActionOptions action;
  int which = 1;
  std::optional<int> x;
  std::optional<int> y;
  if (has_options) {
    std::optional<int> index;
    if (!optional_int_option(context, options, "x", x) ||
        !optional_int_option(context, options, "y", y) ||
        !optional_int_option(context, options, "index", index) ||
        !parse_action_options(context, options, action)) {
      return JS_EXCEPTION;
    }
    if (index) which = *index;
  }
  GuiService* service = binding->service;
  rime::action::Kernel* kernel = binding->kernel;
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  const std::uint64_t cancellation_id = action.cancellation_id;
  const std::int64_t deadline = wait_deadline_unix_ms(action.deadline_ms);
  return start_async(
      context,
      [service, kernel, host, cancellation_id, deadline, text, x, y, which]() -> AsyncOutcome {
        if (!kernel->allows(kUiCreateCapability)) {
          return capability_denied(kUiCreateCapability);
        }
        rime::core::CancellationToken cancel;
        if (cancellation_id != 0) cancel = host->cancellation_token(cancellation_id);
        if (const auto error = service->tool_tip(text, x, y, which, deadline, cancel);
            !error.ok()) {
          return async_failure(error);
        }
        return async_success("null");
      },
      cancellation_id);
}

// AHK TraySetIcon: traySetIcon(file, options?) -> Promise<null>. "" or "*"
// restores the standard icon; iconNumber 0 means 1; `freeze` present sets
// AHK's frozen flag (its mere presence is the "given" signal, so
// `freeze: false` records a deliberate unfreeze).
JSValue ui_tray_set_icon(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  GuiModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:ui is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "traySetIcon(file, options?)");
  std::string file;
  if (!positional_string_arg(context, argc, argv, 0, "file", "traySetIcon(file, options?)",
                             file)) {
    return JS_EXCEPTION;
  }
  bool has_options = false;
  JSValueConst options =
      options_at(context, argc, argv, 1, "traySetIcon(file, options?)", has_options);
  if (JS_IsException(options)) return JS_EXCEPTION;
  ActionOptions action;
  int icon_number = 0;
  bool freeze = false;
  bool freeze_given = false;
  if (has_options) {
    std::optional<int> number;
    if (!optional_int_option(context, options, "iconNumber", number) ||
        !optional_bool_option(context, options, "freeze", freeze) ||
        !parse_action_options(context, options, action)) {
      return JS_EXCEPTION;
    }
    if (number) icon_number = *number;
    JSValue present = JS_GetPropertyStr(context, options, "freeze");
    if (JS_IsException(present)) return JS_EXCEPTION;
    freeze_given = !JS_IsUndefined(present) && !JS_IsNull(present);
    JS_FreeValue(context, present);
  }
  GuiService* service = binding->service;
  rime::action::Kernel* kernel = binding->kernel;
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  const std::uint64_t cancellation_id = action.cancellation_id;
  const std::int64_t deadline = wait_deadline_unix_ms(action.deadline_ms);
  return start_async(
      context,
      [service, kernel, host, cancellation_id, deadline, file, icon_number, freeze,
       freeze_given]() -> AsyncOutcome {
        if (!kernel->allows(kUiCreateCapability)) {
          return capability_denied(kUiCreateCapability);
        }
        rime::core::CancellationToken cancel;
        if (cancellation_id != 0) cancel = host->cancellation_token(cancellation_id);
        if (const auto error =
                service->tray_set_icon(file, icon_number, freeze, freeze_given, deadline, cancel);
            !error.ok()) {
          return async_failure(error);
        }
        return async_success("null");
      },
      cancellation_id);
}

// AHK TrayTip: trayTip(text, options?) -> Promise<null>. `title` rides in
// options; `icon` is AHK's dwInfoFlags (0x10 error, 0x20 warning, 0x40
// info); `mute` maps to NIIF_NOSOUND. Empty text with a title shows a
// title-only balloon (script2.cpp:124-126).
JSValue ui_tray_tip(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  GuiModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:ui is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "trayTip(text, options?)");
  std::string text;
  if (!positional_string_arg(context, argc, argv, 0, "text", "trayTip(text, options?)", text)) {
    return JS_EXCEPTION;
  }
  bool has_options = false;
  JSValueConst options =
      options_at(context, argc, argv, 1, "trayTip(text, options?)", has_options);
  if (JS_IsException(options)) return JS_EXCEPTION;
  ActionOptions action;
  std::string title;
  unsigned int info_flags = 0;
  bool mute = false;
  if (has_options) {
    std::optional<int> icon;
    if (!optional_string_option(context, options, "title", title) ||
        !optional_int_option(context, options, "icon", icon) ||
        !optional_bool_option(context, options, "mute", mute) ||
        !parse_action_options(context, options, action)) {
      return JS_EXCEPTION;
    }
    if (icon) info_flags = static_cast<unsigned int>(*icon);
  }
  GuiService* service = binding->service;
  rime::action::Kernel* kernel = binding->kernel;
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  const std::uint64_t cancellation_id = action.cancellation_id;
  const std::int64_t deadline = wait_deadline_unix_ms(action.deadline_ms);
  return start_async(
      context,
      [service, kernel, host, cancellation_id, deadline, text, title, info_flags,
       mute]() -> AsyncOutcome {
        if (!kernel->allows(kUiCreateCapability)) {
          return capability_denied(kUiCreateCapability);
        }
        rime::core::CancellationToken cancel;
        if (cancellation_id != 0) cancel = host->cancellation_token(cancellation_id);
        if (const auto error =
                service->tray_tip(text, title, info_flags, mute, deadline, cancel);
            !error.ok()) {
          return async_failure(error);
        }
        return async_success("null");
      },
      cancellation_id);
}

int ui_module_init(JSContext* context, JSModuleDef* module) {
  GuiModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:ui requires a gui module binding");
    return -1;
  }
  JSValue ui = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue fn = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(fn)) {
      JS_FreeValue(context, ui);
      return false;
    }
    // JS_SetPropertyStr consumes `fn` on both success and failure.
    if (JS_SetPropertyStr(context, ui, name, fn) < 0) {
      JS_FreeValue(context, ui);
      return false;
    }
    return true;
  };
  if (!add("msgBox", ui_msg_box, 1) || !add("inputBox", ui_input_box, 1) ||
      !add("toolTip", ui_tool_tip, 1) || !add("traySetIcon", ui_tray_set_icon, 1) ||
      !add("trayTip", ui_tray_tip, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "ui", ui);
}

JSModuleDef* create_ui_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:ui", ui_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "ui") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const GuiModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:ui requires a gui service and kernel"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_ui_module(rime::js::Host& host, GuiModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:ui", binding);
  if (const auto error = host.modules().add_native(
          "rime:ui", [](JSContext* context) { return create_ui_module(context); });
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error register_ui_module(rime::js::Runtime& runtime, GuiModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:ui", [](JSContext* context) { return create_ui_module(context); }, binding);
}

}  // namespace rime::win32
