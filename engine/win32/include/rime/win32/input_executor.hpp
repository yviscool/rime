#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/input.hpp"

namespace rime::win32 {

// Executes the `input.send` action (capability `windows.input.inject`) by
// injecting the payload's ordered key steps through InputService::send.
// Payload contract: a JSON array of {"vk": <1..254>, "down": <bool>} steps;
// target contract: {"kind": "input", "id": "keyboard"}. The batch is one
// SendInput call tagged as self input: subscribers observe the events with
// `selfInjected` set while chord bindings ignore them, so an action that
// sends keys can never feed its own chord.
class InputExecutor final : public rime::action::Executor {
 public:
  explicit InputExecutor(InputService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  InputService& service_;
};

}  // namespace rime::win32
