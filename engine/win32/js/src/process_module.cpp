#include "rime/win32/js_process.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

// Capabilities every entry in this module checks before it touches the
// service or builds an Action. The wait family is split by what it observes:
// ProcessWait/ProcessWaitClose only read existence (inspect), while RunWait
// and waitRef wait on a reference a launch created (launch).
constexpr const char* kProcessInspectCapability = "process.inspect";
constexpr const char* kProcessLaunchCapability = "process.launch";
constexpr const char* kProcessManageCapability = "process.manage";
constexpr const char* kProcessRunAsCapability = "process.runas";
constexpr const char* kProcessShutdownCapability = "process.shutdown";

// The slice cadence, the capability_denied() failure and the clock helpers
// live in async_task.hpp, shared with the other poll-style waits.

ProcessModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<ProcessModuleBinding*>(host->module_data("rime:process"));
}

// Reads an optional string property; returns false (with a TypeError raised)
// when present but not a string. Missing/undefined/null yields `out`
// untouched. `prefix` is the message prefix: "options." keeps the historic
// launch texts byte-identical, "runAs(request): " names a dedicated request.
bool optional_string(JSContext* context, JSValueConst object, const char* prefix,
                     const char* name, std::string& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    if (!JS_IsString(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "%s%s must be a string", prefix, name);
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

// Required string field (an empty value is allowed, which is what a blank
// account password needs); a missing or non-string value throws.
bool required_string(JSContext* context, JSValueConst object, const char* label,
                     const char* name, std::string& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsString(property)) {
    JS_FreeValue(context, property);
    JS_ThrowTypeError(context, "%s: %s must be a string", label, name);
    return false;
  }
  const char* text = JS_ToCString(context, property);
  JS_FreeValue(context, property);
  if (!text) return false;
  out = text;
  JS_FreeCString(context, text);
  return true;
}

// Required non-empty string field (user, executable, command); a missing,
// non-string or empty value throws before any Action is built.
bool required_non_empty_string(JSContext* context, JSValueConst object, const char* label,
                               const char* name, std::string& out) {
  if (!required_string(context, object, label, name, out)) return false;
  if (out.empty()) {
    JS_ThrowTypeError(context, "%s: %s must be a non-empty string", label, name);
    return false;
  }
  return true;
}

// Optional boolean payload field: missing/undefined/null leaves `out`, any
// other non-boolean value throws a TypeError prefixed with `label`.
bool optional_bool_field(JSContext* context, JSValueConst object, const char* label,
                         const char* name, bool& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    if (!JS_IsBool(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "%s: %s must be a boolean", label, name);
      return false;
    }
    out = JS_ToBool(context, property);
  }
  JS_FreeValue(context, property);
  return true;
}

// Optional integer payload field within [min, max]: a non-number, a fraction
// or an out-of-range value throws a TypeError prefixed with `label`.
bool optional_int_field(JSContext* context, JSValueConst object, const char* label,
                        const char* name, const std::int64_t min, const std::int64_t max,
                        std::int64_t& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    const std::string what = std::string(label) + ": " + name;
    std::int64_t raw = 0;
    if (!js_int64_strict(context, property, raw, what.c_str())) {
      JS_FreeValue(context, property);
      return false;
    }
    if (raw < min || raw > max) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "%s: %s must be between %lld and %lld", label, name,
                        static_cast<long long>(min), static_cast<long long>(max));
      return false;
    }
    out = raw;
  }
  JS_FreeValue(context, property);
  return true;
}

// Parses the {command, args?, workingDir?} request shared by launch and
// runWait. `label` names the JS entry in the command TypeErrors and `prefix`
// the one for args/workingDir, so launch keeps its original wording.
bool parse_command_request(JSContext* context, JSValueConst object, const char* label,
                           const char* prefix, std::string& command, std::string& args,
                           std::string& working_dir) {
  JSValue property = JS_GetPropertyStr(context, object, "command");
  if (JS_IsException(property)) return false;
  if (!JS_IsString(property)) {
    JS_FreeValue(context, property);
    JS_ThrowTypeError(context, "%s: command must be a non-empty string", label);
    return false;
  }
  const char* command_text = JS_ToCString(context, property);
  JS_FreeValue(context, property);
  if (!command_text) return false;
  command = command_text;
  JS_FreeCString(context, command_text);
  if (command.empty()) {
    JS_ThrowTypeError(context, "%s: command must be a non-empty string", label);
    return false;
  }
  return optional_string(context, object, prefix, "args", args) &&
         optional_string(context, object, prefix, "workingDir", working_dir);
}

