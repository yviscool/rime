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

// Tri-state variant for fields whose absence means "follow the global
// setting" (DetectHiddenWindows): missing/undefined/null leaves `out` unset.
bool optional_flag(JSContext* context, JSValueConst object, const char* name,
                   std::optional<bool>& out) {
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

// Validates every regex pattern a query carries when the effective match mode
// is RegEx (explicit matchMode, else the service's SetTitleMatchMode), so a
// bad pattern fails here as a synchronous TypeError instead of surfacing as
// an async query rejection.
bool validate_regex_fields(JSContext* context, const WindowQuery& query) {
  if (!query.title.empty()) {
    if (const auto error = validate_window_regex(query.title); !error.ok()) {
      JS_ThrowTypeError(context, "%s", error.message.c_str());
      return false;
    }
  }
  if (!query.class_name.empty()) {
    if (const auto error = validate_window_regex(query.class_name); !error.ok()) {
      JS_ThrowTypeError(context, "%s", error.message.c_str());
      return false;
    }
  }
  if (!query.process_name.empty()) {
    if (const auto error = validate_window_regex(query.process_name); !error.ok()) {
      JS_ThrowTypeError(context, "%s", error.message.c_str());
      return false;
    }
  }
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
      !optional_flag(context, value, "includeHidden", out.include_hidden) ||
      !optional_bool(context, value, "active", out.active)) {
    return false;
  }
  JSValue mode = JS_GetPropertyStr(context, value, "matchMode");
  if (JS_IsException(mode)) return false;
  if (!JS_IsUndefined(mode) && !JS_IsNull(mode)) {
    if (!JS_IsString(mode)) {
      JS_FreeValue(context, mode);
      JS_ThrowTypeError(context, "matchMode must be 'startswith', 'contains', 'exact' or 'regex'");
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
      out.title_match_mode = TitleMatchMode::Exact;
    } else if (mode_text == "contains") {
      out.title_match_mode = TitleMatchMode::Contains;
    } else if (mode_text == "startswith") {
      out.title_match_mode = TitleMatchMode::StartsWith;
    } else if (mode_text == "regex") {
      out.title_match_mode = TitleMatchMode::Regex;
    } else {
      JS_FreeValue(context, mode);
      JS_ThrowTypeError(context, "matchMode must be 'startswith', 'contains', 'exact' or 'regex'");
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
  // Effective RegEx mode: the explicit matchMode wins, else the service's
  // global SetTitleMatchMode decides whether these patterns are compiled.
  TitleMatchMode effective = TitleMatchMode::Contains;
  if (out.title_match_mode.has_value()) {
    effective = *out.title_match_mode;
  } else if (const WindowModuleBinding* binding = binding_of(context);
             binding != nullptr && binding->service != nullptr) {
    effective = binding->service->settings().title_match_mode;
  }
  if (effective == TitleMatchMode::Regex && !validate_regex_fields(context, out)) {
    return false;
  }
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
      !query.process_name.empty() || query.id != 0 || query.include_hidden.has_value() ||
      query.title_match_mode.has_value();
  return run_window_read(context, options.cancellation_id, options.deadline_ms, query, use_query);
}

// Shared body for the existence probes: capability check on the JS thread,
// then the probe resolves on the UI lane through the service.
JSValue run_window_probe(JSContext* context, std::uint64_t cancellation_id,
                         std::uint64_t deadline_ms, const WindowQuery& query, bool active_probe) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  rime::action::Kernel* kernel = binding->kernel;
  WindowService* service = binding->service;
  const auto timeout = std::chrono::milliseconds(deadline_ms);
  return start_async(
      context,
      [kernel, service, query, active_probe, timeout]() -> AsyncOutcome {
        if (!kernel->allows(kWindowReadCapability)) {
          return async_failure("capability_denied",
                               std::string("required capability was not granted: ") +
                                   kWindowReadCapability);
        }
        bool found = false;
        const auto error = active_probe ? service->matches_active(query, found, timeout)
                                        : service->exists(query, found, timeout);
        if (!error.ok()) return async_failure(error);
        return async_success(found ? "true" : "false");
      },
      cancellation_id);
}

// WinExist: exists(options) resolves whether any window matches the query.
JSValue windows_exists(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  WindowQuery query;
  ActionOptions options;
  if (argc > 1) return JS_ThrowTypeError(context, "exists(options?)");
  if (argc == 1) {
    if (!parse_window_query(context, argv[0], query) ||
        !parse_action_options(context, argv[0], options)) {
      return JS_EXCEPTION;
    }
  }
  return run_window_probe(context, options.cancellation_id, options.deadline_ms, query, false);
}

// WinActive: isActive(options) resolves whether the foreground window
// matches the query (the service ignores query.active for this probe).
JSValue windows_is_active(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  WindowQuery query;
  ActionOptions options;
  if (argc > 1) return JS_ThrowTypeError(context, "isActive(options?)");
  if (argc == 1) {
    if (!parse_window_query(context, argv[0], query) ||
        !parse_action_options(context, argv[0], options)) {
      return JS_EXCEPTION;
    }
  }
  return run_window_probe(context, options.cancellation_id, options.deadline_ms, query, true);
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

// WinGetControls/WinGetControlsHwnd: child controls with stable ids and
// AHK ClassNN names. One JSON array feeds both (hwnd = entry.id).
JSValue windows_controls(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "controls(windowId, options?)");
  int64_t raw_id = 0;
  if (!js_int64_strict(context, argv[0], raw_id, "controls(windowId)")) return JS_EXCEPTION;
  if (raw_id <= 0) return JS_ThrowTypeError(context, "controls(windowId): id must be positive");
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
        std::vector<ControlInfo> controls;
        if (const auto error = service->controls(window_id, controls, timeout); !error.ok()) {
          return async_failure(error);
        }
        json::Value array = json::Value::array();
        for (const auto& control : controls) {
          json::Value entry = json::Value::object();
          entry.set("id", json::Value::number(static_cast<double>(control.id)));
          entry.set("className", json::Value::string(control.class_name));
          entry.set("classNN", json::Value::string(control.class_nn));
          array.push(std::move(entry));
        }
        return async_success(json::stringify(array));
      },
      options.cancellation_id);
}

