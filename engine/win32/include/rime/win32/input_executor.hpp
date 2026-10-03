#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/input.hpp"

namespace rime::win32 {

// Executes the `input.send` and `input.mouse` actions (capability
// `windows.input.inject`) by injecting the payload's ordered steps through
// InputService::send / InputService::send_mouse.
// Payload contracts: `input.send` is a JSON array of
// {"vk": <1..254>, "down": <bool>, "unicode"?: <bool>} steps (unicode steps
// carry a UTF-16 code unit 0..65535 instead of a VK); `input.mouse` is an
// object {"steps": [...], "speed"?: <0..100>} where steps are move/relmove
// {x, y} and down/up {button 1..3} objects -- speed is validated for contract
// fidelity but never forwarded to the injector (AHK SendInput ignores it
// too). Target contract for both: {"kind": "input", ...}. Each batch is one
// SendInput call tagged as self input: subscribers observe the events with
// `selfInjected` set while chord bindings ignore them, so an action that
// sends input can never feed its own chord.
class InputExecutor final : public rime::action::Executor {
 public:
  explicit InputExecutor(InputService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  InputService& service_;
};

}  // namespace rime::win32
