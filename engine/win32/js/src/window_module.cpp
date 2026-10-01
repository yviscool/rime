#include "rime/win32/js_window.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

constexpr const char* kWindowReadCapability = "windows.window.read";
constexpr const char* kWindowWriteCapability = "windows.window.write";

WindowModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<WindowModuleBinding*>(host->module_data("rime:window"));
}

// Reads an optional string property; returns false (with a TypeError raised)
// when present but not a string. Missing/undefined/null leaves `out`
// untouched.
bool optional_string(JSContext* context, JSValueConst object, const char* name, std::string& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    if (!JS_IsString(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "%s must be a string", name);
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

bool optional_bool(JSContext* context, JSValueConst object, const char* name, bool& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    if (!JS_IsBool(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "%s must be a boolean", name);
      return false;
    }
    out = JS_ToBool(context, property);
  }
  JS_FreeValue(context, property);
  return true;
}

// Parses the optional window query object shared by the read functions:
// {title, matchMode, ahkClass, ahkExe, ahkId, includeHidden, active} plus the
// ActionOptions fields (deadlineMs/cancellationId) consumed separately.
bool parse_window_query(JSContext* context, JSValueConst value, WindowQuery& out) {
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  if (!JS_IsObject(value)) {
    JS_ThrowTypeError(context, "options must be an object");
    return false;
  }
  if (!optional_string(context, value, "title", out.title) ||
      !optional_string(context, value, "ahkClass", out.class_name) ||
      !optional_string(context, value, "ahkExe", out.process_name) ||
      !optional_bool(context, value, "includeHidden", out.include_hidden) ||
      !optional_bool(context, value, "active", out.active)) {
    return false;
  }
  JSValue mode = JS_GetPropertyStr(context, value, "matchMode");
  if (JS_IsException(mode)) return false;
  if (!JS_IsUndefined(mode) && !JS_IsNull(mode)) {
    if (!JS_IsString(mode)) {
      JS_FreeValue(context, mode);
      JS_ThrowTypeError(context, "matchMode must be 'exact' or 'contains'");
      return false;
    }
    const char* text = JS_ToCString(context, mode);
    if (!text) {
      JS_FreeValue(context, mode);
      return false;
    }
    const std::string mode_text(text);
    JS_FreeCString(context, text);
    if (mode_text == "exact") {
      out.exact_title = true;
    } else if (mode_text != "contains") {
      JS_FreeValue(context, mode);
      JS_ThrowTypeError(context, "matchMode must be 'exact' or 'contains'");
      return false;
    }
  }
  JS_FreeValue(context, mode);
  JSValue ahk_id = JS_GetPropertyStr(context, value, "ahkId");
  if (JS_IsException(ahk_id)) return false;
  if (!JS_IsUndefined(ahk_id) && !JS_IsNull(ahk_id)) {
    if (JS_IsNumber(ahk_id)) {
      int64_t raw = 0;
      if (!js_int64_strict(context, ahk_id, raw, "ahkId")) {
        JS_FreeValue(context, ahk_id);
        return false;
      }
      if (raw <= 0) {
        JS_FreeValue(context, ahk_id);
        JS_ThrowTypeError(context, "ahkId must be a positive window id");
        return false;
      }
      out.id = static_cast<std::uint64_t>(raw);
    } else if (JS_IsString(ahk_id)) {
      const char* text = JS_ToCString(context, ahk_id);
      if (!text) {
        JS_FreeValue(context, ahk_id);
        return false;
      }
      const std::string id_text(text);
      JS_FreeCString(context, text);
      // Strict digits via from_chars (same rule as window_executor's
      // parse_window_id): rejects empty/non-digits/overflow, and "0"/"00"
      // parse to 0 so they are refused like numeric 0.
      std::uint64_t parsed = 0;
      const char* begin = id_text.data();
      const char* end = begin + id_text.size();
      const auto converted = std::from_chars(begin, end, parsed);
      if (converted.ec != std::errc{} || converted.ptr != end || parsed == 0) {
        JS_FreeValue(context, ahk_id);
        JS_ThrowTypeError(context, "ahkId must be a positive window id");
        return false;
      }
      out.id = parsed;
    } else {
      JS_FreeValue(context, ahk_id);
      JS_ThrowTypeError(context, "ahkId must be a window id");
      return false;
    }
  }
  JS_FreeValue(context, ahk_id);
  return true;
}

// Shared read body: enforces windows.window.read through the same policy the
// kernel uses, then resolves the query on the UI lane.
JSValue run_window_read(JSContext* context, std::uint64_t cancellation_id,
                        std::uint64_t deadline_ms, WindowQuery query, bool use_query) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  rime::action::Kernel* kernel = binding->kernel;
  WindowService* service = binding->service;
  const auto timeout = std::chrono::milliseconds(deadline_ms);
  return start_async(
      context,
      [kernel, service, query, use_query, timeout]() -> AsyncOutcome {
        // Ownership: kernel/service (raw) outlive the host; the task always
        // settles its promise, so no outcome is dropped.
        if (!kernel->allows(kWindowReadCapability)) {
          return async_failure("capability_denied",
                               std::string("required capability was not granted: ") +
                                   kWindowReadCapability);
        }
        if (use_query) {
          std::vector<WindowInfo> windows;
          if (const auto error = service->query(query, windows, timeout); !error.ok()) {
            return async_failure(error);
          }
          json::Value array = json::Value::array();
          for (const auto& window : windows) array.push(window_info_json(window));
          return async_success(json::stringify(array));
        }
        std::vector<WindowInfo> windows;
        if (const auto error = service->list(windows, timeout); !error.ok()) {
          return async_failure(error);
        }
        json::Value array = json::Value::array();
        for (const auto& window : windows) array.push(window_info_json(window));
        return async_success(json::stringify(array));
      },
      cancellation_id);
}

