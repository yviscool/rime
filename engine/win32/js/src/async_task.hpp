#pragma once

#include "rime/action/action.hpp"
#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/js/host.hpp"

#include "quickjs.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace rime::win32 {

// Options every native-module Action accepts from JS (`ActionOptions`).
// deadline_ms is a relative budget applied at build time; cancellation_id
// binds the action to a runtime cancellation source so AbortSignal-style
// cancellation reaches the executor token.
struct ActionOptions {
  std::uint64_t deadline_ms{5000};
  std::uint64_t cancellation_id{0};
  std::uint64_t parent_action_id{0};
  std::string idempotency_key;
};

// Strict JS number -> int64 conversion shared by id/cancellation parsing.
// Rejects non-numbers, NaN/Infinity, fractions and values outside the
// exactly-representable integer range with a TypeError instead of silently
// truncating or wrapping them.
inline bool js_int64_strict(JSContext* context, JSValueConst value, int64_t& out,
                            const char* what) {
  if (!JS_IsNumber(value)) {
    JS_ThrowTypeError(context, "%s must be a number", what);
    return false;
  }
  double number = 0;
  if (JS_ToFloat64(context, &number, value)) return false;
  if (!std::isfinite(number) || std::trunc(number) != number) {
    JS_ThrowTypeError(context, "%s must be an integer", what);
    return false;
  }
  constexpr double kMaxSafeInteger = 9007199254740991.0;  // 2^53 - 1
  if (number < -kMaxSafeInteger || number > kMaxSafeInteger) {
    JS_ThrowTypeError(context, "%s is out of range", what);
    return false;
  }
  if (JS_ToInt64(context, &out, value)) return false;
  return true;
}

// Outcome of an async native body: JSON payload on success; kernel-style code
// name (see rime::core::error_code_name) plus message on failure. Failures are
// framed as "code:message" through complete_async so Host::apply_completion
// can reject with an Error carrying both properties.
struct AsyncOutcome {
  bool ok{false};
  std::string code;
  std::string payload;
};

inline AsyncOutcome async_success(std::string payload) { return {true, {}, std::move(payload)}; }

inline AsyncOutcome async_failure(std::string code, std::string message) {
  return {false, std::move(code), std::move(message)};
}

inline AsyncOutcome async_failure(rime::core::Error error) {
  return {false, rime::core::error_code_name(error.code), std::move(error.message)};
}

// Reads an optional uint64 property. Missing/undefined/null leaves `out`
// alone; a present NaN, fraction, negative or out-of-range value throws a
// TypeError. Returns false only when a throw happened.
inline bool optional_u64(JSContext* context, JSValueConst object, const char* name,
                         std::uint64_t& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    std::string label = std::string("options.") + name;
    int64_t raw = 0;
    if (!js_int64_strict(context, property, raw, label.c_str())) {
      JS_FreeValue(context, property);
      return false;
    }
    if (raw < 0) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "options.%s must not be negative", name);
      return false;
    }
    out = static_cast<std::uint64_t>(raw);
  }
  JS_FreeValue(context, property);
  return true;
}

// Parses the optional trailing `options` argument shared by every mutating
// native function. Missing/undefined yields defaults; malformed values throw.
inline bool parse_action_options(JSContext* context, JSValueConst value, ActionOptions& out) {
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  if (!JS_IsObject(value)) {
    JS_ThrowTypeError(context, "options must be an object");
    return false;
  }
  if (!optional_u64(context, value, "deadlineMs", out.deadline_ms) ||
      !optional_u64(context, value, "cancellationId", out.cancellation_id) ||
      !optional_u64(context, value, "parentActionId", out.parent_action_id)) {
    return false;
  }
  JSValue key = JS_GetPropertyStr(context, value, "idempotencyKey");
  if (JS_IsException(key)) return false;
  if (!JS_IsUndefined(key) && !JS_IsNull(key)) {
    if (!JS_IsString(key)) {
      JS_FreeValue(context, key);
      JS_ThrowTypeError(context, "options.idempotencyKey must be a string");
      return false;
    }
    const char* text = JS_ToCString(context, key);
    if (!text) {
      JS_FreeValue(context, key);
      return false;
    }
    out.idempotency_key = text;
    JS_FreeCString(context, text);
  }
  JS_FreeValue(context, key);
  return true;
}

// Runs `work` on the timer thread and settles the returned promise with its
// AsyncOutcome. Success resolves as JSON; failure rejects with an Error
// carrying the kernel-style `code` name plus the message. Shared by the native
// modules so every async query/mutation drains through the same host
// accounting. When `cancellation_id` is bound, cancelling it rejects the
// pending promise.
template <typename Work>
JSValue start_async(JSContext* context, Work work, std::uint64_t cancellation_id = 0) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  JSValue promise = JS_UNDEFINED;
  std::uint64_t token = 0;
  if (const auto error = host->begin_async(context, promise, token, cancellation_id); !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  host->schedule_task(token, std::chrono::milliseconds(0),
                      [host, token, work = std::move(work)]() mutable {
                        // Ownership: `host` (raw) outlives every scheduled task;
                        // TimerService::stop runs before Host teardown completes.
                        try {
                          AsyncOutcome outcome = work();
                          if (outcome.ok) {
                            host->complete_async(token, true, std::move(outcome.payload));
                          } else {
                            host->complete_async(token, false,
                                                 outcome.code + ":" + outcome.payload);
                          }
                        } catch (const std::exception& exception) {
                          host->complete_async(
                              token, false,
                              std::string("execution_failed:") + exception.what());
                        } catch (...) {
                          host->complete_async(token, false,
                                               "execution_failed:native module task failed");
                        }
                      });
  return promise;
}

