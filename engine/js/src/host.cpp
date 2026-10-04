#include "rime/js/host.hpp"

#include "rime/core/json.hpp"
#include "rime/core/worker.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

// rime:runtime owns the Win32 diagnostics and working-directory calls behind
// runtime.debug / runtime.cwd / runtime.setCwd (AHK OutputDebug and
// SetWorkingDir), so windows.h lands in this translation unit. It is kept
// lean and macro-free here: min/max and the socket/COM surface must not
// reach the C++ declarations above or the helpers below.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rime::js {
namespace {

namespace json = rime::core::json;

Host* host_of(JSContext* context) {
  return static_cast<Host*>(JS_GetContextOpaque(context));
}

JSValue make_error_value(JSContext* context, const std::string& code,
                           const std::string& message) {
  // Builds the rejection reason: an Error carrying both `message` and the
  // kernel-style `code` name (see rime::core::error_code_name). Property
  // installation can fail under OOM; the failure is swallowed here on purpose
  // so the caller still invokes the reject handler with a degraded reason
  // instead of dropping the rejection.
  JSValue error = JS_NewError(context);
  if (JS_IsException(error)) {
    JSValue fallback = JS_NewString(context, message.c_str());
    if (JS_IsException(fallback)) return JS_UNDEFINED;
    return fallback;
  }
  JSValue message_value = JS_NewString(context, message.c_str());
  if (!JS_IsException(message_value)) {
    // SetProperty/DefineProperty consume the value on both success and
    // failure; a failure only leaves a pending exception behind, which is
    // fetched and dropped so later JS calls start clean.
    if (JS_DefinePropertyValueStr(context, error, "message", message_value,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
      JSValue pending = JS_GetException(context);
      JS_FreeValue(context, pending);
    }
  }
  JSValue code_value = JS_NewString(context, code.c_str());
  if (!JS_IsException(code_value)) {
    if (JS_SetPropertyStr(context, error, "code", code_value) < 0) {
      JSValue pending = JS_GetException(context);
      JS_FreeValue(context, pending);
    }
  }
  return error;
}

// Strict JS number -> int64 for cancellation/subscription ids: non-numbers,
// NaN/Infinity, fractions and out-of-range values raise a TypeError.
bool strict_id(JSContext* context, JSValueConst value, int64_t& out, const char* what) {
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

void call_handler(JSContext* context, JSValue handler, JSValue argument) {
  JSValue result = JS_Call(context, handler, JS_UNDEFINED, 1, &argument);
  if (JS_IsException(result)) {
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
    return;
  }
  JS_FreeValue(context, result);
}

// ---- rime:runtime C module ----

// JS and JSON strings cross every boundary as UTF-8; the Win32 W entry points
// below want UTF-16. Best-effort conversion, the same rule as
// engine/win32/src/utf.hpp: oversized or unconvertible input yields an empty
// wide string instead of truncating mid-sequence.
std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) return {};
  if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          size) == 0) {
    return {};
  }
  return wide;
}

std::string wide_to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  if (wide.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                          nullptr, nullptr);
  if (size <= 0) return {};
  std::string text(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(),
                          size, nullptr, nullptr) == 0) {
    return {};
  }
  return text;
}

// OS failures surface as a plain Error whose message carries the Win32 code,
// the "X failed (win32 error N)" shape the Win32 services already use
// (engine/win32/src/window.cpp SetWindowPos/OpenProcess/TerminateProcess).
JSValue throw_win32_error(JSContext* context, const char* what, unsigned long failure) {
  return JS_ThrowPlainError(context, "%s failed (win32 error %lu)", what, failure);
}

// runtime.debug(text): AHK OutputDebug (script2.cpp:2630-2636) hands the text
// to OutputDebugString and returns nothing; there is no failure mode to
// report, so the only rejection is argument validation - a non-string (or a
// missing) argument is a TypeError before any Win32 call, and an empty string
// passes through exactly like AHK's empty argument.
JSValue runtime_debug(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(context, "debug(text)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "debug(text): text must be a string");
  }
  const char* raw = JS_ToCString(context, argv[0]);
  if (!raw) return JS_EXCEPTION;
  const std::string text(raw);
  JS_FreeCString(context, raw);
  // The W entry point keeps non-ASCII text intact, where OutputDebugStringA
  // would round-trip it through the active ANSI code page.
  const std::wstring wide = utf8_to_wide(text);
  OutputDebugStringW(wide.c_str());
  return JS_UNDEFINED;
}

// runtime.cwd(): the live process working directory as UTF-8. AHK's
// A_WorkingDir reads GetCurrentDirectory on every access (vars.cpp:900-907),
// so there is no cached copy that could go stale here either.
JSValue runtime_cwd(JSContext* context, JSValueConst, int, JSValueConst*) {
  // Reading is two calls - size probe, then copy - and the directory is
  // process-global, so it can grow between them. The buffer therefore grows
  // until a copy fits; four attempts bound the loop far past any realistic
  // churn, and exhaustion reports ERROR_INSUFFICIENT_BUFFER honestly.
  std::wstring buffer(260, L'\0');  // MAX_PATH, the usual case
  for (int attempt = 0; attempt < 4; ++attempt) {
    const DWORD written =
        GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0) return throw_win32_error(context, "GetCurrentDirectoryW", GetLastError());
    if (written < buffer.size()) {
      buffer.resize(written);
      return JS_NewString(context, wide_to_utf8(buffer).c_str());
    }
    // Only a too-small buffer gets here, and then `written` is the required
    // size including the terminator: grow to it and copy again.
    buffer.assign(static_cast<std::size_t>(written), L'\0');
  }
  return throw_win32_error(context, "GetCurrentDirectoryW", ERROR_INSUFFICIENT_BUFFER);
}