JSValue process_list(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "list(options?)");
  ActionOptions options;
  if (argc == 1 && !parse_action_options(context, argv[0], options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  ProcessService* service = binding->service;
  return start_async(
      context,
      [kernel, service]() -> AsyncOutcome {
        if (!kernel->allows(kProcessInspectCapability)) {
          return capability_denied(kProcessInspectCapability);
        }
        std::vector<ProcessInfo> processes;
        if (const auto error = service->list(processes); !error.ok()) {
          return async_failure(error);
        }
        json::Value array = json::Value::array();
        for (const auto& process : processes) array.push(process_info_json(process));
        return async_success(json::stringify(array));
      },
      options.cancellation_id);
}

JSValue process_info(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "info(pid, options?)");
  int64_t raw_pid = 0;
  if (!js_int64_strict(context, argv[0], raw_pid, "info(pid)")) return JS_EXCEPTION;
  if (raw_pid <= 0) return JS_ThrowTypeError(context, "info(pid): pid must be positive");
  if (raw_pid > static_cast<int64_t>(UINT32_MAX)) {
    return JS_ThrowTypeError(context, "info(pid): pid is out of range");
  }
  const std::uint32_t pid = static_cast<std::uint32_t>(raw_pid);
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  ProcessService* service = binding->service;
  return start_async(
      context,
      [kernel, service, pid]() -> AsyncOutcome {
        if (!kernel->allows(kProcessInspectCapability)) {
          return capability_denied(kProcessInspectCapability);
        }
        ProcessInfo info;
        if (const auto error = service->info(pid, info); !error.ok()) {
          return async_failure(error);
        }
        return async_success(json::stringify(process_info_json(info)));
      },
      options.cancellation_id);
}

