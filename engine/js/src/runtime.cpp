#include "rime/js/runtime.hpp"

#include "rime/core/lane.hpp"
#include "rime/js/host.hpp"

#include <tuple>
#include <utility>

namespace rime::js {
namespace {

std::future<rime::core::Error> failed_future(rime::core::Error error) {
  std::promise<rime::core::Error> promise;
  auto future = promise.get_future();
  promise.set_value(std::move(error));
  return future;
}

std::future<std::string> failed_inspect_future(const std::string& message) {
  std::promise<std::string> promise;
  auto future = promise.get_future();
  promise.set_value(message);
  return future;
}

}  // namespace

Runtime::~Runtime() { (void)stop(); }

rime::core::Error Runtime::set_file_root(std::string root) {
  std::lock_guard lock(mutex_);
  if (state_ != RuntimeState::Created) {
    return {rime::core::Error::Code::InvalidState,
            "file root must be configured before the runtime starts"};
  }
  file_root_ = std::move(root);
  return rime::core::Error::none();
}

rime::core::Error Runtime::add_native_module(std::string name,
                                             ModuleRegistry::NativeFactory factory,
                                             void* data) {
  if (name.empty() || !factory) {
    return {rime::core::Error::Code::InvalidContract,
            "native module registration requires a name and factory"};
  }
  std::lock_guard lock(mutex_);
  if (state_ != RuntimeState::Created) {
    return {rime::core::Error::Code::InvalidState,
            "native modules must be registered before the runtime starts"};
  }
  native_modules_.emplace_back(std::move(name), std::move(factory), data);
  return rime::core::Error::none();
}

rime::core::Error Runtime::start() {
  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (state_ != RuntimeState::Created) {
      return {rime::core::Error::Code::InvalidState,
              "JavaScript runtime can only start once"};
    }
    state_ = RuntimeState::Starting;
  }

  try {
    thread_ = std::thread(&Runtime::run, this);
  } catch (const std::exception& ex) {
    std::lock_guard lock(mutex_);
    state_ = RuntimeState::Failed;
    startup_error_ = {rime::core::Error::Code::ExecutionFailed,
                      std::string("failed to launch JS thread: ") + ex.what()};
    condition_.notify_all();
    return startup_error_;
  } catch (...) {
    std::lock_guard lock(mutex_);
    state_ = RuntimeState::Failed;
    startup_error_ = {rime::core::Error::Code::ExecutionFailed,
                      "failed to launch JS thread"};
    condition_.notify_all();
    return startup_error_;
  }
  std::unique_lock lock(mutex_);
  condition_.wait(lock, [this] { return state_ != RuntimeState::Starting; });
  return startup_error_;
}

std::future<rime::core::Error> Runtime::evaluate_module(std::string source,
                                                        std::string filename) {
  std::lock_guard lock(mutex_);
  if (state_ != RuntimeState::Running) {
    return failed_future({rime::core::Error::Code::InvalidState,
                          "JavaScript runtime is not accepting work"});
  }

  EvalTask task{std::move(source), std::move(filename), {}};
  auto future = task.completion.get_future();
  tasks_.push_back(std::move(task));
  condition_.notify_one();
  return future;
}

std::future<std::string> Runtime::inspect(std::string request) {
  std::lock_guard lock(mutex_);
  if (state_ != RuntimeState::Running) {
    return failed_inspect_future("{\"error\":\"runtime is not running\"}");
  }

  InspectTask task{std::move(request), {}};
  auto future = task.completion.get_future();
  inspect_tasks_.push_back(std::move(task));
  condition_.notify_one();
  return future;
}

rime::core::Error Runtime::settle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  if (state_ != RuntimeState::Running) {
    return {rime::core::Error::Code::InvalidState, "runtime is not running"};
  }
  if (!condition_.wait_for(lock, timeout, [this] {
        // Mirrors the hpp quiescence promise: host idle AND no queued work.
        // runtime.exit abandons queued work, so settle must not sit out the
        // timeout once an exit is requested.
        return exit_requested_ || (idle_flag_ && tasks_.empty() && inspect_tasks_.empty());
      })) {
    return {rime::core::Error::Code::ExecutionFailed,
            "runtime did not become idle before the timeout"};
  }
  return rime::core::Error::none();
}

rime::core::Error Runtime::stop() {
  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (state_ == RuntimeState::Stopped) return rime::core::Error::none();
    if (state_ == RuntimeState::Created) {
      state_ = RuntimeState::Stopped;
      return rime::core::Error::none();
    }
    if (state_ == RuntimeState::Failed) {
      // The thread already exited; fall through to join.
    } else {
      state_ = RuntimeState::Stopping;
    }
  }
  stop_interrupt_.store(true, std::memory_order_release);
  condition_.notify_all();
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard lock(mutex_);
    // Preserve Failed: a failed start must stay observable, not be masked as
    // a clean Stopped.
    if (state_ != RuntimeState::Failed) state_ = RuntimeState::Stopped;
  }
  condition_.notify_all();
  return rime::core::Error::none();
}

