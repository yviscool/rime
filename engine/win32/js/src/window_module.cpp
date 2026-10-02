#include "rime/win32/js_window.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <charconv>
#include <chrono>
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

// Parses the WinWait-family `until` field (default "exists"). An unknown
// condition is a synchronous TypeError, so a bad wait never starts a loop.
bool parse_wait_until(JSContext* context, JSValueConst object, WaitCondition& out) {
  JSValue value = JS_GetPropertyStr(context, object, "until");
  if (JS_IsException(value)) return false;
  if (JS_IsUndefined(value) || JS_IsNull(value)) {
    JS_FreeValue(context, value);
    return true;
  }
  if (!JS_IsString(value)) {
    JS_FreeValue(context, value);
    JS_ThrowTypeError(context, "options.until must be a string");
    return false;
  }
  const char* text = JS_ToCString(context, value);
  JS_FreeValue(context, value);
  if (!text) return false;
  const std::string_view condition(text);
  if (condition == "exists") {
    out = WaitCondition::Exists;
  } else if (condition == "active") {
    out = WaitCondition::Active;
  } else if (condition == "closed") {
    out = WaitCondition::Closed;
  } else if (condition == "notActive") {
    out = WaitCondition::NotActive;
  } else {
    JS_FreeCString(context, text);
    JS_ThrowTypeError(context, "options.until must be one of exists, active, closed, notActive");
    return false;
  }
  JS_FreeCString(context, text);
  return true;
}

// Poll cadence and per-poll UI budget for the wait loop: the loop sleeps on
// the scheduler between evaluations (never blocking a thread) and re-checks
// the condition; deadline/cancellation are evaluated after every poll.
constexpr std::chrono::milliseconds kWaitPollInterval{25};
constexpr std::chrono::milliseconds kWaitPollBudget{1000};

// One WinWait-family wait loop. Ownership: held by shared_ptr through the
// worker/timer closures; the Host outlives every armed task (its destructor
// stops the timer service and joins the worker first). Exactly one terminal
// path runs — resolve, reject, or the host's CancelById — and each erases
// this token's pending/timer bookkeeping, so unload cannot wedge on it.
struct WaitLoop {
  rime::js::Host* host;
  WindowService* service;
  rime::action::Kernel* kernel;
  WindowQuery query;
  WaitCondition until{WaitCondition::Exists};
  std::uint64_t token{0};
  std::uint64_t cancellation_id{0};
  std::int64_t deadline_unix_ms{0};  // absolute system ms since epoch
  std::uint64_t budget_ms{0};        // the requested deadlineMs (error text)
};

// One wait step: settles the promise or re-arms the poll. Runs on the worker
// lane; the capability read-policy matches the other window reads (this path
// is exempt from Action dispatch, so there is no Action Trace).
void wait_step(std::shared_ptr<WaitLoop> loop) {
  rime::js::Host* host = loop->host;
  const std::uint64_t token = loop->token;
  std::optional<AsyncOutcome> outcome;
  try {
    if (!loop->kernel->allows(kWindowReadCapability)) {
      outcome = async_failure("capability_denied",
                              std::string("required capability was not granted: ") +
                                  kWindowReadCapability);
    } else if (loop->cancellation_id != 0 && host->is_cancelled(loop->cancellation_id)) {
      outcome = async_failure("cancelled", "wait cancelled");
    } else {
      WaitEvaluation evaluation;
      if (const auto error = loop->service->evaluate_wait(loop->query, loop->until, evaluation,
                                                          kWaitPollBudget);
          !error.ok()) {
        outcome = async_failure(error);
      } else if (evaluation.met) {
        outcome =
            async_success(evaluation.target ? json::stringify(window_info_json(*evaluation.target))
                                            : std::string("null"));
      } else {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (now_ms >= loop->deadline_unix_ms) {
          outcome = async_failure(
              "timeout", "wait timed out after " + std::to_string(loop->budget_ms) + "ms");
        }
      }
    }
  } catch (const std::exception& exception) {
    outcome = async_failure("execution_failed", exception.what());
  } catch (...) {
    outcome = async_failure("execution_failed", "native wait step failed");
  }
  if (outcome.has_value()) {
    if (outcome->ok) {
      host->complete_async(token, true, std::move(outcome->payload));
    } else {
      host->complete_async(token, false, outcome->code + ":" + outcome->payload);
    }
    return;
  }
  // Not met yet: sleep on the scheduler, then poll again on the worker.
  // A cancellation landing between checks resolves the promise through the
  // host's CancelById; the next step observes is_cancelled and drains the
  // timer bookkeeping through complete_async (dropped, but erasing the token).
  host->schedule_task(token, kWaitPollInterval, [loop] {
    loop->host->schedule_worker(loop->token, [loop] { wait_step(loop); });
  });
}

