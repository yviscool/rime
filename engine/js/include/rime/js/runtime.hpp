#pragma once

#include "rime/core/types.hpp"
#include "rime/js/module_registry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace rime::js {

enum class RuntimeState : std::uint8_t { Created, Starting, Running, Stopping, Stopped, Failed };

class Runtime final {
 public:
  Runtime() = default;
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Enables file-based ES modules confined to `root`. Only valid before start.
  rime::core::Error set_file_root(std::string root);
  // Registers an additional native module (e.g. `rime:window`, `rime:input`)
  // for every Host this runtime creates. `data` is handed to the host as the
  // module's wiring pointer (Host::set_module_data). Only valid before start.
  rime::core::Error add_native_module(std::string name,
                                      ModuleRegistry::NativeFactory factory,
                                      void* data = nullptr);
  rime::core::Error start();
  std::future<rime::core::Error> evaluate_module(std::string source,
                                                 std::string filename = "<module>");
  // Inspects the JS thread's live state (modules, functions, subscriptions,
  // tasks, errors) without executing unknown script.
  std::future<std::string> inspect(std::string request = "{}");
  // Blocks until the JS thread reports quiescence: no queued completion and
  // no armed timer. Returns ExecutionFailed on timeout.
  rime::core::Error settle(std::chrono::milliseconds timeout = std::chrono::seconds(5));
  rime::core::Error stop();
  [[nodiscard]] RuntimeState state() const;

 private:
  struct EvalTask {
    std::string source;
    std::string filename;
    std::promise<rime::core::Error> completion;
  };
  struct InspectTask {
    std::string request;
    std::promise<std::string> completion;
  };

  void run();

  mutable std::mutex mutex_;
  std::mutex lifecycle_mutex_;
  std::condition_variable condition_;
  std::vector<EvalTask> tasks_;
  std::vector<InspectTask> inspect_tasks_;
  std::thread thread_;
  RuntimeState state_{RuntimeState::Created};
  rime::core::Error startup_error_;
  bool idle_flag_{true};
  bool wake_pending_{false};
  std::atomic_bool stop_interrupt_{false};
  std::string file_root_;
  std::vector<std::tuple<std::string, ModuleRegistry::NativeFactory, void*>> native_modules_;
};

}  // namespace rime::js
