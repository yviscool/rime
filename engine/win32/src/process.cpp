#include "rime/win32/process.hpp"

#include "utf.hpp"

#include <windows.h>
// WIN32_LEAN_AND_MEAN keeps winreg.h (InitiateSystemShutdownExW) and reason.h
// (SHTDN_REASON_*) out of windows.h, so both are pulled in explicitly here.
#include <reason.h>
#include <tlhelp32.h>
#include <winreg.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

// One bounded wait slice for wait_ref: the handle is polled with this timeout
// so a waiter never occupies a thread for the whole lifetime of a child, and
// the table lock is never held for longer than a single slice.
constexpr std::uint32_t kWaitSliceMs = 25;

struct HandleGuard {
  HANDLE handle = nullptr;

  explicit HandleGuard(HANDLE raw) : handle(raw) {}

  HandleGuard(const HandleGuard&) = delete;
  HandleGuard& operator=(const HandleGuard&) = delete;

  HandleGuard(HandleGuard&& other) noexcept
      : handle(std::exchange(other.handle, nullptr)) {}

  HandleGuard& operator=(HandleGuard&& other) noexcept {
    if (this != &other) {
      reset();
      handle = std::exchange(other.handle, nullptr);
    }
    return *this;
  }

  ~HandleGuard() { reset(); }

  void reset() {
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
      CloseHandle(handle);
    }
    handle = nullptr;
  }
};

void copy_entry(const PROCESSENTRY32W& entry, ProcessInfo& out) {
  out.pid = static_cast<std::uint32_t>(entry.th32ProcessID);
  out.parent_pid = static_cast<std::uint32_t>(entry.th32ParentProcessID);
  out.name = to_utf8(entry.szExeFile);
}

Error find_in_snapshot(const std::uint32_t pid, ProcessInfo& out) {
  const HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (raw == INVALID_HANDLE_VALUE) {
    return {Code::ExecutionFailed, "cannot snapshot processes"};
  }
  HandleGuard guard{raw};
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(raw, &entry)) {
    return {Code::ExecutionFailed, "cannot read the process list"};
  }
  do {
    if (static_cast<std::uint32_t>(entry.th32ProcessID) == pid) {
      copy_entry(entry, out);
      return rime::core::Error::none();
    }
  } while (Process32NextW(raw, &entry));
  return {Code::ExecutionFailed, "process " + std::to_string(pid) + " no longer exists"};
}

std::string image_path(const std::uint32_t pid) {
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!process) return {};
  HandleGuard guard{process};
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
  path.resize(size);
  return to_utf8(path);
}

// SearchPathW with a buffer that grows to the reported size. Returns the
// found path, or an empty string when nothing matches (a lookup that needs
// more than 32768 characters is treated as "not found" so a hostile PATH
// cannot make the buffer grow without bound).
std::wstring search_path_lookup(const std::wstring& name, const wchar_t* extension) {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD found =
        SearchPathW(nullptr, name.c_str(), extension, static_cast<DWORD>(buffer.size()),
                    buffer.data(), nullptr);
    if (found == 0 || found > 32768u) return {};
    if (found < buffer.size()) {
      buffer.resize(found);
      return buffer;
    }
    buffer.resize(static_cast<std::size_t>(found) + 1);
  }
}

