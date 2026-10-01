#include "rime/core/worker.hpp"

#include "rime/core/lane.hpp"

namespace rime::core {

WorkerService& WorkerService::instance() {
  static WorkerService service;
  return service;
}

Error WorkerService::start() {
  std::lock_guard lock(mutex_);
  if (stopping_) return {Error::Code::InvalidState, "worker service is stopping"};
  if (thread_.joinable()) return Error::none();
  stopping_ = false;
  try {
    thread_ = std::thread(&WorkerService::run, this);
  } catch (const std::exception& exception) {
    return {Error::Code::ExecutionFailed,
            std::string("worker thread start failed: ") + exception.what()};
  }
  return Error::none();
}

bool WorkerService::post(std::function<void()> task) {
  std::lock_guard lock(mutex_);
  if (stopping_ || !thread_.joinable()) return false;
  queue_.push_back(std::move(task));
  cv_.notify_one();
  return true;
}

void WorkerService::stop() {
  std::thread joining;
  {
    std::lock_guard lock(mutex_);
    if (!thread_.joinable()) {
      stopping_ = false;
      queue_.clear();
      return;
    }
    stopping_ = true;
    joining = std::move(thread_);
  }
  cv_.notify_all();
  joining.join();
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
    stopping_ = false;
  }
}

bool WorkerService::running() const {
  std::lock_guard lock(mutex_);
  return thread_.joinable() && !stopping_;
}

std::size_t WorkerService::pending() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

void WorkerService::run() {
  // Best effort: a foreign claim (e.g. a test harness) leaves the lane
  // unowned here and executors then fail require_lane with a diagnostic
  // instead of running on the wrong thread.
  (void)LaneRegistry::instance().claim(Lane::Worker);
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) break;
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      task();
    } catch (const std::exception&) {
      // Callers wrap tasks (completion rejection / host error log); this is
      // the last-resort guard so one bad task cannot kill the worker lane.
    } catch (...) {
    }
  }
  LaneRegistry::instance().release(Lane::Worker);
}

}  // namespace rime::core