// runtime.setCwd(path): AHK SetWorkingDir (script2.cpp:1439-1490). The "C:"
// drive-root fixup is applied up front exactly like AHK (a bare "C:" would
// otherwise mean "current directory on C:"); a failed SetCurrentDirectory
// throws and leaves the directory unchanged; success returns nothing (AHK OK).
JSValue runtime_set_cwd(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 1) return JS_ThrowTypeError(context, "setCwd(path)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "setCwd(path): path must be a string");
  }
  const char* raw = JS_ToCString(context, argv[0]);
  if (!raw) return JS_EXCEPTION;
  const std::string path(raw);
  JS_FreeCString(context, raw);

  // AHK script2.cpp:1456-1464: a two-character "X:" becomes "X:\".
  std::wstring wide = path.size() == 2 && path[1] == ':' ? utf8_to_wide(path + "\\")
                                                         : utf8_to_wide(path);
  if (wide.empty() && !path.empty()) {
    return JS_ThrowTypeError(context, "setCwd(path): path is not valid UTF-8");
  }
  if (!SetCurrentDirectoryW(wide.c_str())) {
    return throw_win32_error(context, "SetCurrentDirectoryW", GetLastError());
  }
  return JS_UNDEFINED;
}

// runtime.exit(code?): AHK Exit / ExitApp collapse into one API - there are
// no script threads to leave. The call always throws so the current turn
// unwinds, request_exit arms the interrupt that aborts everything after it
// (uncatchable), and the host entry point returns `code`. First call wins.
JSValue runtime_exit(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  if (argc > 1) return JS_ThrowTypeError(context, "exit(code?)");
  int code = 0;
  if (argc == 1) {
    if (!JS_IsNumber(argv[0])) {
      return JS_ThrowTypeError(context, "exit(code): code must be an integer");
    }
    if (JS_ToInt32(context, &code, argv[0])) {
      // JS_ToInt32 left a pending exception behind; fetch it so the
      // TypeError below becomes the one the caller observes.
      JSValue pending = JS_GetException(context);
      JS_FreeValue(context, pending);
      return JS_ThrowTypeError(context, "exit(code): code must be an integer");
    }
  }
  (void)host->request_exit(code);
  return JS_ThrowPlainError(context, "exit requested (code %d)", code);
}

JSValue runtime_ping(JSContext* context, JSValueConst, int, JSValueConst*) {
  return JS_NewString(context, "pong");
}

JSValue runtime_delay(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  if (argc < 2) return JS_ThrowTypeError(context, "delay(milliseconds, value[, cancellationId])");
  uint32_t milliseconds = 0;
  if (JS_ToUint32(context, &milliseconds, argv[0])) return JS_EXCEPTION;
  std::uint64_t cancellation_id = 0;
  if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
    int64_t raw_id = 0;
    if (!strict_id(context, argv[2], raw_id, "delay(milliseconds, value[, cancellationId])")) {
      return JS_EXCEPTION;
    }
    if (raw_id > 0) cancellation_id = static_cast<std::uint64_t>(raw_id);
  }
  JSValue json_value = JS_JSONStringify(context, argv[1], JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json_value)) return JS_EXCEPTION;
  const char* text = JS_ToCString(context, json_value);
  JS_FreeValue(context, json_value);
  if (!text) return JS_EXCEPTION;
  std::string value_text(text);
  JS_FreeCString(context, text);

  JSValue promise = JS_UNDEFINED;
  std::uint64_t token = 0;
  if (const auto error = host->begin_async(context, promise, token, cancellation_id);
      !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  host->schedule_delay(token, milliseconds, std::move(value_text));
  return promise;
}

JSValue runtime_cancellation(JSContext* context, JSValueConst, int, JSValueConst*) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  return JS_NewInt64(context, static_cast<int64_t>(host->create_cancellation()));
}

JSValue runtime_cancel(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  int64_t raw_id = 0;
  if (argc < 1 || !strict_id(context, argv[0], raw_id, "cancel(cancellationId)")) {
    return JS_EXCEPTION;
  }
  // Cancellation ids start at 1; non-positive values can never exist, so fail
  // explicitly instead of letting the cast below wrap into a huge id.
  if (raw_id <= 0) return JS_NewBool(context, 0);
  return JS_NewBool(context, host->cancel_by_id(static_cast<std::uint64_t>(raw_id)));
}

JSValue runtime_release_cancellation(JSContext* context, JSValueConst, int argc,
                                     JSValueConst* argv) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  int64_t raw_id = 0;
  if (argc < 1 || !strict_id(context, argv[0], raw_id, "releaseCancellation(cancellationId)")) {
    return JS_EXCEPTION;
  }
  if (raw_id <= 0) return JS_NewBool(context, 0);
  return JS_NewBool(context, host->release_cancellation(static_cast<std::uint64_t>(raw_id)));
}

JSValue runtime_subscribe(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  if (argc < 1 || !JS_IsFunction(context, argv[0])) {
    return JS_ThrowTypeError(context, "subscribe(callback)");
  }
  std::uint64_t id = 0;
  if (const auto error = host->add_callback(JS_DupValue(context, argv[0]), id); !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  return JS_NewInt64(context, static_cast<int64_t>(id));
}

JSValue runtime_unsubscribe(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  int64_t raw_id = 0;
  if (argc < 1 || !strict_id(context, argv[0], raw_id, "unsubscribe(subscriptionId)")) {
    return JS_EXCEPTION;
  }
  if (raw_id <= 0) return JS_NewBool(context, 0);
  return JS_NewBool(context, host->remove_callback(static_cast<std::uint64_t>(raw_id)).ok());
}

JSValue runtime_inspect(JSContext* context, JSValueConst, int, JSValueConst*) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  std::string report = host->inspect("{}");
  return JS_NewString(context, report.c_str());
}

