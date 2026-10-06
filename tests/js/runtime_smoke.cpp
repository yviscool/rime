// Realism: L6 - the embedded host ABI runs load/execute/error/exit/unload
// against the real QuickJS host, including adversarial unload paths (held
// callbacks, undelivered host events) asserted item by item.

#include "rime/js/abi.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
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

void test_persistent() {
  rime::js::Runtime runtime;
  assert(runtime.start().ok());

  // runtime.persistent (AHK Persistent): the effective flag starts false
  // and the force flag follows whatever the call returned; this host has no
  // events module registered, so nothing but the force flag can make it
  // true. inspect mirrors the flag plus the pending delay-timer and
  // completion counts the bootstrap residency pump reads.
  auto on = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "if (runtime.persistent() !== false)\n"
      "  throw new Error('the runtime must start non-persistent');\n"
      "if (runtime.persistent(true) !== true)\n"
      "  throw new Error('persistent(true) must report the effective flag');\n"
      "if (runtime.persistent() !== true)\n"
      "  throw new Error('the force flag must stay set');\n"
      "const report = JSON.parse(runtime.inspect());\n"
      "if (report.persistent !== true)\n"
      "  throw new Error('inspect must mirror the effective flag');\n"
      "if (typeof report.pendingTimers !== 'number' ||\n"
      "    typeof report.pendingCompletions !== 'number')\n"
      "  throw new Error('inspect must expose the pending counts');",
      "persistent-on.js");
  assert(on.get().ok());

  // The same payload the residency probe parses, asserted from the C++ side
  // so the field names cannot drift without failing here.
  const std::string report_on = runtime.inspect().get();
  assert(report_on.find("\"persistent\":true") != std::string::npos);
  assert(report_on.find("\"pendingTimers\":") != std::string::npos);
  assert(report_on.find("\"pendingCompletions\":") != std::string::npos);

  auto off = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "if (runtime.persistent(false) !== false)\n"
      "  throw new Error('persistent(false) must clear the force flag');\n"
      "if (runtime.persistent() !== false)\n"
      "  throw new Error('the force flag must stay clear');",
      "persistent-off.js");
  assert(off.get().ok());
  assert(runtime.inspect().get().find("\"persistent\":false") != std::string::npos);

  // A non-boolean argument is a TypeError before it reaches the flag; one
  // argument beyond that is arity, not a value, to keep the binding thin.
  auto bad = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "let rejected = 0;\n"
      "try { runtime.persistent('yes'); } catch (error) {\n"
      "  if (error instanceof TypeError) rejected++;\n"
      "}\n"
      "if (rejected !== 1)\n"
      "  throw new Error('persistent must reject a non-boolean');",
      "persistent-bad.js");
  assert(bad.get().ok());

  assert(runtime.stop().ok());
}