// Resolves the executable name handed to CreateProcessW. A command that
// already carries a path (drive, rooted, UNC or relative with a separator)
// is returned verbatim: no search applies and the historic behaviour cannot
// change. A bare name cannot be resolved by CreateProcessW either (as
// lpApplicationName it only sees the current directory), so it is looked up
// through the documented search order - application directory, current
// directory, system directory, Windows directory, PATH - and the absolute
// result is what gets passed as lpApplicationName. When no entry matches the
// original name is kept, so the launch still fails through the regular
// CreateProcessW error path, which reports the failed search.
// `searched` reports that a lookup ran, `found` that it resolved.
std::wstring resolve_application(const std::wstring& executable, bool& searched, bool& found) {
  searched = false;
  found = false;
  if (executable.find_first_of(L"\\/") != std::wstring::npos ||
      executable.find(L':') != std::wstring::npos) {
    return executable;
  }
  searched = true;
  const std::size_t dot = executable.find_last_of(L'.');
  const bool has_extension = dot != std::wstring::npos && dot > 0;
  // SearchPathW only appends `extension` to a name without one, so an
  // extensionless name is tried as "<name>.exe" first and then verbatim; a
  // name that already carries an extension is only looked up verbatim.
  std::wstring resolved = search_path_lookup(executable, has_extension ? nullptr : L".exe");
  if (resolved.empty() && !has_extension) resolved = search_path_lookup(executable, nullptr);
  if (resolved.empty()) return executable;
  found = true;
  return resolved;
}

// Validation and command-line preparation shared by launch, launch_waitable
// and run_as. `op` names the operation in contract errors so the historic
// launch messages stay byte-identical while run_as reports itself. The result
// is handed to create_prepared_process unchanged.
Error prepare_command(const char* op, const std::wstring& executable,
                      const std::wstring& arguments, std::vector<wchar_t>& mutable_command,
                      std::wstring& application, bool& path_searched, bool& path_found) {
  if (executable.empty()) {
    return {Code::InvalidContract, std::string(op) + " requires a non-empty command"};
  }
  // The executable is wrapped in quotes below; an embedded quote would break
  // out of the quoting and allow argument injection.
  if (executable.find(L'"') != std::wstring::npos) {
    return {Code::InvalidContract, std::string(op) + " executable must not contain a quote"};
  }
  std::wstring command_line = L"\"" + executable + L"\"";
  if (!arguments.empty()) command_line += L" " + arguments;
  // CreateProcessW caps the command line at 32767 characters including the
  // terminating NUL.
  if (command_line.size() >= 32767) {
    return {Code::InvalidContract, std::string(op) + " command line exceeds the Win32 limit"};
  }
  mutable_command.assign(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  // CreateProcessW does not search the path for lpApplicationName, so a bare
  // command is resolved first; the command line above keeps the name exactly
  // as the caller spelled it (argv[0] stays stable).
  application = resolve_application(executable, path_searched, path_found);
  return rime::core::Error::none();
}

// The failure text every launch-shaped entry shares: the failed search is
// named when one ran, and the Win32 error always follows. `head` is the
// message prefix ("cannot launch ", "cannot run <user> with "), which is why
// no credential ever reaches it.
Error launch_failure(const std::string& head, const std::wstring& executable,
                     const bool path_searched, const bool path_found, const DWORD failure) {
  std::string message = head + to_utf8(executable);
  if (path_searched && !path_found) {
    message += ": PATH search failed for the bare command (no search-path entry)";
  }
  message += " (win32 error " + std::to_string(failure) + ")";
  return {Code::ExecutionFailed, message};
}

// Runs CreateProcessW for a prepared command. The thread handle is closed
// here (no caller wants it); the process handle is returned so launch can
// close it immediately and launch_waitable can keep it.
Error create_prepared_process(const std::wstring& executable,
                              std::vector<wchar_t>& mutable_command,
                              const std::wstring& application, const bool path_searched,
                              const bool path_found, const std::wstring& working_dir,
                              const std::string& failure_head, HANDLE& process,
                              std::uint32_t& pid) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION created{};
  const wchar_t* dir = working_dir.empty() ? nullptr : working_dir.c_str();
  if (!CreateProcessW(application.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, dir, &startup, &created)) {
    return launch_failure(failure_head, executable, path_searched, path_found, GetLastError());
  }
  CloseHandle(created.hThread);
  process = created.hProcess;
  pid = static_cast<std::uint32_t>(created.dwProcessId);
  return rime::core::Error::none();
}

struct PriorityMapping {
  const char* name;
  DWORD value;
};

