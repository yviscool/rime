#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/scheduler_policy.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rime::action {

enum class DispatchStatus : std::uint8_t { Accepted, Full, Closed, Coalesced };

// Production queue policy: one bounded queue with idempotency-key
// coalescing, so a repeated mutation supersedes its predecessor instead of
// running twice. Central so no module invents its own policy.
rime::core::SchedulerPolicy default_dispatch_policy();

class Dispatcher final {
 public:
  Dispatcher(Kernel& kernel, std::size_t capacity);
  Dispatcher(Kernel& kernel, rime::core::SchedulerPolicy policy);

  DispatchStatus submit(Action action);
  // Executes up to `budget` pending actions, then returns results for any
  // actions that were dropped or coalesced by the queue policy so every
  // submitted action always produces exactly one Result.
  std::vector<Result> pump(std::size_t budget, rime::core::CancellationToken cancellation = {});
  // Per-action cancellation resolver: one JS queue is shared by every
  // module, so each queued action resolves its own AbortSignal binding when
  // it reaches the head, instead of one batch-wide token.
  std::vector<Result> pump(
      std::size_t budget,
      const std::function<rime::core::CancellationToken(const Action&)>& cancellation_of);
  // NOTE: not noexcept: close() takes mutex_ via std::lock_guard, which may
  // throw; marking it noexcept would risk std::terminate.
  void close();
  [[nodiscard]] bool closed() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t dropped() const;
  [[nodiscard]] std::size_t capacity() const;

 private:
  static std::string coalesce_key(const Action& action);
  // NOTE: requires the caller to already hold mutex_ (called from submit()
  // while the queue lock is held). Name is kept for existing call sites.
  void suppress(Action action, std::string reason);
  // Trace decisions with the shared action sink (kernel.trace_sink()).
  // Never called while mutex_ is held: a user sink re-entering the
  // dispatcher would deadlock on the non-recursive queue lock, and sink
  // latency must not stall queueing. Throws from the sink are swallowed so
  // tracing can never fail a dispatch.
  void record(rime::core::TraceKind kind, std::string subject, rime::core::ActionId id,
              std::string capability, std::string detail, std::string result_code);

  Kernel& kernel_;
  rime::core::SchedulerPolicy policy_;
  mutable std::mutex mutex_;
  std::deque<Action> pending_;
  std::vector<Result> suppressed_;
  std::size_t dropped_{0};
  bool closed_{false};
};

}  // namespace rime::action