// windows.wait(options?): the WinWait family (WinWait/WinWaitActive/
// WinWaitClose/WinWaitNotActive via `until`). Resolves with the target
// snapshot (exists/active) or null (closed/notActive); rejects with
// `timeout` once deadlineMs elapses (default 5000, same as every entry) or
// with `cancelled` when the bound cancellation id fires.
JSValue windows_wait(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  if (argc > 1) return JS_ThrowTypeError(context, "wait(options?)");
  WindowQuery query;
  ActionOptions options;
  WaitCondition until = WaitCondition::Exists;
  if (argc == 1) {
    if (!parse_window_query(context, argv[0], query) ||
        !parse_wait_until(context, argv[0], until) ||
        !parse_action_options(context, argv[0], options)) {
      return JS_EXCEPTION;
    }
  }
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
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
  auto loop = std::make_shared<WaitLoop>(WaitLoop{
      host, binding->service, binding->kernel, std::move(query), until, token,
      options.cancellation_id, deadline_unix_ms, options.deadline_ms});
  host->schedule_worker(token, [loop = std::move(loop)] { wait_step(loop); });
  return promise;
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
    return JS_ThrowTypeError(context, "%s(target[, options?])", function_name);
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
  } else if (std::string_view(action_type) == "window.zorder") {
    if (argc < 2) return JS_ThrowTypeError(context, "%s(target, placement)", function_name);
    if (!JS_IsString(argv[1])) {
      return JS_ThrowTypeError(context, "%s(target, placement): placement must be a string",
                               function_name);
    }
    const char* where_text = JS_ToCString(context, argv[1]);
    if (!where_text) return JS_EXCEPTION;
    placement = where_text;
    JS_FreeCString(context, where_text);
    if (placement != "top" && placement != "bottom") {
      return JS_ThrowTypeError(context,
                               "%s(target, placement): placement must be 'top' or 'bottom'",
                               function_name);
    }
    cursor = 2;
  }
  if (argc > cursor + 1) {
    const char* middle =
        std::string_view(action_type) == "window.move"
            ? "position"
            : (std::string_view(action_type) == "window.zorder" ? "placement" : "");
    if (middle[0] == '\0') {
      return JS_ThrowTypeError(context, "%s(target[, options?])", function_name);
    }
    return JS_ThrowTypeError(context, "%s(target[, %s][, options?])", function_name, middle);
  }
  ActionOptions options;
  if (argc > cursor && !parse_action_options(context, argv[cursor], options)) return JS_EXCEPTION;

  json::Value payload = json::Value::object();
  if (!placement.empty()) {
    const char* key = std::string_view(action_type) == "window.move" ? "position" : "placement";
    payload.set(key, json::Value::string(placement));
  }
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

JSValue windows_zorder(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  // NOTE: required_args stays 1 here; the (target, placement) arity for
  // zorder is enforced by the window.zorder branch inside run_window_mutation.
  return run_window_mutation(context, argc, argv, "zorder", "window.zorder", 1);
}

JSValue windows_kill(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  return run_window_mutation(context, argc, argv, "kill", "window.kill", 1);
}