// The whole ProcessSetPriority mapping lives here so the public header never
// names a Win32 priority class.
constexpr PriorityMapping kPriorityMappings[] = {
    {"idle", IDLE_PRIORITY_CLASS},
    {"belowNormal", BELOW_NORMAL_PRIORITY_CLASS},
    {"normal", NORMAL_PRIORITY_CLASS},
    {"aboveNormal", ABOVE_NORMAL_PRIORITY_CLASS},
    {"high", HIGH_PRIORITY_CLASS},
    {"realtime", REALTIME_PRIORITY_CLASS},
};

bool priority_value(const std::string& priority, DWORD& out) {
  for (const PriorityMapping& mapping : kPriorityMappings) {
    if (priority == mapping.name) {
      out = mapping.value;
      return true;
    }
  }
  return false;
}

// Shutdown mode -> ExitWindowsEx flags. hibernate has no EWX_* bit at all
// (see shutdown_system), so it never reaches this table.
DWORD shutdown_flags(const std::string& mode) {
  if (mode == "logoff") return EWX_LOGOFF;
  if (mode == "shutdown") return EWX_SHUTDOWN;
  if (mode == "reboot") return EWX_REBOOT;
  return EWX_POWEROFF;
}

// Enables SeShutdownName in this process token, exactly like AHK's Shutdown
// (script_autoit.cpp): OpenProcessToken + AdjustTokenPrivileges. This only
// turns on a privilege the token already carries - it never asks Windows for
// elevation, so a token that does not hold it still fails below with the
// plain Win32 error.
Error enable_shutdown_privilege() {
  HANDLE token_raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token_raw)) {
    const DWORD failure = GetLastError();
    return {Code::ExecutionFailed,
            "cannot open the process token (win32 error " + std::to_string(failure) + ")"};
  }
  HandleGuard token{token_raw};
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid)) {
    const DWORD failure = GetLastError();
    return {Code::ExecutionFailed,
            "cannot look up SE_SHUTDOWN_NAME (win32 error " + std::to_string(failure) + ")"};
  }
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  // AdjustTokenPrivileges reports "done" even when it could not apply the
  // change; GetLastError is the real verdict (same as AHK).
  static_cast<void>(AdjustTokenPrivileges(token.handle, FALSE, &privileges, 0, nullptr, nullptr));
  const DWORD adjusted = GetLastError();
  if (adjusted != ERROR_SUCCESS) {
    return {Code::ExecutionFailed,
            "SE_SHUTDOWN_NAME is not available (win32 error " + std::to_string(adjusted) + ")"};
  }
  return rime::core::Error::none();
}

// Hibernate: ExitWindowsEx has no equivalent flag, so powrprof's
// SetSuspendState(TRUE, ...) is used (AHK documents the same DllCall for
// scripts that need to hibernate). The DLL is resolved at run time, which
// keeps hibernate free of a new link dependency.
Error hibernate_system() {
  HMODULE powrprof = LoadLibraryW(L"powrprof.dll");
  if (!powrprof) {
    const DWORD failure = GetLastError();
    return {Code::ExecutionFailed,
            "cannot load powrprof.dll (win32 error " + std::to_string(failure) + ")"};
  }
  using SetSuspendStateFn = BOOLEAN(WINAPI*)(BOOLEAN, BOOLEAN, BOOLEAN);
  auto* set_suspend_state =
      reinterpret_cast<SetSuspendStateFn>(GetProcAddress(powrprof, "SetSuspendState"));
  if (!set_suspend_state) {
    const DWORD failure = GetLastError();
    FreeLibrary(powrprof);
    return {Code::ExecutionFailed,
            "cannot resolve SetSuspendState (win32 error " + std::to_string(failure) + ")"};
  }
  const BOOLEAN requested = set_suspend_state(TRUE, FALSE, FALSE);
  const DWORD failure = GetLastError();
  FreeLibrary(powrprof);
  if (!requested) {
    return {Code::ExecutionFailed,
            "cannot hibernate the system (win32 error " + std::to_string(failure) + ")"};
  }
  return rime::core::Error::none();
}

}  // namespace

