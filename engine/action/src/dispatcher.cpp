#include "rime/action/dispatcher.hpp"

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
  std::lock_guard lock(mutex_);
  if (closed_) return DispatchStatus::Closed;
  if (policy_.capacity == 0) return DispatchStatus::Full;
  const std::string key = coalesce_key(action);

  switch (policy_.overflow) {
    case rime::core::OverflowPolicy::Reject:
      if (pending_.size() >= policy_.capacity) return DispatchStatus::Full;
      break;
    case rime::core::OverflowPolicy::DropOldest:
      if (pending_.size() >= policy_.capacity) {
        Action dropped_action = std::move(pending_.front());
        pending_.pop_front();
        ++dropped_;
        suppress(std::move(dropped_action), "dropped by queue policy");
      }
      break;
    case rime::core::OverflowPolicy::CoalesceByKey: {
      for (Action& pending : pending_) {
        if (coalesce_key(pending) == key) {
          Action superseded = std::move(pending);
          pending = std::move(action);
          suppress(std::move(superseded), "superseded by newer action");
          return DispatchStatus::Coalesced;
        }
      }
      if (pending_.size() >= policy_.capacity) return DispatchStatus::Full;
      break;
    }
  }
  pending_.push_back(std::move(action));
  return DispatchStatus::Accepted;
}

std::vector<Result> Dispatcher::pump(const std::size_t budget,
                                     rime::core::CancellationToken cancellation) {
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
    if (cancellation.cancelled()) {
      Result cancelled;
      cancelled.id = action.id;
      cancelled.cancelled = true;
      cancelled.detail = "cancelled before execute";
      cancelled.error = {rime::core::Error::Code::Cancelled, cancelled.detail};
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
  std::lock_guard lock(mutex_);
  closed_ = true;
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

}  // namespace rime::action
