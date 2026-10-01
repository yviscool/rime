#pragma once

#include "rime/action/action.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/js/host.hpp"

#include "quickjs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace rime::win32 {

// Runs `work` on the timer thread and settles the returned promise with its
// (succeeded, payload) outcome. Payload resolves as JSON on success and
// rejects as an Error message on failure. Shared by the native modules so
// every async query/mutation drains through the same host accounting.
template <typename Work>
JSValue start_async(JSContext* context, Work work) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  JSValue promise = JS_UNDEFINED;
  std::uint64_t token = 0;
  if (const auto error = host->begin_async(context, promise, token); !error.ok()) {
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
// The id comes from the host's shared counter; the deadline is now+5s.
inline rime::action::Action make_action(std::atomic<std::uint64_t>& next_action_id,
                                        std::string module, std::string type,
                                        std::string capability, rime::action::Identity target,
                                        std::string payload) {
  rime::action::Action action;
  action.id = next_action_id.fetch_add(1) + 1;
  action.schema_version = 1;
  action.source = {"js", std::move(module)};
  action.type = std::move(type);
  action.capability = std::move(capability);
  action.target = std::move(target);
  action.payload = std::move(payload);
  action.deadline_unix_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      5000);
  return action;
}

// Executes `action` on the timer thread through the kernel, settling the
// promise with the result value or the failure reason.
inline JSValue run_action(JSContext* context, rime::action::Kernel& kernel,
                          rime::action::Action action) {
  return start_async(context,
                     [&kernel, action = std::move(action)]() -> std::pair<bool, std::string> {
                       const auto result = kernel.execute(action);
                       if (result.succeeded) return {true, rime::core::json::stringify(result.value)};
                       return {false, result.error.ok() ? result.detail : result.error.message};
                     });
}

}  // namespace rime::win32