JSValue runtime_context(JSContext* context, JSValueConst, int, JSValueConst*) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  const std::string snapshot = host->context();
  JSValue value = JS_ParseJSON(context, snapshot.c_str(), snapshot.size(), "<context>");
  if (JS_IsException(value)) return JS_EXCEPTION;
  return value;
}

int runtime_module_init(JSContext* context, JSModuleDef* module) {
  JSValue object = JS_NewObject(context);
  if (JS_IsException(object)) return -1;
  auto set = [&](const char* name, JSValue value) -> bool {
    if (JS_IsException(value)) {
      JS_FreeValue(context, value);
      JS_FreeValue(context, object);
      return false;
    }
    // JS_SetPropertyStr consumes `value` on both success and failure.
    if (JS_SetPropertyStr(context, object, name, value) < 0) {
      JS_FreeValue(context, object);
      return false;
    }
    return true;
  };
  if (!set("ping", JS_NewCFunction(context, runtime_ping, "ping", 0)) ||
      !set("delay", JS_NewCFunction(context, runtime_delay, "delay", 3)) ||
      !set("cancellation", JS_NewCFunction(context, runtime_cancellation, "cancellation", 0)) ||
      !set("cancel", JS_NewCFunction(context, runtime_cancel, "cancel", 1)) ||
      !set("releaseCancellation",
           JS_NewCFunction(context, runtime_release_cancellation, "releaseCancellation", 1)) ||
      !set("subscribe", JS_NewCFunction(context, runtime_subscribe, "subscribe", 1)) ||
      !set("unsubscribe", JS_NewCFunction(context, runtime_unsubscribe, "unsubscribe", 1)) ||
      !set("inspect", JS_NewCFunction(context, runtime_inspect, "inspect", 0)) ||
      !set("context", JS_NewCFunction(context, runtime_context, "context", 0)) ||
      !set("debug", JS_NewCFunction(context, runtime_debug, "debug", 1)) ||
      !set("cwd", JS_NewCFunction(context, runtime_cwd, "cwd", 0)) ||
      !set("setCwd", JS_NewCFunction(context, runtime_set_cwd, "setCwd", 1)) ||
      !set("exit", JS_NewCFunction(context, runtime_exit, "exit", 1))) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "runtime", object);
}

}  // namespace

bool HostEventQueue::push(const std::uint64_t callback_id, std::string payload) {
  Wakeup wakeup;
  {
    std::lock_guard lock(mutex_);
    if (closed_) return false;
    items_.emplace_back(callback_id, std::move(payload));
    wakeup = wakeup_;
  }
  if (wakeup) wakeup();
  return true;
}

std::vector<std::pair<std::uint64_t, std::string>> HostEventQueue::take() {
  std::vector<std::pair<std::uint64_t, std::string>> items;
  std::lock_guard lock(mutex_);
  items.swap(items_);
  return items;
}

bool HostEventQueue::empty() const {
  std::lock_guard lock(mutex_);
  return items_.empty();
}

void HostEventQueue::set_wakeup(Wakeup wakeup) {
  std::lock_guard lock(mutex_);
  wakeup_ = std::move(wakeup);
}

void HostEventQueue::close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
  items_.clear();
  wakeup_ = {};
}

Host::Host() : owner_(std::this_thread::get_id()) {
  events_ = std::make_shared<HostEventQueue>();
  events_->set_wakeup([this] { wake(); });
  runtime_ = JS_NewRuntime();
  if (runtime_) context_ = JS_NewContext(runtime_);
  if (!context_) return;
  JS_SetContextOpaque(context_, this);
  JS_SetModuleLoaderFunc(runtime_, normalize_thunk, loader_thunk, this);
  JS_SetInterruptHandler(runtime_, interrupt_thunk, this);
  JS_SetHostPromiseRejectionTracker(runtime_, rejection_thunk, this);
  modules_.add_native("rime:runtime", [](JSContext* context) -> JSModuleDef* {
    JSModuleDef* module = JS_NewCModule(context, "rime:runtime", runtime_module_init);
    if (!module) return nullptr;
    if (JS_AddModuleExport(context, module, "runtime") < 0) return nullptr;
    return module;
  });
}

Host::~Host() {
  // Shutdown contract: exit handlers run first (while JS callbacks are still
  // alive), then native teardowns release hooks/observers, then the event
  // queue and timers close. Runtime::stop / HostAbi normally ran the exit
  // handlers earlier; this call is the idempotent fallback.
  run_exit_handlers("{\"reason\":\"shutdown\"}");
  run_teardowns();
  if (events_) events_->close();
  timer_.stop();
  // Joins any running worker task (queue pump / async read) before the host
  // memory it captures goes away.
  rime::core::WorkerService::instance().stop();
  subscriptions_.close();
  if (context_) {
    {
      std::lock_guard lock(callback_mutex_);
      for (auto& [id, callback] : callbacks_) JS_FreeValue(context_, callback);
      callbacks_.clear();
    }
    // Settle outstanding promises before releasing their handles so JS
    // observers see a rejection instead of a promise that never settles.
    // Entries are moved out first so user code re-entering the host during
    // the reject handlers cannot deadlock on async_mutex_.
    std::vector<Pending> abandoned;
    {
      std::lock_guard lock(async_mutex_);
      abandoned.swap(pending_);
      completions_.clear();
      delay_timers_.clear();
    }
    for (auto& entry : abandoned) {
      JSValue reason = make_error_value(context_, "cancelled", "shutdown");
      call_handler(context_, entry.reject, reason);
      JS_FreeValue(context_, reason);
      JS_FreeValue(context_, entry.resolve);
      JS_FreeValue(context_, entry.reject);
    }
    JS_FreeContext(context_);
    context_ = nullptr;
  }
  if (runtime_) {
    JS_RunGC(runtime_);
    JS_FreeRuntime(runtime_);
    runtime_ = nullptr;
  }
}

