#pragma once

#include "rime/core/cancellation.hpp"
#include "rime/core/types.hpp"
#include "rime/js/module_registry.hpp"
#include "rime/js/subscriptions.hpp"
#include "rime/js/timer.hpp"

#include "quickjs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rime::js {

struct ErrorRecord {
  std::string where;
  std::string message;
};

// Thread-safe bridge that delivers (callback id, JSON payload) items into
// the host's drain on the JS thread. Native producers (input hooks, COM
// callbacks) hold a shared_ptr; closing the queue on teardown turns their
// pushes into no-ops, so producers never touch a dead host.
class HostEventQueue final {
 public:
  using Wakeup = std::function<void()>;

  // Any thread. Returns false once the queue is closed.
  bool push(std::uint64_t callback_id, std::string payload);
  // JS thread: returns and clears pending items.
  std::vector<std::pair<std::uint64_t, std::string>> take();
  [[nodiscard]] bool empty() const;
  void set_wakeup(Wakeup wakeup);
  // Idempotent: rejects future pushes, drops queued items, clears wakeup.
  void close();

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<std::uint64_t, std::string>> items_;
  Wakeup wakeup_;
  bool closed_{false};
};

// Owns one QuickJS runtime/context. Every method except complete_async /
// cancel_cancellation / set_wakeup / invoke_callback's queue producers must
// run on the owning (JS) thread.
class Host final {
 public:
  Host();
  ~Host();
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  rime::core::Error eval(std::string_view source, std::string_view filename = "<eval>");
  rime::core::Error eval_module(std::string_view source, std::string_view filename = "<module>");

  // Resolves queued async completions, then drains the promise job queue.
  // Returns how many completions and jobs were processed.
  std::size_t drain();
  // True when no completion is queued and no timer is armed.
  [[nodiscard]] bool idle() const;

  // JS thread: creates a promise bound to `token`.
  rime::core::Error begin_async(JSContext* context, JSValue& promise_out,
                                std::uint64_t& token_out, std::uint64_t cancellation_id = 0);
  // Any thread: queue resolution/rejection of a token (JS thread applies it).
  void complete_async(std::uint64_t token, bool succeeded, std::string value);
  // Any thread: reject the token with an error message.
  void reject_async(std::uint64_t token, std::string message);
  // Any thread: cancel every promise bound to `cancellation_id`.
  // Returns false when the id is unknown.
  bool cancel_by_id(std::uint64_t cancellation_id);
  // JS thread: arm a timer that resolves `token` with `value` after `ms`.
  void schedule_delay(std::uint64_t token, std::uint32_t ms, std::string value);
  // JS thread: arm a timer that runs `task` (which normally completes
  // `token`). Until its completion lands, `idle()` stays false; task
  // exceptions reject the token. Cancelling the token's cancellation id
  // disarms the timer.
  void schedule_task(std::uint64_t token, std::chrono::milliseconds delay,
                     std::function<void()> task);
  // JS thread: run `task` on the worker lane (blocking native reads and
  // action queue pumps) instead of the timer thread. Starts the worker
  // lazily; a worker that is stopping rejects the token like schedule_task.
  void schedule_worker(std::uint64_t token, std::function<void()> task);
  // JS thread: worker-lane task without a promise token (e.g. chord pumps
  // that settle nothing). Returns false when the worker is unavailable so
  // the caller can record the failure.
  bool post_worker(std::function<void()> task);
  [[nodiscard]] std::size_t pending_async() const;

  // Cancellation ids shared with JS (runtime.cancellation/runtime.cancel).
  std::uint64_t create_cancellation();
  bool is_cancelled(std::uint64_t id);
  // Any thread: a token bound to the same source cancel_by_id flips, so
  // native executors can observe JS-side AbortSignal cancellation. Unknown
  // ids yield a never-cancelled token.
  rime::core::CancellationToken cancellation_token(std::uint64_t id);
  bool release_cancellation(std::uint64_t id);

  // Async routes bind a producer key (e.g. an Action id) to a promise token
  // plus the cancellation its executor must observe, so a queue pump that
  // executes a foreign action can settle that action's promise. Producers
  // bind before queueing; the pump settles exactly once via take_route.
  // find_route is the non-owning probe used to resolve cancellation during
  // execution. Any thread.
  struct AsyncRoute {
    std::uint64_t token{0};
    rime::core::CancellationToken cancellation;
  };
  void bind_route(std::uint64_t key, AsyncRoute route);
  [[nodiscard]] bool find_route(std::uint64_t key, AsyncRoute& out) const;
  bool take_route(std::uint64_t key, AsyncRoute& out);