JSValue windows_list(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  WindowQuery query;
  ActionOptions options;
  if (argc > 1) return JS_ThrowTypeError(context, "list(options?)");
  if (argc == 1) {
    if (!parse_window_query(context, argv[0], query) ||
        !parse_action_options(context, argv[0], options)) {
      return JS_EXCEPTION;
    }
  }
  const bool use_query =
      query.active || !query.title.empty() || !query.class_name.empty() ||
      !query.process_name.empty() || query.id != 0 || query.include_hidden;
  return run_window_read(context, options.cancellation_id, options.deadline_ms, query, use_query);
}

JSValue windows_active(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "active(options?)");
  ActionOptions options;
  // NOTE: unknown options fields are ignored here; only the shared
  // ActionOptions (deadlineMs/cancellationId/...) are consumed.
  if (argc == 1 && !parse_action_options(context, argv[0], options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  WindowService* service = binding->service;
  const auto timeout = std::chrono::milliseconds(options.deadline_ms);
  return start_async(
      context,
      [kernel, service, timeout]() -> AsyncOutcome {
        if (!kernel->allows(kWindowReadCapability)) {
          return async_failure("capability_denied",
                               std::string("required capability was not granted: ") +
                                   kWindowReadCapability);
        }
        std::optional<WindowInfo> active;
        if (const auto error = service->active(active, timeout); !error.ok()) {
          return async_failure(error);
        }
        if (!active.has_value()) return async_success("null");
        return async_success(json::stringify(window_info_json(*active)));
      },
      options.cancellation_id);
}

JSValue windows_info(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "info(windowId, options?)");
  int64_t raw_id = 0;
  if (!js_int64_strict(context, argv[0], raw_id, "info(windowId)")) return JS_EXCEPTION;
  if (raw_id <= 0) return JS_ThrowTypeError(context, "info(windowId): id must be positive");
  const std::uint64_t window_id = static_cast<std::uint64_t>(raw_id);
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  WindowService* service = binding->service;
  const auto timeout = std::chrono::milliseconds(options.deadline_ms);
  return start_async(
      context,
      [kernel, service, window_id, timeout]() -> AsyncOutcome {
        if (!kernel->allows(kWindowReadCapability)) {
          return async_failure("capability_denied",
                               std::string("required capability was not granted: ") +
                                   kWindowReadCapability);
        }
        WindowInfo info;
        if (const auto error = service->info(window_id, info, timeout); !error.ok()) {
          return async_failure(error);
        }
        return async_success(json::stringify(window_info_json(info)));
      },
      options.cancellation_id);
}

// Normalizes a window target argument: positive number or "active".
bool parse_window_target(JSContext* context, JSValueConst value, std::string& out) {
  if (JS_IsString(value)) {
    const char* text = JS_ToCString(context, value);
    if (!text) return false;
    out = text;
    JS_FreeCString(context, text);
    if (out != "active" && out.find_first_not_of("0123456789") != std::string::npos) {
      JS_ThrowTypeError(context, "target must be an id or 'active'");
      return false;
    }
    if (out != "active" && out.empty()) {
      JS_ThrowTypeError(context, "target id must be positive");
      return false;
    }
    if (out != "active") {
      // Numeric strings go through from_chars so "0"/"00" are refused like
      // numeric 0 instead of being passed through verbatim.
      std::uint64_t parsed = 0;
      const char* begin = out.data();
      const char* end = begin + out.size();
      const auto converted = std::from_chars(begin, end, parsed);
      if (converted.ec != std::errc{} || converted.ptr != end || parsed == 0) {
        JS_ThrowTypeError(context, "target id must be positive");
        return false;
      }
    }
    return true;
  }
  if (JS_IsNumber(value)) {
    int64_t raw_id = 0;
    if (!js_int64_strict(context, value, raw_id, "target")) return false;
    if (raw_id <= 0) {
      JS_ThrowTypeError(context, "target id must be positive");
      return false;
    }
    out = std::to_string(raw_id);
    return true;
  }
  JS_ThrowTypeError(context, "target must be an id or 'active'");
  return false;
}

