// Realism: L5 - real child processes are launched, waited on, re-prioritised
// and terminated through the real process service; PATH resolution is
// asserted against the actual environment the test builds and restores.
// WP6 coverage (waitable references, ProcessSetPriority, RunAs, Shutdown) is
// asserted the same way: every refusal path is exercised, the two OS effects
// that are safe here (a child's priority class, a child's exit code) are
// read back from Windows, and RunAs/Shutdown successes are deliberately not
// run - a logon needs a real credential and a shutdown would end this
// session.

#include "rime/action/kernel.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/win32/process.hpp"
#include "rime/win32/process_executor.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

// Absolute Unix-domain deadline for ProcessService::wait_ref, which takes the
// same wall-clock budget the Action pipeline uses.
std::int64_t unix_deadline(const std::int64_t budget_ms) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count() +
         budget_ms;
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
  if (argc >= 3 && std::string(argv[1]) == "--child-sleep") {
    // Deterministic long-lived child: the duration is an explicit argument so
    // a wait, cancel or priority test can pick a budget no lucky timing can
    // make expire on its own.
    Sleep(static_cast<DWORD>(std::atoi(argv[2])));
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

  // ---- WP6: waitable references, ProcessSetPriority, RunAs, Shutdown ----
  //
  // Every refusal below is decided before the OS is touched. The only OS
  // effects this block creates are a child's priority class, a child's exit
  // code and a child's lifetime - always on a process this file started and
  // always cleaned up inside the block. RunAs successes need a real
  // credential and Shutdown successes would end this session, so only their
  // contract refusals are driven.

  // Waitable reference lifecycle: launch_waitable keeps hProcess under an
  // opaque id, wait_ref consumes it when the child exits, and a consumed id
  // is refused instead of being waited again.
  std::uint32_t waited_pid = 0;
  std::uint64_t waited_ref = 0;
  assert(
      service.launch_waitable({executable, L"--child", L""}, waited_pid, waited_ref).ok());
  assert(waited_ref != 0);
  assert(service.open_waitable_count() == 1);
  bool exited = false;
  std::uint64_t exit_code = 0;
  bool timed_out = false;
  const auto waited_until = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < waited_until) {
    // One call blocks for at most kWaitSliceMs, so this is a bounded
    // condition poll with a wall-clock budget, never a bare sleep.
    assert(service.wait_ref(waited_ref, unix_deadline(10000), exit_code, timed_out).ok());
    if (!timed_out) {
      exited = true;
      break;
    }
  }
  assert(exited);
  assert(exit_code == 0);
  assert(service.open_waitable_count() == 0);
  const auto consumed_again =
      service.wait_ref(waited_ref, unix_deadline(1000), exit_code, timed_out);
  assert(consumed_again.code == rime::core::Error::Code::InvalidContract);
  assert(consumed_again.message.find("already consumed") != std::string::npos);
  assert(service.wait_ref(0, unix_deadline(1000), exit_code, timed_out).code ==
         rime::core::Error::Code::InvalidContract);
  assert(service.wait_ref(0x7ffffff0ull, unix_deadline(1000), exit_code, timed_out).code ==
         rime::core::Error::Code::InvalidContract);
  assert(service.cancel_wait(0x7ffffff0ull).code == rime::core::Error::Code::InvalidContract);

  // A deadline that passes reports timed_out and keeps the reference
  // registered; cancel_wait releases it and cancelling again is a contract
  // error. The child is expected to outlive both calls.
  std::uint32_t long_pid = 0;
  std::uint64_t long_ref = 0;
  assert(service
             .launch_waitable({executable, L"--child-sleep 20000", L""}, long_pid, long_ref)
             .ok());
  assert(service.open_waitable_count() == 1);
  assert(service.wait_ref(long_ref, unix_deadline(50), exit_code, timed_out).ok());
  assert(timed_out);
  // exit_code is (re)initialised to 0 by wait_ref: no exit was observed, so
  // it stays untouched - the timed_out flag is the actual verdict.
  assert(exit_code == 0);
  assert(service.open_waitable_count() == 1);
  assert(service.cancel_wait(long_ref).ok());
  assert(service.open_waitable_count() == 0);
  assert(service.cancel_wait(long_ref).code == rime::core::Error::Code::InvalidContract);
  assert(service.terminate(long_pid, 1).ok());
  assert(wait_gone(service, long_pid, 4000ms));

  // open_waitable_pid: a live pid opens, a pid the snapshot does not list
  // fails open without an error and without issuing a reference, and pid 0 is
  // a contract error. The non-existence is observed from the OS first, so
  // the expectation is not computed by the code under test.
  std::uint64_t self_ref = 0;
  bool opened = false;
  assert(service.open_waitable_pid(self_pid, self_ref, opened).ok());
  assert(opened);
  assert(self_ref != 0);
  assert(service.open_waitable_count() == 1);
  assert(service.cancel_wait(self_ref).ok());
  assert(service.open_waitable_count() == 0);

  const std::uint32_t absent_pid = 0x10000000u;
  ProcessInfo absent_probe;
  assert(!service.info(absent_pid, absent_probe).ok());
  std::uint64_t absent_ref = 0;
  opened = true;
  assert(service.open_waitable_pid(absent_pid, absent_ref, opened).ok());
  assert(!opened);
  assert(absent_ref == 0);
  assert(service.open_waitable_count() == 0);
  assert(service.open_waitable_pid(0, absent_ref, opened).code ==
         rime::core::Error::Code::InvalidContract);

  // stop() sweeps whatever a caller abandoned and is idempotent, which is
  // what a host stop chain and the destructor both need.
  std::uint32_t swept_pid = 0;
  std::uint64_t swept_ref = 0;
  assert(service
             .launch_waitable({executable, L"--child-sleep 20000", L""}, swept_pid, swept_ref)
             .ok());
  assert(service.open_waitable_count() == 1);
  assert(service.stop() == 1u);
  assert(service.open_waitable_count() == 0);
  assert(service.stop() == 0u);
  assert(service.terminate(swept_pid, 1).ok());
  assert(wait_gone(service, swept_pid, 4000ms));

  // One grammar for the six priority spellings, shared by service and
  // executor: the export accepts exactly what the executor validates.
  assert(rime::win32::is_valid_priority("belowNormal"));
  assert(rime::win32::is_valid_priority("realtime"));
  assert(!rime::win32::is_valid_priority("BelowNormal"));
  assert(!rime::win32::is_valid_priority("normal "));

  // ProcessSetPriority against a child of this file, read back through
  // Windows rather than through the code under test.
  std::uint32_t priority_pid = 0;
  assert(service.launch({executable, L"--child-sleep 20000", L""}, priority_pid).ok());
  assert(service.set_priority(priority_pid, "idle").ok());
  const HANDLE priority_handle =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, priority_pid);
  assert(priority_handle != nullptr);
  assert(GetPriorityClass(priority_handle) == IDLE_PRIORITY_CLASS);
  assert(CloseHandle(priority_handle));
  assert(service.set_priority(priority_pid, "not-a-priority").code ==
         rime::core::Error::Code::InvalidContract);
  assert(service.set_priority(0, "idle").code == rime::core::Error::Code::InvalidContract);
  assert(service.set_priority(absent_pid, "idle").code ==
         rime::core::Error::Code::ExecutionFailed);
  assert(service.terminate(priority_pid, 1).ok());
  assert(wait_gone(service, priority_pid, 4000ms));

  // RunAs: argument-only refusals that never reach CreateProcessWithLogonW
  // (empty user, then empty executable, then an executable with a quote).
  rime::win32::RunAsSpec run_as_spec;
  std::uint32_t run_as_pid = 0;
  assert(service.run_as(run_as_spec, run_as_pid).code == rime::core::Error::Code::InvalidContract);
  run_as_spec.user = L"tester";
  assert(service.run_as(run_as_spec, run_as_pid).code == rime::core::Error::Code::InvalidContract);
  run_as_spec.executable = L"bad\"name";
  assert(service.run_as(run_as_spec, run_as_pid).code == rime::core::Error::Code::InvalidContract);

  // Shutdown: only refusals. Every call below carries at least one illegal
  // field, and each is rejected by a contract check that runs before the
  // privilege is enabled and before any shutdown API is named. The invariant
  // the block relies on: mode shutdown/reboot only ever appears with
  // timeoutSec 601 (out of bound), and every other mode only with a non-zero
  // timeoutSec - so no path here can log off, reboot, power off or suspend
  // this host.
  rime::win32::ShutdownSpec shutdown_spec;
  shutdown_spec.mode = "bogus";
  assert(service.shutdown_system(shutdown_spec).code == rime::core::Error::Code::InvalidContract);
  shutdown_spec.mode = "";
  assert(service.shutdown_system(shutdown_spec).code == rime::core::Error::Code::InvalidContract);
  shutdown_spec.mode = "logoff";
  shutdown_spec.timeout_sec = 10;
  assert(service.shutdown_system(shutdown_spec).code == rime::core::Error::Code::InvalidContract);
  shutdown_spec.mode = "reboot";
  shutdown_spec.timeout_sec = 601;
  assert(service.shutdown_system(shutdown_spec).code == rime::core::Error::Code::InvalidContract);
  shutdown_spec.mode = "hibernate";
  shutdown_spec.timeout_sec = 1;
  assert(service.shutdown_system(shutdown_spec).code == rime::core::Error::Code::InvalidContract);
  shutdown_spec.timeout_sec = 0;
  shutdown_spec.force = true;
  assert(service.shutdown_system(shutdown_spec).code == rime::core::Error::Code::InvalidContract);
  shutdown_spec.mode = "poweroff";
  shutdown_spec.timeout_sec = 600;
  assert(service.shutdown_system(shutdown_spec).code == rime::core::Error::Code::InvalidContract);

  // The three new action types through the kernel: capability gate, payload
  // contract, the one safe success (set.priority on a child of this file)
  // and the Trace entry it writes.
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel wp6(
      std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{
          "process.launch", "process.terminate", "process.manage", "process.runas",
          "process.shutdown"}),
      trace);
  const auto wp6_executor = std::make_shared<rime::win32::ProcessExecutor>(service);
  assert(wp6.register_executor("process.launch", wp6_executor).ok());
  assert(wp6.register_executor("process.terminate", wp6_executor).ok());
  assert(wp6.register_executor("process.set.priority", wp6_executor).ok());
  assert(wp6.register_executor("process.runas", wp6_executor).ok());
  assert(wp6.register_executor("process.shutdown", wp6_executor).ok());

  std::uint32_t kernel_pid = 0;
  assert(service.launch({executable, L"--child-sleep 20000", L""}, kernel_pid).ok());

  rime::action::Action set_priority;
  set_priority.id = 6;
  set_priority.source = {"test", "process_tests"};
  set_priority.type = "process.set.priority";
  set_priority.capability = "process.manage";
  set_priority.target = {"process", std::to_string(kernel_pid)};
  set_priority.deadline_unix_ms = deadline_ms();
  rime::core::json::Value priority_payload = rime::core::json::Value::object();
  priority_payload.set("pid", rime::core::json::Value::number(static_cast<double>(kernel_pid)));
  priority_payload.set("priority", rime::core::json::Value::string("belowNormal"));
  set_priority.payload = rime::core::json::stringify(priority_payload);
  const auto priority_result = wp6.execute(set_priority);
  assert(priority_result.succeeded);
  assert(priority_result.detail == "priority updated");
  const HANDLE kernel_priority_handle =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, kernel_pid);
  assert(kernel_priority_handle != nullptr);
  assert(GetPriorityClass(kernel_priority_handle) == BELOW_NORMAL_PRIORITY_CLASS);
  assert(CloseHandle(kernel_priority_handle));

  bool saw_started = false;
  bool saw_finished = false;
  for (const auto& entry : trace->snapshot()) {
    if (entry.subject != "process.set.priority") continue;
    if (entry.kind == rime::core::TraceKind::ActionStarted) saw_started = true;
    if (entry.kind == rime::core::TraceKind::ActionFinished) {
      saw_finished = true;
      assert(entry.action_id == 6);
      assert(entry.capability == "process.manage");
      assert(entry.result_code == "none");
      // The kernel writes a fixed envelope detail ("succeeded"); the
      // executor's own detail ("priority updated") is on the Result, not in
      // the Trace, which is exactly why no payload can leak through it.
      assert(entry.detail == "succeeded");
    }
  }
  assert(saw_started);
  assert(saw_finished);

  // set.priority payload contract: target/payload pid disagreement, an
  // unknown spelling, a missing priority and a non-positive pid all reject
  // before the process is opened.
  rime::action::Action mismatched = set_priority;
  mismatched.id = 7;
  mismatched.target = {"process", std::to_string(kernel_pid + 1)};
  const auto mismatched_result = wp6.execute(mismatched);
  assert(!mismatched_result.succeeded);
  assert(mismatched_result.error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action unknown_priority = set_priority;
  unknown_priority.id = 8;
  rime::core::json::Value ultra_payload = rime::core::json::Value::object();
  ultra_payload.set("pid", rime::core::json::Value::number(static_cast<double>(kernel_pid)));
  ultra_payload.set("priority", rime::core::json::Value::string("ultra"));
  unknown_priority.payload = rime::core::json::stringify(ultra_payload);
  const auto unknown_priority_result = wp6.execute(unknown_priority);
  assert(!unknown_priority_result.succeeded);
  assert(unknown_priority_result.error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action missing_priority = set_priority;
  missing_priority.id = 9;
  rime::core::json::Value pid_only_payload = rime::core::json::Value::object();
  pid_only_payload.set("pid", rime::core::json::Value::number(static_cast<double>(kernel_pid)));
  missing_priority.payload = rime::core::json::stringify(pid_only_payload);
  const auto missing_priority_result = wp6.execute(missing_priority);
  assert(!missing_priority_result.succeeded);
  assert(missing_priority_result.error.code == rime::core::Error::Code::InvalidContract);

  rime::action::Action zero_pid = set_priority;
  zero_pid.id = 10;
  rime::core::json::Value zero_payload = rime::core::json::Value::object();
  zero_payload.set("pid", rime::core::json::Value::number(0));
  zero_payload.set("priority", rime::core::json::Value::string("idle"));
  zero_pid.payload = rime::core::json::stringify(zero_payload);
  const auto zero_pid_result = wp6.execute(zero_pid);
  assert(!zero_pid_result.succeeded);
  assert(zero_pid_result.error.code == rime::core::Error::Code::InvalidContract);

  // RunAs refusals. Invariant of this block: at least one field of every
  // payload below is invalid, so none of them can reach a logon attempt.
  rime::action::Action run_as_action;
  run_as_action.id = 11;
  run_as_action.source = {"test", "process_tests"};
  run_as_action.type = "process.runas";
  run_as_action.capability = "process.runas";
  run_as_action.target = {"process", "new"};
  run_as_action.deadline_unix_ms = deadline_ms();
  const std::string secret = "s3cr3t-not-in-trace";
  rime::core::json::Value run_as_payload = rime::core::json::Value::object();
  run_as_payload.set("user", rime::core::json::Value::string("tester"));
  run_as_payload.set("password", rime::core::json::Value::string(secret));
  run_as_payload.set("executable", rime::core::json::Value::string(""));
  run_as_action.payload = rime::core::json::stringify(run_as_payload);
  const auto run_as_result = wp6.execute(run_as_action);
  assert(!run_as_result.succeeded);
  assert(run_as_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(run_as_result.error.message.find(secret) == std::string::npos);

  rime::action::Action run_as_target = run_as_action;
  run_as_target.id = 12;
  run_as_target.target = {"process", std::to_string(kernel_pid)};
  const auto run_as_target_result = wp6.execute(run_as_target);
  assert(!run_as_target_result.succeeded);
  assert(run_as_target_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(run_as_target_result.error.message.find("must be 'new'") != std::string::npos);

  rime::action::Action run_as_no_password = run_as_action;
  run_as_no_password.id = 13;
  rime::core::json::Value no_password_payload = rime::core::json::Value::object();
  no_password_payload.set("user", rime::core::json::Value::string("tester"));
  no_password_payload.set("executable", rime::core::json::Value::string("x.exe"));
  run_as_no_password.payload = rime::core::json::stringify(no_password_payload);
  const auto no_password_result = wp6.execute(run_as_no_password);
  assert(!no_password_result.succeeded);
  assert(no_password_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(no_password_result.error.message.find("password") != std::string::npos);

  rime::action::Action run_as_empty_user = run_as_action;
  run_as_empty_user.id = 14;
  rime::core::json::Value empty_user_payload = rime::core::json::Value::object();
  empty_user_payload.set("user", rime::core::json::Value::string(""));
  empty_user_payload.set("password", rime::core::json::Value::string(secret));
  empty_user_payload.set("executable", rime::core::json::Value::string("x.exe"));
  run_as_empty_user.payload = rime::core::json::stringify(empty_user_payload);
  const auto empty_user_result = wp6.execute(run_as_empty_user);
  assert(!empty_user_result.succeeded);
  assert(empty_user_result.error.code == rime::core::Error::Code::InvalidContract);

  // No Trace entry - started or finished, any subject - may carry the
  // credential that is sitting in those payloads.
  for (const auto& entry : trace->snapshot()) {
    assert(entry.detail.find(secret) == std::string::npos);
    assert(entry.subject.find(secret) == std::string::npos);
    assert(entry.capability.find(secret) == std::string::npos);
  }

  // Shutdown refusals through the kernel: a bad target, a bad mode, an
  // out-of-bound timeout, a non-boolean force and a fractional timeoutSec.
  rime::action::Action shutdown_action;
  shutdown_action.id = 15;
  shutdown_action.source = {"test", "process_tests"};
  shutdown_action.type = "process.shutdown";
  shutdown_action.capability = "process.shutdown";
  shutdown_action.deadline_unix_ms = deadline_ms();

  shutdown_action.target = {"process", "new"};
  shutdown_action.payload = R"({"mode":"shutdown"})";
  const auto shutdown_target_result = wp6.execute(shutdown_action);
  assert(!shutdown_target_result.succeeded);
  assert(shutdown_target_result.error.code == rime::core::Error::Code::InvalidContract);

  shutdown_action.id = 16;
  shutdown_action.target = {"process", "system"};
  shutdown_action.payload = R"({"mode":"bogus"})";
  const auto shutdown_mode_result = wp6.execute(shutdown_action);
  assert(!shutdown_mode_result.succeeded);
  assert(shutdown_mode_result.error.code == rime::core::Error::Code::InvalidContract);

  shutdown_action.id = 17;
  // "shutdown" with an out-of-bound timeoutSec: the bound is checked before
  // the service (and therefore before any shutdown API) is reached.
  shutdown_action.payload = R"({"mode":"shutdown","timeoutSec":601})";
  const auto shutdown_timeout_result = wp6.execute(shutdown_action);
  assert(!shutdown_timeout_result.succeeded);
  assert(shutdown_timeout_result.error.code == rime::core::Error::Code::InvalidContract);

  shutdown_action.id = 18;
  shutdown_action.payload = R"({"mode":"reboot","force":"yes"})";
  const auto shutdown_force_result = wp6.execute(shutdown_action);
  assert(!shutdown_force_result.succeeded);
  assert(shutdown_force_result.error.code == rime::core::Error::Code::InvalidContract);

  shutdown_action.id = 19;
  shutdown_action.payload = R"({"mode":"hibernate","timeoutSec":1.5})";
  const auto shutdown_fraction_result = wp6.execute(shutdown_action);
  assert(!shutdown_fraction_result.succeeded);
  assert(shutdown_fraction_result.error.code == rime::core::Error::Code::InvalidContract);

  shutdown_action.id = 20;
  // "logoff" with a grace period the service refuses: reaches the service,
  // still no OS call, same contract error.
  shutdown_action.payload = R"({"mode":"logoff","timeoutSec":10})";
  const auto shutdown_service_result = wp6.execute(shutdown_action);
  assert(!shutdown_service_result.succeeded);
  assert(shutdown_service_result.error.code == rime::core::Error::Code::InvalidContract);

  // A policy that grants none of the new capabilities denies every type
  // before its executor runs, so none of these three calls reaches the OS.
  rime::action::Kernel no_capabilities(
      std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{}));
  assert(no_capabilities.register_executor("process.set.priority", wp6_executor).ok());
  assert(no_capabilities.register_executor("process.runas", wp6_executor).ok());
  assert(no_capabilities.register_executor("process.shutdown", wp6_executor).ok());
  assert(no_capabilities.execute(set_priority).error.code ==
         rime::core::Error::Code::CapabilityDenied);
  assert(no_capabilities.execute(run_as_action).error.code ==
         rime::core::Error::Code::CapabilityDenied);
  assert(no_capabilities.execute(shutdown_action).error.code ==
         rime::core::Error::Code::CapabilityDenied);

  assert(service.terminate(kernel_pid, 1).ok());
  assert(wait_gone(service, kernel_pid, 4000ms));

  // Nothing this file opened is still registered when it ends.
  assert(service.open_waitable_count() == 0);

  return 0;
}