JSValue windows_redraw(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  return run_window_mutation(context, argc, argv, "redraw", "window.redraw", 1);
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

// Shared body for the target-less desktop mutations (minimizeAll /
// minimizeAllUndo): the action target is the fixed {"desktop", "all"} pair,
// so only the optional ActionOptions argument is parsed here.
JSValue run_desktop_mutation(JSContext* context, int argc, JSValueConst* argv,
                             const char* usage, const char* action_type) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "%s", usage);
  ActionOptions options;
  if (argc == 1 && !parse_action_options(context, argv[0], options)) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  auto action = make_action(*binding->next_action_id, "rime:window", action_type,
                            kWindowWriteCapability, {"desktop", "all"},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

JSValue windows_minimize_all(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                             void*) {
  return run_desktop_mutation(context, argc, argv, "minimizeAll(options?)", "window.minimizeall");
}

JSValue windows_minimize_all_undo(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                                  int, void*) {
  return run_desktop_mutation(context, argc, argv, "minimizeAllUndo(options?)",
                              "window.minimizeall.undo");
}

// ---------------------------------------------------------------------------
// groups: named window groups (AHK GroupAdd/GroupActivate/GroupDeactivate/
// GroupClose). Every call dispatches a write action through the kernel like
// the other mutations; the group registry and the visited-window cycle
// state live on the UI lane inside WindowService. All argument validation
// (name, query shape, mode, reverse) happens synchronously here so contract
// mistakes reject without an Action Trace entry.
// ---------------------------------------------------------------------------

bool parse_group_name(JSContext* context, JSValueConst value, std::string& out) {
  if (!JS_IsString(value)) {
    JS_ThrowTypeError(context, "group name must be a string");
    return false;
  }
  const char* text = JS_ToCString(context, value);
  if (!text) return false;
  out = text;
  JS_FreeCString(context, text);
  if (out.empty()) {
    JS_ThrowTypeError(context, "group name must not be empty");
    return false;
  }
  return true;
}

// Wire twin of parse_window_query: serializes a parsed query back into the
// group.add payload object (the executor parses it with the same field
// names and semantics).
json::Value group_query_payload(const WindowQuery& query) {
  json::Value payload = json::Value::object();
  if (!query.title.empty()) payload.set("title", json::Value::string(query.title));
  if (!query.class_name.empty()) payload.set("ahkClass", json::Value::string(query.class_name));
  if (!query.process_name.empty()) payload.set("ahkExe", json::Value::string(query.process_name));
  if (query.id != 0) payload.set("ahkId", json::Value::number(static_cast<double>(query.id)));
  if (query.include_hidden.has_value()) {
    payload.set("includeHidden", json::Value::boolean(*query.include_hidden));
  }
  if (query.active) payload.set("active", json::Value::boolean(true));
  if (query.title_match_mode.has_value()) {
    const char* mode = "contains";
    switch (*query.title_match_mode) {
      case TitleMatchMode::StartsWith:
        mode = "startswith";
        break;
      case TitleMatchMode::Exact:
        mode = "exact";
        break;
      case TitleMatchMode::Regex:
        mode = "regex";
        break;
      case TitleMatchMode::Contains:
        break;
    }
    payload.set("matchMode", json::Value::string(mode));
  }
  return payload;
}

// Shared setup for the group calls: binding check plus the non-empty group
// name in argv[0].
bool group_call_entry(JSContext* context, int argc, JSValueConst* argv, const char* usage,
                      WindowModuleBinding*& binding, std::string& name) {
  binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    JS_ThrowInternalError(context, "rime:window is not wired");
    return false;
  }
  if (argc < 1) {
    JS_ThrowTypeError(context, "%s", usage);
    return false;
  }
  return parse_group_name(context, argv[0], name);
}

// Reads the optional `reverse` boolean from an options object (shared by
// activate/deactivate); absent or null means false.
bool parse_reverse_option(JSContext* context, JSValueConst options, bool& reverse) {
  if (JS_IsUndefined(options) || JS_IsNull(options)) return true;
  JSValue flag = JS_GetPropertyStr(context, options, "reverse");
  if (JS_IsException(flag)) return false;
  if (!JS_IsUndefined(flag) && !JS_IsNull(flag)) {
    if (!JS_IsBool(flag)) {
      JS_FreeValue(context, flag);
      JS_ThrowTypeError(context, "options.reverse must be a boolean");
      return false;
    }
    reverse = JS_ToBool(context, flag);
  }
  JS_FreeValue(context, flag);
  return true;
}

