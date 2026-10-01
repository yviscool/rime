#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/window.hpp"

namespace rime::win32 {

// Executes the `window.*` write actions (capability `windows.window.write`)
// against a WindowService: move/focus/close/hide/show/minimize/maximize/
// restore. Payload contract: `window.move` requires
// {"position": "left|right|top|bottom|full"}; the state actions take an empty
// object. Target contract: {"kind": "window", "id": "<numeric id>" | "active"}.
// The action deadline bounds the queued UI phase: the remaining time is
// recomputed before every UI round-trip (resolve/info/op/info) and passed
// as that call's per-call timeout; an expired deadline fails with Timeout.
class WindowExecutor final : public rime::action::Executor {
 public:
  explicit WindowExecutor(WindowService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  WindowService& service_;
};

}  // namespace rime::win32