bool is_valid_priority(const std::string& priority) {
  DWORD scratch = 0;
  return priority_value(priority, scratch);
}

ProcessService::~ProcessService() { stop(); }

Error ProcessService::list(std::vector<ProcessInfo>& out) const {
  out.clear();
  const HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (raw == INVALID_HANDLE_VALUE) {
    return {Code::ExecutionFailed, "cannot snapshot processes"};
  }
  HandleGuard guard{raw};
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(raw, &entry)) {
    return {Code::ExecutionFailed, "cannot read the process list"};
  }
  do {
    ProcessInfo info;
    copy_entry(entry, info);
    out.push_back(std::move(info));
  } while (Process32NextW(raw, &entry));
  return rime::core::Error::none();
}

Error ProcessService::info(const std::uint32_t pid, ProcessInfo& out) const {
  if (pid == 0) return {Code::InvalidContract, "pid must be positive"};
  if (const auto error = find_in_snapshot(pid, out); !error.ok()) return error;
  out.exe_path = image_path(pid);
  return rime::core::Error::none();
}

Error ProcessService::launch(const LaunchSpec& spec, std::uint32_t& pid) const {
  std::vector<wchar_t> mutable_command;
  std::wstring application;
  bool path_searched = false;
  bool path_found = false;
  if (const auto prepared =
          prepare_command("launch", spec.executable, spec.arguments, mutable_command, application,
                          path_searched, path_found);
      !prepared.ok()) {
    return prepared;
  }
  HANDLE process = nullptr;
  if (const auto created = create_prepared_process(spec.executable, mutable_command, application,
                                                   path_searched, path_found, spec.working_dir,
                                                   "cannot launch ", process, pid);
      !created.ok()) {
    return created;
  }
  // The process handle is closed immediately, so the returned pid is only a
  // snapshot: it can be recycled by the OS once the process exits (TOCTOU).
  // Callers must re-resolve the pid before acting on it. Pinning the lifetime
  // with a Job Object is deliberately left out to keep this change small.
  // launch_waitable() is the counterpart that keeps the handle instead.
  CloseHandle(process);
  return rime::core::Error::none();
}

Error ProcessService::launch_waitable(const LaunchSpec& spec, std::uint32_t& pid,
                                      std::uint64_t& ref_id) {
  std::vector<wchar_t> mutable_command;
  std::wstring application;
  bool path_searched = false;
  bool path_found = false;
  if (const auto prepared =
          prepare_command("launch", spec.executable, spec.arguments, mutable_command, application,
                          path_searched, path_found);
      !prepared.ok()) {
    return prepared;
  }
  HANDLE process = nullptr;
  if (const auto created = create_prepared_process(spec.executable, mutable_command, application,
                                                   path_searched, path_found, spec.working_dir,
                                                   "cannot launch ", process, pid);
      !created.ok()) {
    return created;
  }
  // Ownership of hProcess moves into the table here; from now on only
  // wait_ref (exit), cancel_wait (abandon) and stop() (sweep) may close it.
  std::lock_guard<std::mutex> lock(waitables_mutex_);
  ref_id = next_waitable_ref_++;
  waitables_.emplace(ref_id, process);
  return rime::core::Error::none();
}

Error ProcessService::launch_waitable(const std::string& executable_utf8,
                                      const std::string& arguments_utf8,
                                      const std::string& working_dir_utf8, std::uint32_t& pid,
                                      std::uint64_t& ref_id) {
  LaunchSpec spec;
  spec.executable = from_utf8(executable_utf8);
  spec.arguments = from_utf8(arguments_utf8);
  spec.working_dir = from_utf8(working_dir_utf8);
  return launch_waitable(spec, pid, ref_id);
}