// WinGetText: concatenated control text ("\r\n"-separated) as a JSON string.
JSValue windows_text(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "text(windowId, options?)");
  int64_t raw_id = 0;
  if (!js_int64_strict(context, argv[0], raw_id, "text(windowId)")) return JS_EXCEPTION;
  if (raw_id <= 0) return JS_ThrowTypeError(context, "text(windowId): id must be positive");
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
        std::string text;
        if (const auto error = service->text(window_id, text, timeout); !error.ok()) {
          return async_failure(error);
        }
        return async_success(json::stringify(json::Value::string(text)));
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
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
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
  auto action = make_action(*binding->next_action_id, "rime:window", action_type,
                            kWindowWriteCapability, {"window", std::move(target_text)},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
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

// ---------------------------------------------------------------------------
// settings.window: the synchronous home of SetTitleMatchMode /
// DetectHiddenWindows / DetectHiddenText and the A_TitleMatchMode* /
// A_DetectHidden* builtin variables. State lives in WindowService atomics,
// so every read and write stays on the JS thread (a set followed by a query
// is ordered by the query's own queue hop). Setters require the write
// capability (detectHiddenWindows widens what reads may see), getters the
// read capability; each actual change records a StateChanged trace entry.
// ---------------------------------------------------------------------------

// Which single knob an operation touches; each patch below carries exactly
// one field, and the operation returns that knob's previous value (AHK's
// Set* functions return-previous contract).
enum class SettingsField { TitleMatchMode, TitleMatchModeSpeed, DetectHiddenWindows, DetectHiddenText };

void record_settings_trace(WindowModuleBinding* binding, const char* key,
                           const std::string& before, const std::string& after) {
  const auto& sink = binding->kernel->trace_sink();
  if (!sink) return;
  rime::core::TraceEntry entry;
  entry.kind = rime::core::TraceKind::StateChanged;
  entry.subject = "window.settings";
  entry.detail = std::string(key) + " " + before + " -> " + after;
  sink->record(std::move(entry));
}

// Applies one field's patch and returns the previous value as a JS string or
// bool. Throws (sync) when the write capability is missing; the caller has
// already validated the value.
JSValue apply_settings_patch(JSContext* context, WindowModuleBinding* binding,
                             const SettingsField field, const WindowSettingsPatch& patch) {
  if (!binding->kernel->allows(kWindowWriteCapability)) {
    return JS_ThrowTypeError(context, "required capability was not granted: %s",
                              kWindowWriteCapability);
  }
  const WindowSettings previous = binding->service->set_settings(patch);
  const WindowSettings current = binding->service->settings();
  switch (field) {
    case SettingsField::TitleMatchMode: {
      const std::string before = title_match_mode_text(previous.title_match_mode);
      const std::string after = title_match_mode_text(current.title_match_mode);
      if (before != after) record_settings_trace(binding, "titleMatchMode", before, after);
      return JS_NewString(context, before.c_str());
    }
    case SettingsField::TitleMatchModeSpeed: {
      const std::string before = previous.title_match_mode_slow ? "Slow" : "Fast";
      const std::string after = current.title_match_mode_slow ? "Slow" : "Fast";
      if (before != after) record_settings_trace(binding, "titleMatchModeSpeed", before, after);
      return JS_NewString(context, before.c_str());
    }
    case SettingsField::DetectHiddenWindows: {
      const bool before = previous.detect_hidden_windows;
      if (before != current.detect_hidden_windows) {
        record_settings_trace(binding, "detectHiddenWindows", before ? "true" : "false",
                              current.detect_hidden_windows ? "true" : "false");
      }
      return JS_NewBool(context, before);
    }
    case SettingsField::DetectHiddenText: {
      const bool before = previous.detect_hidden_text;
      if (before != current.detect_hidden_text) {
        record_settings_trace(binding, "detectHiddenText", before ? "true" : "false",
                              current.detect_hidden_text ? "true" : "false");
      }
      return JS_NewBool(context, before);
    }
  }
  return JS_UNDEFINED;
}

// magic 0..3 selects the property; values mirror the A_* builtin variables.
JSValue settings_window_get(JSContext* context, JSValueConst, int, JSValueConst*, int magic,
                            void* opaque) {
  auto* binding = static_cast<WindowModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (!binding->kernel->allows(kWindowReadCapability)) {
    return JS_ThrowTypeError(context, "required capability was not granted: %s",
                              kWindowReadCapability);
  }
  const WindowSettings current = binding->service->settings();
  switch (magic) {
    case 0:
      return JS_NewString(context, title_match_mode_text(current.title_match_mode).c_str());
    case 1:
      return JS_NewString(context, current.title_match_mode_slow ? "Slow" : "Fast");
    case 2:
      return JS_NewBool(context, current.detect_hidden_windows);
    case 3:
      return JS_NewBool(context, current.detect_hidden_text);
    default:
      return JS_ThrowInternalError(context, "unknown window setting");
  }
}

JSValue settings_window_set(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                            int magic, void* opaque) {
  auto* binding = static_cast<WindowModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "a settings value is required");
  WindowSettingsPatch patch;
  switch (magic) {
    case 0: {
      if (!JS_IsString(argv[0])) {
        return JS_ThrowTypeError(context, "titleMatchMode must be '1', '2', '3' or 'RegEx'");
      }
      const char* text = JS_ToCString(context, argv[0]);
      if (!text) return JS_EXCEPTION;
      const auto mode = parse_title_match_mode(text);
      JS_FreeCString(context, text);
      if (!mode.has_value()) {
        return JS_ThrowTypeError(context, "titleMatchMode must be '1', '2', '3' or 'RegEx'");
      }
      patch.title_match_mode = *mode;
      break;
    }
    case 1: {
      if (!JS_IsString(argv[0])) {
        return JS_ThrowTypeError(context, "titleMatchModeSpeed must be 'Fast' or 'Slow'");
      }
      const char* text = JS_ToCString(context, argv[0]);
      if (!text) return JS_EXCEPTION;
      const std::string speed(text);
      JS_FreeCString(context, text);
      if (speed != "Fast" && speed != "Slow") {
        return JS_ThrowTypeError(context, "titleMatchModeSpeed must be 'Fast' or 'Slow'");
      }
      patch.title_match_mode_slow = speed == "Slow";
      break;
    }
    case 2:
      if (!JS_IsBool(argv[0])) {
        return JS_ThrowTypeError(context, "detectHiddenWindows must be a boolean");
      }
      patch.detect_hidden_windows = JS_ToBool(context, argv[0]);
      break;
    case 3:
      if (!JS_IsBool(argv[0])) {
        return JS_ThrowTypeError(context, "detectHiddenText must be a boolean");
      }
      patch.detect_hidden_text = JS_ToBool(context, argv[0]);
      break;
    default:
      return JS_ThrowInternalError(context, "unknown window setting");
  }
  JSValue previous =
      apply_settings_patch(context, binding, static_cast<SettingsField>(magic), patch);
  if (JS_IsException(previous)) return previous;  // e.g. the write capability check
  JS_FreeValue(context, previous);  // property assignment never observes the return
  return JS_UNDEFINED;
}

