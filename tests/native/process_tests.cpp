// Realism: L5 - real child processes are launched and terminated through
// the real process service; PATH resolution is asserted against the actual
// environment the test builds and restores.

#include "rime/action/kernel.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/json.hpp"
#include "rime/win32/process.hpp"
#include "rime/win32/process_executor.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::win32::LaunchSpec;
using rime::win32::ProcessInfo;
using rime::win32::ProcessService;

std::uint64_t deadline_ms() {
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         5000;
}

std::string narrow(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                          nullptr, nullptr);
  assert(size > 0);
  std::string text(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size,
                      nullptr, nullptr);
  return text;
}

std::wstring self_path() {
  std::wstring path(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  assert(size > 0 && size < path.size());
  path.resize(size);
  return path;
}

std::wstring system_directory() {
  wchar_t buffer[MAX_PATH]{};
  const DWORD size = GetSystemDirectoryW(buffer, MAX_PATH);
  assert(size > 0 && size < static_cast<DWORD>(MAX_PATH));
  return std::wstring(buffer, size);
}

std::string lowercase(std::string text) {
  for (char& character : text) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return text;
}

bool wait_gone(const ProcessService& service, const std::uint32_t pid,
               const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  ProcessInfo info;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!service.info(pid, info).ok()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return !service.info(pid, info).ok();
}

// Cleanup for a bounded child: it must exit on its own; a process that is
// still alive when the deadline passes is force-terminated first, so the
// caller never fails with an uncleaned process left behind.
bool wait_gone_cleaning(const ProcessService& service, const std::uint32_t pid) {
  if (wait_gone(service, pid, 6000ms)) return true;
  static_cast<void>(service.terminate(pid, 1));
  return wait_gone(service, pid, 4000ms);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--child") {
    Sleep(3000);
    return 0;
  }

  // Executors require the worker lane; this harness executes on the main thread.
  assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Worker).ok());
  ProcessService service;

  // The snapshot contains this process with a real image name.
  std::vector<ProcessInfo> processes;
  assert(service.list(processes).ok());
  assert(!processes.empty());
  const std::uint32_t self_pid = GetCurrentProcessId();
  bool found_self = false;
  for (const auto& process : processes) {
    if (process.pid == self_pid) {
      found_self = true;
      assert(!process.name.empty());
      assert(process.name.find(".exe") != std::string::npos);
    }
  }
  assert(found_self);

  // info resolves our own image path.
  ProcessInfo mine;
  assert(service.info(self_pid, mine).ok());
  assert(mine.pid == self_pid);
  assert(!mine.exe_path.empty());
  assert(mine.exe_path.find(mine.name) != std::string::npos);

  // Invalid pids and commands are rejected.
  ProcessInfo scratch;
  assert(service.info(0, scratch).code == rime::core::Error::Code::InvalidContract);
  std::uint32_t pid = 0;
  assert(service.launch({L"", L"", L""}, pid).code == rime::core::Error::Code::InvalidContract);

  // A launched child is queryable, then disappears on its own.
  const std::wstring executable = self_path();
  assert(service.launch({executable, L"--child", L""}, pid).ok());
  assert(pid != 0 && pid != self_pid);
  ProcessInfo child;
  assert(service.info(pid, child).ok());
  assert(child.name == mine.name);
  assert(wait_gone(service, pid, 6000ms));

  // Terminate ends a live child; repeating reports it as gone.
  assert(service.launch({executable, L"--child", L""}, pid).ok());
  assert(service.terminate(pid, 7).ok());
  assert(wait_gone(service, pid, 2000ms));
  assert(!service.terminate(pid, 1).ok());
  assert(service.terminate(0, 1).code == rime::core::Error::Code::InvalidContract);

  // Command resolution: a bare command (no path separator) is looked up
  // through the Win32 search path and launched from the resolved absolute
  // path; the same command spelled with a full path must keep working
  // unchanged. `cmd.exe` is used instead of this binary's own image because
  // ctest runs the test from the directory that holds the test binary, so a
  // bare self-name would resolve from the current directory and prove
  // nothing; `cmd.exe` never exists there, so the bare launch can only
  // succeed through the search path (the pre-fix code passed the bare name to
  // CreateProcessW as lpApplicationName, which completes it against the
  // current directory only and would fail with win32 error 2).
  wchar_t current_dir[MAX_PATH]{};
  const DWORD current_dir_length = GetCurrentDirectoryW(MAX_PATH, current_dir);
  assert(current_dir_length > 0 && current_dir_length < static_cast<DWORD>(MAX_PATH));
  const std::wstring self_dir = executable.substr(0, executable.find_last_of(L'\\'));
  // The first two search steps (the directory this application was loaded
  // from, then the current directory) must both be free of the command,
  // otherwise the bare launch below could succeed without any path search.
  assert(GetFileAttributesW((self_dir + L"\\cmd.exe").c_str()) == INVALID_FILE_ATTRIBUTES);
  assert(GetFileAttributesW((std::wstring(current_dir) + L"\\cmd.exe").c_str()) ==
         INVALID_FILE_ATTRIBUTES);

  const std::wstring full_command = system_directory() + L"\\cmd.exe";
  assert(GetFileAttributesW(full_command.c_str()) != INVALID_FILE_ATTRIBUTES);
  // The child stays alive for about two seconds, long enough to be observed;
  // cleanup waits for that exit and only force-terminates past the deadline.
  const std::wstring bounded_args = L"/c ping -n 3 127.0.0.1 >nul";

  std::uint32_t bare_pid = 0;
  assert(service.launch({L"cmd.exe", bounded_args, L""}, bare_pid).ok());
  assert(bare_pid != 0 && bare_pid != self_pid);
  ProcessInfo bare_child;
  assert(service.info(bare_pid, bare_child).ok());
  const std::string bare_image = lowercase(bare_child.exe_path);
  const std::string system_image = lowercase(narrow(full_command));
  if (bare_image != system_image) {
    std::fprintf(stderr, "search path resolved cmd.exe to '%s', expected '%s'\n",
                 bare_image.c_str(), system_image.c_str());
    std::fflush(stderr);
  }
  assert(bare_image == system_image);
  assert(wait_gone_cleaning(service, bare_pid));

  std::uint32_t path_pid = 0;
  assert(service.launch({full_command, bounded_args, L""}, path_pid).ok());
  assert(path_pid != 0 && path_pid != self_pid);
  ProcessInfo path_child;
  assert(service.info(path_pid, path_child).ok());
  assert(lowercase(path_child.exe_path) == system_image);
  assert(wait_gone_cleaning(service, path_pid));

  // A bare command that no search-path entry matches reports the failed
  // search instead of a bare CreateProcessW error.
  std::uint32_t missing_pid = 0;
  const auto missing =
      service.launch({L"rime-definitely-not-on-any-path.exe", L"", L""}, missing_pid);
  assert(missing.code == rime::core::Error::Code::ExecutionFailed);
  assert(missing.message.find("PATH search failed") != std::string::npos);
  assert(missing_pid == 0);

  // The executor drives both action types through the kernel.
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"process.launch", "process.terminate"}));
  const auto executor = std::make_shared<rime::win32::ProcessExecutor>(service);
  assert(kernel.register_executor("process.launch", executor).ok());
  assert(kernel.register_executor("process.terminate", executor).ok());

  rime::action::Action launch;
  launch.id = 1;
  launch.source = {"test", "process_tests"};
  launch.type = "process.launch";
  launch.capability = "process.launch";
  launch.target = {"process", "new"};
  launch.deadline_unix_ms = deadline_ms();
  rime::core::json::Value payload = rime::core::json::Value::object();
  payload.set("command", rime::core::json::Value::string(narrow(executable)));
  payload.set("args", rime::core::json::Value::string("--child"));
  launch.payload = rime::core::json::stringify(payload);
  const auto launched = kernel.execute(launch);
  assert(launched.succeeded);
  const auto* launched_pid = launched.value.find("pid");
  assert(launched_pid && launched_pid->is_number() && launched_pid->as_number() > 0);
  const auto child_pid = static_cast<std::uint32_t>(launched_pid->as_number());

  rime::action::Action terminate;
  terminate.id = 2;
  terminate.source = {"test", "process_tests"};
  terminate.type = "process.terminate";
  terminate.capability = "process.terminate";
  terminate.target = {"process", std::to_string(child_pid)};
  terminate.deadline_unix_ms = deadline_ms();
  terminate.payload = "{}";
  const auto terminated = kernel.execute(terminate);
  assert(terminated.succeeded);
  assert(wait_gone(service, child_pid, 2000ms));

  // Contract violations reject with InvalidContract.
  rime::action::Action bad_payload = launch;
  bad_payload.id = 3;
  bad_payload.payload = "{}";
  const auto bad_payload_result = kernel.execute(bad_payload);
  assert(!bad_payload_result.succeeded);
  assert(bad_payload_result.error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action bad_target = terminate;
  bad_target.id = 4;
  bad_target.target = {"process", "0"};
  const auto bad_target_result = kernel.execute(bad_target);
  assert(!bad_target_result.succeeded);
  assert(bad_target_result.error.code == rime::core::Error::Code::InvalidContract);

  // A policy without the capability denies before the executor runs.
  rime::action::Kernel denied(
      std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{}));
  assert(denied.register_executor("process.launch", executor).ok());
  const auto refused = denied.execute(launch);
  assert(!refused.succeeded);
  assert(refused.error.code == rime::core::Error::Code::CapabilityDenied);

  // An executor registered under an unknown type rejects its input.
  rime::action::Kernel unsupported(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{"process.launch", "process.terminate"}));
  assert(unsupported.register_executor("process.kill", executor).ok());
  rime::action::Action unknown = terminate;
  unknown.id = 5;
  unknown.type = "process.kill";
  const auto rejected = unsupported.execute(unknown);
  assert(!rejected.succeeded);
  assert(rejected.error.code == rime::core::Error::Code::InvalidContract);

  return 0;
}
