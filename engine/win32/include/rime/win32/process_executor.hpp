#pragma once

#include "rime/action/action.hpp"
#include "rime/win32/process.hpp"

namespace rime::win32 {

// Executes `process.launch` (capability `process.launch`),
// `process.terminate` (capability `process.terminate`),
// `process.set.priority` (capability `process.manage`),
// `process.runas` (capability `process.runas`) and
// `process.shutdown` (capability `process.shutdown`).
// Launch payload: {"command": string, "args"?: string, "workingDir"?: string}.
// Launch target: {"kind": "process", "id": "new"} (the process gets its id).
// Terminate target: {"kind": "process", "id": "<positive pid>"}.
// SetPriority target: {"kind": "process", "id": "<pid>"}; payload:
//   {"pid": number, "priority": "idle"|"belowNormal"|"normal"|"aboveNormal"|
//    "high"|"realtime"} (pid must equal the target id).
// RunAs target: {"kind": "process", "id": "new"}; payload:
//   {"user": string, "password": string, "domain"?: string,
//    "executable": string, "arguments"?: string, "workingDir"?: string}.
//   The password is never echoed in an error message or a Trace entry.
// Shutdown target: {"kind": "process", "id": "system"}; payload:
//   {"mode": "logoff"|"shutdown"|"reboot"|"poweroff"|"hibernate",
//    "force"?: boolean, "timeoutSec"?: integer 0..600}. Validation runs
//   before any Win32 call, so a rejected request never reaches the OS.
class ProcessExecutor final : public rime::action::Executor {
 public:
  explicit ProcessExecutor(ProcessService& service) : service_(service) {}

  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken cancellation) override;

 private:
  ProcessService& service_;
};

}  // namespace rime::win32
