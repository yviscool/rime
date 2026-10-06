#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/registry.hpp"

namespace rime::win32 {

// Executes `registry.write` (capability `registry.write`).
// Payload contract: {"op": "set"|"createKey"|"delete"|"deleteKey", "key":
// string, "name"?: string, "type"?: "sz"|"expand_sz"|"dword"|"qword"|
// "multi_sz"|"binary", "value"?: <shape by type>}.
// Target contract: {"kind": "registry", "id": <the payload key>} - the target
// id is the key, so a queued Action can be inspected without re-parsing its
// payload.
// Reads go through RegistryService::read directly (rime:registry.read), which
// is capability-checked in the module but never enters the Action kernel.
class RegistryExecutor final : public rime::action::Executor {
 public:
  explicit RegistryExecutor(RegistryService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  RegistryService& service_;
};

}  // namespace rime::win32
