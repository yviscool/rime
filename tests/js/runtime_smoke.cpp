#include "rime/js/abi.hpp"
#include "rime/js/runtime.hpp"

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

void test_threaded_runtime() {
  rime::js::Runtime runtime;
  assert(runtime.start().ok());

  // delay resolves; settle only returns once the promise chain completed.
  auto delay_task = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "runtime.delay(30, 'ready').then(v => { globalThis.result = v; });",
      "delay.js");
  assert(delay_task.get().ok());
  assert(runtime.settle(2000ms).ok());
  auto check = runtime.evaluate_module(
      "if (globalThis.result !== 'ready') throw new Error('settle returned early');",
      "check.js");
  assert(check.get().ok());

  // cancellation rejects and disarms the timer.
  auto cancel_task = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "const id = runtime.cancellation();\n"
      "globalThis.cancelOutcome = 'pending';\n"
      "runtime.delay(5000, 1, id).then(\n"
      "  () => { globalThis.cancelOutcome = 'resolved'; },\n"
      "  () => { globalThis.cancelOutcome = 'cancelled'; });\n"
      "runtime.cancel(id);",
      "cancel.js");
  assert(cancel_task.get().ok());
  assert(runtime.settle(2000ms).ok());
  auto cancel_check = runtime.evaluate_module(
      "if (globalThis.cancelOutcome !== 'cancelled') throw new Error('cancel did not reject');",
      "cancel_check.js");
  assert(cancel_check.get().ok());

  // inspect reads live state from the JS thread without executing script.
  const std::string report = runtime.inspect(R"({"kind":"modules"})").get();
  assert(report.find("\"rime:runtime\"") != std::string::npos);

  // settle reports a timeout when a timer outlasts the budget.
  auto slow = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "runtime.delay(3000, 1);",
      "slow.js");
  assert(slow.get().ok());
  assert(!runtime.settle(100ms).ok());

  // stop is repeatable and drains to Stopped even with a pending timer.
  assert(runtime.stop().ok());
  assert(runtime.stop().ok());
  assert(runtime.state() == rime::js::RuntimeState::Stopped);
}

void test_context() {
  rime::js::Runtime runtime;
  assert(runtime.start().ok());

  // runtime.context is a read-only snapshot: fresh per call, schema'd,
  // listing this host's registered modules and live ownership counts.
  auto context_task = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "const before = runtime.context();\n"
      "if (before.schemaVersion !== 1) throw new Error('bad context version');\n"
      "if (!before.modules.includes('rime:runtime')) throw new Error('self not listed');\n"
      "if (typeof before.tasks.async !== 'number' ||\n"
      "    typeof before.tasks.timers !== 'number' ||\n"
      "    typeof before.subscriptions !== 'number')\n"
      "  throw new Error('bad context shape');\n"
      "before.schemaVersion = 99;\n"
      "if (runtime.context().schemaVersion !== 1)\n"
      "  throw new Error('context must be read-only');\n"
      "const base = runtime.context().cancellations;\n"
      "const id = runtime.cancellation();\n"
      "if (runtime.context().cancellations !== base + 1)\n"
      "  throw new Error('cancellation not counted');\n"
      "if (!runtime.releaseCancellation(id)) throw new Error('release failed');\n"
      "if (runtime.context().cancellations !== base)\n"
      "  throw new Error('cancellation not released');",
      "context.js");
  assert(context_task.get().ok());

  assert(runtime.stop().ok());
}

void test_busy_loop_interrupt() {
  rime::js::Runtime runtime;
  assert(runtime.start().ok());
  auto busy = runtime.evaluate_module("while (true) { }", "busy.js");
  std::this_thread::sleep_for(100ms);
  assert(runtime.stop().ok());
  assert(!busy.get().ok());
  assert(runtime.state() == rime::js::RuntimeState::Stopped);
}

