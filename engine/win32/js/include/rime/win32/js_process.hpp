#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/process.hpp"

#include <atomic>
#include <cstdint>

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring a native module needs: process operations, the Action kernel every
// mutation routes through, and the action id source. The owner keeps the
// binding alive for the whole host/runtime lifetime.
struct ProcessModuleBinding {
  ProcessService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
};

// Registers `rime:process` (exports `process`) on a host. Queries are async
// reads; `process.launch` and `process.terminate` build Actions and execute
// them through the kernel.
rime::core::Error register_process_module(rime::js::Host& host, ProcessModuleBinding* binding);
// Same for a Runtime (applies to every host it creates; pre-start only).
rime::core::Error register_process_module(rime::js::Runtime& runtime, ProcessModuleBinding* binding);

}  // namespace rime::win32