int Host::interrupt_thunk(JSRuntime*, void* opaque) {
  auto* host = static_cast<Host*>(opaque);
  if (!host) return 0;
  if (host->interrupt_.load(std::memory_order_relaxed)) return 1;
  std::atomic_bool* source = host->interrupt_source_;
  return (source && source->load(std::memory_order_relaxed)) ? 1 : 0;
}

char* Host::normalize_thunk(JSContext* context, const char* base_name,
                            const char* module_name, void* opaque) {
  auto* host = static_cast<Host*>(opaque);
  if (!host) return nullptr;
  return host->modules_.normalize(context, base_name, module_name);
}

JSModuleDef* Host::loader_thunk(JSContext* context, const char* module_name, void* opaque) {
  auto* host = static_cast<Host*>(opaque);
  if (!host) return nullptr;
  return host->modules_.load(context, module_name);
}

void Host::rejection_thunk(JSContext* context, JSValueConst, JSValueConst reason,
                           bool is_handled, void* opaque) {
  if (is_handled) return;
  auto* host = static_cast<Host*>(opaque);
  if (!host || !context) return;
  JSValue string = JS_ToString(context, reason);
  if (JS_IsException(string)) {
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
    host->record("promise", "unhandled rejection");
    return;
  }
  const char* text = JS_ToCString(context, string);
  JS_FreeValue(context, string);
  std::string message = text ? text : "unhandled rejection";
  if (text) JS_FreeCString(context, text);
  host->record("promise", std::move(message));
}

rime::core::Error Host::check_thread() const {
  if (std::this_thread::get_id() != owner_) {
    return {rime::core::Error::Code::InvalidState,
            "QuickJS context may only be used on its owner thread"};
  }
  if (!runtime_ || !context_) {
    return {rime::core::Error::Code::InvalidState, "QuickJS initialization failed"};
  }
  return rime::core::Error::none();
}

rime::core::Error Host::record(std::string where, std::string message) {
  std::vector<std::uint64_t> observers;
  {
    std::lock_guard lock(error_mutex_);
    errors_.push_back({where, message});
    // Loop prevention: an error recorded while an observer callback is
    // running (its own throw, or anything it triggers) stays in errors_ for
    // inspect but is not re-delivered, so a always-failing observer cannot
    // spin the drain forever.
    if (!error_observers_.empty() && !delivering_errors_.load(std::memory_order_acquire)) {
      observers = error_observers_;
    }
  }
  if (!observers.empty() && events_) {
    json::Value value = json::Value::object();
    value.set("where", json::Value::string(where));
    value.set("message", json::Value::string(message));
    const std::string payload = json::stringify(value);
    for (const std::uint64_t id : observers) (void)events_->push(id, payload);
  }
  return {rime::core::Error::Code::ExecutionFailed, std::move(message)};
}

rime::core::Error Host::exception_error(std::string where) {
  JSValue exception = JS_GetException(context_);
  const char* message = JS_ToCString(context_, exception);
  std::string text = message ? message : "JavaScript exception";
  if (message) JS_FreeCString(context_, message);
  JS_FreeValue(context_, exception);
  return record(std::move(where), std::move(text));
}

rime::core::Error Host::eval(const std::string_view source, const std::string_view filename) {
  if (const auto thread_error = check_thread(); !thread_error.ok()) return thread_error;
  const std::string source_copy(source);
  const std::string filename_copy(filename);
  JSValue result = JS_Eval(context_, source_copy.c_str(), source_copy.size(),
                           filename_copy.c_str(), JS_EVAL_TYPE_GLOBAL);
  if (!JS_IsException(result)) {
    JS_FreeValue(context_, result);
    return rime::core::Error::none();
  }
  return exception_error(filename_copy);
}

rime::core::Error Host::eval_module(const std::string_view source,
                                    const std::string_view filename) {
  if (const auto thread_error = check_thread(); !thread_error.ok()) return thread_error;
  const std::string source_copy(source);
  const std::string filename_copy(filename);
  const std::size_t error_mark = error_count();
  JSValue module = JS_Eval(context_, source_copy.c_str(), source_copy.size(),
                           filename_copy.c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(module)) return exception_error(filename_copy);
  JSValue result = JS_EvalFunction(context_, module);
  if (JS_IsException(result)) return exception_error(filename_copy);
  JS_FreeValue(context_, result);
  // Module evaluation failures reject the module promise instead of throwing;
  // the rejection tracker records them synchronously during EvalFunction.
  if (std::string recorded = new_error_since(error_mark); !recorded.empty()) {
    return {rime::core::Error::Code::ExecutionFailed, std::move(recorded)};
  }
  return rime::core::Error::none();
}

std::size_t Host::drain() {
  if (std::this_thread::get_id() != owner_ || !context_) return 0;
  std::vector<Completion> ready;
  {
    std::lock_guard lock(async_mutex_);
    ready.swap(completions_);
  }
  for (const auto& completion : ready) apply_completion(completion);

  std::size_t processed = ready.size();
  if (events_) {
    for (auto& [callback_id, payload] : events_->take()) {
      // Error-observer deliveries are marked so record() called while they
      // run logs without re-queueing (see Host::record).
      bool is_error_observer = false;
      {
        std::lock_guard lock(error_mutex_);
        for (const std::uint64_t id : error_observers_) {
          if (id == callback_id) {
            is_error_observer = true;
            break;
          }
        }
      }
      if (is_error_observer) delivering_errors_.store(true, std::memory_order_release);
      const bool invoked = invoke_callback(callback_id, payload).ok();
      if (is_error_observer) delivering_errors_.store(false, std::memory_order_release);
      if (invoked) ++processed;
    }
  }
  for (;;) {
    JSContext* job_context = nullptr;
    const int status = JS_ExecutePendingJob(runtime_, &job_context);
    if (status == 0) break;
    if (status < 0) {
      if (job_context) {
        JSValue exception = JS_GetException(job_context);
        const char* message = JS_ToCString(job_context, exception);
        std::string text = message ? message : "promise job failed";
        if (message) JS_FreeCString(job_context, message);
        JS_FreeValue(job_context, exception);
        record("job", std::move(text));
      }
      break;
    }
    ++processed;
  }
  return processed;
}