// Shared mutation body: builds the Action (capability windows.window.write)
// with the parsed options and executes it through the kernel.
JSValue run_window_mutation(JSContext* context, int argc, JSValueConst* argv,
                            const char* function_name, const char* action_type,
                            int required_args) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc < required_args) {
    return JS_ThrowTypeError(context, "%s(target[, position][, options?])", function_name);
  }
  std::string target_text;
  if (!parse_window_target(context, argv[0], target_text)) return JS_EXCEPTION;

  int cursor = 1;
  std::string placement;
  if (std::string_view(action_type) == "window.move") {
    if (argc < 2) return JS_ThrowTypeError(context, "%s(target, position)", function_name);
    if (!JS_IsString(argv[1])) {
      return JS_ThrowTypeError(context, "%s(target, position): position must be a string",
                               function_name);
    }
    const char* position_text = JS_ToCString(context, argv[1]);
    if (!position_text) return JS_EXCEPTION;
    placement = position_text;
    JS_FreeCString(context, position_text);
    if (placement.empty()) {
      return JS_ThrowTypeError(context, "%s(target, position): position must not be empty",
                               function_name);
    }
    cursor = 2;
  }
  if (argc > cursor + 1) {
    return JS_ThrowTypeError(context, "%s(target[, position][, options?])", function_name);
  }
  ActionOptions options;
  if (argc > cursor && !parse_action_options(context, argv[cursor], options)) return JS_EXCEPTION;

  json::Value payload = json::Value::object();
  if (!placement.empty()) payload.set("position", json::Value::string(placement));
  rime::action::Kernel* kernel = binding->kernel;
  auto action = make_action(*binding->next_action_id, "rime:window", action_type,
                            kWindowWriteCapability, {"window", std::move(target_text)},
                            json::stringify(payload), options);
  return run_action(context, *kernel, std::move(action), options.cancellation_id);
}

JSValue windows_move(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  // NOTE: required_args stays 1 here; the (target, position) arity for move is
  // enforced by the window.move branch inside run_window_mutation.
  return run_window_mutation(context, argc, argv, "move", "window.move", 1);
}

JSValue windows_focus(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void*) {
  return run_window_mutation(context, argc, argv, "focus", "window.focus", 1);
}

JSValue windows_close(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void*) {
  return run_window_mutation(context, argc, argv, "close", "window.close", 1);
}

JSValue windows_hide(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  return run_window_mutation(context, argc, argv, "hide", "window.hide", 1);
}

JSValue windows_show(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  return run_window_mutation(context, argc, argv, "show", "window.show", 1);
}

JSValue windows_minimize(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  return run_window_mutation(context, argc, argv, "minimize", "window.minimize", 1);
}

JSValue windows_maximize(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  return run_window_mutation(context, argc, argv, "maximize", "window.maximize", 1);
}

JSValue windows_restore(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void*) {
  return run_window_mutation(context, argc, argv, "restore", "window.restore", 1);
}

int window_module_init(JSContext* context, JSModuleDef* module) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    JS_ThrowInternalError(context, "rime:window requires a window module binding");
    return -1;
  }
  JSValue windows = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(value)) {
      JS_FreeValue(context, windows);
      return false;
    }
    // JS_SetPropertyStr consumes `value` on both success and failure.
    if (JS_SetPropertyStr(context, windows, name, value) < 0) {
      JS_FreeValue(context, windows);
      return false;
    }
    return true;
  };
  if (!add("list", windows_list, 0) || !add("active", windows_active, 0) ||
      !add("info", windows_info, 1) || !add("move", windows_move, 2) ||
      !add("focus", windows_focus, 1) || !add("close", windows_close, 1) ||
      !add("hide", windows_hide, 1) || !add("show", windows_show, 1) ||
      !add("minimize", windows_minimize, 1) || !add("maximize", windows_maximize, 1) ||
      !add("restore", windows_restore, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "windows", windows);
}

JSModuleDef* create_window_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:window", window_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "windows") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const WindowModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:window requires a window service, kernel and action id source"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_window_module(rime::js::Host& host, WindowModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:window", binding);
  host.modules().add_native("rime:window",
                            [](JSContext* context) { return create_window_module(context); });
  return rime::core::Error::none();
}

rime::core::Error register_window_module(rime::js::Runtime& runtime,
                                         WindowModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:window", [](JSContext* context) { return create_window_module(context); }, binding);
}

}  // namespace rime::win32
