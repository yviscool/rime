#pragma once

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/registry.hpp"

#include <atomic>
#include <cstdint>

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring a native module needs: the registry operations, the Action kernel
// every mutation is checked against, the dispatcher queue every mutation is
// submitted to, and the action id source. The owner keeps the binding alive
// for the whole host/runtime lifetime.
struct RegistryModuleBinding {
  RegistryService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  rime::action::Dispatcher* dispatcher{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
};

// Registers `rime:registry` (exports `registry`) on a host. `registry.read`
// and `registry.view`/`registry.setView` are direct service calls (capability
// checked for read, never traced); `registry.write` builds a `registry.write`
// Action and submits it to the dispatcher queue.
rime::core::Error register_registry_module(rime::js::Host& host, RegistryModuleBinding* binding);
// Same for a Runtime (applies to every host it creates; pre-start only).
rime::core::Error register_registry_module(rime::js::Runtime& runtime,
                                            RegistryModuleBinding* binding);

}  // namespace rime::win32
