#pragma once

#include "rime/core/types.hpp"
#include "rime/win32/input.hpp"

#include <cstdint>
#include <unordered_map>

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring for `rime:input`. The owner keeps the binding alive for the whole
// host/runtime lifetime. The `callbacks` map is touched on the JS thread
// only.
struct InputModuleBinding {
  InputService* service{nullptr};
  std::unordered_map<std::uint64_t, std::uint64_t> callbacks;  // service id -> host callback id
};

// Registers `rime:input` (exports `input`). `input.subscribe(handler)`
// installs a hook subscription whose events are delivered to `handler` on
// the JS thread through the host event queue; `input.unsubscribe(id)` closes
// the subscription and releases the handler.
rime::core::Error register_input_module(rime::js::Host& host, InputModuleBinding* binding);
rime::core::Error register_input_module(rime::js::Runtime& runtime, InputModuleBinding* binding);

}  // namespace rime::win32
