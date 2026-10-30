#pragma once

#include "rime/action/action.hpp"
#include "rime/automation/uia_service.hpp"
#include "rime/win32/window.hpp"

namespace rime::win32 {

// Control verbs (@rime/control Phase 1 & 2): 3-tier strategy
// (Direct Win32 message -> Semantic UIA pattern -> Visual coordinate fallback)
// behind WindowService::control_* and UiaService.
class ControlExecutor final : public rime::action::Executor {
 public:
  explicit ControlExecutor(WindowService& service,
                           rime::automation::UiaService* uia_service = nullptr)
      : service_(service), uia_service_(uia_service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  WindowService& service_;
  rime::automation::UiaService* uia_service_{nullptr};
};

}  // namespace rime::win32
