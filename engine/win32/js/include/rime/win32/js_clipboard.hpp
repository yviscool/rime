#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/clipboard.hpp"

#include <atomic>
#include <cstdint>

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring a native module needs: clipboard operations, the Action kernel
// every mutation routes through, and the action id source. The owner keeps
// the binding alive for the whole host/runtime lifetime.
struct ClipboardModuleBinding {
  ClipboardService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
};

// Registers `rime:clipboard` (exports `clipboard`) on a host. `clipboard.read`
// is an async read; `clipboard.write` builds a `clipboard.write` Action and
// executes it through the kernel.
rime::core::Error register_clipboard_module(rime::js::Host& host, ClipboardModuleBinding* binding);
// Same for a Runtime (applies to every host it creates; pre-start only).
rime::core::Error register_clipboard_module(rime::js::Runtime& runtime,
                                            ClipboardModuleBinding* binding);

}  // namespace rime::win32
