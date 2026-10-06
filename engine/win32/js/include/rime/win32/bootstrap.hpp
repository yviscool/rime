#pragma once

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/automation/uia_service.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/clipboard.hpp"
#include "rime/win32/input.hpp"
#include "rime/win32/js_automation.hpp"
#include "rime/win32/js_clipboard.hpp"
#include "rime/win32/js_input.hpp"
#include "rime/win32/js_process.hpp"
#include "rime/win32/js_registry.hpp"
#include "rime/win32/js_screen.hpp"
#include "rime/win32/js_storage.hpp"
#include "rime/win32/js_window.hpp"
#include "rime/win32/process.hpp"
#include "rime/win32/registry.hpp"
#include "rime/win32/screen.hpp"
#include "rime/win32/storage.hpp"
#include "rime/win32/window.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace rime::js {
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// The one production wiring shape: the eight Win32 services (input, process,
// clipboard, window, UIA, storage, registry, screen), the capability policy the
// Kernel enforces, the shared Dispatcher queue, every implemented action
// executor from contracts/registry/actions.json and the eight JS modules.
// The bundle harness and the desktop host both run through this class, so
// what the tests exercise is exactly what production runs.
class Bootstrap final {
 public:
  explicit Bootstrap(std::unordered_set<std::string> capabilities);
  ~Bootstrap();

  Bootstrap(const Bootstrap&) = delete;
  Bootstrap& operator=(const Bootstrap&) = delete;

  // Registers `rime:input`, `rime:process`, `rime:clipboard`, `rime:window`,
  // `rime:automation`, `rime:storage`, `rime:registry` and `rime:screen` on
  // the runtime. Must be called before the runtime starts; the bindings stay
  // owned by this object for the whole lifetime.
  rime::core::Error register_modules(rime::js::Runtime& runtime);

  // Registers every implemented action executor on the kernel, then starts
  // the services (input hook, window thread, UIA thread, storage UI pump
  // attachment). A partial start is rolled back before the error is
  // returned; stop() and the destructor are idempotent.
  rime::core::Error start();
  rime::core::Error stop();

  [[nodiscard]] rime::action::Kernel& kernel() noexcept { return kernel_; }
  [[nodiscard]] rime::action::Dispatcher& dispatcher() noexcept { return dispatcher_; }

 private:
  rime::core::Error register_executors();

  InputService input_service_;
  ProcessService process_service_;
  ClipboardService clipboard_service_;
  WindowService window_service_;
  rime::automation::UiaService automation_service_;
  StorageService storage_service_;
  RegistryService registry_service_;
  ScreenService screen_service_;

  rime::action::Kernel kernel_;
  rime::action::Dispatcher dispatcher_;
  std::atomic<std::uint64_t> next_action_id_{0};

  InputModuleBinding input_binding_;
  ProcessModuleBinding process_binding_;
  ClipboardModuleBinding clipboard_binding_;
  WindowModuleBinding window_binding_;
  AutomationModuleBinding automation_binding_;
  StorageModuleBinding storage_binding_;
  RegistryModuleBinding registry_binding_;
  ScreenModuleBinding screen_binding_;

  bool started_{false};
};

// The grant the Rim demo bundle documents: reads plus the input hook;
// mutations still fail the policy with a clear reason.
std::unordered_set<std::string> demo_capabilities();
// The production grant: every implemented action capability in
// contracts/registry/actions.json; planned capabilities stay out.
std::unordered_set<std::string> production_capabilities();

// Reads the file at `path` and drives it through the full production
// lifecycle: bootstrap start, module registration, runtime start, evaluate,
// settle async work, surface globalThis.__rim_failure, stop. Returns the
// process exit code (0 ok, 1 failure, 2 usage/IO) and reports diagnostics
// on stderr.
int run_bundle_file(const std::string& path, std::unordered_set<std::string> capabilities);

}  // namespace rime::win32
