#pragma once

#include "rime/core/types.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace rime::core {

// Dedicated worker-thread lane: blocking native reads and action queue pumps
// run here so the timer thread never executes executors and the JS/UI lanes
// stay free for their own work. One process-wide worker; hosts lazily start
// it on first use and stop it during teardown. Task failures are reported by
// the caller's wrapper (completion rejection or the host error log); the
// worker itself only guarantees that a posted task runs on the worker lane or
// is dropped when the worker stops.
//
// Ownership: worker tasks may capture their Host raw (see schedule_worker)
// and rely on ~Host joining them before teardown. Concurrent live Hosts
// sharing this singleton are unsupported: one Host's teardown drops the
// other's queued tasks. Keep at most one active Host per process (tests run
// serially for the same reason).
class WorkerService final {
 public:
  static WorkerService& instance();

  WorkerService(const WorkerService&) = delete;
  WorkerService& operator=(const WorkerService&) = delete;

  // Starts the worker thread (which claims Lane::Worker). Idempotent while
  // running; fails only while a stop is in progress.
  [[nodiscard]] Error start();
  // Queues a task. Returns false when the worker is stopping or stopped, so
  // the caller can reject its promise instead of leaking it.
  bool post(std::function<void()> task);
  // Rejects new posts, drops queued tasks, joins the running task and lets
  // the thread release the lane. Safe to call repeatedly.
  void stop();
  [[nodiscard]] bool running() const;
  [[nodiscard]] std::size_t pending() const;

 private:
  WorkerService() = default;
  void run();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  std::thread thread_;
  bool stopping_{false};
};

}  // namespace rime::core