// Builds the Action every native-module mutation sends through the kernel.
// The id comes from the host's shared counter; the deadline is now plus the
// options budget (default 5s).
inline rime::action::Action make_action(std::atomic<std::uint64_t>& next_action_id,
                                        std::string module, std::string type,
                                        std::string capability, rime::action::Identity target,
                                        std::string payload,
                                        const ActionOptions& options = {}) {
  rime::action::Action action;
  action.id = next_action_id.fetch_add(1) + 1;
  action.schema_version = 1;
  action.source = {"js", std::move(module)};
  action.type = std::move(type);
  action.capability = std::move(capability);
  action.target = std::move(target);
  action.payload = std::move(payload);
  action.parent_action_id = options.parent_action_id;
  action.idempotency_key = options.idempotency_key;
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const std::uint64_t now_u = now_ms > 0 ? static_cast<std::uint64_t>(now_ms) : 0;
  // Saturate instead of wrapping: an extreme deadlineMs pins the deadline at
  // the end of the uint64 range instead of overflowing into the past.
  action.deadline_unix_ms =
      options.deadline_ms > UINT64_MAX - now_u ? UINT64_MAX : now_u + options.deadline_ms;
  return action;
}

// Settles a routed promise from its dispatcher Result. Success resolves as
// JSON; failure rejects with the kernel-style `code` name plus the message.
// Shared with the queue pump so direct and queued settlement format exactly
// alike; a token already settled (e.g. by cancel_by_id) is tolerated by
// Host::apply_completion.
inline void settle_from_result(rime::js::Host* host, std::uint64_t token,
                               const rime::action::Result& result) {
  if (result.succeeded) {
    host->complete_async(token, true, rime::core::json::stringify(result.value));
    return;
  }
  const bool has_error = !result.error.ok();
  const std::string code =
      has_error ? rime::core::error_code_name(result.error.code) : "execution_failed";
  const std::string& message = has_error ? result.error.message : result.detail;
  host->complete_async(token, false, code + ":" + message);
}

// Queues `action` through the dispatcher and settles the promise when a
// queue pump executes it, so every mutation shares one bounded, inspectable
// pipeline (capacity, coalescing, trace) instead of racing straight against
// the kernel. The route is bound before submit so the pump can always find
// the promise; a submit rejected by queue policy settles it inline.
inline JSValue run_action(JSContext* context, rime::action::Dispatcher& dispatcher,
                          rime::action::Action action, std::uint64_t cancellation_id = 0) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  JSValue promise = JS_UNDEFINED;
  std::uint64_t token = 0;
  if (const auto error = host->begin_async(context, promise, token, cancellation_id); !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  rime::core::CancellationToken cancellation;
  if (cancellation_id != 0) cancellation = host->cancellation_token(cancellation_id);
  const std::uint64_t action_id = action.id;
  host->bind_route(action_id, rime::js::Host::AsyncRoute{token, cancellation});
  const auto status = dispatcher.submit(std::move(action));
  if (status != rime::action::DispatchStatus::Accepted &&
      status != rime::action::DispatchStatus::Coalesced) {
    rime::js::Host::AsyncRoute route;
    host->take_route(action_id, route);
    host->complete_async(token, false,
                         status == rime::action::DispatchStatus::Closed
                             ? "invalid_state:dispatcher closed"
                             : "queue_full:action queue is full");
    return promise;
  }
  rime::action::Dispatcher* dispatcher_ptr = &dispatcher;
  host->schedule_task(token, std::chrono::milliseconds(0),
                      [host, token, dispatcher_ptr]() mutable {
                        // Ownership: `host`/`dispatcher_ptr` (raw) outlive every
                        // scheduled task; kernel, dispatcher and host teardown only
                        // run after pending tasks settle.
                        try {
                          const auto results = dispatcher_ptr->pump(
                              dispatcher_ptr->capacity(),
                              [host](const rime::action::Action& queued) {
                                rime::js::Host::AsyncRoute route;
                                if (host->find_route(queued.id, route)) return route.cancellation;
                                return rime::core::CancellationToken{};
                              });
                          for (const auto& result : results) {
                            rime::js::Host::AsyncRoute route;
                            if (host->take_route(result.id, route)) {
                              settle_from_result(host, route.token, result);
                            }
                          }
                        } catch (const std::exception& exception) {
                          host->complete_async(
                              token, false,
                              std::string("execution_failed:") + exception.what());
                        } catch (...) {
                          host->complete_async(token, false,
                                               "execution_failed:native module task failed");
                        }
                      });
  return promise;
}

}  // namespace rime::win32
