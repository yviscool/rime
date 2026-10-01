// Needs an interactive desktop, exclusive run: injects real keys/mouse and subscribes to global input.
#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/input.hpp"
#include "rime/win32/js_input.hpp"

#include <windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;
using rime::win32::InputService;

void send_vk(const WORD virtual_key) {
  INPUT inputs[2]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = virtual_key;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = virtual_key;
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
  // NOTE: hoisted out of assert() so the SendInput side effect still runs
  // under NDEBUG where assert() is compiled out.
  const UINT sent = SendInput(2, inputs, sizeof(INPUT));
  assert(sent == 2);
}

void send_mouse_to(const int x, const int y) {
  const int width = GetSystemMetrics(SM_CXSCREEN);
  const int height = GetSystemMetrics(SM_CYSCREEN);
  assert(width > 1 && height > 1);
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
  input.mi.dx = static_cast<LONG>(x * 65535 / (width - 1));
  input.mi.dy = static_cast<LONG>(y * 65535 / (height - 1));
  // NOTE: hoisted out of assert() so the SendInput side effect still runs
  // under NDEBUG where assert() is compiled out.
  const UINT sent = SendInput(1, &input, sizeof(INPUT));
  assert(sent == 1);
}

void run(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js step failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

// Records the chord-built action it executed so the slice can assert the
// exact IR that crossed the hook -> event queue -> dispatcher -> kernel
// boundary; runs on the timer thread during the shared queue pump.
class ProbeExecutor final : public rime::action::Executor {
 public:
  rime::action::Result execute(const rime::action::Action& action,
                               rime::core::CancellationToken) override {
    std::lock_guard<std::mutex> lock(mutex_);
    action_id_ = action.id;
    type_ = action.type;
    source_kind_ = action.source.kind;
    target_kind_ = action.target.kind;
    target_id_ = action.target.id;
    payload_ = action.payload;
    ++executed_;
    rime::action::Result result;
    result.id = action.id;
    result.succeeded = true;
    result.value = rime::core::json::Value::object();
    result.value.set("fired", rime::core::json::Value::boolean(true));
    return result;
  }

  [[nodiscard]] int executed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return executed_;
  }
  [[nodiscard]] std::uint64_t action_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return action_id_;
  }
  [[nodiscard]] std::string type() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return type_;
  }
  [[nodiscard]] std::string source_kind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_kind_;
  }
  [[nodiscard]] std::string target_kind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_kind_;
  }
  [[nodiscard]] std::string target_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_id_;
  }
  [[nodiscard]] std::string payload() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return payload_;
  }

 private:
  mutable std::mutex mutex_;
  int executed_{0};
  std::uint64_t action_id_{0};
  std::string type_;
  std::string source_kind_;
  std::string target_kind_;
  std::string target_id_;
  std::string payload_;
};

}  // namespace

