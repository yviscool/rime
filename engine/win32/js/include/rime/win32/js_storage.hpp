#pragma once

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/storage.hpp"

#include <atomic>
#include <cstdint>

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring a native module needs: the storage operations, the Action kernel
// every capability check and every storage.write execution is measured
// against, the dispatcher queue storage.write is submitted to, and the action
// id source. The owner keeps the binding alive for the whole host/runtime
// lifetime.
struct StorageModuleBinding {
  StorageService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  rime::action::Dispatcher* dispatcher{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
};

// Registers `rime:storage` (exports `storage`) on a host. The reads, the
// handle family, encoding and the two selectors are direct service calls
// (capability checked inside the worker body, never traced); `storage.write`
// builds a `storage.write` Action and submits it to the dispatcher queue.
rime::core::Error register_storage_module(rime::js::Host& host, StorageModuleBinding* binding);
// Same for a Runtime (applies to every host it creates; pre-start only).
rime::core::Error register_storage_module(rime::js::Runtime& runtime,
                                          StorageModuleBinding* binding);

}  // namespace rime::win32