bool Host::idle() const {
  if (std::this_thread::get_id() != owner_) return false;
  std::lock_guard lock(async_mutex_);
  return completions_.empty() && delay_timers_.empty() && events_->empty();
}

rime::core::Error Host::begin_async(JSContext* context, JSValue& promise_out,
                                    std::uint64_t& token_out,
                                    const std::uint64_t cancellation_id) {
  if (const auto thread_error = check_thread(); !thread_error.ok()) return thread_error;
  if (context != context_) {
    return {rime::core::Error::Code::InvalidState, "async bound to a foreign context"};
  }
  JSValue resolving[2];
  JSValue promise = JS_NewPromiseCapability(context_, resolving);
  if (JS_IsException(promise)) return exception_error("begin_async");
  std::uint64_t token = 0;
  {
    std::lock_guard lock(async_mutex_);
    token = next_token_++;
    pending_.push_back({token, resolving[0], resolving[1], cancellation_id});
  }
  promise_out = promise;
  token_out = token;
  return rime::core::Error::none();
}

void Host::complete_async(const std::uint64_t token, const bool succeeded,
                          std::string value) {
  {
    std::lock_guard lock(async_mutex_);
    completions_.push_back(
        {token, 0, succeeded ? CompletionKind::Resolve : CompletionKind::Reject,
         std::move(value)});
  }
  wake();
}

void Host::reject_async(const std::uint64_t token, std::string message) {
  {
    std::lock_guard lock(async_mutex_);
    completions_.push_back({token, 0, CompletionKind::Reject, std::move(message)});
  }
  wake();
}

bool Host::cancel_by_id(const std::uint64_t cancellation_id) {
  // Ids start at 1; 0 (and anything produced by wrapping a negative JS
  // number) can never exist.
  if (cancellation_id == 0) return false;
  {
    std::lock_guard lock(cancellation_mutex_);
    const auto found = cancellations_.find(cancellation_id);
    if (found == cancellations_.end()) return false;
    found->second.cancel();
  }
  {
    std::lock_guard lock(async_mutex_);
    completions_.push_back({0, cancellation_id, CompletionKind::CancelById, {}});
  }
  wake();
  return true;
}

void Host::schedule_delay(const std::uint64_t token, const std::uint32_t ms,
                          std::string value) {
  const std::uint64_t timer_id = timer_.schedule(
      std::chrono::milliseconds(ms),
      // Ownership: captures `this` raw; TimerService::stop() in ~Host joins
      // pending callbacks, so the host outlives the task.
      [this, token, value = std::move(value)] { complete_async(token, true, value); });
  std::lock_guard lock(async_mutex_);
  if (timer_id == 0) {
    completions_.push_back(
        {token, 0, CompletionKind::Reject, "invalid_state:timer service is stopping"});
    return;
  }
  delay_timers_[token] = timer_id;
}

void Host::schedule_task(const std::uint64_t token, const std::chrono::milliseconds delay,
                         std::function<void()> task) {
  const std::uint64_t timer_id = timer_.schedule(delay, [this, token, task = std::move(task)] {
    // Ownership: captures `this` raw; see schedule_delay above.
    try {
      task();
    } catch (const std::exception& exception) {
      complete_async(token, false, std::string("execution_failed:") + exception.what());
    } catch (...) {
      complete_async(token, false, "execution_failed:native task failed");
    }
  });
  {
    std::lock_guard lock(async_mutex_);
    if (timer_id != 0) {
      delay_timers_[token] = timer_id;
      return;
    }
    completions_.push_back(
        {token, 0, CompletionKind::Reject, "invalid_state:timer service is stopping"});
  }
  wake();
}

void Host::schedule_worker(const std::uint64_t token, std::function<void()> task) {
  auto& worker = rime::core::WorkerService::instance();
  if (!worker.running()) (void)worker.start();
  // Register the token as in-flight so idle() stays false (and settle waits)
  // until the worker's completion lands; apply_completion erases it. Mirrors
  // schedule_task's delay_timers_ entry with timer id 0 (no OS timer armed).
  {
    std::lock_guard lock(async_mutex_);
    delay_timers_[token] = 0;
  }
  const bool accepted = worker.post([this, token, task = std::move(task)]() mutable {
    // Ownership: captures `this` raw; Host dtor stops the worker and joins
    // this task before tearing the host down.
    try {
      task();
    } catch (const std::exception& exception) {
      complete_async(token, false, std::string("execution_failed:") + exception.what());
    } catch (...) {
      complete_async(token, false, "execution_failed:native worker task failed");
    }
  });
  if (!accepted) {
    std::lock_guard lock(async_mutex_);
    completions_.push_back(
        {token, 0, CompletionKind::Reject, "invalid_state:worker service is stopping"});
    wake();
  }
}

bool Host::post_worker(std::function<void()> task) {
  auto& worker = rime::core::WorkerService::instance();
  if (!worker.running()) (void)worker.start();
  return worker.post(std::move(task));
}

std::size_t Host::pending_async() const {
  std::lock_guard lock(async_mutex_);
  return pending_.size();
}

std::uint64_t Host::create_cancellation() {
  std::lock_guard lock(cancellation_mutex_);
  const std::uint64_t id = next_cancellation_++;
  // NOTE: cancellations_ has no eviction; every id created here must be paired
  // with release_cancellation by the caller or the map grows without bound.
  cancellations_.emplace(id, rime::core::CancellationSource{});
  return id;
}

