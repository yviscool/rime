#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/window.hpp"

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

// Wiring for `rime:control`. The owner keeps the binding alive for the whole
// host/runtime lifetime; every member is set once at register time and only
// read afterwards. `kernel` gates the mutating verbs behind
// windows.automation.control; `dispatcher` and `next_action_id` turn them
// into queued, traceable actions executed by ControlExecutor. resolve() and
// the synchronous queries call WindowService directly (no Action).
struct ControlModuleBinding {
  WindowService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  rime::action::Dispatcher* dispatcher{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
};

// Registers `rime:control` (exports `control`).
// `control.resolve(windowId, spec)` returns {id, className, classNN};
// click/focus/setText/getText/sendText are Actions (capability
// windows.automation.control); isVisible/isEnabled/rect/dispose are
// synchronous (single Win32 reads / liveness probe, no Action).
rime::core::Error register_control_module(rime::js::Host& host, ControlModuleBinding* binding);
rime::core::Error register_control_module(rime::js::Runtime& runtime,
                                           ControlModuleBinding* binding);

}  // namespace rime::win32
