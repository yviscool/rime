#pragma once

#include "rime/action/action.hpp"
#include "rime/automation/uia_service.hpp"

namespace rime::automation {

// Executes the three `rime:automation` actions against UiaService:
//   automation.find   (capability windows.automation.find)
//     payload: {"name"?, "controlType"?, "automationId"?, "fromId"?,
//               "maxResults"?}; target: {"automation", "desktop"}
//   automation.read   (capability windows.automation.read)
//     target: {"element", "<decimal element id>"}
//   automation.invoke (capability windows.automation.invoke)
//     target: {"element", "<decimal element id>"}
// The payload/target contracts are re-validated here - the JS entry checks
// shape for good TypeErrors, this executor is the contract enforcer.
class UiaExecutor final : public rime::action::Executor {
 public:
  enum class Op { Find, Read, Invoke };

  UiaExecutor(UiaService& service, Op op) : service_(service), op_(op) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  UiaService& service_;
  Op op_;
};

}  // namespace rime::automation
