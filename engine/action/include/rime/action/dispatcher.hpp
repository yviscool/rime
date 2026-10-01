#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/scheduler_policy.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace rime::action {

enum class DispatchStatus : std::uint8_t { Accepted, Full, Closed, Coalesced };

class Dispatcher final {
 public:
  Dispatcher(Kernel& kernel, std::size_t capacity);
  Dispatcher(Kernel& kernel, rime::core::SchedulerPolicy policy);

  DispatchStatus submit(Action action);
  // Executes up to `budget` pending actions, then returns results for any
  // actions that were dropped or coalesced by the queue policy so every
  // submitted action always produces exactly one Result.
  std::vector<Result> pump(std::size_t budget, rime::core::CancellationToken cancellation = {});
  // NOTE: not noexcept: close() takes mutex_ via std::lock_guard, which may
  // throw; marking it noexcept would risk std::terminate.
  void close();
  [[nodiscard]] bool closed() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t dropped() const;

 private:
  static std::string coalesce_key(const Action& action);
  // NOTE: requires the caller to already hold mutex_ (called from submit()
  // while the queue lock is held). Name is kept for existing call sites.
  void suppress(Action action, std::string reason);

  Kernel& kernel_;
  rime::core::SchedulerPolicy policy_;
  // TODO(trace): record submit/pump/suppress/close decisions (input, Context,
  // executor, result, error) into TraceSink. Not injected here to avoid a
  // cross-module constructor/ownership chain; keep Dispatcher trace-free until
  // the Trace ownership design lands.
  mutable std::mutex mutex_;
  std::deque<Action> pending_;
  std::vector<Result> suppressed_;
  std::size_t dropped_{0};
  bool closed_{false};
};

}  // namespace rime::action