  // JS callbacks registered through rime:runtime.subscribe.
  rime::core::Error add_callback(JSValue callback, std::uint64_t& id_out);
  rime::core::Error remove_callback(std::uint64_t id);
  // JS thread: invokes the callback with a parsed JSON argument.
  rime::core::Error invoke_callback(std::uint64_t id, std::string_view argument_json);
  [[nodiscard]] std::size_t callback_count() const;

  // Reserves a unique id from the same counter add_callback draws from, so a
  // native module that registers its own SubscriptionRegistry entries (or
  // tracks ids without registering them, like onExit) can never collide with
  // "js#<callback>" ids. The caller registers the id in subscriptions() or
  // releases it; this only hands out the number.
  [[nodiscard]] std::uint64_t allocate_subscription_id();

  // JS thread: error observers (rime:input.onError) receive {where, message}
  // for every record()ed failure. record() is the single sink, so eval
  // failures, unhandled rejections and native diagnostics all arrive here.
  // While an observer callback runs, record() appends but does not re-push:
  // an observer that always throws would otherwise loop forever.
  void add_error_observer(std::uint64_t callback_id);
  bool remove_error_observer(std::uint64_t callback_id);

  // JS thread: ordered exit handlers (rime:input.onExit). Registration after
  // run_exit_handlers() has fired is refused - exit runs once. Each entry
  // owns its callback plus the SubscriptionRegistry id its registration
  // added, so running the handlers releases both (unload stays clean).
  rime::core::Error add_exit_handler(std::uint64_t callback_id, std::uint64_t subscription_id);
  // JS thread: removes a not-yet-run exit handler (subscription.close() on an
  // onExit registration). False when unknown or after the handlers ran.
  bool remove_exit_handler(std::uint64_t subscription_id);
  // JS thread: how many exit handlers are still waiting. Unload excludes
  // them from the held-callback count because running them is part of a
  // successful unload (see HostAbi::unload).
  [[nodiscard]] std::size_t exit_handler_count() const { return exit_handlers_.size(); }
  // JS thread: invokes every exit handler in registration order with
  // `payload_json` ({"reason": ...}), then releases them. Idempotent; runs
  // from Runtime::stop, HostAbi exit/unload and ~Host, so handlers always
  // complete before unload/teardown finishes.
  void run_exit_handlers(std::string_view payload_json);
  [[nodiscard]] bool exit_handlers_ran() const { return exit_ran_; }

  // JS thread: runtime.exit(code?) - records the first requested code, arms
  // the interrupt that aborts the rest of the script, and reports the code
  // to the exit notifier. First call wins: later calls are ignored and
  // return none, so a script can never overwrite an earlier decision.
  rime::core::Error request_exit(int code);
  // JS thread only: mirrors request_exit (see above); safe to read from the
  // owner thread once the JS thread is joined.
  [[nodiscard]] bool exit_requested() const { return exit_requested_; }
  [[nodiscard]] int exit_code() const { return exit_code_; }
  // Thread convention matches set_wakeup: installed by the owning Runtime on
  // the JS thread before any script runs, invoked on the JS thread from
  // request_exit. JS-thread only, so it needs no lock.
  void set_exit_notifier(std::function<void(int)> notifier);

  // JS thread: native teardown hooks (unsubscribe services, remove UI
  // observers). run_teardowns() runs them exactly once; ~Host and a
  // successful HostAbi::unload both call it. Unlike exit handlers these run
  // without touching JS.
  void add_teardown(std::function<void()> teardown);
  void run_teardowns();

  // Producers push events from any thread; drain() invokes the callbacks.
  [[nodiscard]] std::shared_ptr<HostEventQueue> event_queue() const { return events_; }

  TimerService& timers();

  // Per-module wiring data (e.g. the window/input service binding). Set
  // before the module loads; the caller keeps ownership for the host's
  // lifetime.
  void set_module_data(std::string name, void* data);
  [[nodiscard]] void* module_data(std::string_view name) const;

  ModuleRegistry& modules() { return modules_; }
  const ModuleRegistry& modules() const { return modules_; }
  SubscriptionRegistry& subscriptions() { return subscriptions_; }

