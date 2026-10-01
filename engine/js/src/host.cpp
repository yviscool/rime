#include "rime/js/host.hpp"

#include "rime/core/json.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace rime::js {
namespace {

namespace json = rime::core::json;

Host* host_of(JSContext* context) {
  return static_cast<Host*>(JS_GetContextOpaque(context));
}

JSValue make_error_value(JSContext* context, const std::string& message) {
  JSValue error = JS_NewError(context);
  JS_SetPropertyStr(context, error, "message", JS_NewString(context, message.c_str()));
  return error;
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
    if (JS_ToInt64(context, &raw_id, argv[2])) return JS_EXCEPTION;
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
  if (argc < 1 || JS_ToInt64(context, &raw_id, argv[0])) return JS_EXCEPTION;
  return JS_NewBool(context, host->cancel_by_id(static_cast<std::uint64_t>(raw_id)));
}

JSValue runtime_release_cancellation(JSContext* context, JSValueConst, int argc,
                                     JSValueConst* argv) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  int64_t raw_id = 0;
  if (argc < 1 || JS_ToInt64(context, &raw_id, argv[0])) return JS_EXCEPTION;
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
  if (argc < 1 || JS_ToInt64(context, &raw_id, argv[0])) return JS_EXCEPTION;
  return JS_NewBool(context, host->remove_callback(static_cast<std::uint64_t>(raw_id)).ok());
}

JSValue runtime_inspect(JSContext* context, JSValueConst, int, JSValueConst*) {
  Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  std::string report = host->inspect("{}");
  return JS_NewString(context, report.c_str());
}

int runtime_module_init(JSContext* context, JSModuleDef* module) {
  JSValue object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "ping", JS_NewCFunction(context, runtime_ping, "ping", 0));
  JS_SetPropertyStr(context, object, "delay",
                    JS_NewCFunction(context, runtime_delay, "delay", 3));
  JS_SetPropertyStr(context, object, "cancellation",
                    JS_NewCFunction(context, runtime_cancellation, "cancellation", 0));
  JS_SetPropertyStr(context, object, "cancel",
                    JS_NewCFunction(context, runtime_cancel, "cancel", 1));
  JS_SetPropertyStr(context, object, "releaseCancellation",
                    JS_NewCFunction(context, runtime_release_cancellation,
                                     "releaseCancellation", 1));
  JS_SetPropertyStr(context, object, "subscribe",
                    JS_NewCFunction(context, runtime_subscribe, "subscribe", 1));
  JS_SetPropertyStr(context, object, "unsubscribe",
                    JS_NewCFunction(context, runtime_unsubscribe, "unsubscribe", 1));
  JS_SetPropertyStr(context, object, "inspect",
                    JS_NewCFunction(context, runtime_inspect, "inspect", 0));
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
  if (events_) events_->close();
  timer_.stop();
  subscriptions_.close();
  if (context_) {
    {
      std::lock_guard lock(callback_mutex_);
      for (auto& [id, callback] : callbacks_) JS_FreeValue(context_, callback);
      callbacks_.clear();
    }
    {
      std::lock_guard lock(async_mutex_);
      for (auto& entry : pending_) {
        JS_FreeValue(context_, entry.resolve);
        JS_FreeValue(context_, entry.reject);
      }
      pending_.clear();
      completions_.clear();
      delay_timers_.clear();
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
  {
    std::lock_guard lock(error_mutex_);
    errors_.push_back({std::move(where), message});
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
      if (invoke_callback(callback_id, payload).ok()) ++processed;
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
      [this, token, value = std::move(value)] { complete_async(token, true, value); });
  std::lock_guard lock(async_mutex_);
  if (timer_id == 0) {
    completions_.push_back({token, 0, CompletionKind::Reject, "timer service is stopping"});
    return;
  }
  delay_timers_[token] = timer_id;
}

void Host::schedule_task(const std::uint64_t token, const std::chrono::milliseconds delay,
                         std::function<void()> task) {
  const std::uint64_t timer_id = timer_.schedule(delay, [this, token, task = std::move(task)] {
    try {
      task();
    } catch (const std::exception& exception) {
      complete_async(token, false, exception.what());
    } catch (...) {
      complete_async(token, false, "native task failed");
    }
  });
  {
    std::lock_guard lock(async_mutex_);
    if (timer_id != 0) {
      delay_timers_[token] = timer_id;
      return;
    }
    completions_.push_back({token, 0, CompletionKind::Reject, "timer service is stopping"});
  }
  wake();
}

std::size_t Host::pending_async() const {
  std::lock_guard lock(async_mutex_);
  return pending_.size();
}

std::uint64_t Host::create_cancellation() {
  std::lock_guard lock(cancellation_mutex_);
  const std::uint64_t id = next_cancellation_++;
  cancellations_.emplace(id, rime::core::CancellationSource{});
  return id;
}

bool Host::is_cancelled(const std::uint64_t id) {
  std::lock_guard lock(cancellation_mutex_);
  const auto found = cancellations_.find(id);
  return found != cancellations_.end() && found->second.token().cancelled();
}

bool Host::release_cancellation(const std::uint64_t id) {
  std::lock_guard lock(cancellation_mutex_);
  return cancellations_.erase(id) > 0;
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
      JSValue reason = make_error_value(context_, "cancelled");
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
    JSValue reason = make_error_value(context_, completion.value);
    call_handler(context_, entry.reject, reason);
    JS_FreeValue(context_, reason);
    JS_FreeValue(context_, entry.resolve);
    JS_FreeValue(context_, entry.reject);
    return;
  }

  if (entry.cancellation_id != 0 && is_cancelled(entry.cancellation_id)) {
    JSValue reason = make_error_value(context_, "cancelled");
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
