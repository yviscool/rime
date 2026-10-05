#include "rime/action/dispatcher.hpp"

#include "rime/core/clock.hpp"

#include <algorithm>
#include <utility>

namespace rime::action {

Dispatcher::Dispatcher(Kernel& kernel, const std::size_t capacity)
    : Dispatcher(kernel, rime::core::SchedulerPolicy::bounded(capacity)) {}

Dispatcher::Dispatcher(Kernel& kernel, rime::core::SchedulerPolicy policy)
    : kernel_(kernel), policy_(policy) {}

std::string Dispatcher::coalesce_key(const Action& action) {
  // Explicit idempotency keys are preserved as-is so retries coalesce.
  if (!action.idempotency_key.empty()) return action.idempotency_key;
  // NOTE: key must include capability + payload, not just type + target.
  // Otherwise window.move left would swallow a queued window.move right
  // because both share type/target but differ in payload.
  std::string key;
  key.reserve(action.type.size() + action.capability.size() + action.target.kind.size() +
              action.target.id.size() + action.payload.size() + 8);
  key.append(action.type);
  key.push_back('|');
  key.append(action.capability);
  key.push_back('|');
  key.append(action.target.kind);
  key.push_back(':');
  key.append(action.target.id);
  key.push_back('|');
  key.append(action.payload);
  return key;
}

// NOTE: requires the caller to already hold mutex_. Kept non-locking because
// submit() and pump() call it while holding the queue lock; do not add a
// lock here (would self-deadlock on std::mutex).
void Dispatcher::suppress(Action action, std::string reason) {
  Result result;
  result.id = action.id;
  result.cancelled = true;
  result.detail = std::move(reason);
  result.error = {rime::core::Error::Code::Cancelled, result.detail};
  suppressed_.push_back(std::move(result));
}

DispatchStatus Dispatcher::submit(Action action) {
  // Queue decisions are staged under mutex_ and traced only after the lock
  // is released: a user sink re-entering the dispatcher would deadlock on
  // the non-recursive queue lock, and sink latency must not stall queueing.
  struct Decision {
    rime::core::TraceKind kind;
    std::string subject;
    rime::core::ActionId id;
    std::string capability;
    std::string detail;
    std::string result_code;
  };
  std::vector<Decision> decisions;
  const auto accept = [&] {
    // Queue-wait start point: the accepted wall time rides along with the
    // action itself so the Kernel can compute TraceEntry::queue_wait_ms on
    // ActionStarted. Stamped only when unset: production actions arrive with
    // 0 (codec never decodes this field), in-process tests may preset it to
    // inject a deterministic accept time. Not serialized anywhere.
    if (action.accepted_unix_ms == 0) {
      action.accepted_unix_ms = static_cast<std::uint64_t>(
          rime::core::SystemClock::instance().unix_ms());
    }
    decisions.push_back({rime::core::TraceKind::ActionAccepted, action.type, action.id,
                         action.capability, "queued", {}});
  };
  const auto refuse_action = [&](const Action& refused, std::string detail,
                                 std::string result_code) {
    decisions.push_back({rime::core::TraceKind::ActionRefused, refused.type, refused.id,
                         refused.capability, std::move(detail), std::move(result_code)});
  };
  DispatchStatus status = DispatchStatus::Accepted;
  bool enqueued = false;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      status = DispatchStatus::Closed;
      refuse_action(action, "rejected: dispatcher closed", "invalid_state");
    } else if (policy_.capacity == 0) {
      status = DispatchStatus::Full;
      refuse_action(action, "rejected: queue full", "queue_full");
    } else {
      switch (policy_.overflow) {
        case rime::core::OverflowPolicy::Reject:
          if (pending_.size() >= policy_.capacity) {
            status = DispatchStatus::Full;
            refuse_action(action, "rejected: queue full", "queue_full");
            break;
          }
          accept();
          enqueued = true;
          break;
        case rime::core::OverflowPolicy::DropOldest:
          if (pending_.size() >= policy_.capacity) {
            Action dropped_action = std::move(pending_.front());
            pending_.pop_front();
            ++dropped_;
            refuse_action(dropped_action, "dropped by queue policy", "cancelled");
            suppress(std::move(dropped_action), "dropped by queue policy");
          }
          accept();
          enqueued = true;
          break;
        case rime::core::OverflowPolicy::CoalesceByKey: {
          const std::string key = coalesce_key(action);
          bool coalesced = false;
          for (Action& pending : pending_) {
            if (coalesce_key(pending) == key) {
              Action superseded = std::move(pending);
              refuse_action(superseded, "superseded by newer action", "cancelled");
              suppress(std::move(superseded), "superseded by newer action");
              accept();
              pending = std::move(action);
              status = DispatchStatus::Coalesced;
              coalesced = true;
              break;
            }
          }
          if (coalesced) break;
          if (pending_.size() >= policy_.capacity) {
            status = DispatchStatus::Full;
            refuse_action(action, "rejected: queue full", "queue_full");
            break;
          }
          accept();
          enqueued = true;
          break;
        }
      }
      if (enqueued) pending_.push_back(std::move(action));
    }
  }
  for (Decision& decision : decisions) {
    record(decision.kind, std::move(decision.subject), decision.id,
           std::move(decision.capability), std::move(decision.detail),
           std::move(decision.result_code));
  }
  return status;
}