bool Host::is_cancelled(const std::uint64_t id) {
  std::lock_guard lock(cancellation_mutex_);
  const auto found = cancellations_.find(id);
  return found != cancellations_.end() && found->second.token().cancelled();
}

rime::core::CancellationToken Host::cancellation_token(const std::uint64_t id) {
  std::lock_guard lock(cancellation_mutex_);
  const auto found = cancellations_.find(id);
  if (found == cancellations_.end()) return {};
  return found->second.token();
}

bool Host::release_cancellation(const std::uint64_t id) {
  std::lock_guard lock(cancellation_mutex_);
  return cancellations_.erase(id) > 0;
}

void Host::bind_route(const std::uint64_t key, AsyncRoute route) {
  std::lock_guard lock(route_mutex_);
  routes_.insert_or_assign(key, std::move(route));
}

bool Host::find_route(const std::uint64_t key, AsyncRoute& out) const {
  std::lock_guard lock(route_mutex_);
  const auto found = routes_.find(key);
  if (found == routes_.end()) return false;
  out = found->second;
  return true;
}

bool Host::take_route(const std::uint64_t key, AsyncRoute& out) {
  std::lock_guard lock(route_mutex_);
  const auto found = routes_.find(key);
  if (found == routes_.end()) return false;
  out = std::move(found->second);
  routes_.erase(found);
  return true;
}

rime::core::Error Host::add_callback(JSValue callback, std::uint64_t& id_out) {
  if (const auto thread_error = check_thread(); !thread_error.ok()) return thread_error;
  if (!JS_IsFunction(context_, callback)) {
    JS_FreeValue(context_, callback);
    return {rime::core::Error::Code::InvalidContract, "subscribe expects a function"};
  }
  if (subscriptions_.closed()) {
    JS_FreeValue(context_, callback);
    return {rime::core::Error::Code::InvalidState, "runtime is shutting down"};
  }
  std::uint64_t id = 0;
  {
    std::lock_guard lock(callback_mutex_);
    id = next_callback_++;
    callbacks_.emplace_back(id, callback);
  }
  if (const auto registry_error = subscriptions_.add("js", id); !registry_error.ok()) {
    std::lock_guard lock(callback_mutex_);
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
      if (it->first == id) {
        JS_FreeValue(context_, it->second);
        callbacks_.erase(it);
        break;
      }
    }
    return registry_error;
  }
  id_out = id;
  return rime::core::Error::none();
}

rime::core::Error Host::remove_callback(const std::uint64_t id) {
  if (const auto thread_error = check_thread(); !thread_error.ok()) return thread_error;
  bool found = false;
  {
    std::lock_guard lock(callback_mutex_);
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
      if (it->first == id) {
        JS_FreeValue(context_, it->second);
        callbacks_.erase(it);
        found = true;
        break;
      }
    }
  }
  if (!found) return {rime::core::Error::Code::InvalidState, "subscription does not exist"};
  return subscriptions_.remove(id);
}

rime::core::Error Host::invoke_callback(const std::uint64_t id,
                                        const std::string_view argument_json) {
  if (const auto thread_error = check_thread(); !thread_error.ok()) return thread_error;
  JSValue handler = JS_UNDEFINED;
  {
    std::lock_guard lock(callback_mutex_);
    for (const auto& [callback_id, callback] : callbacks_) {
      if (callback_id == id) {
        handler = JS_DupValue(context_, callback);
        break;
      }
    }
  }
  if (JS_IsUndefined(handler)) {
    // The callback was removed while its event was in flight; drop quietly.
    return rime::core::Error::none();
  }
  const std::string text(argument_json);
  JSValue argument = JS_ParseJSON(context_, text.c_str(), text.size(), "<event>");
  if (JS_IsException(argument)) {
    JS_FreeValue(context_, handler);
    return exception_error("invoke_callback");
  }
  call_handler(context_, handler, argument);
  JS_FreeValue(context_, argument);
  JS_FreeValue(context_, handler);
  return rime::core::Error::none();
}

std::size_t Host::callback_count() const {
  std::lock_guard lock(callback_mutex_);
  return callbacks_.size();
}

std::uint64_t Host::allocate_subscription_id() {
  // Shares next_callback_ so module ids and callback ids form one unique
  // space; SubscriptionRegistry keys entries by id alone.
  std::lock_guard lock(callback_mutex_);
  return next_callback_++;
}

void Host::add_error_observer(const std::uint64_t callback_id) {
  std::lock_guard lock(error_mutex_);
  error_observers_.push_back(callback_id);
}

bool Host::remove_error_observer(const std::uint64_t callback_id) {
  std::lock_guard lock(error_mutex_);
  for (auto it = error_observers_.begin(); it != error_observers_.end(); ++it) {
    if (*it == callback_id) {
      error_observers_.erase(it);
      return true;
    }
  }
  return false;
}

rime::core::Error Host::add_exit_handler(const std::uint64_t callback_id,
                                         const std::uint64_t subscription_id) {
  if (const auto thread_error = check_thread(); !thread_error.ok()) return thread_error;
  if (exit_ran_) {
    return {rime::core::Error::Code::InvalidState,
            "exit handlers can no longer be registered: exit already ran"};
  }
  exit_handlers_.emplace_back(callback_id, subscription_id);
  return rime::core::Error::none();
}

bool Host::remove_exit_handler(const std::uint64_t subscription_id) {
  if (exit_ran_) return false;
  for (auto it = exit_handlers_.begin(); it != exit_handlers_.end(); ++it) {
    if (it->second == subscription_id) {
      const std::uint64_t callback_id = it->first;
      exit_handlers_.erase(it);
      (void)remove_callback(callback_id);
      return true;
    }
  }
  return false;
}

