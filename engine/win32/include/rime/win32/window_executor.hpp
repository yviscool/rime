#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/window.hpp"

namespace rime::win32 {

// Executes `window.move` actions (capability `window.write`) against a
// WindowService. Payload contract: {"position": "left|right|top|bottom|full"}.
// Target contract: {"kind": "window", "id": "<numeric id>" | "active"}.
class WindowExecutor final : public rime::action::Executor {
 public:
  explicit WindowExecutor(WindowService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  WindowService& service_;
};

}  // namespace rime::win32