Error ProcessService::open_waitable_pid(const std::uint32_t pid, std::uint64_t& ref_id,
                                        bool& opened) {
  ref_id = 0;
  opened = false;
  if (pid == 0) return {Code::InvalidContract, "pid must be positive"};
  // SYNCHRONIZE is the only right requested: it is what a wait for exit needs
  // and nothing else, so a process the caller may not otherwise touch (or a
  // pid that just exited, or a protected one) is never turned into an error
  // here. Falling back to existence polling observes every one of those cases.
  HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!handle) return rime::core::Error::none();
  std::lock_guard<std::mutex> lock(waitables_mutex_);
  ref_id = next_waitable_ref_++;
  waitables_.emplace(ref_id, handle);
  opened = true;
  return rime::core::Error::none();
}

Error ProcessService::wait_ref(const std::uint64_t ref_id, const std::int64_t deadline_unix_ms,
                               std::uint64_t& exit_code, bool& timed_out) {
  exit_code = 0;
  timed_out = false;
  if (ref_id == 0) return {Code::InvalidContract, "process reference must be positive"};
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(waitables_mutex_);
      if (ref_id >= next_waitable_ref_) {
        return {Code::InvalidContract, "unknown process reference"};
      }
      const auto found = waitables_.find(ref_id);
      if (found == waitables_.end()) {
        // Issued once, gone for good: either wait_ref consumed it or
        // cancel_wait released it. Both mean "this reference cannot be waited
        // on again", and ids are never recycled, so no consumed-id ledger is
        // needed to separate them from an id that never existed.
        return {Code::InvalidContract, "process reference already consumed"};
      }
      const HANDLE handle = static_cast<HANDLE>(found->second);
      // The lock is held across the slice on purpose: stop()/cancel_wait() can
      // then never CloseHandle out from under an in-flight wait, and the worst
      // they block is one kWaitSliceMs.
      const DWORD wait = WaitForSingleObject(handle, kWaitSliceMs);
      if (wait == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (!GetExitCodeProcess(handle, &code)) {
          const DWORD failure = GetLastError();
          CloseHandle(handle);
          waitables_.erase(found);
          return {Code::ExecutionFailed,
                  "cannot read the exit code of process reference " + std::to_string(ref_id) +
                      " (win32 error " + std::to_string(failure) + ")"};
        }
        // Consumed: the handle is closed and the entry dropped while the lock
        // is still held, so a concurrent wait_ref observes either the live
        // entry or the "already consumed" error, never a half-consumed state.
        CloseHandle(handle);
        waitables_.erase(found);
        exit_code = code;
        return rime::core::Error::none();
      }
      if (wait == WAIT_FAILED) {
        const DWORD failure = GetLastError();
        CloseHandle(handle);
        waitables_.erase(found);
        return {Code::ExecutionFailed,
                "cannot wait for process reference " + std::to_string(ref_id) +
                    " (win32 error " + std::to_string(failure) + ")"};
      }
      // WAIT_TIMEOUT: fall through with the lock released below.
    }
    // The deadline is evaluated between slices: the process always gets one
    // full slice to report its exit before an already-passed deadline turns
    // into a timeout, and a timeout keeps the handle registered for a later
    // wait_ref or cancel_wait.
    const auto now_ms = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (now_ms >= deadline_unix_ms) {
      timed_out = true;
      return rime::core::Error::none();
    }
  }
}

Error ProcessService::cancel_wait(const std::uint64_t ref_id) {
  if (ref_id == 0) return {Code::InvalidContract, "process reference must be positive"};
  std::lock_guard<std::mutex> lock(waitables_mutex_);
  const auto found = waitables_.find(ref_id);
  if (found == waitables_.end()) {
    if (ref_id >= next_waitable_ref_) {
      return {Code::InvalidContract, "unknown process reference"};
    }
    return {Code::InvalidContract, "process reference already consumed"};
  }
  CloseHandle(static_cast<HANDLE>(found->second));
  waitables_.erase(found);
  return rime::core::Error::none();
}

std::size_t ProcessService::open_waitable_count() const {
  std::lock_guard<std::mutex> lock(waitables_mutex_);
  return waitables_.size();
}

