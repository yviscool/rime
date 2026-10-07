#pragma once

#include "rime/core/json.hpp"
#include "rime/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rime::win32 {

struct ProcessInfo {
  std::uint32_t pid{0};
  std::uint32_t parent_pid{0};
  // Executable file name, e.g. "notepad.exe".
  std::string name;
  // Full image path; empty when the process cannot be queried.
  std::string exe_path;
};

struct LaunchSpec {
  std::wstring executable;
  std::wstring arguments;
  std::wstring working_dir;
};

// Credentials for run_as (AHK RunAs). `password` exists only inside this
// struct and the local CreateProcessWithLogonW call: it is never written to a
// log, never placed in an error message and never part of an Action Trace.
// The basis for the last claim is Kernel::record (engine/action/src/
// kernel.cpp:221), which writes the Action envelope - type, capability,
// detail, result_code - and never Action::payload, so a Trace consumer cannot
// observe the credential.
struct RunAsSpec {
  std::wstring user;
  std::wstring password;
  std::wstring domain;
  std::wstring executable;
  std::wstring arguments;
  std::wstring working_dir;
};

// AHK Shutdown as a string enum plus its two orthogonal flags: `mode` picks
// the operation, `force` is AHK's EWX_FORCE bit and `timeout_sec` is a grace
// period in seconds (0 = act immediately). The mapping from AHK's numeric
// flag word to these fields lives in docs/api/process-shell.md; the mode
// spellings are rejected before any Win32 call.
struct ShutdownSpec {
  std::string mode;
  bool force{false};
  std::uint32_t timeout_sec{0};
};

// Snapshot-based process queries plus an internal waitable-handle table.
// list/info/launch/terminate own no long-lived handles. launch_waitable keeps
// hProcess under an opaque numeric reference so RunWait can observe exit
// without blocking: ref ids are plain numbers, never Win32 handles, and never
// reach JS. wait_ref consumes a reference (one bounded slice per call),
// cancel_wait releases one, and stop()/the destructor sweep whatever a caller
// abandoned.
class ProcessService final {
 public:
  ProcessService() = default;
  // Releases every reference still registered (see stop()); an abandoned wait
  // can therefore never leak a process handle past the service's lifetime.
  ~ProcessService();

  ProcessService(const ProcessService&) = delete;
  ProcessService& operator=(const ProcessService&) = delete;

  [[nodiscard]] rime::core::Error list(std::vector<ProcessInfo>& out) const;
  [[nodiscard]] rime::core::Error info(std::uint32_t pid, ProcessInfo& out) const;
  [[nodiscard]] rime::core::Error launch(const LaunchSpec& spec, std::uint32_t& pid) const;
  [[nodiscard]] rime::core::Error terminate(std::uint32_t pid, int exit_code) const;
  // Opens a file for editing (AHK Edit rule): the shell "edit" verb first,
  // falling back to notepad with the quoted path. The already-open-window
  // foregrounding AHK does is deferred (it needs editor-specific title
  // matching, documented in docs/api/process-shell.md).
  [[nodiscard]] rime::core::Error edit(const std::string& path_utf8) const;

  // Same path and same contract errors as launch(), but hProcess is kept in
  // the waitable table instead of being closed. The id that comes back is a
  // reference into that table (starting at 1, never reused), not a handle.
  [[nodiscard]] rime::core::Error launch_waitable(const LaunchSpec& spec, std::uint32_t& pid,
                                                   std::uint64_t& ref_id);
  // UTF-8 convenience for callers that keep windows.h out of their translation
  // unit (the rime:process JS module): converts and delegates.
  [[nodiscard]] rime::core::Error launch_waitable(const std::string& executable_utf8,
                                                   const std::string& arguments_utf8,
                                                   const std::string& working_dir_utf8,
                                                   std::uint32_t& pid, std::uint64_t& ref_id);
  // Open an already-running pid into the same table (SYNCHRONIZE only) so
  // ProcessWaitClose can wait for exit instead of polling the snapshot.
  // `opened` stays false, without an error, whenever the OS refuses the open
  // (pid already gone, protected process, access denied): the caller then
  // falls back to existence polling, which observes both cases anyway.
  [[nodiscard]] rime::core::Error open_waitable_pid(std::uint32_t pid, std::uint64_t& ref_id,
                                                    bool& opened);
  // Bounded slice of a wait: blocks for at most kWaitSliceMs on the handle and
  // then reports. Exit -> exit_code is filled, the handle is closed and the
  // reference is consumed (waiting on it again is InvalidContract). A passed
  // deadline -> timed_out is true and the handle is kept open for a later
  // wait_ref or cancel_wait. An unknown/consumed reference -> InvalidContract.
  [[nodiscard]] rime::core::Error wait_ref(std::uint64_t ref_id, std::int64_t deadline_unix_ms,
                                           std::uint64_t& exit_code, bool& timed_out);
  // Abandon/cancel path: closes the handle and drops the reference. A reference
  // owned by an in-flight wait_ref must not be cancelled from another thread
  // until that call returns.
  [[nodiscard]] rime::core::Error cancel_wait(std::uint64_t ref_id);
  // How many waitable handles are still registered (tests/diagnostics).
  [[nodiscard]] std::size_t open_waitable_count() const;
  // Shutdown sweep: closes and forgets every registered handle, returning how
  // many were released. Idempotent, safe to call repeatedly from a host stop
  // chain; the destructor calls it as well.
  std::size_t stop();

  // ProcessSetPriority. `priority` is one of idle, belowNormal, normal,
  // aboveNormal, high, realtime (is_valid_priority); anything else is
  // InvalidContract before the process is opened. "realtime" is never elevated
  // for: when the OS refuses the class the plain open/set failure is reported
  // as ExecutionFailed.
  [[nodiscard]] rime::core::Error set_priority(std::uint32_t pid, const std::string& priority);
  // RunAs (AHK RunAs): CreateProcessWithLogonW with LOGON_WITH_PROFILE, so the
  // target user's profile is loaded - this call can take seconds and is the
  // one ProcessService call that is not short.
  [[nodiscard]] rime::core::Error run_as(const RunAsSpec& spec, std::uint32_t& pid);
  // Shutdown (AHK Shutdown). Every contract check runs before the first Win32
  // call, so a rejected mode/timeout/force combination can never reach the
  // shutdown APIs.
  [[nodiscard]] rime::core::Error shutdown_system(const ShutdownSpec& spec);

 private:
  mutable std::mutex waitables_mutex_;
  // HANDLE values are stored as void* so this public header keeps no windows.h
  // dependency (same rule as ui_thread.hpp) and so no raw handle is ever
  // handed to JS. An entry exists only while its reference is live: consuming
  // or cancelling erases it, which is why open_waitable_count() doubles as the
  // leak diagnostic.
  std::unordered_map<std::uint64_t, void*> waitables_;
  // Ref ids start at 1 and are never reused, so "issued but no longer in the
  // table" is distinguishable from "never issued" without a consumed ledger.
  std::uint64_t next_waitable_ref_{1};
};

// True for the six ProcessSetPriority spellings. Shared by the executor
// (payload contract) and the service (call contract) so both layers accept
// exactly one grammar.
[[nodiscard]] bool is_valid_priority(const std::string& priority);

[[nodiscard]] rime::core::json::Value process_info_json(const ProcessInfo& process);

}  // namespace rime::win32
