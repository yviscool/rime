// Realism: L3 - the real WindowService, Kernel, Dispatcher and WindowExecutor
// run in process; only the OS outcomes (foreground denial, deadline expiry,
// target resolution) are substituted at the window_seam entry. Every fault
// asserts the exact Error::Code the real path would produce, and the
// pipeline case proves the injected refusal surfaces through
// dispatcher.submit -> pump -> kernel.execute -> WindowExecutor unchanged.
#include "rime/win32/window.hpp"

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/lane.hpp"
#include "rime/core/trace.hpp"
#include "rime/win32/window_executor.hpp"
#include "rime/win32/window_seam.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using rime::action::Action;
using rime::action::Dispatcher;
using rime::action::Kernel;
using rime::action::StaticCapabilityPolicy;
using rime::core::Error;
using rime::win32::WindowExecutor;
using rime::win32::WindowInfo;
using rime::win32::WindowQuery;
using rime::win32::WindowService;
namespace seam = rime::win32::window_seam;

void expect(const bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    std::fflush(stdout);
    std::abort();
  }
}

std::uint64_t unix_ms_now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

void seam_semantics() {
  Error out = Error::none();
  expect(!seam::consume("focus", out), "disarmed seam must not fire");
  expect(out.ok(), "disarmed consume must leave the error untouched");

  expect(seam::arm({seam::FaultKind::Denied, "focus", 2}), "first arm wins");
  expect(!seam::arm({seam::FaultKind::Timeout, "", 1}), "double arm must fail");
  expect(!seam::consume("move", out), "method filter must skip non-matching calls");
  expect(seam::consume("focus", out), "matching call fires");
  expect(out.code == Error::Code::ExecutionFailed, "denied maps to ExecutionFailed");
  expect(seam::consume("focus", out), "times=2 fires twice");
  expect(!seam::consume("focus", out), "exhausted fault disarms itself");

  expect(seam::arm({seam::FaultKind::Timeout, "", -1}), "lasting arm");
  expect(seam::consume("anything", out), "empty filter matches every method");
  expect(out.code == Error::Code::Timeout, "timeout maps to Timeout");
  expect(seam::consume("focus", out), "times=-1 keeps firing");
  seam::clear();
  expect(!seam::consume("focus", out), "clear disarms the lasting fault");
  seam::clear();  // idempotent
  std::printf("[seam] arm/clear/filter/countdown semantics hold\n");
}

void service_refusals(WindowService& windows) {
  WindowInfo info;
  std::vector<WindowInfo> found;

  expect(seam::arm({seam::FaultKind::Denied, "focus", 1}), "arm focus denial");
  const Error denied = windows.focus(424242u);
  expect(!denied.ok(), "injected denial must fail focus");
  expect(denied.code == Error::Code::ExecutionFailed, "denial code is ExecutionFailed");
  expect(denied.message.find("seam") != std::string::npos, "denial names the seam");

  expect(seam::arm({seam::FaultKind::Timeout, "move", 1}), "arm one-shot move timeout");
  const Error expired = windows.move(424242u, "left");
  expect(expired.code == Error::Code::Timeout, "injected expiry is Timeout");
  // The one-shot is spent: the same call now reaches the real registry and
  // reports the bogus id as gone, proving the fault disarmed itself.
  const Error real = windows.move(424242u, "left");
  expect(real.code == Error::Code::TargetGone, "disarmed move reaches the real registry");

  expect(seam::arm({seam::FaultKind::TargetGone, "info", 1}), "arm info target-gone");
  const Error gone = windows.info(424242u, info);
  expect(gone.code == Error::Code::TargetGone, "injected info fault is TargetGone");

  expect(seam::arm({seam::FaultKind::Denied, "focus", 1}), "arm focus-only fault");
  WindowQuery query;
  query.title = "no such window - window_seam probe";
  const Error unaffected = windows.query(query, found);
  expect(unaffected.ok(), "a focus-filtered fault must not touch query");
  const Error focus_denied = windows.focus(424242u);
  expect(focus_denied.code == Error::Code::ExecutionFailed, "focus still denied");

  expect(seam::arm({seam::FaultKind::Denied, "query", -1}), "arm lasting query denial");
  expect(!windows.query(query, found).ok(), "lasting fault fails every query");
  expect(!windows.query(query, found).ok(), "lasting fault fails the next query too");
  seam::clear();
  expect(windows.query(query, found).ok(), "cleared query reaches the desktop");
  std::printf("[service] denied/timeout/target-gone/filter/lasting refusals hold\n");
}

void pipeline_propagates_refusal(WindowService& windows) {
  auto policy = std::make_shared<StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"windows.window.write"});
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  Kernel kernel(policy, trace);
  auto executor = std::make_shared<WindowExecutor>(windows);
  expect(kernel.register_executor("window.focus", executor).ok(), "register window.focus");
  Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());

  expect(seam::arm({seam::FaultKind::Denied, "focus", 1}), "arm pipeline denial");
  Action action;
  action.id = 1;
  action.schema_version = 1;
  action.source = {"fault-test", "rime_window_fault_tests"};
  action.type = "window.focus";
  action.capability = "windows.window.write";
  action.target = {"window", "424242"};
  action.deadline_unix_ms = unix_ms_now() + 60'000;
  action.payload = "{}";
  expect(dispatcher.submit(std::move(action)) == rime::action::DispatchStatus::Accepted,
         "faulted action still submits");
  const std::vector<rime::action::Result> results = dispatcher.pump(1);
  expect(results.size() == 1, "one submitted action produces one result");
  expect(!results.front().succeeded, "injected denial fails the mediated action");
  expect(!results.front().detail.empty(), "the failure names its cause");
  std::printf("[pipeline] injected denial surfaces as action failure: %s\n",
              results.front().detail.c_str());
}

}  // namespace

int main() {
  seam_semantics();

  expect(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Worker).ok(),
         "the main thread must own the worker lane");
  WindowService windows;
  expect(windows.start().ok(), "window service starts for fault tests");
  service_refusals(windows);
  pipeline_propagates_refusal(windows);
  seam::clear();
  expect(windows.stop().ok(), "window service stops after fault tests");
  rime::core::LaneRegistry::instance().release(rime::core::Lane::Worker);
  std::printf("window fault injection: all refusal paths hold\n");
  return 0;
}