  void set_wakeup(std::function<void()> wakeup);
  void set_interrupt_source(std::atomic_bool* source);
  void request_interrupt();

  std::string inspect(const std::string& request_json);
  // Read-only environment snapshot for scripts (runtime.context): a stable,
  // versioned view of what this runtime currently is - registered modules
  // plus live host-owned work and ownership. Unlike inspect's debug protocol
  // (kind requests, error detail) this is a script contract: flat, versioned,
  // diagnostics-free. Built fresh per call; callers can mutate the result
  // without affecting the host.
  std::string context();
  [[nodiscard]] std::vector<ErrorRecord> errors() const;
  [[nodiscard]] std::size_t error_count() const;
  // Returns the newest message recorded after `mark`, or "" when quiet.
  [[nodiscard]] std::string new_error_since(std::size_t mark) const;
  // Appends a diagnostic to the host error log (thread-safe); native modules
  // use it for failures that have no promise to settle, so the same errors
  // the inspect/debugger interfaces read stay complete.
  rime::core::Error record(std::string where, std::string message);

  [[nodiscard]] std::thread::id owner() const { return owner_; }

 private:
  enum class CompletionKind : std::uint8_t { Resolve, Reject, CancelById };
  struct Pending {
    std::uint64_t token;
    JSValue resolve;
    JSValue reject;
    std::uint64_t cancellation_id;
  };
  struct Completion {
    std::uint64_t token;
    std::uint64_t cancellation_id;
    CompletionKind kind;
    std::string value;
  };

  static int interrupt_thunk(JSRuntime* runtime, void* opaque);
  static char* normalize_thunk(JSContext* context, const char* base_name,
                               const char* module_name, void* opaque);
  static JSModuleDef* loader_thunk(JSContext* context, const char* module_name, void* opaque);
  static void rejection_thunk(JSContext* context, JSValueConst promise, JSValueConst reason,
                              bool is_handled, void* opaque);

  void register_runtime_module();
  rime::core::Error check_thread() const;
  rime::core::Error exception_error(std::string where);
  void apply_completion(const Completion& completion);
  void wake();

  JSRuntime* runtime_{nullptr};
  JSContext* context_{nullptr};
  std::thread::id owner_;
  std::atomic_bool interrupt_{false};
  std::atomic_bool* interrupt_source_{nullptr};
  // JS thread only: runtime.exit bookkeeping written by request_exit and
  // read by the exit accessors / run_exit_handlers on the same thread.
  bool exit_requested_{false};
  int exit_code_{0};
  std::function<void(int)> exit_notifier_;

  TimerService timer_;

  mutable std::mutex async_mutex_;
  std::vector<Pending> pending_;
  std::vector<Completion> completions_;
  std::unordered_map<std::uint64_t, std::uint64_t> delay_timers_;
  std::uint64_t next_token_{1};

  mutable std::mutex cancellation_mutex_;
  std::unordered_map<std::uint64_t, rime::core::CancellationSource> cancellations_;
  std::uint64_t next_cancellation_{1};

  mutable std::mutex route_mutex_;
  std::unordered_map<std::uint64_t, AsyncRoute> routes_;

  mutable std::mutex callback_mutex_;
  std::vector<std::pair<std::uint64_t, JSValue>> callbacks_;
  std::uint64_t next_callback_{1};

  mutable std::mutex wakeup_mutex_;
  std::function<void()> wakeup_;

  mutable std::mutex error_mutex_;
  std::vector<ErrorRecord> errors_;
  // Guarded by error_mutex_; copied under the lock, pushed outside it.
  std::vector<std::uint64_t> error_observers_;
  // Set on the JS thread while an error-observer callback is executing.
  std::atomic<bool> delivering_errors_{false};

  // JS thread only: ordered exit handlers (callback id, subscription id).
  std::vector<std::pair<std::uint64_t, std::uint64_t>> exit_handlers_;
  bool exit_ran_{false};
  // JS thread only: native teardown hooks.
  std::vector<std::function<void()>> teardowns_;

  ModuleRegistry modules_;
  SubscriptionRegistry subscriptions_;
  std::shared_ptr<HostEventQueue> events_;

  mutable std::mutex module_data_mutex_;
  std::vector<std::pair<std::string, void*>> module_data_;
};

}  // namespace rime::js