// SetTitleMatchMode(mode): accepts a mode string or the "Fast"/"Slow" speed
// keyword and resolves with the previous value of the knob it changed.
JSValue settings_set_title_match_mode(JSContext* context, JSValueConst, int argc,
                                      JSValueConst* argv, int, void* opaque) {
  auto* binding = static_cast<WindowModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc != 1 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "setTitleMatchMode(mode): mode must be a string");
  }
  const char* text = JS_ToCString(context, argv[0]);
  if (!text) return JS_EXCEPTION;
  const std::string mode_text(text);
  JS_FreeCString(context, text);
  WindowSettingsPatch patch;
  SettingsField field = SettingsField::TitleMatchMode;
  if (mode_text == "Fast" || mode_text == "Slow") {
    field = SettingsField::TitleMatchModeSpeed;
    patch.title_match_mode_slow = mode_text == "Slow";
  } else if (const auto mode = parse_title_match_mode(mode_text); mode.has_value()) {
    patch.title_match_mode = *mode;
  } else {
    return JS_ThrowTypeError(context,
                             "setTitleMatchMode(mode): mode must be '1', '2', '3', 'RegEx', "
                             "'Fast' or 'Slow'");
  }
  return apply_settings_patch(context, binding, field, patch);
}

// DetectHiddenWindows(mode) / DetectHiddenText(mode): boolean toggles that
// resolve with the previous value.
JSValue settings_set_detect_hidden(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                                   int magic, void* opaque) {
  auto* binding = static_cast<WindowModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  const char* name = magic == 0 ? "setDetectHiddenWindows" : "setDetectHiddenText";
  if (argc != 1 || !JS_IsBool(argv[0])) {
    return JS_ThrowTypeError(context, "%s(mode): mode must be a boolean", name);
  }
  WindowSettingsPatch patch;
  if (magic == 0) {
    patch.detect_hidden_windows = JS_ToBool(context, argv[0]);
    return apply_settings_patch(context, binding, SettingsField::DetectHiddenWindows, patch);
  }
  patch.detect_hidden_text = JS_ToBool(context, argv[0]);
  return apply_settings_patch(context, binding, SettingsField::DetectHiddenText, patch);
}