void Host::run_exit_handlers(const std::string_view payload_json) {
  // Exit handlers are the final trusted JS turn: drop the abort flag
  // request_exit armed so the handlers can actually run to completion. The
  // stop source (interrupt_source_) keeps its existing cadence, and a
  // pending request_exit flag itself stays set for the exit accessors.
  interrupt_.store(false, std::memory_order_relaxed);
  if (exit_ran_ || !context_) return;
  exit_ran_ = true;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> handlers;
  handlers.swap(exit_handlers_);
  for (const auto& [callback_id, subscription_id] : handlers) {
    if (const auto error = invoke_callback(callback_id, payload_json); !error.ok()) {
      (void)record("onExit", error.message);
    }
    (void)remove_callback(callback_id);
    (void)subscriptions_.remove(subscription_id);
  }
}

rime::core::Error Host::request_exit(const int code) {
  if (const auto thread_error = check_thread(); !thread_error.ok()) return thread_error;
  if (exit_requested_) return rime::core::Error::none();  // first call wins; later ones ignored
  exit_requested_ = true;
  exit_code_ = code;
  // Arm the abort: interrupt_thunk turns this into an uncatchable interrupt,
  // so everything after the throwing runtime.exit() call stops.
  interrupt_.store(true, std::memory_order_relaxed);
  if (exit_notifier_) exit_notifier_(code);
  return rime::core::Error::none();
}

void Host::set_exit_notifier(std::function<void(int)> notifier) {
  exit_notifier_ = std::move(notifier);
}

void Host::add_teardown(std::function<void()> teardown) {
  if (teardown) teardowns_.push_back(std::move(teardown));
}

void Host::run_teardowns() {
  std::vector<std::function<void()>> hooks;
  hooks.swap(teardowns_);
  for (const auto& hook : hooks) {
    try {
      hook();
    } catch (...) {
      // Teardown is native-only and must never propagate across shutdown.
    }
  }
}

void Host::set_module_data(std::string name, void* data) {
  std::lock_guard lock(module_data_mutex_);
  for (auto& [key, value] : module_data_) {
    if (key == name) {
      value = data;
      return;
    }
  }
  module_data_.emplace_back(std::move(name), data);
}

void* Host::module_data(const std::string_view name) const {
  std::lock_guard lock(module_data_mutex_);
  for (const auto& [key, value] : module_data_) {
    if (key == name) return value;
  }
  return nullptr;
}

TimerService& Host::timers() { return timer_; }

void Host::set_wakeup(std::function<void()> wakeup) {
  std::lock_guard lock(wakeup_mutex_);
  wakeup_ = std::move(wakeup);
}

void Host::set_interrupt_source(std::atomic_bool* source) { interrupt_source_ = source; }

void Host::request_interrupt() { interrupt_.store(true, std::memory_order_release); }

void Host::wake() {
  std::function<void()> wakeup;
  {
    std::lock_guard lock(wakeup_mutex_);
    wakeup = wakeup_;
  }
  if (wakeup) wakeup();
}

