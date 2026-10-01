#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/clipboard.hpp"

namespace rime::win32 {

// Executes `clipboard.write` (capability `clipboard.write`).
// Payload contract: {"text": string}.
// Target contract: {"kind": "clipboard", "id": "default"} (the system
// clipboard is global).
class ClipboardExecutor final : public rime::action::Executor {
 public:
  explicit ClipboardExecutor(ClipboardService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  ClipboardService& service_;
};

}  // namespace rime::win32