std::vector<Result> Dispatcher::pump(const std::size_t budget,
                                     rime::core::CancellationToken cancellation) {
  return pump(budget, [&cancellation](const Action&) { return cancellation; });
}

std::vector<Result> Dispatcher::pump(
    const std::size_t budget,
    const std::function<rime::core::CancellationToken(const Action&)>& cancellation_of) {
  std::vector<Action> batch;
  std::vector<Result> results;
  {
    std::lock_guard lock(mutex_);
    const auto count = std::min(budget, pending_.size());
    batch.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      batch.push_back(std::move(pending_.front()));
      pending_.pop_front();
    }
  }

  results.reserve(batch.size());
  for (const auto& action : batch) {
    // Check cancellation before every step so a cancelled batch still yields
    // one Result per action instead of swallowing the remainder.
    const rime::core::CancellationToken cancellation = cancellation_of(action);
    if (cancellation.cancelled()) {
      Result cancelled;
      cancelled.id = action.id;
      cancelled.cancelled = true;
      cancelled.detail = "cancelled before execute";
      cancelled.error = {rime::core::Error::Code::Cancelled, cancelled.detail};
      record(rime::core::TraceKind::ActionRefused, action.type, action.id, action.capability,
             cancelled.detail, "cancelled");
      results.push_back(std::move(cancelled));
      continue;
    }
    results.push_back(kernel_.execute(action, cancellation));
  }

  std::lock_guard lock(mutex_);
  for (Result& result : suppressed_) results.push_back(std::move(result));
  suppressed_.clear();
  return results;
}

// NOTE: not noexcept on purpose: std::lock_guard<std::mutex> can throw on
// lock acquisition, and a throwing close() inside noexcept would terminate.
void Dispatcher::close() {
  bool transitioned = false;
  {
    std::lock_guard lock(mutex_);
    if (!closed_) {
      closed_ = true;
      transitioned = true;
    }
  }
  // Trace only the real transition so repeated close() calls stay idempotent
  // instead of stacking duplicate StateChanged entries.
  if (transitioned) record(rime::core::TraceKind::StateChanged, "dispatcher", 0, {}, "closed", {});
}

bool Dispatcher::closed() const {
  std::lock_guard lock(mutex_);
  return closed_;
}

std::size_t Dispatcher::size() const {
  std::lock_guard lock(mutex_);
  return pending_.size();
}

std::size_t Dispatcher::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

std::size_t Dispatcher::capacity() const { return policy_.capacity; }

rime::core::SchedulerPolicy default_dispatch_policy() {
  // Capacity bounds the queue (backpressure via queue_full rejection);
  // coalescing dedupes same-key mutations. Matches the native scheduler
  // policy vocabulary so traces read the same in both layers.
  return rime::core::SchedulerPolicy::coalescing(64);
}

void Dispatcher::record(const rime::core::TraceKind kind, std::string subject,
                        const rime::core::ActionId id, std::string capability,
                        std::string detail, std::string result_code) {
  // Mirrors Kernel::record: the shared sink may throw, and tracing must
  // never propagate out of a dispatch decision.
  try {
    const auto& sink = kernel_.trace_sink();
    if (sink) {
      sink->record({0, kind, std::move(subject), std::move(detail), id, std::move(capability),
                    std::move(result_code), 0});
    }
  } catch (...) {
  }
}

}  // namespace rime::action