int window_module_init(JSContext* context, JSModuleDef* module) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    JS_ThrowInternalError(context, "rime:window requires a window module binding");
    return -1;
  }
  JSValue windows = JS_NewObject(context);
  auto add = [&](JSValue target, const char* name, JSCClosure* function, int length,
                 int magic) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, magic, binding);
    if (JS_IsException(value)) return false;
    // JS_SetPropertyStr consumes `value` on both success and failure.
    if (JS_SetPropertyStr(context, target, name, value) < 0) return false;
    return true;
  };
  if (!add(windows, "list", windows_list, 0, 0) ||
      !add(windows, "active", windows_active, 0, 0) ||
      !add(windows, "exists", windows_exists, 1, 0) ||
      !add(windows, "isActive", windows_is_active, 1, 0) ||
      !add(windows, "info", windows_info, 1, 0) ||
      !add(windows, "controls", windows_controls, 1, 0) ||
      !add(windows, "text", windows_text, 1, 0) || !add(windows, "move", windows_move, 2, 0) ||
      !add(windows, "focus", windows_focus, 1, 0) ||
      !add(windows, "close", windows_close, 1, 0) ||
      !add(windows, "hide", windows_hide, 1, 0) || !add(windows, "show", windows_show, 1, 0) ||
      !add(windows, "minimize", windows_minimize, 1, 0) ||
      !add(windows, "maximize", windows_maximize, 1, 0) ||
      !add(windows, "restore", windows_restore, 1, 0)) {
    JS_FreeValue(context, windows);
    return -1;
  }

  // settings.window: property accessors (A_TitleMatchMode/A_DetectHidden*,
  // assignment included) plus the AHK Set* functions with their
  // return-previous contract.
  JSValue settings = JS_NewObject(context);
  JSValue window_settings = JS_NewObject(context);
  const struct {
    const char* name;
    int magic;
  } properties[] = {{"titleMatchMode", 0},
                    {"titleMatchModeSpeed", 1},
                    {"detectHiddenWindows", 2},
                    {"detectHiddenText", 3}};
  for (const auto& property : properties) {
    JSValue getter = JS_NewCClosure(context, settings_window_get, property.name, nullptr, 0,
                                    property.magic, binding);
    JSValue setter = JS_NewCClosure(context, settings_window_set, property.name, nullptr, 1,
                                    property.magic, binding);
    if (JS_IsException(getter) || JS_IsException(setter)) {
      JS_FreeValue(context, getter);
      JS_FreeValue(context, setter);
      JS_FreeValue(context, window_settings);
      JS_FreeValue(context, settings);
      JS_FreeValue(context, windows);
      return -1;
    }
    const JSAtom atom = JS_NewAtom(context, property.name);
    if (atom == JS_ATOM_NULL) {
      JS_FreeValue(context, window_settings);
      JS_FreeValue(context, settings);
      JS_FreeValue(context, windows);
      return -1;
    }
    // Consumes getter and setter on both success and failure.
    const int defined =
        JS_DefinePropertyGetSet(context, window_settings, atom, getter, setter, JS_PROP_ENUMERABLE);
    JS_FreeAtom(context, atom);
    if (defined < 0) {
      JS_FreeValue(context, window_settings);
      JS_FreeValue(context, settings);
      JS_FreeValue(context, windows);
      return -1;
    }
  }
  if (!add(window_settings, "setTitleMatchMode", settings_set_title_match_mode, 1, 0) ||
      !add(window_settings, "setDetectHiddenWindows", settings_set_detect_hidden, 1, 0) ||
      !add(window_settings, "setDetectHiddenText", settings_set_detect_hidden, 1, 1)) {
    JS_FreeValue(context, window_settings);
    JS_FreeValue(context, settings);
    JS_FreeValue(context, windows);
    return -1;
  }
  if (JS_SetPropertyStr(context, settings, "window", window_settings) < 0) {
    JS_FreeValue(context, settings);
    JS_FreeValue(context, windows);
    return -1;
  }
  if (JS_SetModuleExport(context, module, "windows", windows) < 0) {
    JS_FreeValue(context, settings);
    return -1;
  }
  return JS_SetModuleExport(context, module, "settings", settings);
}

JSModuleDef* create_window_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:window", window_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "windows") < 0) return nullptr;
  if (JS_AddModuleExport(context, module, "settings") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const WindowModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:window requires a window service, kernel, dispatcher and action id source"};
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