RuntimeState Runtime::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

void Runtime::set_exit_notifier(std::function<void(int)> notifier) {
  std::lock_guard lock(mutex_);
  exit_notifier_ = std::move(notifier);
}

bool Runtime::exit_requested() const {
  std::lock_guard lock(mutex_);
  return exit_requested_;
}

int Runtime::exit_code() const {
  std::lock_guard lock(mutex_);
  return exit_code_;
}

void Runtime::run() {
  if (const auto lane_error = rime::core::LaneRegistry::instance().claim(rime::core::Lane::Js);
      !lane_error.ok()) {
    std::lock_guard lock(mutex_);
    state_ = RuntimeState::Failed;
    startup_error_ = lane_error;
    condition_.notify_all();
    return;
  }

  {
    Host host;
    // NOTE: `data` keeps its void* type to avoid a signature cascade.
    // Ownership stays with the add_native_module caller for the host's
    // lifetime; never hand the raw pointer to another thread - pass stable
    // ids or serialized snapshots instead.
    for (auto& [name, factory, data] : native_modules_) {
      if (data) host.set_module_data(name, data);
      host.modules().add_native(name, factory);
    }

    bool ready = true;
    if (!file_root_.empty()) {
      if (const auto root_error = host.modules().set_file_root(file_root_); !root_error.ok()) {
        ready = false;
        std::lock_guard lock(mutex_);
        state_ = RuntimeState::Failed;
        startup_error_ = root_error;
        condition_.notify_all();
      }
    }

    if (ready) {
      host.set_interrupt_source(&stop_interrupt_);
      host.set_wakeup([this] {
        {
          std::lock_guard lock(mutex_);
          wake_pending_ = true;
        }
        condition_.notify_all();
      });
      // Exit notifier: records the first requested code for the exit
      // accessors, wakes any settle() waiter, then reports to the embedder
      // observer. Installed before any script runs, so it is stable for the
      // host's whole life.
      host.set_exit_notifier([this](int code) {
        std::function<void(int)> observer;
        {
          std::lock_guard lock(mutex_);
          exit_requested_ = true;
          exit_code_ = code;
          observer = exit_notifier_;
        }
        condition_.notify_all();
        if (observer) observer(code);
      });

      {
        std::lock_guard lock(mutex_);
        if (state_ == RuntimeState::Starting) state_ = RuntimeState::Running;
        condition_.notify_all();
      }

      for (;;) {
        EvalTask eval_task;
        InspectTask inspect_task;
        bool has_eval = false;
        bool has_inspect = false;
        {
          std::unique_lock lock(mutex_);
          condition_.wait(lock, [this] {
            return state_ == RuntimeState::Stopping || wake_pending_ || !tasks_.empty() ||
                   !inspect_tasks_.empty();
          });
          wake_pending_ = false;
          if (state_ == RuntimeState::Stopping && tasks_.empty() && inspect_tasks_.empty()) {
            break;
          }
          if (!tasks_.empty()) {
            eval_task = std::move(tasks_.front());
            // TODO(perf): vector-as-queue erase(front) is O(N); kept as vector
            // to avoid header churn (deque would touch the public header).
            // Switch to std::deque if queues grow.
            tasks_.erase(tasks_.begin());
            has_eval = true;
          } else if (!inspect_tasks_.empty()) {
            inspect_task = std::move(inspect_tasks_.front());
            // TODO(perf): see above - vector-as-queue erase(front) is O(N).
            inspect_tasks_.erase(inspect_tasks_.begin());
            has_inspect = true;
          }
        }

        rime::core::Error result = rime::core::Error::none();
        if (has_eval) {
          result = host.eval_module(eval_task.source, eval_task.filename);
        } else if (has_inspect) {
          inspect_task.completion.set_value(host.inspect(inspect_task.request));
        }

        host.drain();
        {
          std::lock_guard lock(mutex_);
          idle_flag_ = host.idle();
        }
        condition_.notify_all();

        if (has_eval) eval_task.completion.set_value(std::move(result));
      }
    }

    // Exit contract: JS exit handlers run on the JS thread after the loop
    // stops and before ~Host tears the context down (idempotent with the
    // destructor fallback). Exit/ExitApp drain observes reason "exit" plus
    // the requested code; a normal stop keeps "stop". Read from the host on
    // this thread instead of the runtime members - it is the same owner.
    const auto payload = host.exit_requested()
                             ? (std::string("{\"reason\":\"exit\",\"code\":") +
                                std::to_string(host.exit_code()) + "}")
                             : std::string("{\"reason\":\"stop\"}");
    host.run_exit_handlers(payload);
  }

  rime::core::LaneRegistry::instance().release(rime::core::Lane::Js);
  {
    std::lock_guard lock(mutex_);
    if (state_ != RuntimeState::Failed) state_ = RuntimeState::Stopped;
  }
  condition_.notify_all();
}

}  // namespace rime::js