JSValue groups_add(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  WindowModuleBinding* binding = nullptr;
  std::string name;
  if (!group_call_entry(context, argc, argv, "add(name, query, options?)", binding, name)) {
    return JS_EXCEPTION;
  }
  if (argc > 3) return JS_ThrowTypeError(context, "add(name, query, options?)");
  if (argc < 2 || !JS_IsObject(argv[1])) {
    return JS_ThrowTypeError(context, "add(name, query, options?): query must be an object");
  }
  WindowQuery spec;
  if (!parse_window_query(context, argv[1], spec)) return JS_EXCEPTION;
  ActionOptions options;
  if (argc == 3 && !parse_action_options(context, argv[2], options)) return JS_EXCEPTION;
  auto action = make_action(*binding->next_action_id, "rime:window", "window.group.add",
                            kWindowWriteCapability, {"group", std::move(name)},
                            json::stringify(group_query_payload(spec)), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// Shared body for activate/deactivate(name[, options]) - both only differ
// in the action type and the detail the executor reports.
JSValue run_group_focus_cycle(JSContext* context, int argc, JSValueConst* argv,
                              const char* usage, const char* action_type) {
  WindowModuleBinding* binding = nullptr;
  std::string name;
  if (!group_call_entry(context, argc, argv, usage, binding, name)) return JS_EXCEPTION;
  if (argc > 2) return JS_ThrowTypeError(context, "%s", usage);
  ActionOptions options;
  bool reverse = false;
  if (argc == 2) {
    if (!parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
    if (!parse_reverse_option(context, argv[1], reverse)) return JS_EXCEPTION;
  }
  json::Value payload = json::Value::object();
  if (reverse) payload.set("reverse", json::Value::boolean(true));
  auto action = make_action(*binding->next_action_id, "rime:window", action_type,
                            kWindowWriteCapability, {"group", std::move(name)},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

JSValue groups_activate(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void*) {
  return run_group_focus_cycle(context, argc, argv, "activate(name, options?)",
                               "window.group.activate");
}

JSValue groups_deactivate(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  return run_group_focus_cycle(context, argc, argv, "deactivate(name, options?)",
                               "window.group.deactivate");
}

JSValue groups_close(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  WindowModuleBinding* binding = nullptr;
  std::string name;
  if (!group_call_entry(context, argc, argv, "close(name, mode?, options?)", binding, name)) {
    return JS_EXCEPTION;
  }
  if (argc > 3) return JS_ThrowTypeError(context, "close(name, mode?, options?)");
  if (argc == 3 && !JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context, "close(name, mode, options?): mode must be a string");
  }
  std::string mode;
  int cursor = 1;
  if (argc >= 2 && JS_IsString(argv[1])) {
    const char* text = JS_ToCString(context, argv[1]);
    if (!text) return JS_EXCEPTION;
    mode = text;
    JS_FreeCString(context, text);
    if (mode != "" && mode != "reverse" && mode != "all") {
      return JS_ThrowTypeError(context, "close(name, mode): mode must be '', 'reverse' or 'all'");
    }
    cursor = 2;
  }
  ActionOptions options;
  if (argc > cursor && !parse_action_options(context, argv[cursor], options)) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  if (!mode.empty()) payload.set("mode", json::Value::string(mode));
  auto action = make_action(*binding->next_action_id, "rime:window", "window.group.close",
                            kWindowWriteCapability, {"group", std::move(name)},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
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
      !add(windows, "wait", windows_wait, 1, 0) ||
      !add(windows, "info", windows_info, 1, 0) ||
      !add(windows, "controls", windows_controls, 1, 0) ||
      !add(windows, "text", windows_text, 1, 0) || !add(windows, "move", windows_move, 2, 0) ||
      !add(windows, "focus", windows_focus, 1, 0) ||
      !add(windows, "zorder", windows_zorder, 2, 0) ||
      !add(windows, "kill", windows_kill, 1, 0) ||
      !add(windows, "redraw", windows_redraw, 1, 0) ||
      !add(windows, "close", windows_close, 1, 0) ||
      !add(windows, "hide", windows_hide, 1, 0) || !add(windows, "show", windows_show, 1, 0) ||
      !add(windows, "minimize", windows_minimize, 1, 0) ||
      !add(windows, "maximize", windows_maximize, 1, 0) ||
      !add(windows, "restore", windows_restore, 1, 0) ||
      !add(windows, "minimizeAll", windows_minimize_all, 0, 0) ||
      !add(windows, "minimizeAllUndo", windows_minimize_all_undo, 0, 0)) {
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
  // groups: the named-group mutation surface (add/activate/deactivate/
  // close); wired last so every earlier failure path stays unchanged.
  JSValue groups = JS_NewObject(context);
  if (JS_IsException(groups)) {
    JS_FreeValue(context, settings);
    JS_FreeValue(context, windows);
    return -1;
  }
  if (!add(groups, "add", groups_add, 2, 0) || !add(groups, "activate", groups_activate, 1, 0) ||
      !add(groups, "deactivate", groups_deactivate, 1, 0) ||
      !add(groups, "close", groups_close, 1, 0)) {
    JS_FreeValue(context, groups);
    JS_FreeValue(context, settings);
    JS_FreeValue(context, windows);
    return -1;
  }
  if (JS_SetModuleExport(context, module, "windows", windows) < 0) {
    JS_FreeValue(context, groups);
    JS_FreeValue(context, settings);
    return -1;
  }
  if (JS_SetModuleExport(context, module, "settings", settings) < 0) {
    JS_FreeValue(context, groups);
    return -1;
  }
  return JS_SetModuleExport(context, module, "groups", groups);
}

JSModuleDef* create_window_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:window", window_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "windows") < 0) return nullptr;
  if (JS_AddModuleExport(context, module, "settings") < 0) return nullptr;
  if (JS_AddModuleExport(context, module, "groups") < 0) return nullptr;
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
