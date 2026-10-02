#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/window.hpp"

namespace rime::win32 {

// Executes the `window.*` write actions (capability `windows.window.write`)
// against a WindowService: move/focus/close/hide/show/minimize/maximize/
// restore/zorder plus the window-group family group.add/group.activate/
// group.deactivate/group.close. Payload contract: `window.move` requires
// {"position": "left|right|top|bottom|full"}; `window.zorder` requires
// {"placement": "top|bottom"}; the state actions take an empty
// empty object. Target contract: {"kind": "window", "id": "<numeric id>" |
// "active"} for the window actions and {"kind": "group", "id": "<group
// name>"} for the group actions (payload: a query object for group.add,
// optional {reverse} for activate/deactivate, optional
// {mode: ""|"reverse"|"all"} for close).
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
