#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/window.hpp"

namespace rime::win32 {

// Control verbs (@rime/control Phase 1): Win32-direct implementations over
// WindowService::control_* (window_control.cpp). Target kind is "control",
// target id the decimal stable id from controls()/window_at().
class ControlExecutor final : public rime::action::Executor {
 public:
  explicit ControlExecutor(WindowService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  WindowService& service_;
};

}  // namespace rime::win32