std::size_t ProcessService::stop() {
  std::lock_guard<std::mutex> lock(waitables_mutex_);
  const std::size_t released = waitables_.size();
  for (const auto& entry : waitables_) {
    if (entry.second != nullptr) CloseHandle(static_cast<HANDLE>(entry.second));
  }
  waitables_.clear();
  return released;
}

Error ProcessService::set_priority(const std::uint32_t pid, const std::string& priority) {
  if (pid == 0) return {Code::InvalidContract, "pid must be positive"};
  DWORD class_value = 0;
  if (!priority_value(priority, class_value)) {
    return {Code::InvalidContract,
            "priority must be one of idle, belowNormal, normal, aboveNormal, high, realtime"};
  }
  const HANDLE process =
      OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!process) {
    const DWORD failure = GetLastError();
    if (failure == ERROR_INVALID_PARAMETER) {
      return {Code::ExecutionFailed, "process " + std::to_string(pid) + " no longer exists"};
    }
    if (priority == "realtime") {
      // No privilege is raised anywhere in this function: when the caller is
      // not allowed the class, the plain open failure is reported as-is.
      return {Code::ExecutionFailed,
              "realtime priority requires elevation (win32 error " + std::to_string(failure) +
                  ")"};
    }
    return {Code::ExecutionFailed,
            "cannot open process " + std::to_string(pid) + " (win32 error " +
                std::to_string(failure) + ")"};
  }
  HandleGuard guard{process};
  if (!SetPriorityClass(process, class_value)) {
    const DWORD failure = GetLastError();
    if (priority == "realtime") {
      return {Code::ExecutionFailed,
              "realtime priority requires elevation (win32 error " + std::to_string(failure) +
                  ")"};
    }
    return {Code::ExecutionFailed,
            "cannot set the priority of process " + std::to_string(pid) + " (win32 error " +
                std::to_string(failure) + ")"};
  }
  return rime::core::Error::none();
}

Error ProcessService::run_as(const RunAsSpec& spec, std::uint32_t& pid) {
  if (spec.user.empty()) return {Code::InvalidContract, "runAs requires a non-empty user"};
  std::vector<wchar_t> mutable_command;
  std::wstring application;
  bool path_searched = false;
  bool path_found = false;
  if (const auto prepared =
          prepare_command("runAs", spec.executable, spec.arguments, mutable_command, application,
                          path_searched, path_found);
      !prepared.ok()) {
    return prepared;
  }
  // Credential handling: spec.password is read exactly once, here, and goes
  // straight into CreateProcessWithLogonW. It is never copied into a message,
  // a log line or a Trace entry - Kernel::record (engine/action/src/
  // kernel.cpp:221) writes the Action envelope and never Action::payload, so
  // the Trace cannot surface it either. The failure text below is built from
  // the user and the executable only.
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION created{};
  const wchar_t* dir = spec.working_dir.empty() ? nullptr : spec.working_dir.c_str();
  if (!CreateProcessWithLogonW(spec.user.c_str(),
                               spec.domain.empty() ? nullptr : spec.domain.c_str(),
                               spec.password.c_str(), LOGON_WITH_PROFILE, application.c_str(),
                               mutable_command.data(), 0, nullptr, dir, &startup, &created)) {
    const DWORD failure = GetLastError();
    return launch_failure("cannot run " + to_utf8(spec.user) + " with ", spec.executable,
                          path_searched, path_found, failure);
  }
  CloseHandle(created.hThread);
  // Like launch(), RunAs reports a snapshot pid: the profile-loading call
  // above can take seconds, and the created handle is closed immediately.
  CloseHandle(created.hProcess);
  pid = static_cast<std::uint32_t>(created.dwProcessId);
  return rime::core::Error::none();
}

