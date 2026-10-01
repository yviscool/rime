#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/input.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace rime::action {
class Dispatcher;
}
namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// One `input.bind` registration. The binding owns it for its own lifetime;
// the JS closure delivered to by the host event queue only borrows it, so
// teardown order (remove the callback, then erase the registration) can
// never race a finalizer. Filled once at bind time and read-only on the
// trigger path.
struct ChordBinding {
  std::uint32_t vk{0};
  std::uint8_t mask{0};  // exact modifier mask: bit 0 alt, 1 control, 2 shift, 3 super
  std::string type;
  std::string capability;
  std::string target_kind;
  std::string target_id;
  std::string payload;  // JSON object text for the built Action
};

// Wiring for `rime:input`. The owner keeps the binding alive for the whole
// host/runtime lifetime. The `callbacks`, `chords` and `next_chord_id` members
// are touched on the JS thread only. `kernel` gates hook subscriptions behind
// `windows.hook.global`; `dispatcher` and `next_action_id` turn chord bindings
// into queued, traceable actions.
struct InputModuleBinding {
  InputService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  rime::action::Dispatcher* dispatcher{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
  std::unordered_map<std::uint64_t, std::uint64_t> callbacks;  // service id -> host callback id

  // Chord bindings: binding id -> delivery chain plus the owned context.
  struct ChordRegistration {
    std::uint64_t subscription_id{0};
    std::uint64_t host_callback_id{0};
    std::unique_ptr<ChordBinding> binding;
  };
  std::unordered_map<std::uint64_t, ChordRegistration> chords;
  std::uint64_t next_chord_id{1};  // monotonic, JS thread only
};

// Registers `rime:input` (exports `input`). `input.subscribe(handler)`
// installs a hook subscription whose events are delivered to `handler` on
// the JS thread through the host event queue; `input.unsubscribe(id)` closes
// the subscription and releases the handler. `input.bind(chord, action)`
// maps an exact-match chord (e.g. "ctrl+shift+k") onto an action template:
// every matching key-down builds a fresh Action and submits it to the
// dispatcher queue; `input.unbind(id)` closes the binding.
rime::core::Error register_input_module(rime::js::Host& host, InputModuleBinding* binding);
rime::core::Error register_input_module(rime::js::Runtime& runtime, InputModuleBinding* binding);

}  // namespace rime::win32