JSValue process_launch(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "launch(options)");
  if (!JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(context, "launch(options): options must be an object");
  }

  std::string command;
  std::string args;
  std::string working_dir;
  if (!parse_command_request(context, argv[0], "launch(options)", "options.", command, args,
                             working_dir)) {
    return JS_EXCEPTION;
  }

  json::Value payload = json::Value::object();
  payload.set("command", json::Value::string(command));
  if (!args.empty()) payload.set("args", json::Value::string(args));
  if (!working_dir.empty()) payload.set("workingDir", json::Value::string(working_dir));

  // ActionOptions (deadlineMs/cancellationId/...) live alongside launch
  // fields in the same options object.
  ActionOptions options;
  if (!parse_action_options(context, argv[0], options)) return JS_EXCEPTION;

  auto action = make_action(*binding->next_action_id, "rime:process", "process.launch",
                            "process.launch", {"process", "new"}, json::stringify(payload),
                            options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// process.edit(path[, options]): opens a file for editing (AHK Edit rule:
// shell "edit" verb, notepad fallback). Capability process.launch: it
// launches programs either way.
JSValue process_edit(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "edit(path[, options])");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "edit(path): path must be a string");
  }
  const char* path_text = JS_ToCString(context, argv[0]);
  if (!path_text) return JS_EXCEPTION;
  const std::string path(path_text);
  JS_FreeCString(context, path_text);
  if (path.empty()) {
    return JS_ThrowTypeError(context, "edit(path): path must not be empty");
  }
  // No JS-side capability gate (like launch): the kernel denies at dispatch.
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  payload.set("path", json::Value::string(path));
  auto action = make_action(*binding->next_action_id, "rime:process", "process.edit",
                            kProcessLaunchCapability, {"process", "new"},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

JSValue process_terminate(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "terminate(pid, options?)");
  int64_t raw_pid = 0;
  if (!js_int64_strict(context, argv[0], raw_pid, "terminate(pid)")) return JS_EXCEPTION;
  if (raw_pid <= 0) return JS_ThrowTypeError(context, "terminate(pid): pid must be positive");
  if (raw_pid > static_cast<int64_t>(UINT32_MAX)) {
    return JS_ThrowTypeError(context, "terminate(pid): pid is out of range");
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  auto action = make_action(*binding->next_action_id, "rime:process", "process.terminate",
                            "process.terminate", {"process", std::to_string(raw_pid)}, "{}",
                            options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// ---- wait family: runWait / wait / waitClose -----------------------------
//
// The wait entries dispatch no Action, exactly like windows.wait: they run on
// the worker lane in bounded kSliceWaitInterval slices, so nothing blocks or
// polls on the JS thread and there is no Action Trace for them (the
// capability read-policy inside each evaluation is the audit surface for
// this family). What they do share with the action entries is the capability
// gate, the deadline and the cancellation id.
//
// RunWait waits on the process handle a launch_waitable() reference owns and
// therefore needs process.launch; waitClose opens the target (SYNCHRONIZE
// only) and falls back to the snapshot when the OS refuses the open;
// ProcessWait can only poll the snapshot, because a pid that does not exist
// yet cannot be opened.
struct ProcessWaitLoop {
  enum class Kind { Exists, Close, RunWait };
  Kind kind;
  std::string command;
  std::string args;
  std::string working_dir;
  std::uint32_t pid{0};
  // A reference into ProcessService's table (1-based, never a handle, never
  // reused). Non-zero only while this loop owns a waitable handle: it is
  // consumed by wait_ref on exit and released on the single terminal path.
  std::uint64_t ref_id{0};
  // RunWait: the launch happens on the first worker step, never on the JS
  // thread. Close: the open is attempted at most once (a pid that is gone
  // already satisfies the condition, and a pid that refuses the open is
  // watched through the snapshot instead).
  bool launched{false};
  bool open_attempted{false};
  rime::js::Host* host;
  ProcessService* service;
  rime::action::Kernel* kernel;
  std::uint64_t token{0};
  // Read from options but deliberately NOT bound to begin_async, unlike the
  // WinWait loop: Host::cancel_by_id would settle this promise and disarm the
  // slice timer, and this loop owns a native resource (a reference, hence a
  // process handle) that only a running step can release. Each step polls
  // is_cancelled instead, so a cancel still lands within one slice.
  std::uint64_t cancellation_id{0};
  std::int64_t deadline_unix_ms{0};  // absolute system ms since epoch
  std::int64_t budget_ms{0};         // the requested deadlineMs (error text)

  std::string timeout_message() const {
    return "wait timed out after " + std::to_string(budget_ms) + "ms";
  }

  // One evaluation on the worker lane: capability, cancellation, the
  // kind-specific setup (launch / open) and then the wait itself. Unset
  // means "not settled yet", so slice_wait_step re-arms the slice timer
  // instead of holding the worker for the whole deadline.
  std::optional<AsyncOutcome> evaluate() {
    const char* capability =
        kind == Kind::RunWait ? kProcessLaunchCapability : kProcessInspectCapability;
    if (!kernel->allows(capability)) return capability_denied(capability);
    if (cancellation_id != 0 && host->is_cancelled(cancellation_id)) {
      return async_failure("cancelled", "wait cancelled");
    }
    if (kind == Kind::RunWait && !launched) {
      // First step only: launch on the worker lane, keep hProcess as a
      // reference. A launch error settles immediately (no reference issued).
      std::uint32_t launched_pid = 0;
      std::uint64_t reference = 0;
      if (const auto error =
              service->launch_waitable(command, args, working_dir, launched_pid, reference);
          !error.ok()) {
        return async_failure(error);
      }
      pid = launched_pid;
      ref_id = reference;
      launched = true;
    } else if (kind == Kind::Close && !open_attempted) {
      // Try to wait on a real handle; `opened` false keeps the snapshot path.
      std::uint64_t reference = 0;
      bool opened = false;
      if (const auto error = service->open_waitable_pid(pid, reference, opened); !error.ok()) {
        return async_failure(error);
      }
      ref_id = reference;
      open_attempted = true;
    }

    if (ref_id != 0) {
      // Handle-backed slice (RunWait, or waitClose once opened): exit is
      // reported by wait_ref, which consumes the reference on success.
      std::uint64_t exit_code = 0;
      bool timed_out = false;
      if (const auto error = service->wait_ref(ref_id, deadline_unix_ms, exit_code, timed_out);
          !error.ok()) {
        return async_failure(error);
      }
      if (timed_out) return async_failure("timeout", timeout_message());
      ref_id = 0;
      json::Value value = json::Value::object();
      if (kind == Kind::RunWait) {
        value.set("pid", json::Value::number(static_cast<double>(pid)));
      }
      value.set("exitCode", json::Value::number(static_cast<double>(exit_code)));
      return async_success(json::stringify(value));
    }

    // Snapshot path (ProcessWait, and waitClose when the open was
    // refused): existence only, so an already-gone pid satisfies waitClose
    // on the first step.
    ProcessInfo info;
    const bool exists = service->info(pid, info).ok();
    const bool met = kind == Kind::Exists ? exists : !exists;
    if (met) {
      json::Value value = json::Value::object();
      value.set("pid", json::Value::number(static_cast<double>(pid)));
      return async_success(json::stringify(value));
    }
    if (now_unix_ms() >= deadline_unix_ms) return async_failure("timeout", timeout_message());
    return std::nullopt;
  }

  // The single terminal path: a reference the loop still owns (timeout,
  // cancel, capability loss, service error) is released here, so no handle
  // outlives its wait. slice_wait_step runs this before it settles the
  // promise, which then erases this token's pending/timer bookkeeping even
  // when CancelById already settled it.
  void on_terminal(const AsyncOutcome&) {
    if (ref_id != 0) {
      static_cast<void>(service->cancel_wait(ref_id));
      ref_id = 0;
    }
  }
};

// Shared tail of the wait entries: creates the promise token (unbound by
// design, see ProcessWaitLoop::cancellation_id), computes the absolute
// deadline and arms the first worker step.
JSValue arm_process_wait(JSContext* context, ProcessModuleBinding* binding,
                         const ActionOptions& options, const ProcessWaitLoop::Kind kind,
                         const std::uint32_t pid, std::uint64_t ref_id, std::string command,
                         std::string args, std::string working_dir) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  JSValue promise = JS_UNDEFINED;
  std::uint64_t token = 0;
  if (const auto error = host->begin_async(context, promise, token, 0); !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  const auto deadline_unix_ms = wait_deadline_unix_ms(options.deadline_ms);
  auto loop = std::make_shared<ProcessWaitLoop>(ProcessWaitLoop{
      kind, std::move(command), std::move(args), std::move(working_dir), pid, ref_id,
      false, false, host, binding->service, binding->kernel, token, options.cancellation_id,
      deadline_unix_ms, static_cast<std::int64_t>(options.deadline_ms)});
  host->schedule_worker(token, [loop] { slice_wait_step(loop); });
  return promise;
}

// process.wait(pid, options?): ProcessWait - resolves with {pid} as soon as
// the process exists (immediately when it already does), rejects with
// `timeout` after deadlineMs (default 5000, like every entry) or `cancelled`
// when the bound cancellation id fires. Capability: process.inspect.
JSValue process_wait(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "wait(pid, options?)");
  int64_t raw_pid = 0;
  if (!js_int64_strict(context, argv[0], raw_pid, "wait(pid)")) return JS_EXCEPTION;
  if (raw_pid <= 0) return JS_ThrowTypeError(context, "wait(pid): pid must be positive");
  if (raw_pid > static_cast<int64_t>(UINT32_MAX)) {
    return JS_ThrowTypeError(context, "wait(pid): pid is out of range");
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  return arm_process_wait(context, binding, options, ProcessWaitLoop::Kind::Exists,
                          static_cast<std::uint32_t>(raw_pid), 0, {}, {}, {});
}

// process.waitClose(pid, options?): ProcessWaitClose - resolves with {pid}
// once the process is gone. Waits on a SYNCHRONIZE handle when the OS grants
// one and falls back to snapshot existence otherwise; rejects `timeout`,
// `cancelled` or capability_denied (process.inspect) like process.wait.
JSValue process_wait_close(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                           int, void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "waitClose(pid, options?)");
  int64_t raw_pid = 0;
  if (!js_int64_strict(context, argv[0], raw_pid, "waitClose(pid)")) return JS_EXCEPTION;
  if (raw_pid <= 0) return JS_ThrowTypeError(context, "waitClose(pid): pid must be positive");
  if (raw_pid > static_cast<int64_t>(UINT32_MAX)) {
    return JS_ThrowTypeError(context, "waitClose(pid): pid is out of range");
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  return arm_process_wait(context, binding, options, ProcessWaitLoop::Kind::Close,
                          static_cast<std::uint32_t>(raw_pid), 0, {}, {}, {});
}

// process.runWait(request, options?): RunWait - launches on the worker lane,
// keeps the process handle as a reference and resolves with {pid, exitCode}
// when the process exits. Rejects with `timeout` when deadlineMs elapses
// first (the handle is released then, so the child is not abandoned silently:
// it keeps running and is simply no longer awaited), with `cancelled`, with
// capability_denied (process.launch) or with the launch's own error.
JSValue process_run_wait(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "runWait(request, options?)");
  if (!JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(context, "runWait(request): request must be an object");
  }
  std::string command;
  std::string args;
  std::string working_dir;
  if (!parse_command_request(context, argv[0], "runWait(request)", "runWait(request): ", command,
                             args, working_dir)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  return arm_process_wait(context, binding, options, ProcessWaitLoop::Kind::RunWait, 0, 0,
                          std::move(command), std::move(args), std::move(working_dir));
}

// process.setPriority(pid, priority, options?): ProcessSetPriority through
// the action pipeline (action type process.set.priority, capability
// process.manage, trace under the Action). `priority` is validated against
// the executor's six spellings here so a typo is a TypeError before an Action
// exists.
JSValue process_set_priority(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                             int, void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 2 || argc > 3) return JS_ThrowTypeError(context, "setPriority(pid, priority, options?)");
  int64_t raw_pid = 0;
  if (!js_int64_strict(context, argv[0], raw_pid, "setPriority(pid)")) return JS_EXCEPTION;
  if (raw_pid <= 0) return JS_ThrowTypeError(context, "setPriority(pid): pid must be positive");
  if (raw_pid > static_cast<int64_t>(UINT32_MAX)) {
    return JS_ThrowTypeError(context, "setPriority(pid): pid is out of range");
  }
  if (!JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context,
                             "setPriority(pid, priority): priority must be a string");
  }
  const char* priority_text = JS_ToCString(context, argv[1]);
  if (!priority_text) return JS_EXCEPTION;
  const std::string priority = priority_text;
  JS_FreeCString(context, priority_text);
  if (!is_valid_priority(priority)) {
    return JS_ThrowTypeError(context, "setPriority(pid, priority): priority must be one of idle, "
                                      "belowNormal, normal, aboveNormal, high, realtime");
  }
  ActionOptions options;
  if (argc == 3 && !parse_action_options(context, argv[2], options)) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  payload.set("pid", json::Value::number(static_cast<double>(raw_pid)));
  payload.set("priority", json::Value::string(priority));
  auto action = make_action(*binding->next_action_id, "rime:process", "process.set.priority",
                            kProcessManageCapability, {"process", std::to_string(raw_pid)},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// process.runAs(request, options?): RunAs through the action pipeline (action
// type process.runas, capability process.runas). The password is required as
// a string (an empty one is a legal account password) and travels only in the
// Action payload: Kernel::record never writes payload, so no Trace entry can
// observe it.
JSValue process_run_as(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "runAs(request, options?)");
  if (!JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(context, "runAs(request): request must be an object");
  }
  std::string user;
  std::string password;
  std::string executable;
  std::string domain;
  std::string arguments;
  std::string working_dir;
  if (!required_non_empty_string(context, argv[0], "runAs(request)", "user", user) ||
      !required_string(context, argv[0], "runAs(request)", "password", password) ||
      !required_non_empty_string(context, argv[0], "runAs(request)", "executable", executable) ||
      !optional_string(context, argv[0], "runAs(request): ", "domain", domain) ||
      !optional_string(context, argv[0], "runAs(request): ", "arguments", arguments) ||
      !optional_string(context, argv[0], "runAs(request): ", "workingDir", working_dir)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  payload.set("user", json::Value::string(user));
  payload.set("password", json::Value::string(password));
  payload.set("executable", json::Value::string(executable));
  if (!domain.empty()) payload.set("domain", json::Value::string(domain));
  if (!arguments.empty()) payload.set("arguments", json::Value::string(arguments));
  if (!working_dir.empty()) payload.set("workingDir", json::Value::string(working_dir));
  auto action = make_action(*binding->next_action_id, "rime:process", "process.runas",
                            kProcessRunAsCapability, {"process", "new"}, json::stringify(payload),
                            options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// process.shutdown(payload, options?): Shutdown through the action pipeline
// (capability process.shutdown). mode/force/timeoutSec are validated here and
// again by the executor, and the executor validates before any Win32 call, so
// a bad request never reaches the shutdown APIs.
JSValue process_shutdown(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "shutdown(payload, options?)");
  if (!JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(context, "shutdown(payload): payload must be an object");
  }
  JSValue mode_value = JS_GetPropertyStr(context, argv[0], "mode");
  if (JS_IsException(mode_value)) return JS_EXCEPTION;
  if (!JS_IsString(mode_value)) {
    JS_FreeValue(context, mode_value);
    return JS_ThrowTypeError(context, "shutdown(payload): mode must be one of logoff, shutdown, "
                                      "reboot, poweroff, hibernate");
  }
  const char* mode_text = JS_ToCString(context, mode_value);
  JS_FreeValue(context, mode_value);
  if (!mode_text) return JS_EXCEPTION;
  const std::string mode = mode_text;
  JS_FreeCString(context, mode_text);
  if (mode != "logoff" && mode != "shutdown" && mode != "reboot" && mode != "poweroff" &&
      mode != "hibernate") {
    return JS_ThrowTypeError(context, "shutdown(payload): mode must be one of logoff, shutdown, "
                                      "reboot, poweroff, hibernate");
  }
  bool force = false;
  std::int64_t timeout_sec = 0;
  if (!optional_bool_field(context, argv[0], "shutdown(payload)", "force", force) ||
      !optional_int_field(context, argv[0], "shutdown(payload)", "timeoutSec", 0, 600,
                          timeout_sec)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  json::Value payload = json::Value::object();
  payload.set("mode", json::Value::string(mode));
  payload.set("force", json::Value::boolean(force));
  payload.set("timeoutSec", json::Value::number(static_cast<double>(timeout_sec)));
  auto action = make_action(*binding->next_action_id, "rime:process", "process.shutdown",
                            kProcessShutdownCapability, {"process", "system"},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

int process_module_init(JSContext* context, JSModuleDef* module) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    JS_ThrowInternalError(context, "rime:process requires a process module binding");
    return -1;
  }
  JSValue process = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(value)) {
      JS_FreeValue(context, process);
      return false;
    }
    // JS_SetPropertyStr consumes `value` on both success and failure.
    if (JS_SetPropertyStr(context, process, name, value) < 0) {
      JS_FreeValue(context, process);
      return false;
    }
    return true;
  };
  if (!add("list", process_list, 0) || !add("info", process_info, 1) ||
      !add("launch", process_launch, 1) || !add("edit", process_edit, 1) ||
      !add("terminate", process_terminate, 1) ||
      !add("wait", process_wait, 1) || !add("waitClose", process_wait_close, 1) ||
      !add("runWait", process_run_wait, 1) || !add("setPriority", process_set_priority, 2) ||
      !add("runAs", process_run_as, 1) || !add("shutdown", process_shutdown, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "process", process);
}

JSModuleDef* create_process_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:process", process_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "process") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const ProcessModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:process requires a process service, kernel, dispatcher and action id source"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_process_module(rime::js::Host& host, ProcessModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:process", binding);
  if (const auto error = host.modules().add_native(
          "rime:process", [](JSContext* context) { return create_process_module(context); });
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error register_process_module(rime::js::Runtime& runtime,
                                          ProcessModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:process", [](JSContext* context) { return create_process_module(context); }, binding);
}

}  // namespace rime::win32
