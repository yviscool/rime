#include "rime/win32/process.hpp"

#include "utf.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <string>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

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

}  // namespace

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
  if (spec.executable.empty()) {
    return {Code::InvalidContract, "launch requires a non-empty command"};
  }
  // The executable is wrapped in quotes below; an embedded quote would break
  // out of the quoting and allow argument injection.
  if (spec.executable.find(L'"') != std::wstring::npos) {
    return {Code::InvalidContract, "launch executable must not contain a quote"};
  }
  std::wstring command_line = L"\"" + spec.executable + L"\"";
  if (!spec.arguments.empty()) command_line += L" " + spec.arguments;
  // CreateProcessW caps the command line at 32767 characters including the
  // terminating NUL.
  if (command_line.size() >= 32767) {
    return {Code::InvalidContract, "launch command line exceeds the Win32 limit"};
  }
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  // CreateProcessW does not search the path for lpApplicationName, so a bare
  // command is resolved first; the command line above keeps the name exactly
  // as the caller spelled it (argv[0] stays stable).
  bool path_searched = false;
  bool path_found = false;
  const std::wstring application = resolve_application(spec.executable, path_searched, path_found);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION created{};
  const wchar_t* working_dir = spec.working_dir.empty() ? nullptr : spec.working_dir.c_str();
  if (!CreateProcessW(application.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, working_dir, &startup, &created)) {
    const DWORD failure = GetLastError();
    std::string message = "cannot launch " + to_utf8(spec.executable);
    if (path_searched && !path_found) {
      message += ": PATH search failed for the bare command (no search-path entry)";
    }
    message += " (win32 error " + std::to_string(failure) + ")";
    return {Code::ExecutionFailed, message};
  }
  CloseHandle(created.hThread);
  // The process handle is closed immediately, so the returned pid is only a
  // snapshot: it can be recycled by the OS once the process exits (TOCTOU).
  // Callers must re-resolve the pid before acting on it. Pinning the lifetime
  // with a Job Object is deliberately left out to keep this change small.
  CloseHandle(created.hProcess);
  pid = static_cast<std::uint32_t>(created.dwProcessId);
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
