#pragma once

#include "rime/action/action.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/js/host.hpp"

#include "quickjs.h"

#include <atomic>
#include <chrono>
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

// Reads an optional uint64 property. Missing/undefined leaves `out` alone;
// a present non-number or negative value throws a TypeError. Returns false
// only when a throw happened.
inline bool optional_u64(JSContext* context, JSValueConst object, const char* name,
                         std::uint64_t& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property)) {
    if (!JS_IsNumber(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "options.%s must be a number", name);
      return false;
    }
    int64_t raw = 0;
    if (JS_ToInt64(context, &raw, property)) {
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
  if (!JS_IsUndefined(key)) {
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
// (succeeded, payload) outcome. Payload resolves as JSON on success and
// rejects as an Error message on failure. Shared by the native modules so
// every async query/mutation drains through the same host accounting. When
// `cancellation_id` is bound, cancelling it rejects the pending promise.
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
                        try {
                          auto outcome = work();
                          host->complete_async(token, outcome.first, std::move(outcome.second));
                        } catch (const std::exception& exception) {
                          host->complete_async(token, false, exception.what());
                        } catch (...) {
                          host->complete_async(token, false, "native module task failed");
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
  action.deadline_unix_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      static_cast<std::int64_t>(options.deadline_ms));
  return action;
}

// Executes `action` on the timer thread through the kernel, settling the
// promise with the result value or the failure reason. `cancellation_id`
// (when non-zero) passes the live cancellation token into the executor and
// binds the promise so cancel_by_id rejects it early.
inline JSValue run_action(JSContext* context, rime::action::Kernel& kernel,
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
  rime::action::Kernel* kernel_ptr = &kernel;
  host->schedule_task(
      token, std::chrono::milliseconds(0),
      [host, token, kernel_ptr, action = std::move(action), cancellation]() mutable {
        try {
          const auto result = kernel_ptr->execute(action, cancellation);
          if (result.succeeded) {
            host->complete_async(token, true, rime::core::json::stringify(result.value));
          } else {
            host->complete_async(token, false,
                                 result.error.ok() ? result.detail : result.error.message);
          }
        } catch (const std::exception& exception) {
          host->complete_async(token, false, exception.what());
        } catch (...) {
          host->complete_async(token, false, "native module task failed");
        }
      });
  return promise;
}

}  // namespace rime::win32
