#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/window.hpp"

namespace rime::win32 {

// Executes the `window.*` write actions (capability `windows.window.write`)
// against a WindowService: move/focus/close/kill/redraw/hide/show/minimize/
// maximize/restore/zorder plus the window-group family group.add/
// group.activate/group.deactivate/group.close, the desktop pair
// minimizeall/minimizeall.undo, and the window set family set.title/
// set.enabled/set.alwaysontop. Payload contract:
// `window.move` requires
// {"position": "left|right|top|bottom|full"}; `window.zorder` requires
// {"placement": "top|bottom"}; `window.set.title` requires {"title":
// string}; `window.set.enabled` requires {"value": -1|0|1};
// `window.set.alwaysontop` takes an optional {"value": -1|0|1} (absent
// means topmost); the state actions take an empty
// empty object. Target contract: {"kind": "window", "id": "<numeric id>" |
// "active"} for the window actions, {"kind": "group", "id": "<group
// name>"} for the group actions (payload: a query object for group.add,
// optional {reverse} for activate/deactivate, optional
// {mode: ""|"reverse"|"all"} for close), and {"kind": "desktop", "id":
// "all"} for the desktop actions (fire-and-forget shell tray command;
// the caller observes the effect through a later info()).
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