Error ProcessService::shutdown_system(const ShutdownSpec& spec) {
  // Contract checks come first: tests only ever exercise rejection paths, so
  // an illegal mode/timeout/force combination must be refused before the
  // token, the privilege or the shutdown call is touched.
  if (spec.mode != "logoff" && spec.mode != "shutdown" && spec.mode != "reboot" &&
      spec.mode != "poweroff" && spec.mode != "hibernate") {
    return {Code::InvalidContract,
            "shutdown mode must be one of logoff, shutdown, reboot, poweroff, hibernate"};
  }
  if (spec.timeout_sec > 600) {
    return {Code::InvalidContract, "shutdown timeoutSec must be between 0 and 600 seconds"};
  }
  if (spec.timeout_sec > 0 && spec.mode != "shutdown" && spec.mode != "reboot") {
    // ExitWindowsEx (logoff/poweroff) and SetSuspendState (hibernate) have no
    // grace period to give; refusing beats silently dropping the delay.
    return {Code::InvalidContract, "shutdown timeoutSec requires mode shutdown or reboot"};
  }
  if (spec.force && spec.mode == "hibernate") {
    return {Code::InvalidContract, "shutdown force is not supported for mode hibernate"};
  }
  if (spec.mode == "hibernate") return hibernate_system();

  if (const auto privilege_error = enable_shutdown_privilege(); !privilege_error.ok()) {
    return privilege_error;
  }
  if (spec.timeout_sec > 0) {
    // A grace period is only expressible through InitiateSystemShutdownExW
    // (dwTimeout), which knows shutdown and reboot - the two modes accepted
    // above. The reason codes mark this as a planned application shutdown so
    // the event log does not file it as an unplanned power loss.
    const DWORD reason = SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED;
    if (!InitiateSystemShutdownExW(nullptr, nullptr, static_cast<DWORD>(spec.timeout_sec),
                                   spec.force ? TRUE : FALSE, spec.mode == "reboot", reason)) {
      const DWORD failure = GetLastError();
      return {Code::ExecutionFailed,
              "cannot initiate system shutdown (win32 error " + std::to_string(failure) + ")"};
    }
    return rime::core::Error::none();
  }
  DWORD flags = shutdown_flags(spec.mode);
  if (spec.force) flags |= EWX_FORCE;
  if (!ExitWindowsEx(flags, 0)) {
    const DWORD failure = GetLastError();
    return {Code::ExecutionFailed,
            "cannot request system " + spec.mode + " (win32 error " + std::to_string(failure) +
                ")"};
  }
  return rime::core::Error::none();
}

Error ProcessService::terminate(const std::uint32_t pid, const int exit_code) const {
  if (pid == 0) return {Code::InvalidContract, "pid must be positive"};
  // TerminateProcess takes an unsigned exit code; reject negatives instead of
  // silently wrapping them. The signature is left unchanged.
  if (exit_code < 0) return {Code::InvalidContract, "exit code must be non-negative"};
  const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (!process) {
    const DWORD failure = GetLastError();
    if (failure == ERROR_INVALID_PARAMETER) {
      return {Code::ExecutionFailed, "process " + std::to_string(pid) + " no longer exists"};
    }
    return {Code::ExecutionFailed,
            "cannot open process " + std::to_string(pid) + " (win32 error " +
                std::to_string(failure) + ")"};
  }
  HandleGuard guard{process};
  if (!TerminateProcess(process, static_cast<UINT>(exit_code))) {
    const DWORD failure = GetLastError();
    return {Code::ExecutionFailed,
            "cannot terminate process " + std::to_string(pid) + " (win32 error " +
                std::to_string(failure) + ")"};
  }
  return rime::core::Error::none();
}

rime::core::json::Value process_info_json(const ProcessInfo& process) {
  rime::core::json::Value value = rime::core::json::Value::object();
  value.set("pid", rime::core::json::Value::number(static_cast<double>(process.pid)));
  value.set("parentPid", rime::core::json::Value::number(static_cast<double>(process.parent_pid)));
  value.set("name", rime::core::json::Value::string(process.name));
  value.set("exePath", rime::core::json::Value::string(process.exe_path));
  return value;
}

}  // namespace rime::win32