void Host::apply_completion(const Completion& completion) {
  if (!context_) return;

  if (completion.kind == CompletionKind::CancelById) {
    {
      std::lock_guard lock(cancellation_mutex_);
      const auto found = cancellations_.find(completion.cancellation_id);
      if (found != cancellations_.end()) found->second.cancel();
    }
    struct Cancelled {
      Pending entry;
      std::uint64_t timer_id;
    };
    std::vector<Cancelled> cancelled;
    {
      std::lock_guard lock(async_mutex_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->cancellation_id == completion.cancellation_id) {
          std::uint64_t timer_id = 0;
          const auto timer = delay_timers_.find(it->token);
          if (timer != delay_timers_.end()) {
            timer_id = timer->second;
            delay_timers_.erase(timer);
          }
          cancelled.push_back({*it, timer_id});
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (const auto& item : cancelled) {
      if (item.timer_id != 0) timer_.cancel(item.timer_id);
      JSValue reason = make_error_value(context_, "cancelled", "cancelled");
      call_handler(context_, item.entry.reject, reason);
      JS_FreeValue(context_, reason);
      JS_FreeValue(context_, item.entry.resolve);
      JS_FreeValue(context_, item.entry.reject);
    }
    return;
  }

  Pending entry{};
  bool found = false;
  {
    std::lock_guard lock(async_mutex_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->token == completion.token) {
        entry = *it;
        pending_.erase(it);
        found = true;
        break;
      }
    }
    delay_timers_.erase(completion.token);
  }
  if (!found) return;

  if (completion.kind == CompletionKind::Reject) {
    // Native failures arrive framed as "code:message" (see AsyncOutcome);
    // anything else keeps its full text and defaults to "execution_failed".
    // The prefix must be an exact error_code_name or it is treated as text,
    // so messages containing ':' can never spoof a code.
    std::string code = "execution_failed";
    std::string message = completion.value;
    if (const auto colon = completion.value.find(':'); colon != std::string::npos) {
      rime::core::Error::Code parsed = rime::core::Error::Code::ExecutionFailed;
      if (rime::core::error_code_from_name(
              std::string_view(completion.value.data(), colon), parsed)) {
        code = std::string(completion.value.data(), colon);
        message = completion.value.substr(colon + 1);
      }
    }
    JSValue reason = make_error_value(context_, code, message);
    // The rejection is always delivered: make_error_value never returns an
    // exception (worst case the handler observes undefined under OOM).
    call_handler(context_, entry.reject, reason);
    JS_FreeValue(context_, reason);
    JS_FreeValue(context_, entry.resolve);
    JS_FreeValue(context_, entry.reject);
    return;
  }

  if (entry.cancellation_id != 0 && is_cancelled(entry.cancellation_id)) {
    JSValue reason = make_error_value(context_, "cancelled", "cancelled");
    call_handler(context_, entry.reject, reason);
    JS_FreeValue(context_, reason);
    JS_FreeValue(context_, entry.resolve);
    JS_FreeValue(context_, entry.reject);
    return;
  }

  JSValue value = JS_ParseJSON(context_, completion.value.c_str(), completion.value.size(),
                               "<completion>");
  if (JS_IsException(value)) {
    JSValue exception = JS_GetException(context_);
    JS_FreeValue(context_, exception);
    value = JS_NewString(context_, completion.value.c_str());
  }
  call_handler(context_, entry.resolve, value);
  JS_FreeValue(context_, value);
  JS_FreeValue(context_, entry.resolve);
  JS_FreeValue(context_, entry.reject);
}

std::string Host::context() {
  json::Value modules = json::Value::array();
  for (const auto& name : modules_.native_modules()) modules.push(json::Value::string(name));

  json::Value tasks = json::Value::object();
  std::size_t promise_count = 0;
  std::size_t completion_count = 0;
  {
    std::lock_guard lock(async_mutex_);
    promise_count = pending_.size();
    completion_count = completions_.size();
  }
  tasks.set("async", json::Value::number(static_cast<double>(promise_count)));
  tasks.set("queued", json::Value::number(static_cast<double>(completion_count)));
  tasks.set("timers", json::Value::number(static_cast<double>(timer_.pending())));
  tasks.set("callbacks", json::Value::number(static_cast<double>(callback_count())));

  std::size_t cancellation_count = 0;
  {
    std::lock_guard lock(cancellation_mutex_);
    cancellation_count = cancellations_.size();
  }

  json::Value result = json::Value::object();
  result.set("schemaVersion", json::Value::number(1));
  result.set("modules", std::move(modules));
  result.set("tasks", std::move(tasks));
  result.set("subscriptions",
             json::Value::number(static_cast<double>(subscriptions_.list().size())));
  result.set("cancellations", json::Value::number(static_cast<double>(cancellation_count)));
  return json::stringify(result);
}

std::string Host::inspect(const std::string& request_json) {
  json::Value kind_request = json::Value::object();
  if (const auto parsed = json::parse(request_json); parsed.ok()) {
    kind_request = std::move(*parsed.value);
  }
  const json::Value* kind_value = kind_request.find("kind");
  const std::string kind =
      kind_value && kind_value->is_string() ? kind_value->as_string() : "all";
  const auto include = [&kind](std::string_view key) {
    return kind == "all" || kind == key;
  };

  json::Value result = json::Value::object();

  if (include("modules")) {
    json::Value modules_value = json::Value::object();
    json::Value natives = json::Value::array();
    for (const auto& name : modules_.native_modules()) natives.push(json::Value::string(name));
    json::Value files = json::Value::array();
    for (const auto& name : modules_.loaded_files()) files.push(json::Value::string(name));
    modules_value.set("native", std::move(natives));
    modules_value.set("files", std::move(files));
    modules_value.set("fileRoot", json::Value::string(modules_.file_root()));
    result.set("modules", std::move(modules_value));
  }

  if (include("functions") && check_thread().ok()) {
    json::Value functions = json::Value::array();
    JSValue global = JS_GetGlobalObject(context_);
    JSPropertyEnum* properties = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context_, &properties, &count, global, JS_GPN_STRING_MASK) >= 0) {
      for (uint32_t index = 0; index < count; ++index) {
        const char* name = JS_AtomToCString(context_, properties[index].atom);
        JSValue value = JS_GetProperty(context_, global, properties[index].atom);
        if (name && JS_IsFunction(context_, value)) functions.push(json::Value::string(name));
        if (name) JS_FreeCString(context_, name);
        JS_FreeValue(context_, value);
      }
    }
    if (properties) JS_FreePropertyEnum(context_, properties, count);
    JS_FreeValue(context_, global);
    result.set("functions", std::move(functions));
  }

  if (include("subscriptions")) {
    json::Value subscriptions = json::Value::array();
    for (const auto& label : subscriptions_.list()) subscriptions.push(json::Value::string(label));
    result.set("subscriptions", std::move(subscriptions));
  }

  if (include("tasks")) {
    json::Value tasks = json::Value::object();
    std::size_t promise_count = 0;
    std::size_t completion_count = 0;
    {
      std::lock_guard lock(async_mutex_);
      promise_count = pending_.size();
      completion_count = completions_.size();
    }
    tasks.set("async", json::Value::number(static_cast<double>(promise_count)));
    tasks.set("queued", json::Value::number(static_cast<double>(completion_count)));
  tasks.set("timers", json::Value::number(static_cast<double>(timer_.pending())));
  tasks.set("worker", json::Value::number(
                          static_cast<double>(rime::core::WorkerService::instance().pending())));
    tasks.set("worker", json::Value::number(
                            static_cast<double>(rime::core::WorkerService::instance().pending())));
    tasks.set("callbacks", json::Value::number(static_cast<double>(callback_count())));
    result.set("tasks", std::move(tasks));
  }

  if (include("errors")) {
    json::Value errors = json::Value::array();
    std::lock_guard lock(error_mutex_);
    for (const auto& entry : errors_) {
      json::Value item = json::Value::object();
      item.set("where", json::Value::string(entry.where));
      item.set("message", json::Value::string(entry.message));
      errors.push(std::move(item));
    }
    result.set("errors", std::move(errors));
  }

  return json::stringify(result);
}

std::vector<ErrorRecord> Host::errors() const {
  std::lock_guard lock(error_mutex_);
  return errors_;
}

std::size_t Host::error_count() const {
  std::lock_guard lock(error_mutex_);
  return errors_.size();
}

std::string Host::new_error_since(const std::size_t mark) const {
  std::lock_guard lock(error_mutex_);
  if (errors_.size() <= mark) return {};
  return errors_.back().message;
}

}  // namespace rime::js
