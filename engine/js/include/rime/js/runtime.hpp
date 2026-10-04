#pragma once

#include "rime/core/types.hpp"
#include "rime/js/module_registry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
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
  // NOTE: `data` intentionally stays void* (changing it cascades). The caller
  // retains ownership for the host's lifetime and must not pass the raw
  // pointer across threads - use stable ids/snapshots instead.
  rime::core::Error add_native_module(std::string name,
                                      ModuleRegistry::NativeFactory factory,
                                      void* data = nullptr);
  [[nodiscard]] rime::core::Error start();
  [[nodiscard]] std::future<rime::core::Error> evaluate_module(std::string source,
                                                               std::string filename = "<module>");
  // Inspects the JS thread's live state (modules, functions, subscriptions,
  // tasks, errors) without executing unknown script.
  std::future<std::string> inspect(std::string request = "{}");
  // Blocks until the JS thread reports quiescence: no queued completion and
  // no armed timer. Returns ExecutionFailed on timeout.
  [[nodiscard]] rime::core::Error settle(std::chrono::milliseconds timeout = std::chrono::seconds(5));
  [[nodiscard]] rime::core::Error stop();
  [[nodiscard]] RuntimeState state() const;

  // runtime.exit(code?) observability (AHK Exit / ExitApp).
  // - Host-attached: the JS thread's Host writes the state through the exit
  //   notifier installed by run(); both readers lock the runtime mutex, so
  //   the owning thread may query them while the JS thread is live.
  // - The notifier must be installed before start() (same convention as the
  //   native module list); it fires once, on the JS thread, with the first
  //   requested code.
  // - An exit abandons queued work: settle() returns as soon as an exit is
  //   requested instead of waiting out its timeout, and run() drains the
  //   exit handlers with {"reason":"exit","code":N} instead of the normal
  //   {"reason":"stop"} payload.
  void set_exit_notifier(std::function<void(int)> notifier);
  [[nodiscard]] bool exit_requested() const;
  [[nodiscard]] int exit_code() const;

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
  // Guarded by mutex_; written from the JS thread through the exit notifier
  // run() installs on the Host, read by the exit accessors and settle().
  bool exit_requested_{false};
  int exit_code_{0};
  // Embedder observer installed through set_exit_notifier (before start);
  // copied out of the lock by run()'s notifier before it fires.
  std::function<void(int)> exit_notifier_;
  std::string file_root_;
  // NOTE: void* wiring pointers are caller-owned (see add_native_module);
  // never dereference them here or share them across threads.
  std::vector<std::tuple<std::string, ModuleRegistry::NativeFactory, void*>> native_modules_;
};

}  // namespace rime::js