void test_host_abi() {
  rime::js::HostAbi abi;
  assert(abi.abi_version() == rime::js::k_host_abi_version);
  assert(abi.state() == rime::js::HostAbiState::Created);
  assert(abi.exit_code() == 0);

  int exit_code = -1;
  abi.set_exit_handler([&](int code) { exit_code = code; });

  // load stages, execute evaluates, and execute repeats while loaded.
  assert(abi.load("globalThis.booted = 1;", "boot.js").ok());
  assert(abi.state() == rime::js::HostAbiState::Loaded);
  assert(abi.execute().ok());
  assert(abi.state() == rime::js::HostAbiState::Executed);
  assert(abi.execute().ok());

  // The state machine refuses a second load.
  const auto second_load = abi.load("globalThis.other = 1;", "other.js");
  assert(!second_load.ok());
  assert(second_load.code == rime::core::Error::Code::InvalidState);

  // exit records the code and fires the handler.
  abi.exit(3);
  assert(exit_code == 3);
  assert(abi.exit_code() == 3);

  // Execution failure fires on_error with location and moves to failed.
  rime::js::HostAbi failing;
  assert(failing.load("throw new Error('abi boom');", "fail.js").ok());
  std::string fail_where;
  std::string fail_message;
  failing.set_error_handler([&](std::string_view where, std::string_view message) {
    fail_where = where;
    fail_message = message;
  });
  assert(!failing.execute().ok());
  assert(failing.state() == rime::js::HostAbiState::Failed);
  assert(fail_where == "fail.js");
  assert(fail_message.find("abi boom") != std::string::npos);
  assert(!failing.execute().ok());  // failed is terminal for execute

  // unload must fail while a subscription is outstanding, explaining why.
  rime::js::HostAbi busy_abi;
  assert(busy_abi.load("import { runtime } from 'rime:runtime';\n"
                       "globalThis.sid = runtime.subscribe(() => {});",
                       "subscribe.mjs")
             .ok());
  assert(busy_abi.execute().ok());
  const auto busy_unload = busy_abi.unload();
  assert(!busy_unload.ok());
  assert(busy_unload.code == rime::core::Error::Code::InvalidState);
  assert(busy_unload.message.find("subscription") != std::string::npos);
  assert(busy_abi.state() == rime::js::HostAbiState::Executed);

  // Release the subscription, then unload succeeds and repeats cleanly.
  assert(busy_abi.execute("import { runtime } from 'rime:runtime';\n"
                          "runtime.unsubscribe(globalThis.sid);",
                          "release.mjs")
             .ok());
  assert(busy_abi.unload().ok());
  assert(busy_abi.state() == rime::js::HostAbiState::Unloaded);
  assert(busy_abi.unload().ok());
  const auto dead = busy_abi.execute();
  assert(!dead.ok());
  assert(dead.code == rime::core::Error::Code::InvalidState);

  // Unresolved promises and armed timers block unload with their own named
  // reasons, so an embedder can see what to wait for; cancelling the work
  // clears both and unload proceeds.
  rime::js::HostAbi pending_abi;
  assert(pending_abi.load("import { runtime } from 'rime:runtime';\n"
                          "const id = runtime.cancellation();\n"
                          "globalThis.cid = id;\n"
                          "globalThis.pending = runtime.delay(5000, 1, id);",
                          "pending.mjs")
             .ok());
  assert(pending_abi.execute().ok());
  const auto pending_unload = pending_abi.unload();
  assert(!pending_unload.ok());
  assert(pending_unload.code == rime::core::Error::Code::InvalidState);
  assert(pending_unload.message.find("unresolved promise") != std::string::npos);
  assert(pending_unload.message.find("armed timer") != std::string::npos);
  assert(pending_abi.execute("import { runtime } from 'rime:runtime';\n"
                             "runtime.cancel(globalThis.cid);",
                             "cancel-pending.mjs")
             .ok());
  assert(pending_abi.unload().ok());
  assert(pending_abi.state() == rime::js::HostAbiState::Unloaded);

  // Empty sources are a contract violation, not a state error.
  rime::js::HostAbi empty;
  const auto empty_load = empty.load("", "empty.js");
  assert(!empty_load.ok());
  assert(empty_load.code == rime::core::Error::Code::InvalidContract);
}

}  // namespace

int main() {
  test_threaded_runtime();
  test_context();
  test_busy_loop_interrupt();
  test_host_abi();
  return 0;
}