int main() {
  InputService service;
  assert(service.start().ok());

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{"windows.hook.global", "probe.chord"}),
      trace);
  auto probe = std::make_shared<ProbeExecutor>();
  assert(kernel.register_executor("probe.chord", probe).ok());
  // Chord bindings queue their actions through the same bounded, traced
  // pipeline every module mutation uses.
  rime::action::Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  std::atomic<std::uint64_t> next_action_id{0};
  rime::win32::InputModuleBinding binding;
  binding.service = &service;
  binding.kernel = &kernel;
  binding.dispatcher = &dispatcher;
  binding.next_action_id = &next_action_id;
  rime::js::Runtime runtime;
  assert(rime::win32::register_input_module(runtime, &binding).ok());
  assert(runtime.start().ok());

  // Wiring is part of the contract: a binding without the dispatcher chord
  // actions queue through is rejected at register time.
  {
    rime::win32::InputModuleBinding partial;
    partial.service = &service;
    partial.kernel = &kernel;
    rime::js::Runtime partial_runtime;
    assert(rime::win32::register_input_module(partial_runtime, &partial).code ==
           rime::core::Error::Code::InvalidContract);
  }

  // subscribe returns a positive id; non-function handlers throw TypeError;
  // waitFor polls with runtime.delay so later modules can await conditions.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "import { runtime } from 'rime:runtime';\n"
      "globalThis.input = input;\n"
      "globalThis.events = [];\n"
      "globalThis.sid = input.subscribe(ev => { globalThis.events.push(ev); });\n"
      "globalThis.badArg = false;\n"
      "try { input.subscribe(42); } catch (e) { globalThis.badArg = (e instanceof TypeError); }\n"
      "globalThis.waitFor = async (predicate, timeoutMs) => {\n"
      "  const deadline = Date.now() + timeoutMs;\n"
      "  while (Date.now() < deadline) {\n"
      "    if (predicate()) return true;\n"
      "    await runtime.delay(20, null);\n"
      "  }\n"
      "  return predicate();\n"
      "};",
      "input-subscribe.mjs");
  run(runtime,
      "if (!(globalThis.sid > 0)) throw new Error('subscribe must return a positive id');\n"
      "if (!globalThis.badArg) throw new Error('non-function handler must throw TypeError');",
      "input-subscribe-check.mjs");

  // Keyboard: a synthetic F24 press must reach the handler as key events.
  send_vk(VK_F24);
  run(runtime,
      "globalThis.found = 'pending';\n"
      "waitFor(() => events.some(e => e.kind === 'key' && e.vk === 135 && e.down) &&\n"
      "              events.some(e => e.kind === 'key' && e.vk === 135 && !e.down), 3000)\n"
      "  .then(v => { globalThis.found = v; });",
      "input-wait-key.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.found !== true) throw new Error('F24 key events did not arrive');\n"
      "const key = events.find(e => e.kind === 'key' && e.vk === 135);\n"
      "if (typeof key.sequence !== 'number' || key.sequence <= 0) throw new Error('bad sequence');\n"
      "if (typeof key.alt !== 'boolean' || typeof key.control !== 'boolean')\n"
      "  throw new Error('bad modifier shape');\n"
      "if (typeof key.injected !== 'boolean') throw new Error('bad injected flag');",
      "input-check-key.mjs");

  // Mouse: an absolute move arrives with exact coordinates.
  send_mouse_to(321, 123);
  run(runtime,
      "globalThis.found = 'pending';\n"
      "waitFor(() => events.some(e => e.kind === 'mouse' && e.action === 'move' &&\n"
      "                            e.x === 321 && e.y === 123), 3000)\n"
      "  .then(v => { globalThis.found = v; });",
      "input-wait-mouse.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.found !== true) throw new Error('mouse move event did not arrive');",
      "input-check-mouse.mjs");

  // Chord slice: bind F24 to a probe action; the press travels hook ->
  // event queue -> matcher -> dispatcher -> kernel and executes it. The
  // trace counts double as the wait condition so the assertion never races
  // the pump task's Finished record.
  auto probe_trace_counts = [&trace]() {
    int accepted = 0;
    int started = 0;
    int finished = 0;
    for (const auto& entry : trace->snapshot()) {
      if (entry.subject != "probe.chord") continue;
      if (entry.kind == rime::core::TraceKind::ActionAccepted) ++accepted;
      if (entry.kind == rime::core::TraceKind::ActionStarted) ++started;
      if (entry.kind == rime::core::TraceKind::ActionFinished) ++finished;
    }
    return std::tuple{accepted, started, finished};
  };
  run(runtime,
      "import { input } from 'rime:input';\n"
      "const template = { type: 'probe.chord', capability: 'probe.chord',\n"
      "                   target: { kind: 'chord', id: 'main' } };\n"
      "globalThis.bindId = input.bind('f24', { ...template, payload: { note: 'f24' } });\n"
      "globalThis.bindErrors = {};\n"
      "const expect = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.bindErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expect('trailing', () => input.bind('ctrl+shift+', template));\n"
      "expect('modOnly', () => input.bind('ctrl', template));\n"
      "expect('dupModifier', () => input.bind('ctrl+ctrl+f24', template));\n"
      "expect('unknownKey', () => input.bind('ctrl+notakey', template));\n"
      "expect('fnRange', () => input.bind('f99', template));\n"
      "expect('emptyType', () => input.bind('f24', { ...template, type: '' }));\n"
      "expect('missingTarget', () => input.bind('f24', { type: 'x', capability: 'c' }));\n"
      "expect('arity', () => input.bind('f24'));",
      "input-bind.mjs");
  run(runtime,
      "if (!(globalThis.bindId > 0)) throw new Error('bind must return a positive id');\n"
      "const errors = globalThis.bindErrors;\n"
      "for (const key of ['trailing', 'modOnly', 'dupModifier', 'unknownKey', 'fnRange',\n"
      "                   'emptyType', 'missingTarget', 'arity']) {\n"
      "  if (!errors[key]) throw new Error('expected a TypeError for ' + key);\n"
      "}",
      "input-bind-check.mjs");

  send_vk(VK_F24);
  const auto fired_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (probe_trace_counts() != std::tuple{1, 1, 1} &&
         std::chrono::steady_clock::now() < fired_by) {
    // Pump the JS lane so the drained event reaches the chord matcher; the
    // probe lives on the C++ side, so the wait polls trace counts directly.
    run(runtime,
        "globalThis.chordTick = 'pending';\n"
        "globalThis.waitFor(() => false, 50).then(v => { globalThis.chordTick = v; });",
        "input-chord-pump.mjs");
    (void)runtime.settle(500ms);
  }
  assert(runtime.settle(2000ms).ok());
  assert((probe_trace_counts() == std::tuple{1, 1, 1}));
  assert(probe->executed() == 1);
  assert(probe->action_id() > 0);
  assert(probe->type() == "probe.chord");
  assert(probe->source_kind() == "chord");
  assert(probe->target_kind() == "chord");
  assert(probe->target_id() == "main");
  assert(probe->payload().find("f24") != std::string::npos);

  // Unbind closes the whole chain: double close and unknown ids are false,
  // and a later press dispatches nothing.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.unbound = input.unbind(globalThis.bindId);\n"
      "globalThis.unboundAgain = input.unbind(globalThis.bindId);\n"
      "globalThis.unboundUnknown = input.unbind(9999999);",
      "input-unbind.mjs");
  run(runtime,
      "if (!globalThis.unbound) throw new Error('unbind must succeed');\n"
      "if (globalThis.unboundAgain) throw new Error('second unbind must be false');\n"
      "if (globalThis.unboundUnknown) throw new Error('unknown id must be false');",
      "input-unbind-check.mjs");
  send_vk(VK_F24);
  run(runtime,
      "globalThis.frozenTick = 'pending';\n"
      "globalThis.waitFor(() => false, 400).then(v => { globalThis.frozenTick = v; });",
      "input-chord-frozen.mjs");
  assert(runtime.settle(2000ms).ok());
  assert(probe->executed() == 1);
  assert((probe_trace_counts() == std::tuple{1, 1, 1}));

  // Unsubscribe stops delivery: double close is false, unknown ids are false.
  run(runtime,
      "globalThis.unsub = input.unsubscribe(sid);\n"
      "globalThis.unsubAgain = input.unsubscribe(sid);\n"
      "globalThis.unsubUnknown = input.unsubscribe(9999999);",
      "input-unsubscribe.mjs");
  run(runtime,
      "if (!globalThis.unsub) throw new Error('unsubscribe must succeed');\n"
      "if (globalThis.unsubAgain) throw new Error('second unsubscribe must be false');\n"
      "if (globalThis.unsubUnknown) throw new Error('unknown id must be false');\n"
      "globalThis.stable = 'pending';\n"
      "waitFor(() => false, 400).then(() => {\n"
      "  globalThis.stable = events.some(e => e.kind === 'key' && e.vk === 134);\n"
      "});",
      "input-unsubscribe-check.mjs");
  send_vk(VK_F23);
  assert(runtime.settle(3000ms).ok());
  run(runtime,
      "if (globalThis.stable !== false) throw new Error('events arrived after unsubscribe');",
      "input-frozen-check.mjs");

  // Lifetime: a live subscription may outlive the runtime; the host event
  // queue closes on teardown so producers become no-ops.
  run(runtime, "globalThis.sid2 = input.subscribe(() => {});", "input-resubscribe.mjs");
  assert(runtime.stop().ok());
  assert(runtime.stop().ok());

  // Capability gate: after the first runtime released the JS lane, a fresh
  // runtime with an empty policy sees subscribe and bind denied by Error
  // (not TypeError) naming the capability. The checks run before the service
  // is touched, so the stopped input service is irrelevant here.
  {
    rime::action::Kernel denied_kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
        std::unordered_set<std::string>{}));
    rime::action::Dispatcher denied_dispatcher(denied_kernel,
                                               rime::action::default_dispatch_policy());
    std::atomic<std::uint64_t> denied_next_action_id{0};
    rime::win32::InputModuleBinding denied_binding;
    denied_binding.service = &service;
    denied_binding.kernel = &denied_kernel;
    denied_binding.dispatcher = &denied_dispatcher;
    denied_binding.next_action_id = &denied_next_action_id;
    rime::js::Runtime denied_runtime;
    assert(rime::win32::register_input_module(denied_runtime, &denied_binding).ok());
    assert(denied_runtime.start().ok());
    run(denied_runtime,
        "import { input } from 'rime:input';\n"
        "globalThis.denied = null;\n"
        "globalThis.deniedBind = null;\n"
        "try { input.subscribe(() => {}); }\n"
        "catch (e) { globalThis.denied = e.message; }\n"
        "try { input.bind('f24', { type: 'probe.chord', capability: 'probe.chord',\n"
        "                          target: { kind: 'chord', id: 'main' } }); }\n"
        "catch (e) { globalThis.deniedBind = e.message; }",
        "input-deny.mjs");
    run(denied_runtime,
        "if (!globalThis.denied || !globalThis.denied.includes('windows.hook.global'))\n"
        "  throw new Error('subscribe must be denied with the capability name: ' +\n"
        "                  globalThis.denied);\n"
        "if (!globalThis.deniedBind || !globalThis.deniedBind.includes('windows.hook.global'))\n"
        "  throw new Error('bind must be denied with the capability name: ' +\n"
        "                  globalThis.deniedBind);",
        "input-deny-check.mjs");
    assert(denied_runtime.stop().ok());
  }

  assert(service.stop().ok());
  assert(service.stop().ok());
  return 0;
}
