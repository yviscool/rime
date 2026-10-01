#pragma once

#include "rime/core/types.hpp"
#include "rime/js/host.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace rime::js {

// Version of the embeddable host contract. Bump on any breaking change to
// load/execute/inspect/unload/exit semantics; contract tests pin this value.
inline constexpr int k_host_abi_version = 1;

enum class HostAbiState : std::uint8_t { Created, Loaded, Executed, Failed, Unloaded };

const char* host_abi_state_name(HostAbiState state);

// Synchronous embedding façade over one Host. The creating thread is the JS
// thread; every method must be called from it.
class HostAbi final {
 public:
  using ErrorHandler = std::function<void(std::string_view where, std::string_view message)>;
  using ExitHandler = std::function<void(int code)>;

  HostAbi();
  ~HostAbi();
  HostAbi(const HostAbi&) = delete;
  HostAbi& operator=(const HostAbi&) = delete;

  [[nodiscard]] int abi_version() const { return k_host_abi_version; }
  [[nodiscard]] HostAbiState state() const { return state_; }

  // Stages a script. ES module vs. script is decided at execute time via
  // JS_DetectModule.
  rime::core::Error load(std::string_view source, std::string_view filename);
  // Evaluates the staged script and drains its jobs. Repeatable while
  // Loaded/Executed. Passing `source` replaces the staged script first.
  // Failures move the ABI to Failed and fire on_error.
  rime::core::Error execute(std::string_view source = {}, std::string_view filename = {});
  // Reads live host state without executing script.
  std::string inspect(std::string_view request_json = "{}");
  std::size_t drain();
  // Unloads the host. Fails with a reason list while subscriptions, JS
  // callbacks or unresolved promises are outstanding. Idempotent.
  rime::core::Error unload();
  // Records an embedder exit code and fires on_exit.
  void exit(int code);
  [[nodiscard]] int exit_code() const { return exit_code_; }

  void set_error_handler(ErrorHandler handler);
  void set_exit_handler(ExitHandler handler);

 private:
  HostAbiState state_{HostAbiState::Created};
  std::string source_;
  std::string filename_{"<script>"};
  int exit_code_{0};
  ErrorHandler on_error_;
  ExitHandler on_exit_;
  Host host_;
};

}  // namespace rime::js
