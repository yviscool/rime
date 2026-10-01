#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/process.hpp"

namespace rime::win32 {

// Executes `process.launch` (capability `process.launch`) and
// `process.terminate` (capability `process.terminate`).
// Launch payload: {"command": string, "args"?: string, "workingDir"?: string}.
// Launch target: {"kind": "process", "id": "new"} (the process gets its id).
// Terminate target: {"kind": "process", "id": "<positive pid>"}.
class ProcessExecutor final : public rime::action::Executor {
 public:
  explicit ProcessExecutor(ProcessService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  ProcessService& service_;
};

}  // namespace rime::win32
