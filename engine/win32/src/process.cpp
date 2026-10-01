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
  HANDLE handle;
  ~HandleGuard() {
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr) CloseHandle(handle);
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
  std::wstring command_line = L"\"" + spec.executable + L"\"";
  if (!spec.arguments.empty()) command_line += L" " + spec.arguments;
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION created{};
  const wchar_t* working_dir = spec.working_dir.empty() ? nullptr : spec.working_dir.c_str();
  if (!CreateProcessW(spec.executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, working_dir, &startup, &created)) {
    const DWORD failure = GetLastError();
    return {Code::ExecutionFailed,
            "cannot launch " + to_utf8(spec.executable) + " (win32 error " +
                std::to_string(failure) + ")"};
  }
  CloseHandle(created.hThread);
  CloseHandle(created.hProcess);
  pid = static_cast<std::uint32_t>(created.dwProcessId);
  return rime::core::Error::none();
}

Error ProcessService::terminate(const std::uint32_t pid, const int exit_code) const {
  if (pid == 0) return {Code::InvalidContract, "pid must be positive"};
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
