#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/win32/process.hpp"
#include "rime/win32/process_executor.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
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

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--child") {
    Sleep(3000);
    return 0;
  }

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
  assert(!kernel.execute(bad_payload).succeeded);
  assert(kernel.execute(bad_payload).error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action bad_target = terminate;
  bad_target.id = 4;
  bad_target.target = {"process", "0"};
  assert(!kernel.execute(bad_target).succeeded);
  assert(kernel.execute(bad_target).error.code == rime::core::Error::Code::InvalidContract);

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
