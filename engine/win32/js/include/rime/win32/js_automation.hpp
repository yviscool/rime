#pragma once

#include "rime/action/kernel.hpp"
#include "rime/automation/uia_service.hpp"
#include "rime/core/types.hpp"

#include <atomic>
#include <cstdint>

namespace rime::action {
class Dispatcher;
}
namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring for `rime:automation`. The owner keeps the binding alive for the
// whole host/runtime lifetime; every member is set once at register time
// and only read afterwards. `kernel` gates find/read/invoke behind the
// windows.automation.* capabilities; `dispatcher` and `next_action_id` turn
// them into queued, traceable actions executed by rime::automation::
// UiaExecutor against the UiaService MTA thread.
struct AutomationModuleBinding {
  rime::automation::UiaService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  rime::action::Dispatcher* dispatcher{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
};

// Registers `rime:automation` (exports `automation`).
// `automation.find(query)` resolves with matching element snapshots;
// `automation.read(id)` re-reads one registered element; `automation.
// invoke(id)` presses it (UIA invoke pattern); `automation.release(id)`
// drops our reference. find/read/invoke are Actions (capability windows.
// automation.find/read/invoke); release is a local reference drop.
rime::core::Error register_automation_module(rime::js::Host& host,
                                             AutomationModuleBinding* binding);
rime::core::Error register_automation_module(rime::js::Runtime& runtime,
                                             AutomationModuleBinding* binding);

}  // namespace rime::win32
