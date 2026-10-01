#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/window.hpp"

#include <atomic>
#include <cstdint>

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring a native module needs: window operations, the Action kernel every
// mutation routes through, and the action id source. The owner keeps the
// binding alive for the whole host/runtime lifetime.
struct WindowModuleBinding {
  WindowService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
};

// Registers `rime:window` (exports `windows`) on a host. Queries are async
// reads; `windows.move` builds a `window.move` Action and executes it
// through the kernel, resolving with the moved window info.
rime::core::Error register_window_module(rime::js::Host& host, WindowModuleBinding* binding);
// Same for a Runtime (applies to every host it creates; pre-start only).
rime::core::Error register_window_module(rime::js::Runtime& runtime, WindowModuleBinding* binding);

}  // namespace rime::win32