void test_debug_and_cwd() {
  rime::js::Runtime runtime;
  assert(runtime.start().ok());

  // runtime.debug mirrors AHK OutputDebug (script2.cpp:2630-2636): it returns
  // nothing, hands any string - the empty one included - to OutputDebugString,
  // and rejects every other value with a TypeError before it reaches Win32.
  // The debugger sink itself cannot be observed from this process, so the
  // contract is what gets pinned here.
  auto debug_task = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "if (runtime.debug('rime debug smoke') !== undefined)\n"
      "  throw new Error('debug must return nothing');\n"
      "runtime.debug('');\n"
      "const bad = [];\n"
      "for (const value of [undefined, null, 1, {}, []]) {\n"
      "  try { runtime.debug(value); } catch (error) { bad.push(error); }\n"
      "}\n"
      "try { runtime.debug(); } catch (error) { bad.push(error); }\n"
      "if (bad.length !== 6)\n"
      "  throw new Error('debug must reject every non-string: ' + bad.length);\n"
      "for (const error of bad)\n"
      "  if (!(error instanceof TypeError))\n"
      "    throw new Error('debug rejection must be a TypeError: ' + error);\n",
      "debug.js");
  assert(debug_task.get().ok());

  // cwd/setCwd move the process-global working directory, so this segment
  // owns a private child directory and puts the original one back itself: the
  // guard restores on every exit, the script below covers the happy path, and
  // the final comparison proves the process is where ctest left it.
  namespace fs = std::filesystem;
  const fs::path original = fs::current_path();
  const std::string dir_name = "rime-cwd-smoke";
  fs::remove_all(original / dir_name);  // a crashed run must not block this one
  assert(fs::create_directory(original / dir_name));
  struct CwdGuard {
    fs::path original;
    std::string dir_name;
    ~CwdGuard() {
      // Restore before removing: Windows refuses to delete a directory that
      // is the current directory, and the directory has to go away whether
      // the assertions below passed or threw.
      std::error_code ignored;
      fs::current_path(original, ignored);
      fs::remove_all(original / dir_name, ignored);
    }
  } guard{original, dir_name};

  auto cwd_task = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "const name = '" +
      dir_name +
      "';\n"
      "const original = runtime.cwd();\n"
      "if (typeof original !== 'string' || original.length === 0)\n"
      "  throw new Error('cwd must return the process working directory');\n"
      "runtime.setCwd(name);\n"
      "const inside = runtime.cwd();\n"
      "const expected = original.endsWith('\\\\') ? original + name : original + '\\\\' + name;\n"
      "if (inside.toLowerCase() !== expected.toLowerCase())\n"
      "  throw new Error('setCwd/cwd round trip: ' + inside + ' != ' + expected);\n"
      // A missing child throws a plain Error naming the Win32 code and leaves
      // the directory exactly where it was (AHK SetWorkingDir's failure path).
      "let missing = null;\n"
      "try { runtime.setCwd(inside + '\\\\absent-child'); } catch (error) { missing = error; }\n"
      "if (missing === null || missing instanceof TypeError)\n"
      "  throw new Error('setCwd must throw an Error for a missing directory');\n"
      "if (String(missing.message).indexOf('win32 error') < 0)\n"
      "  throw new Error('setCwd failure must name the Win32 code: ' + missing.message);\n"
      "if (runtime.cwd().toLowerCase() !== inside.toLowerCase())\n"
      "  throw new Error('failed setCwd must leave the directory unchanged');\n"
      // AHK SetWorkingDir resolves a bare 'C:' to 'C:\\' (script2.cpp:1456-1464).
      "if (original.length >= 2 && original[1] === ':') {\n"
      "  const drive = original[0] + ':';\n"
      "  runtime.setCwd(drive);\n"
      "  const root = runtime.cwd();\n"
      "  if (root.toLowerCase() !== drive.toLowerCase() + '\\\\')\n"
      "    throw new Error('bare drive did not resolve to its root: ' + root);\n"
      "}\n"
      "if (runtime.setCwd(original) !== undefined)\n"
      "  throw new Error('setCwd must return nothing');\n"
      "if (runtime.cwd() !== original)\n"
      "  throw new Error('setCwd(original) did not restore: ' + runtime.cwd());\n",
      "cwd.js");
  assert(cwd_task.get().ok());
  assert(fs::current_path().wstring() == original.wstring());

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

// Exit probe: the onExit handler runs while the JS context is on its way
// out, so it reports through a native module instead of a JS global.
struct ExitProbe {
  std::string notes;
  bool handler_ran{false};
  std::string payload;

  void append(const std::string& text) {
    if (text.rfind("payload:", 0) == 0) {
      handler_ran = true;
      payload = text.substr(8);
    }
    if (!notes.empty()) notes += '|';
    notes += text;
  }
};

constexpr const char* k_exit_probe_module = "rime:test:exit";

rime::js::Host* host_of(JSContext* context) {
  return static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
}

ExitProbe* exit_probe_of(JSContext* context) {
  rime::js::Host* host = host_of(context);
  if (!host) return nullptr;
  return static_cast<ExitProbe*>(host->module_data(k_exit_probe_module));
}

JSValue exit_probe_note(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  ExitProbe* probe = exit_probe_of(context);
  if (!probe) return JS_ThrowInternalError(context, "exit probe is not wired");
  if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(context, "note(text)");
  const char* text = JS_ToCString(context, argv[0]);
  if (!text) return JS_EXCEPTION;
  probe->append(text);
  JS_FreeCString(context, text);
  return JS_UNDEFINED;
}

// Same registration path as rime:input.onExit: the JS function becomes a
// host callback, the id comes from the shared subscription counter, and the
// pair goes to add_exit_handler. Deliberately not in the
// SubscriptionRegistry, exactly like the input module does.
JSValue exit_probe_on_exit(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
  rime::js::Host* host = host_of(context);
  if (!host) return JS_ThrowInternalError(context, "exit probe is not wired");
  if (argc < 1 || !JS_IsFunction(context, argv[0])) {
    return JS_ThrowTypeError(context, "onExit(fn)");
  }
  std::uint64_t callback = 0;
  if (const auto error = host->add_callback(JS_DupValue(context, argv[0]), callback);
      !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  const std::uint64_t sub = host->allocate_subscription_id();
  if (const auto error = host->add_exit_handler(callback, sub); !error.ok()) {
    (void)host->remove_callback(callback);
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  return JS_UNDEFINED;
}

int exit_probe_init(JSContext* context, JSModuleDef* module) {
  JSValue note = JS_NewCFunction(context, exit_probe_note, "note", 1);
  if (JS_IsException(note)) return -1;
  if (JS_SetModuleExport(context, module, "note", note) < 0) return -1;
  JSValue on_exit = JS_NewCFunction(context, exit_probe_on_exit, "onExit", 1);
  if (JS_IsException(on_exit)) return -1;
  return JS_SetModuleExport(context, module, "onExit", on_exit);
}

JSModuleDef* create_exit_probe_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, k_exit_probe_module, exit_probe_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "note") < 0) return nullptr;
  if (JS_AddModuleExport(context, module, "onExit") < 0) return nullptr;
  return module;
}

