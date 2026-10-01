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
  void close() noexcept;
  [[nodiscard]] bool closed() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t dropped() const;

 private:
  static std::string coalesce_key(const Action& action);
  void suppress(Action action, std::string reason);

  Kernel& kernel_;
  rime::core::SchedulerPolicy policy_;
  mutable std::mutex mutex_;
  std::deque<Action> pending_;
  std::vector<Result> suppressed_;
  std::size_t dropped_{0};
  bool closed_{false};
};

}  // namespace rime::action