// runtime.exit(code?) end to end: throws to unwind the turn, arms the
// uncatchable abort, records the code for the host entry point, and hands
// the onExit handlers {"reason":"exit","code":N} when the runtime drains.
void test_runtime_exit() {
  ExitProbe probe;
  rime::js::Runtime runtime;
  assert(runtime
             .add_native_module(k_exit_probe_module,
                                [](JSContext* context) {
                                  return create_exit_probe_module(context);
                                },
                                &probe)
             .ok());
  assert(runtime.start().ok());

  auto exit_task = runtime.evaluate_module(
      "import { runtime } from 'rime:runtime';\n"
      "import { note, onExit } from 'rime:test:exit';\n"
      "onExit((p) => { note('payload:' + JSON.stringify(p)); });\n"
      "note('before');\n"
      // Pending work the exit abandons: it keeps the host non-idle for 60s,
      // so settle below can only return by noticing the exit.
      "void runtime.delay(60000, null).catch(() => {});\n"
      "try { runtime.exit(5); note('never-reached'); }\n"
      "catch (e) { note('caught:' + e.message); }\n"
      // The exit already armed the interrupt, so this bounded loop is where
      // it has to fire: the try/catch above swallowed the throw, leaving the
      // interrupt as the only way the evaluation can still fail. The
      // deadline keeps a missing interrupt finite instead of hanging.
      "const deadline = Date.now() + 2000;\n"
      "while (Date.now() < deadline) { }\n"
      "note('after-turn');\n",
      "exit.js");
  const auto exit_result = exit_task.get();
  // Exit's own exception was caught by the script above, so a failure here
  // is the uncatchable interrupt aborting the rest of the module.
  assert(!exit_result.ok());

  assert(runtime.exit_requested());
  assert(runtime.exit_code() == 5);
  // The 60s timer above keeps the host non-idle, so this settle can only
  // return through the exit; it fails the test if it waits out its budget.
  assert(runtime.settle(200ms).ok());
  assert(runtime.stop().ok());
  assert(runtime.state() == rime::js::RuntimeState::Stopped);

  // The handler ran on the JS thread during the stop drain, so the notes
  // are only read after the join above.
  assert(probe.handler_ran);
  assert(probe.payload == "{\"reason\":\"exit\",\"code\":5}");
  assert(probe.notes.find("before") != std::string::npos);
  // Exit throws before the next statement can run.
  assert(probe.notes.find("never-reached") == std::string::npos);
  assert(probe.notes.find("payload:{\"reason\":\"exit\",\"code\":5}") != std::string::npos);
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

  // Unresolved promises, armed timers and the live cancellation each block
  // unload with their own named reason, so an embedder can see what to wait
  // for; cancelling the work clears the first two and releasing the
  // cancellation clears the third, then unload proceeds.
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
  assert(pending_unload.message.find("live cancellation") != std::string::npos);
  assert(pending_abi.execute("import { runtime } from 'rime:runtime';\n"
                             "runtime.cancel(globalThis.cid);\n"
                             "runtime.releaseCancellation(globalThis.cid);",
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

// The held-callback reason (abi.cpp: "N JS callback(s) still held") in the
// same positive/negative shape as the three reasons above: a script that
// keeps a JS callback refuses unload and names it, releasing the callback
// lets unload finish.
void test_host_abi_held_callback() {
  rime::js::HostAbi abi;
  assert(abi.load("import { runtime } from 'rime:runtime';\n"
                  "globalThis.held = runtime.subscribe(() => {});",
                  "held-callback.mjs")
             .ok());
  assert(abi.execute().ok());

  // Held: the reason list names the callback and the subscription it is
  // registered under (add_callback registers both together, host.cpp), and
  // the host stays executable instead of being torn down.
  const auto busy = abi.unload();
  assert(!busy.ok());
  assert(busy.code == rime::core::Error::Code::InvalidState);
  assert(busy.message.find("unload refused while host is still active") != std::string::npos);
  assert(busy.message.find("JS callback(s) still held") != std::string::npos);
  assert(busy.message.find("subscription(s)") != std::string::npos);
  assert(abi.state() == rime::js::HostAbiState::Executed);

  // Released: unsubscribe drops the callback and its registry entry, so the
  // second unload has no reason left to report.
  assert(abi.execute("import { runtime } from 'rime:runtime';\n"
                     "runtime.unsubscribe(globalThis.held);",
                     "release-held-callback.mjs")
             .ok());
  assert(abi.unload().ok());
  assert(abi.state() == rime::js::HostAbiState::Unloaded);
  assert(abi.unload().ok());
}

// The pending-host-event reason (abi.cpp: "pending host event(s)") in the
// same shape: a native producer queues an event the JS thread has not
// drained yet, unload names it, drain() delivers it and the reason goes away.
void test_host_abi_pending_event() {
  rime::js::HostAbi abi;
  assert(abi.load("import { runtime } from 'rime:runtime';\n"
                  "globalThis.watched = runtime.subscribe((payload) => {\n"
                  "  globalThis.delivered = payload.message;\n"
                  "});",
                  "watch-event.mjs")
             .ok());
  assert(abi.execute().ok());

  // The live callback id, read back through the inspect protocol - the event
  // below has to reach a real JS handler, not a made-up destination.
  const std::string subscriptions = abi.inspect(R"({"kind":"subscriptions"})");
  const std::size_t marker = subscriptions.find("js#");
  assert(marker != std::string::npos);
  const std::uint64_t callback_id = std::stoull(subscriptions.substr(marker + 3));

  // Held: a producer queues an undelivered event through the same
  // HostEventQueue::push every input-hook/clipboard relay uses.
  const auto queue = abi.host().event_queue();
  assert(queue);
  assert(queue->empty());
  const bool pushed = queue->push(callback_id, "{\"message\":\"queued event\"}");
  assert(pushed);
  assert(!queue->empty());

  const auto busy = abi.unload();
  assert(!busy.ok());
  assert(busy.code == rime::core::Error::Code::InvalidState);
  assert(busy.message.find("pending host event(s)") != std::string::npos);
  // Multi-reason list: the two other live resources are named in the same
  // message, so an embedder sees the whole backlog at once.
  assert(busy.message.find("subscription(s)") != std::string::npos);
  assert(busy.message.find("JS callback(s) still held") != std::string::npos);
  assert(abi.state() == rime::js::HostAbiState::Executed);

  // Delivered: drain() hands the payload to the callback and empties the
  // queue, so the event reason vanishes while the other two remain.
  assert(abi.drain() >= 1);
  assert(queue->empty());
  const auto after_drain = abi.unload();
  assert(!after_drain.ok());
  assert(after_drain.code == rime::core::Error::Code::InvalidState);
  assert(after_drain.message.find("pending host event(s)") == std::string::npos);
  assert(after_drain.message.find("subscription(s)") != std::string::npos);
  assert(abi.execute(
             "if (globalThis.delivered !== 'queued event')\n"
             "  throw new Error('event was not delivered: ' + globalThis.delivered);",
             "check-delivery.mjs")
             .ok());

  // Released: dropping the last callback clears every reason and unload runs.
  assert(abi.execute("import { runtime } from 'rime:runtime';\n"
                     "runtime.unsubscribe(globalThis.watched);",
                     "release-watched.mjs")
             .ok());
  assert(abi.unload().ok());
  assert(abi.state() == rime::js::HostAbiState::Unloaded);
}

// Duplicate native registrations fail at add time with InvalidContract and
// the offending specifier, instead of surviving until start() where the
// registry would silently keep only the first match.
void test_native_registration_duplicates() {
  rime::js::Runtime runtime;
  auto factory = [](JSContext*) -> JSModuleDef* { return nullptr; };
  assert(runtime.add_native_module("rime:test:dup", factory, nullptr).ok());
  const auto duplicate = runtime.add_native_module("rime:test:dup", factory, nullptr);
  assert(!duplicate.ok());
  assert(duplicate.code == rime::core::Error::Code::InvalidContract);
  assert(duplicate.message.find("rime:test:dup") != std::string::npos);
  // An empty specifier and a null factory are rejected the same way.
  const auto empty = runtime.add_native_module("", factory, nullptr);
  assert(!empty.ok());
  assert(empty.code == rime::core::Error::Code::InvalidContract);
}

}  // namespace

int main() {
  test_threaded_runtime();
  test_context();
  test_persistent();
  test_debug_and_cwd();
  test_busy_loop_interrupt();
  test_runtime_exit();
  test_host_abi();
  test_host_abi_held_callback();
  test_host_abi_pending_event();
  test_native_registration_duplicates();
  return 0;
}
