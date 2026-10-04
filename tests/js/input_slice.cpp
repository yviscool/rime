// Needs an interactive desktop, exclusive run: injects real keys/mouse and subscribes to global input.
#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/input.hpp"
#include "rime/win32/input_executor.hpp"
#include "rime/win32/js_input.hpp"
#include "rime/win32/window.hpp"

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

// One raw transition with no paired counterpart: the block slice needs to
// hold a key down across polls. Raw SendInput carries no self marker, so
// the hook treats it as foreign input.
void send_key_state(const WORD virtual_key, const bool down) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = virtual_key;
  if (!down) input.ki.dwFlags = KEYEVENTF_KEYUP;
  // NOTE: hoisted out of assert() so the SendInput side effect still runs
  // under NDEBUG where assert() is compiled out.
  const UINT sent = SendInput(1, &input, sizeof(INPUT));
  assert(sent == 1);
}

// One foreign lock-key tap: dwExtraInfo stays 0, so the hook sees an
// injected event that is NOT our own - exactly the input an armed
// setLockForce must swallow, and the input that must flip the toggle again
// once it is neutral. Scan code and the extended bit mirror the production
// injector (engine/win32/src/input.cpp send()) so the tap lands like the
// real key, minus the self marker.
void send_foreign_key(const WORD virtual_key) {
  const WORD scan = static_cast<WORD>(MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC));
  const DWORD extended =
      (virtual_key == VK_NUMLOCK || virtual_key == VK_SCROLL)
          ? static_cast<DWORD>(KEYEVENTF_EXTENDEDKEY)
          : 0u;
  INPUT inputs[2]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = virtual_key;
  inputs[0].ki.wScan = scan;
  inputs[0].ki.dwFlags = extended;
  inputs[0].ki.dwExtraInfo = 0;
  inputs[1] = inputs[0];
  inputs[1].ki.dwFlags = extended | KEYEVENTF_KEYUP;
  // NOTE: hoisted out of assert() so the SendInput side effect still runs
  // under NDEBUG where assert() is compiled out.
  const UINT sent = SendInput(2, inputs, sizeof(INPUT));
  assert(sent == 2);
}

template <typename Predicate>
bool wait_for(Predicate predicate, const std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
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
  // mouseGetPos reads through the window service (window + control under the
  // cursor), so the slice wires it exactly like the bootstrap does.
  rime::win32::WindowService window_service;
  assert(window_service.start().ok());

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(
      std::make_shared<rime::action::StaticCapabilityPolicy>(
          std::unordered_set<std::string>{"windows.hook.global", "probe.chord",
                                          "windows.input.inject", "windows.input.read"}),
      trace);
  auto probe = std::make_shared<ProbeExecutor>();
  assert(kernel.register_executor("probe.chord", probe).ok());
  // send()/mouse() queue through the same pipeline; one executor injects both.
  const auto input_executor = std::make_shared<rime::win32::InputExecutor>(service);
  assert(kernel.register_executor("input.send", input_executor).ok());
  assert(kernel.register_executor("input.mouse", input_executor).ok());
  // Chord bindings queue their actions through the same bounded, traced
  // pipeline every module mutation uses.
  rime::action::Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  std::atomic<std::uint64_t> next_action_id{0};
  rime::win32::InputModuleBinding binding;
  binding.service = &service;
  binding.window_service = &window_service;
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

  // modifiers(): a synchronous nine-field per-side snapshot behind the
  // windows.input.inject gate.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.mods = input.modifiers();\n",
      "input-modifiers.mjs");
  run(runtime,
      "const expected = ['lcontrol', 'rcontrol', 'lshift', 'rshift',\n"
      "                  'lalt', 'ralt', 'lwin', 'rwin', 'capsLock'];\n"
      "for (const field of expected) {\n"
      "  if (typeof globalThis.mods[field] !== 'boolean')\n"
      "    throw new Error('modifiers().' + field + ' must be a boolean');\n"
      "}\n",
      "input-modifiers-check.mjs");

  // Reentrancy: a subscribe handler runs while the event drain is in
  // progress; entering a module mutation (bind) from inside it must neither
  // deadlock the pump nor corrupt the chord registry.
  run(runtime,
      "globalThis.reentrantBindId = null;\n"
      "globalThis.reentrantError = null;\n"
      "globalThis.sid3 = input.subscribe(ev => {\n"
      "  if (globalThis.reentrantBindId !== null || globalThis.reentrantError !== null) return;\n"
      "  if (!ev.down || ev.vk !== 135) return;\n"
      "  try {\n"
      "    globalThis.reentrantBindId =\n"
      "        input.bind('f23', { type: 'probe.chord', capability: 'probe.chord',\n"
      "                            target: { kind: 'chord', id: 'reentrant' } });\n"
      "  } catch (e) { globalThis.reentrantError = String(e); }\n"
      "});",
      "input-reentrant.mjs");
  send_vk(VK_F24);
  run(runtime,
      "globalThis.reentrantDone = 'pending';\n"
      "waitFor(() => globalThis.reentrantBindId !== null || globalThis.reentrantError !== null,\n"
      "        3000)\n"
      "  .then(v => { globalThis.reentrantDone = v; });",
      "input-reentrant-wait.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.reentrantDone !== true)\n"
      "  throw new Error('reentrant bind never completed: ' + globalThis.reentrantError);\n"
      "if (globalThis.reentrantError)\n"
      "  throw new Error('reentrant bind failed: ' + globalThis.reentrantError);\n"
      "if (!(globalThis.reentrantBindId > 0))\n"
      "  throw new Error('reentrant bind must return a positive id');\n"
      "if (!input.unbind(globalThis.reentrantBindId))\n"
      "  throw new Error('reentrant bind must be unbindable');\n"
      "input.unsubscribe(globalThis.sid3);",
      "input-reentrant-check.mjs");

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

  // send() validates its steps at the JS boundary: anything but a non-empty
  // array of {vk: 1..254 integer, down: boolean} steps is a TypeError.
  run(runtime,
      "globalThis.sendErrors = {};\n"
      "const expectSend = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.sendErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectSend('notArray', () => input.send({}));\n"
      "expectSend('empty', () => input.send([]));\n"
      "expectSend('stepNotObject', () => input.send([7]));\n"
      "expectSend('vkMissing', () => input.send([{ down: true }]));\n"
      "expectSend('vkRange', () => input.send([{ vk: 0, down: true }]));\n"
      "expectSend('vkFraction', () => input.send([{ vk: 65.5, down: true }]));\n"
      "expectSend('downMissing', () => input.send([{ vk: 65 }]));\n"
      "expectSend('arity', () => input.send());",
      "input-send-validate.mjs");
  run(runtime,
      "const sendErrors = globalThis.sendErrors;\n"
      "for (const key of ['notArray', 'empty', 'stepNotObject', 'vkMissing', 'vkRange',\n"
      "                   'vkFraction', 'downMissing', 'arity']) {\n"
      "  if (!sendErrors[key]) throw new Error('expected a TypeError for send ' + key);\n"
      "}",
      "input-send-validate-check.mjs");

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

  // Self-injection: send() tags its batch, so subscribers observe
  // selfInjected events while the bound chord must not re-fire on our own
  // input - the loop-prevention rule for actions that inject keys.
  run(runtime,
      "globalThis.sendResult = null;\n"
      "globalThis.sendError = null;\n"
      "input.send([{ vk: 135, down: true }, { vk: 135, down: false }])\n"
      "  .then(r => { globalThis.sendResult = r; },\n"
      "        e => { globalThis.sendError = String(e); });",
      "input-send.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.sendError) throw new Error('send failed: ' + globalThis.sendError);\n"
      "if (!globalThis.sendResult || globalThis.sendResult.sent !== 2)\n"
      "  throw new Error('send must resolve with the sent count: ' +\n"
      "                  JSON.stringify(globalThis.sendResult));\n"
      "globalThis.selfSeen = 'pending';\n"
      "waitFor(() => events.some(e => e.kind === 'key' && e.vk === 135 && e.down &&\n"
      "                            e.selfInjected === true), 3000)\n"
      "  .then(v => { globalThis.selfSeen = v; });",
      "input-send-check.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.selfSeen !== true)\n"
      "  throw new Error('self-injected key events must reach subscribe');",
      "input-send-seen.mjs");
  // Chord suppression: the self events already reached both subscriptions,
  // so a stability window without probe activity proves the skip.
  const auto self_stable_until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  while (std::chrono::steady_clock::now() < self_stable_until) {
    run(runtime,
        "globalThis.selfTick = 'pending';\n"
        "globalThis.waitFor(() => false, 50).then(v => { globalThis.selfTick = v; });",
        "input-self-pump.mjs");
    (void)runtime.settle(100ms);
  }
  assert(runtime.settle(500ms).ok());
  assert(probe->executed() == 1);
  assert((probe_trace_counts() == std::tuple{1, 1, 1}));

  // Unicode send: out-of-range units are TypeErrors at the JS boundary, a
  // UTF-16 batch resolves { sent: 2 } and lands as vk-less self events.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.uniErrors = {};\n"
      "const expectUni = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.uniErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectUni('vkZero', () => input.send([{ vk: 0, down: true }]));\n"
      "expectUni('unitRange', () => input.send([{ vk: 65536, down: true, unicode: true }]));\n"
      "expectUni('unicodeFlag', () => input.send([{ vk: 65, down: true, unicode: 'yes' }]));\n"
      "globalThis.uniResult = null;\n"
      "globalThis.uniError = null;\n"
      "input.send([{ vk: 0x4f60, down: true, unicode: true },\n"
      "             { vk: 0x4f60, down: false, unicode: true }])\n"
      "  .then(r => { globalThis.uniResult = r; },\n"
      "        e => { globalThis.uniError = String(e); });",
      "input-send-unicode.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "const uniErrors = globalThis.uniErrors;\n"
      "for (const key of ['vkZero', 'unitRange', 'unicodeFlag']) {\n"
      "  if (!uniErrors[key]) throw new Error('expected a TypeError for unicode ' + key);\n"
      "}\n"
      "if (globalThis.uniError) throw new Error('unicode send failed: ' + globalThis.uniError);\n"
      "if (!globalThis.uniResult || globalThis.uniResult.sent !== 2)\n"
      "  throw new Error('unicode send must resolve { sent: 2 }');\n"
      "globalThis.uniSeen = 'pending';\n"
      "waitFor(() => events.some(e => e.kind === 'key' && e.vk === 231 &&\n"
      "                            e.scan === 0x4f60 && e.selfInjected), 3000)\n"
      "  .then(v => { globalThis.uniSeen = v; });",
      "input-send-unicode-check.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.uniSeen !== true)\n"
      "  throw new Error('unicode self events (vk 231, scan 0x4f60) must reach subscribe');",
      "input-send-unicode-seen.mjs");

  // input.mouse: payload TypeErrors at the JS boundary, then a real absolute
  // move through the action pipeline (worker lane, self-injected, <= 1px).
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.mouseErrors = {};\n"
      "const expectMouse = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.mouseErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectMouse('notObject', () => input.mouse(42));\n"
      "expectMouse('stepsMissing', () => input.mouse({}));\n"
      "expectMouse('stepsNotArray', () => input.mouse({ steps: 1 }));\n"
      "expectMouse('stepsEmpty', () => input.mouse({ steps: [] }));\n"
      "expectMouse('stepNotObject', () => input.mouse({ steps: [7] }));\n"
      "expectMouse('actionMissing', () => input.mouse({ steps: [{}] }));\n"
      "expectMouse('actionUnknown', () => input.mouse({ steps: [{ action: 'wheel' }] }));\n"
      "expectMouse('xyMissing', () => input.mouse({ steps: [{ action: 'move' }] }));\n"
      "expectMouse('xFraction',\n"
      "            () => input.mouse({ steps: [{ action: 'move', x: 1.5, y: 2 }] }));\n"
      "expectMouse('buttonRange',\n"
      "            () => input.mouse({ steps: [{ action: 'down', button: 4 }] }));\n"
      "expectMouse('buttonMissing', () => input.mouse({ steps: [{ action: 'up' }] }));\n"
      "expectMouse('speedRange',\n"
      "            () => input.mouse({ steps: [{ action: 'move', x: 1, y: 2 }], speed: 101 }));\n"
      "expectMouse('speedFraction',\n"
      "            () => input.mouse({ steps: [{ action: 'move', x: 1, y: 2 }], speed: 1.5 }));\n"
      "expectMouse('arity', () => input.mouse());\n",
      "input-mouse-validate.mjs");
  run(runtime,
      "const mouseErrors = globalThis.mouseErrors;\n"
      "for (const key of ['notObject', 'stepsMissing', 'stepsNotArray', 'stepsEmpty',\n"
      "                   'stepNotObject', 'actionMissing', 'actionUnknown', 'xyMissing',\n"
      "                   'xFraction', 'buttonRange', 'buttonMissing', 'speedRange',\n"
      "                   'speedFraction', 'arity']) {\n"
      "  if (!mouseErrors[key]) throw new Error('expected a TypeError for mouse ' + key);\n"
      "}",
      "input-mouse-validate-check.mjs");
  POINT mouse_origin{};
  // NOTE: hoisted out of assert(): GetCursorPos writes mouse_origin even when
  // NDEBUG compiles the assertion out.
  const BOOL got_mouse_origin = GetCursorPos(&mouse_origin);
  if (got_mouse_origin == FALSE) return 1;
  // The move target must stay inside the smallest supported desktop: CI runs
  // 1024x768 with the taskbar top at y=720, and SM_CYSCREEN there can claim a
  // taller mode than the real session surface (it once reported >789 while the
  // desktop ended at 768), so the base point is fixed below y=720 and the
  // clamp below it is defense in depth only.
  const int screen_width = GetSystemMetrics(SM_CXSCREEN);
  const int screen_height = GetSystemMetrics(SM_CYSCREEN);
  assert(screen_width > 1 && screen_height > 1);
  const int mouse_target_x = 456 < screen_width ? 456 : screen_width - 1;
  const int mouse_target_y = 650 < screen_height ? 650 : screen_height - 1;
  const std::string mouse_target_js = "{ action: 'move', x: " + std::to_string(mouse_target_x) +
                                      ", y: " + std::to_string(mouse_target_y) + " }";
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.mouseResult = null;\n"
      "globalThis.mouseError = null;\n"
      "input.mouse({ steps: [" +
          mouse_target_js + "] })\n"
                        "  .then(r => { globalThis.mouseResult = r; },\n"
                        "        e => { globalThis.mouseError = String(e); });",
      "input-mouse-move.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.mouseError) throw new Error('mouse failed: ' + globalThis.mouseError);\n"
      "if (!globalThis.mouseResult || globalThis.mouseResult.sent !== 1)\n"
      "  throw new Error('mouse must resolve { sent: 1 }: ' +\n"
      "                  JSON.stringify(globalThis.mouseResult));\n"
      "globalThis.mouseSeen = 'pending';\n"
      "waitFor(() => events.some(e => e.kind === 'mouse' && e.action === 'move' &&\n"
      "                            e.selfInjected === true &&\n"
      "                            Math.abs(e.x - " +
          std::to_string(mouse_target_x) + ") <= 1 && Math.abs(e.y - " + std::to_string(mouse_target_y) +
          ") <= 1), 3000)\n"
          "  .then(v => { globalThis.mouseSeen = v; });",
      "input-mouse-move-check.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.mouseSeen !== true)\n"
      "  throw new Error('self mouse move did not reach subscribe');",
      "input-mouse-seen.mjs");

  // mouseGetPos: the cursor is still at the clamped move target above; the
  // read is gated by windows.input.read and validates options like every
  // other async native call.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.posResult = null;\n"
      "globalThis.posError = null;\n"
      "input.mouseGetPos().then(r => { globalThis.posResult = r; },\n"
      "                        e => { globalThis.posError = { code: e.code,\n"
      "                                                       message: e.message }; });\n"
      "globalThis.posTypeError = false;\n"
      "try { input.mouseGetPos(42); } catch (e) { globalThis.posTypeError = (e instanceof TypeError); }\n"
      "globalThis.posArity = false;\n"
      "try { input.mouseGetPos({}, {}); } catch (e) { globalThis.posArity = (e instanceof TypeError); }",
      "input-mouse-getpos.mjs");
  assert(runtime.settle(5000ms).ok());
  // The desktop is interactive: the physical cursor can be moved between our
  // move and the read (it happened while the user was browsing). Re-issue the
  // move and re-read on a bounded poll; only a move that never lands (a real
  // fault) exhausts the attempts and fails.
  run(runtime,
      "if (globalThis.posError)\n"
      "  throw new Error('getPos failed: ' + JSON.stringify(globalThis.posError));\n"
      "if (!globalThis.posTypeError) throw new Error('getPos(42) must throw TypeError');\n"
      "if (!globalThis.posArity) throw new Error('getPos arity must throw TypeError');\n"
      "if (!globalThis.posResult || typeof globalThis.posResult.x !== 'number' ||\n"
      "    typeof globalThis.posResult.y !== 'number')\n"
      "  throw new Error('getPos must return numeric x/y: ' +\n"
      "                  JSON.stringify(globalThis.posResult));\n"
      "const inRange = (p) => p !== null && typeof p.x === 'number' &&\n"
      "    p.x >= " +
      std::to_string(mouse_target_x - 1) + " && p.x <= " + std::to_string(mouse_target_x + 1) +
      " &&\n    p.y >= " + std::to_string(mouse_target_y - 1) + " && p.y <= " +
      std::to_string(mouse_target_y + 1) + ";\n"
      "globalThis.posGood = inRange(globalThis.posResult) ? globalThis.posResult : null;\n"
      "globalThis.posTries = 0;\n"
      "globalThis.waitFor(() => {\n"
      "  if (globalThis.posError || globalThis.posGood) return true;\n"
      "  if (globalThis.posTries >= 15) return true;\n"
      "  globalThis.posTries += 1;\n"
      "  input.mouse({ steps: [{ action: 'move', x: " +
      std::to_string(mouse_target_x) + ", y: " + std::to_string(mouse_target_y) + " }] })\n"
      "    .then(() => input.mouseGetPos())\n"
      "    .then(p => { globalThis.posResult = p;\n"
      "                 if (inRange(p) && !globalThis.posGood) globalThis.posGood = p; },\n"
      "          e => { globalThis.posError = e; });\n"
      "  return false;\n"
      "}, 8000).then(v => { globalThis.posSettled = v; });",
      "input-mouse-getpos-check.mjs");
  assert(runtime.settle(9500ms).ok());
  run(runtime,
      "if (globalThis.posError)\n"
      "  throw new Error('getPos failed: ' + JSON.stringify(globalThis.posError));\n"
      "if (!globalThis.posGood)\n"
      "  throw new Error('getPos x/y must match the cursor: ' +\n"
      "                  JSON.stringify(globalThis.posResult));\n"
      "const pos = globalThis.posGood;\n"
      "if (pos.window !== null && (typeof pos.window !== 'object' ||\n"
      "    typeof pos.window.id !== 'number'))\n"
      "  throw new Error('window must be an object with an id or null');\n"
      "if (pos.control !== null && (typeof pos.control !== 'object' ||\n"
      "    typeof pos.control.className !== 'string' ||\n"
      "    typeof pos.control.classNN !== 'string'))\n"
      "  throw new Error('control must carry className/classNN');",
      "input-mouse-getpos-final.mjs");
  // NOTE: hoisted out of assert(): SetCursorPos has a side effect (moves the
  // cursor) that must run even when NDEBUG compiles assert() out.
  const BOOL restored_mouse = SetCursorPos(mouse_origin.x, mouse_origin.y);
  if (restored_mouse == FALSE) return 1;

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

  // --- M2-B: getKeyState / keyWait / blockInput / keyHistory ---

  // getKeyState: argument validation is a synchronous TypeError, the toggle
  // mode agrees with modifiers().capsLock (both read the GetKeyState toggle
  // bit), and the default mode returns a boolean.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.stateErrors = {};\n"
      "const expectState = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.stateErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectState('unknownKey', () => input.getKeyState('notakey'));\n"
      "expectState('emptyKey', () => input.getKeyState(''));\n"
      "expectState('arity', () => input.getKeyState());\n"
      "expectState('modeNotString', () => input.getKeyState('a', 42));\n"
      "expectState('modeUnknown', () => input.getKeyState('a', 'x'));\n"
      "expectState('modeEmpty', () => input.getKeyState('a', ''));\n"
      "globalThis.capsLikeMods =\n"
      "    input.getKeyState('capslock', 't') === input.modifiers().capsLock;\n"
      "globalThis.stateType = typeof input.getKeyState('f24');\n",
      "input-getkeystate.mjs");
  run(runtime,
      "const errors = globalThis.stateErrors;\n"
      "for (const key of ['unknownKey', 'emptyKey', 'arity', 'modeNotString',\n"
      "                   'modeUnknown', 'modeEmpty']) {\n"
      "  if (!errors[key]) throw new Error('expected a TypeError for getKeyState ' + key);\n"
      "}\n"
      "if (!globalThis.capsLikeMods)\n"
      "  throw new Error('toggle mode must agree with modifiers().capsLock');\n"
      "if (globalThis.stateType !== 'boolean')\n"
      "  throw new Error('getKeyState must return a boolean');",
      "input-getkeystate-check.mjs");

  // getKeySC (AHK GetKeySC, script2.cpp:2303-2310): unparseable names and
  // mouse buttons yield 0 instead of throwing, extended keys carry the 0xE0
  // high byte, and the letter anchors assume the standard US layout that
  // this suite already runs on (AHK resolves the same layout-dependent codes).
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.scErrors = {};\n"
      "const expectSc = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.scErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectSc('arity', () => input.getKeySC());\n"
      "expectSc('notString', () => input.getKeySC(42));\n"
      "globalThis.sc = {\n"
      "  a: input.getKeySC('a'),\n"
      "  A: input.getKeySC('A'),\n"
      "  b: input.getKeySC('b'),\n"
      "  f1: input.getKeySC('f1'),\n"
      "  right: input.getKeySC('right'),\n"
      "  insert: input.getKeySC('insert'),\n"
      "  rcontrol: input.getKeySC('rcontrol'),\n"
      "  unknown: input.getKeySC('notakey'),\n"
      "  empty: input.getKeySC('')\n"
      "};\n",
      "input-getkeysc.mjs");
  run(runtime,
      "const errors = globalThis.scErrors;\n"
      "if (!errors.arity || !errors.notString)\n"
      "  throw new Error('getKeySC must reject non-string names with a TypeError');\n"
      "const sc = globalThis.sc;\n"
      "if (sc.unknown !== 0 || sc.empty !== 0)\n"
      "  throw new Error('getKeySC must map unparseable names to 0: ' + JSON.stringify(sc));\n"
      "if (sc.f1 !== 59)\n"
      "  throw new Error('F1 must be scan 59 (layout independent): ' + sc.f1);\n"
      "if (sc.a !== 30 || sc.A !== 30 || sc.a === sc.b)\n"
      "  throw new Error('letter codes must be US-layout stable and case-insensitive: ' +\n"
      "                  JSON.stringify(sc));\n"
      "for (const key of ['right', 'insert', 'rcontrol']) {\n"
      "  if ((sc[key] & 0xE000) !== 0xE000)\n"
      "    throw new Error(key + ' must carry the E0 extended byte: 0x' +\n"
      "                    sc[key].toString(16));\n"
      "}\n"
      "if (typeof sc.a !== 'number') throw new Error('getKeySC must return a number');",
      "input-getkeysc-check.mjs");

  // --- M3: getKeyVK / getKeyName ---

  // getKeyVK mirrors AHK GetKeyVK: every spelling the shared grammar accepts
  // resolves to its VK (chord tokens, case-insensitive names, vkXX and the
  // scNNN scan-code form), unparseable names fall to 0 instead of throwing,
  // and only a non-string argument is a TypeError. The sc anchors assume the
  // same standard layout the getKeySC anchors above already rely on.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.vkErrors = {};\n"
      "const expectVk = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.vkErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectVk('arity', () => input.getKeyVK());\n"
      "expectVk('notString', () => input.getKeyVK(42));\n"
      "expectVk('nullName', () => input.getKeyVK(null));\n"
      "globalThis.vk = {\n"
      "  a: input.getKeyVK('a'),\n"
      "  upper: input.getKeyVK('A'),\n"
      "  f1: input.getKeyVK('F1'),\n"
      "  escape: input.getKeyVK('escape'),\n"
      "  vk1b: input.getKeyVK('vk1b'),\n"
      "  lbutton: input.getKeyVK('lbutton'),\n"
      "  ctrl: input.getKeyVK('ctrl'),\n"
      "  right: input.getKeyVK('right'),\n"
      "  m: input.getKeyVK('m'),\n"
      "  pause: input.getKeyVK('pause'),\n"
      "  numlock: input.getKeyVK('numlock'),\n"
      "  rshift: input.getKeyVK('rshift'),\n"
      "  sc01e: input.getKeyVK('sc01e'),\n"
      "  sc032: input.getKeyVK('sc032'),\n"
      "  sc04d: input.getKeyVK('sc04d'),\n"
      "  sc14d: input.getKeyVK('sc14d'),\n"
      "  sce04d: input.getKeyVK('sce04d'),\n"
      "  sc145: input.getKeyVK('sc145'),\n"
      "  sc045: input.getKeyVK('sc045'),\n"
      "  sc136: input.getKeyVK('sc136'),\n"
      "  bogus: input.getKeyVK('bogus'),\n"
      "  empty: input.getKeyVK(''),\n"
      "  sczz: input.getKeyVK('sczz'),\n"
      "  scOnly: input.getKeyVK('sc'),\n"
      "  scRange: input.getKeyVK('sc1ffff')\n"
      "};\n",
      "input-getkeyvk.mjs");
  run(runtime,
      "const errors = globalThis.vkErrors;\n"
      "for (const key of ['arity', 'notString', 'nullName']) {\n"
      "  if (!errors[key]) throw new Error('expected a TypeError for getKeyVK ' + key);\n"
      "}\n"
      "const vk = globalThis.vk;\n"
      "if (vk.a !== 0x41 || vk.upper !== 0x41)\n"
      "  throw new Error('letters must resolve case-insensitively: ' + JSON.stringify(vk));\n"
      "if (vk.f1 !== 0x70)\n"
      "  throw new Error('F1 must be 0x70: ' + vk.f1);\n"
      "if (vk.escape !== 0x1B || vk.vk1b !== 0x1B)\n"
      "  throw new Error('escape and vk1b must resolve alike: ' + JSON.stringify(vk));\n"
      "if (vk.lbutton !== 0x01 || vk.ctrl !== 0x11)\n"
      "  throw new Error('mouse and modifier names must keep their VKs: ' + JSON.stringify(vk));\n"
      "if (vk.sc01e !== 0x41 || vk.sc01e !== vk.a)\n"
      "  throw new Error('sc01e must resolve like the letter a: ' + vk.sc01e);\n"
      "if (vk.sc14d !== 0x27 || vk.sce04d !== 0x27 || vk.sc14d !== vk.right)\n"
      "  throw new Error('extended sc spellings must all resolve like right: ' +\n"
      "                  JSON.stringify(vk));\n"
      "if (vk.sc04d !== vk.right)\n"
      "  throw new Error('sc04d is the base make code of right, not M: ' + vk.sc04d);\n"
      "if (vk.sc032 !== 0x4D || vk.sc032 !== vk.m)\n"
      "  throw new Error('sc032 must resolve like the letter m: ' + JSON.stringify(vk));\n"
      "if (vk.sc145 !== 0x90 || vk.sc145 !== vk.numlock)\n"
      "  throw new Error('sc145 must resolve like numlock: ' + JSON.stringify(vk));\n"
      "if (vk.sc045 !== 0x13 || vk.sc045 !== vk.pause)\n"
      "  throw new Error('sc045 must resolve like pause: ' + JSON.stringify(vk));\n"
      "if (vk.sc136 !== 0xA1 || vk.sc136 !== vk.rshift)\n"
      "  throw new Error('sc136 must resolve like rshift: ' + JSON.stringify(vk));\n"
      "for (const key of ['bogus', 'empty', 'sczz', 'scOnly', 'scRange']) {\n"
      "  if (vk[key] !== 0)\n"
      "    throw new Error('getKeyVK must map unparseable names to 0, got ' + key + ': ' +\n"
      "                    vk[key]);\n"
      "}\n"
      "if (typeof vk.a !== 'number') throw new Error('getKeyVK must return a number');",
      "input-getkeyvk-check.mjs");

  // getKeyName inverts the mapping onto this repo's canonical lowercase
  // tokens (AHK returns display-table spellings and vkNN fallbacks -
  // deviations recorded by the hub): unparseable names and VKs no token
  // names fall to "", and the scNNN form reads through getKeyState too.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.nameErrors = {};\n"
      "const expectName = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.nameErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectName('arity', () => input.getKeyName());\n"
      "expectName('notString', () => input.getKeyName(42));\n"
      "expectName('nullName', () => input.getKeyName(null));\n"
      "globalThis.names = {\n"
      "  f1: input.getKeyName('f1'),\n"
      "  upperF1: input.getKeyName('F1'),\n"
      "  vk41: input.getKeyName('vk41'),\n"
      "  sc01e: input.getKeyName('sc01e'),\n"
      "  right: input.getKeyName('right'),\n"
      "  escape: input.getKeyName('Escape'),\n"
      "  lbutton: input.getKeyName('lbutton'),\n"
      "  rshift: input.getKeyName('rshift'),\n"
      "  ctrl: input.getKeyName('ctrl'),\n"
      "  bogus: input.getKeyName('bogus'),\n"
      "  unnamedVk: input.getKeyName('vkfe')\n"
      "};\n"
      "globalThis.scStateSame =\n"
      "    input.getKeyState('sc14d', 'l') === input.getKeyState('right', 'l');\n",
      "input-getkeyname.mjs");
  run(runtime,
      "const errors = globalThis.nameErrors;\n"
      "for (const key of ['arity', 'notString', 'nullName']) {\n"
      "  if (!errors[key]) throw new Error('expected a TypeError for getKeyName ' + key);\n"
      "}\n"
      "const names = globalThis.names;\n"
      "const expected = {\n"
      "  f1: 'f1', upperF1: 'f1', vk41: 'a', sc01e: 'a', right: 'right',\n"
      "  escape: 'escape', lbutton: 'lbutton', rshift: 'rshift', ctrl: 'ctrl'\n"
      "};\n"
      "for (const key of Object.keys(expected)) {\n"
      "  if (names[key] !== expected[key])\n"
      "    throw new Error('getKeyName(' + key + ') must be ' + expected[key] + ', got ' +\n"
      "                    JSON.stringify(names[key]));\n"
      "}\n"
      "if (names.bogus !== '' || names.unnamedVk !== '')\n"
      "  throw new Error('unparseable and unnamed keys must return an empty string: ' +\n"
      "                  JSON.stringify(names));\n"
      "if (!globalThis.scStateSame)\n"
      "  throw new Error('getKeyState must read the scNNN form like the named form');",
      "input-getkeyname-check.mjs");

  // Round trip: a canonical name is always a spelling the shared grammar
  // reads back to the same virtual key, and getKeyState accepts it again.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.roundTrip = true;\n"
      "for (const token of ['f1', 'right', 'a', 'escape', 'lbutton', 'ctrl']) {\n"
      "  const name = input.getKeyName(token);\n"
      "  if (!name) {\n"
      "    globalThis.roundTrip = token + ' has no canonical name';\n"
      "    break;\n"
      "  }\n"
      "  if (input.getKeyVK(name) !== input.getKeyVK(token)) {\n"
      "    globalThis.roundTrip = token + ' -> ' + name;\n"
      "    break;\n"
      "  }\n"
      "  if (typeof input.getKeyState(name) !== 'boolean') {\n"
      "    globalThis.roundTrip = 'getKeyState rejected ' + name;\n"
      "    break;\n"
      "  }\n"
      "}\n",
      "input-keyname-roundtrip.mjs");
  run(runtime,
      "if (globalThis.roundTrip !== true)\n"
      "  throw new Error('key name round trip failed at: ' + globalThis.roundTrip);",
      "input-keyname-roundtrip-check.mjs");

  // A real hold: physical and logical both read the held F24 as down, and
  // both clear again after the release. waitFor polls on the JS lane, so
  // this also exercises reading while the lane is alive.
  send_key_state(VK_F24, true);
  run(runtime,
      "globalThis.heldState = 'pending';\n"
      "waitFor(() => input.getKeyState('f24', 'p') && input.getKeyState('f24', 'l'), 3000)\n"
      "  .then(v => { globalThis.heldState = v; });",
      "input-getkeystate-held.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.heldState !== true)\n"
      "  throw new Error('held F24 must read down in physical and logical modes');",
      "input-getkeystate-held-check.mjs");
  send_key_state(VK_F24, false);
  run(runtime,
      "globalThis.releasedState = 'pending';\n"
      "waitFor(() => !input.getKeyState('f24', 'p') && !input.getKeyState('f24', 'l'), 3000)\n"
      "  .then(v => { globalThis.releasedState = v; });",
      "input-getkeystate-released.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.releasedState !== true)\n"
      "  throw new Error('released F24 must read up in physical and logical modes');",
      "input-getkeystate-released-check.mjs");

  // keyWait: a released key resolves true on the first poll, a press that
  // never arrives rejects timeout after its budget, a bound cancellation
  // rejects cancelled, and option mistakes throw synchronously. Defaults
  // follow AHK: release wait, physical mode, 5s budget.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "import { runtime } from 'rime:runtime';\n"
      "globalThis.waitOut = {};\n"
      "input.keyWait('f24')\n"
      "  .then(v => { globalThis.waitOut.released = v; },\n"
      "        e => { globalThis.waitOut.released = e.code; });\n"
      "input.keyWait('f24', { down: true, deadlineMs: 200 })\n"
      "  .then(() => { globalThis.waitOut.timeout = 'resolved'; },\n"
      "        e => { globalThis.waitOut.timeout = e.code; });\n"
      "const cid = runtime.cancellation();\n"
      "input.keyWait('f24', { down: true, deadlineMs: 30000, cancellationId: cid })\n"
      "  .then(() => { globalThis.waitOut.cancel = 'resolved'; },\n"
      "        e => { globalThis.waitOut.cancel = e.code; });\n"
      "runtime.cancel(cid);\n"
      "globalThis.waitErrors = {};\n"
      "const expectWait = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.waitErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectWait('unknownKey', () => input.keyWait('notakey'));\n"
      "expectWait('arity', () => input.keyWait());\n"
      "expectWait('optionsNotObject', () => input.keyWait('f24', 42));\n"
      "expectWait('downNotBool', () => input.keyWait('f24', { down: 'yes' }));\n"
      "expectWait('modeUnknown', () => input.keyWait('f24', { mode: 'nope' }));\n",
      "input-keywait.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "const out = globalThis.waitOut;\n"
      "if (out.released !== true)\n"
      "  throw new Error('keyWait on a released key must resolve true, got: ' + out.released);\n"
      "if (out.timeout !== 'timeout')\n"
      "  throw new Error('keyWait deadline must reject with timeout, got: ' + out.timeout);\n"
      "if (out.cancel !== 'cancelled')\n"
      "  throw new Error('keyWait cancel must reject with cancelled, got: ' + out.cancel);\n"
      "const errors = globalThis.waitErrors;\n"
      "for (const key of ['unknownKey', 'arity', 'optionsNotObject', 'downNotBool',\n"
      "                   'modeUnknown']) {\n"
      "  if (!errors[key]) throw new Error('expected a TypeError for keyWait ' + key);\n"
      "}",
      "input-keywait-check.mjs");

  // Real satisfaction: down-wait resolves while the key is held; the
  // release wait is armed first, then this thread releases the key and the
  // worker poll observes the transition.
  send_key_state(VK_F24, true);
  run(runtime,
      "globalThis.waitHeld = null;\n"
      "input.keyWait('f24', { down: true }).then(v => { globalThis.waitHeld = v; });",
      "input-keywait-held.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.waitHeld !== true)\n"
      "  throw new Error('keyWait down must resolve true while held');\n"
      "globalThis.waitRelease = null;\n"
      "input.keyWait('f24').then(v => { globalThis.waitRelease = v; });",
      "input-keywait-release.mjs");
  send_key_state(VK_F24, false);
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.waitRelease !== true)\n"
      "  throw new Error('keyWait release must resolve true after the key goes up');",
      "input-keywait-release-check.mjs");

  // blockInput: mode validation is a synchronous TypeError; the real block
  // keeps foreign input away from the OS while the hook still records it
  // for subscribers, lets self batches through, releases the flag when a
  // bound cancellation fires, and 'off' restores delivery (false once the
  // guard already released).
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.blockEvents = [];\n"
      "globalThis.sidBlock = input.subscribe(ev => globalThis.blockEvents.push(ev));\n"
      "globalThis.blockOut = {};\n"
      "try { input.blockInput('sideways'); }\n"
      "catch (e) { globalThis.blockOut.typeError = (e instanceof TypeError); }\n"
      "try { input.blockInput(); }\n"
      "catch (e) { globalThis.blockOut.arity = (e instanceof TypeError); }\n"
      "globalThis.blockOut.on = input.blockInput('on');\n",
      "input-block.mjs");
  run(runtime,
      "if (!globalThis.blockOut.typeError)\n"
      "  throw new Error('blockInput unknown mode must throw TypeError');\n"
      "if (!globalThis.blockOut.arity)\n"
      "  throw new Error('blockInput arity must throw TypeError');\n"
      "if (globalThis.blockOut.on !== true)\n"
      "  throw new Error('blockInput on must return true while running');",
      "input-block-check.mjs");
  // Foreign hold while blocked: SendInput returns after the hook verdict,
  // so the OS state must stay up...
  send_key_state(VK_F24, true);
  std::this_thread::sleep_for(250ms);
  {
    const short blocked_state = GetAsyncKeyState(VK_F24);
    assert((blocked_state & 0x8000) == 0);
  }
  // ...while the subscription still records the event (enqueue before
  // swallow) and the physical snapshot tracks it.
  run(runtime,
      "globalThis.blockSeen = 'pending';\n"
      "waitFor(() => globalThis.blockEvents.some(\n"
      "             e => e.kind === 'key' && e.vk === 135 && e.down), 3000)\n"
      "  .then(v => { globalThis.blockSeen = v; });",
      "input-block-seen.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.blockSeen !== true)\n"
      "  throw new Error('blocked foreign input must still reach subscribe');",
      "input-block-seen-check.mjs");
  assert(service.physical_key_down(VK_F24));
  // Self-injected input bypasses the block in both directions.
  assert(service.send({{VK_F24, true}}).ok());
  assert(wait_for([&] { return (GetAsyncKeyState(VK_F24) & 0x8000) != 0; }));
  assert(service.send({{VK_F24, false}}).ok());
  assert(wait_for([&] { return (GetAsyncKeyState(VK_F24) & 0x8000) == 0; }));
  // The foreign release is swallowed too, so the snapshot clears.
  send_key_state(VK_F24, false);
  assert(wait_for([&] { return !service.physical_key_down(VK_F24); }));
  // A bound cancellation releases the block from the timer guard.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "import { runtime } from 'rime:runtime';\n"
      "globalThis.blockCid = runtime.cancellation();\n"
      "globalThis.blockOut.guardOn =\n"
      "    input.blockInput('on', { cancellationId: globalThis.blockCid });\n"
      "runtime.cancel(globalThis.blockCid);",
      "input-block-cancel.mjs");
  assert(wait_for([&] { return !service.blocked(); }, 5000ms));
  // 'off' reports false once the guard already released, then delivery is
  // observable again with a plain foreign tap.
  run(runtime,
      "globalThis.blockOut.off = input.blockInput('off');\n"
      "input.unsubscribe(globalThis.sidBlock);",
      "input-block-off.mjs");
  run(runtime,
      "if (globalThis.blockOut.guardOn !== true)\n"
      "  throw new Error('blockInput with a cancellation id must return true');\n"
      "if (globalThis.blockOut.off !== false)\n"
      "  throw new Error('blockInput off must return false once released');",
      "input-block-off-check.mjs");
  send_key_state(VK_F24, true);
  assert(wait_for([&] { return (GetAsyncKeyState(VK_F24) & 0x8000) != 0; }));
  send_key_state(VK_F24, false);
  assert(wait_for([&] { return (GetAsyncKeyState(VK_F24) & 0x8000) == 0; }));

  // keyHistory: maxEvents validation (0..500 integers), then a real report
  // whose capacity resize trims the ring - six fresh F24 events against
  // capacity 4 leave exactly the four newest rows.
  run(runtime,
      "import { input } from 'rime:input';\n"
      "globalThis.histErrors = {};\n"
      "const expectHist = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.histErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectHist('range', () => input.keyHistory({ maxEvents: 501 }));\n"
      "expectHist('negative', () => input.keyHistory({ maxEvents: -1 }));\n"
      "expectHist('fraction', () => input.keyHistory({ maxEvents: 1.5 }));\n"
      "expectHist('notNumber', () => input.keyHistory({ maxEvents: 'x' }));\n"
      "expectHist('optionsNotObject', () => input.keyHistory('x'));\n"
      "expectHist('arity', () => input.keyHistory({}, {}));\n"
      "globalThis.histResized = input.keyHistory({ maxEvents: 4 }).capacity;\n",
      "input-keyhistory.mjs");
  run(runtime,
      "const errors = globalThis.histErrors;\n"
      "for (const key of ['range', 'negative', 'fraction', 'notNumber',\n"
      "                   'optionsNotObject', 'arity']) {\n"
      "  if (!errors[key]) throw new Error('expected a TypeError for keyHistory ' + key);\n"
      "}\n"
      "if (globalThis.histResized !== 4)\n"
      "  throw new Error('keyHistory must report the resized capacity: ' +\n"
      "                  globalThis.histResized);",
      "input-keyhistory-check.mjs");
  for (int tap = 0; tap < 3; ++tap) send_vk(VK_F24);
  run(runtime,
      "globalThis.histReport = input.keyHistory();",
      "input-keyhistory-report.mjs");
  run(runtime,
      "const report = globalThis.histReport;\n"
      "if (report.capacity !== 4)\n"
      "  throw new Error('capacity must stay 4: ' + report.capacity);\n"
      "if (report.count !== 4 || report.events.length !== 4)\n"
      "  throw new Error('six events against capacity 4 must leave four rows, got ' +\n"
      "                  report.count);\n"
      "for (const row of report.events) {\n"
      "  if (row.vk !== 135) throw new Error('history must hold only the newest F24 rows');\n"
      "  if (typeof row.down !== 'boolean' || typeof row.injected !== 'boolean' ||\n"
      "      typeof row.selfInjected !== 'boolean')\n"
      "    throw new Error('history row flags must be booleans');\n"
      "  if (typeof row.timestamp !== 'number' || typeof row.elapsed !== 'number')\n"
      "    throw new Error('history row timing must be numeric');\n"
      "}",
      "input-keyhistory-report-check.mjs");

  // --- M3: setLockState / setLockForce ---

  // Every step below flips one of the three lock toggles. Snapshot them here
  // (the same read getKeyState(key, 't') does); each section pulls its own
  // key back once it finishes and the segment's last run block pulls all
  // three once more, so a failure in a later section cannot leave an earlier
  // key turned.
  const bool caps_original =
      rime::win32::read_key_state(VK_CAPITAL, rime::win32::KeyStateType::Toggle);
  const bool numlock_original =
      rime::win32::read_key_state(VK_NUMLOCK, rime::win32::KeyStateType::Toggle);
  const bool scrolllock_original =
      rime::win32::read_key_state(VK_SCROLL, rime::win32::KeyStateType::Toggle);

  // keyboard.setLockState lives in the sdk, which is TypeScript this slice
  // cannot load (no file root, no transpile step), so the facade body below
  // mirrors sdk/src/input.ts verbatim - force first, then read, then the
  // self-injected tap - and runs against the real rime:input exports. What
  // this slice proves is the native contract that body drives: setLockForce's
  // gate order, the self-injected pass-through and the force's suppression.
  // The sdk copy itself belongs to tests/sdk.
  const std::string lock_mirror =
      "globalThis.keyboard = {\n"
      "  setLockState(keyName, state) {\n"
      "    if (typeof keyName !== 'string') {\n"
      "      throw new TypeError('keyboard.setLockState(keyName): keyName must be a string, ' +\n"
      "                          'got ' + String(keyName));\n"
      "    }\n"
      "    const key = keyName.toLowerCase();\n"
      "    if (key !== 'capslock' && key !== 'numlock' && key !== 'scrolllock') {\n"
      "      throw new TypeError('keyboard.setLockState(keyName): keyName must be capslock, ' +\n"
      "                          'numlock or scrolllock, got ' + keyName);\n"
      "    }\n"
      "    if (state !== undefined && state !== null && typeof state !== 'string') {\n"
      "      throw new TypeError('keyboard.setLockState(state): state must be a string, ' +\n"
      "                          'got ' + String(state));\n"
      "    }\n"
      "    const word = (state ?? '').toLowerCase();\n"
      "    if (word !== '' && word !== 'on' && word !== 'off' && word !== 'alwayson' &&\n"
      "        word !== 'alwaysoff') {\n"
      "      throw new TypeError('keyboard.setLockState(state): state must be ' +\n"
      "                          '\"on\"|\"off\"|\"alwaysOn\"|\"alwaysOff\", got ' + String(state));\n"
      "    }\n"
      "    if (word === 'alwayson') input.setLockForce(key, 'on');\n"
      "    else if (word === 'alwaysoff') input.setLockForce(key, 'off');\n"
      "    else input.setLockForce(key, 'neutral');\n"
      "    const want = word === 'on' || word === 'alwayson' ? true\n"
      "               : word === 'off' || word === 'alwaysoff' ? false : null;\n"
      "    if (want === null) return Promise.resolve({ changed: false });\n"
      "    if (input.getKeyState(key, 't') === want) {\n"
      "      return Promise.resolve({ changed: false });\n"
      "    }\n"
      "    const vk = input.getKeyVK(key);\n"
      "    const steps = [];\n"
      "    if (input.getKeyState(key, 'l')) steps.push({ vk, down: false });\n"
      "    steps.push({ vk, down: true }, { vk, down: false });\n"
      "    return input.send(steps).then(() => ({ changed: true }));\n"
      "  }\n"
      "};\n";

  // One key's pull-back: setLockState(word) is the path the sdk uses, so it
  // also clears any armed force (neutral first) before it taps; the C++ read
  // then proves the LED really settled on the snapshot value.
  const auto restore_lock = [&](const char* key, const std::uint32_t vk, const bool original,
                                const char* filename) {
    run(runtime,
        "import { input } from 'rime:input';\n" + lock_mirror +
            "globalThis.lockRestore = null;\n"
            "globalThis.lockRestoreError = null;\n"
            "(async () => {\n"
            "  globalThis.lockRestore =\n"
            "      await keyboard.setLockState('" +
            key + "', " + (original ? "'on'" : "'off'") + ");\n"
            "})().then(() => {}, e => { globalThis.lockRestoreError = String(e); });",
        filename);
    assert(runtime.settle(15000ms).ok());
    run(runtime,
        "if (globalThis.lockRestoreError)\n"
        "  throw new Error('setLockState restore failed: ' + globalThis.lockRestoreError);\n",
        std::string(filename) + "-check");
    assert(wait_for([&] {
      return rime::win32::read_key_state(vk, rime::win32::KeyStateType::Toggle) == original;
    }));
  };

  // Argument contract: the facade throws synchronously for a bad key or word,
  // and the native setLockForce throws TypeErrors of its own for a non-lock
  // key, an unknown force word, a wrong arity or a non-string argument.
  // Nothing here arms a force or moves a toggle.
  run(runtime,
      "import { input } from 'rime:input';\n" + lock_mirror +
      "globalThis.lockErrors = {};\n"
      "globalThis.forceErrors = {};\n"
      "globalThis.expectLock = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.lockErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "globalThis.expectForce = (name, fn) => {\n"
      "  try { fn(); } catch (e) { globalThis.forceErrors[name] = e instanceof TypeError; }\n"
      "};\n"
      "expectLock('keyLetter', () => keyboard.setLockState('a', 'on'));\n"
      "expectLock('keyNumber', () => keyboard.setLockState(42));\n"
      "expectLock('badWord', () => keyboard.setLockState('capslock', 'sometimes'));\n"
      "expectLock('badState', () => keyboard.setLockState('capslock', 5));\n"
      "expectForce('badWord', () => input.setLockForce('capslock', 'sometimes'));\n"
      "expectForce('letterKey', () => input.setLockForce('a', 'on'));\n"
      "expectForce('fnKey', () => input.setLockForce('f1', 'off'));\n"
      "expectForce('unknownKey', () => input.setLockForce('notakey', 'on'));\n"
      "expectForce('shortArity', () => input.setLockForce('capslock'));\n"
      "expectForce('longArity', () => input.setLockForce('capslock', 'on', 1));\n"
      "expectForce('keyNotString', () => input.setLockForce(42, 'on'));\n"
      "expectForce('forceNotString', () => input.setLockForce('capslock', 5));\n"
      "globalThis.forceExport = typeof input.setLockForce === 'function';\n",
      "input-setlock-validate.mjs");
  run(runtime,
      "for (const key of ['keyLetter', 'keyNumber', 'badWord', 'badState']) {\n"
      "  if (!globalThis.lockErrors[key])\n"
      "    throw new Error('expected a TypeError for setLockState ' + key);\n"
      "}\n"
      "for (const key of ['badWord', 'letterKey', 'fnKey', 'unknownKey', 'shortArity',\n"
      "                   'longArity', 'keyNotString', 'forceNotString']) {\n"
      "  if (!globalThis.forceErrors[key])\n"
      "    throw new Error('expected a TypeError for setLockForce ' + key);\n"
      "}\n"
      "if (!globalThis.forceExport)\n"
      "  throw new Error('input.setLockForce must be exported');\n",
      "input-setlock-validate-check.mjs");

  // on/off and the changed flag (capslock): a tap only happens when the LED
  // differs, so the second identical call must report unchanged; the mixed
  // case spelling proves key names stay case-insensitive. The toggle settles
  // asynchronously, hence the poll after every tap.
  run(runtime,
      "import { input } from 'rime:input';\n" + lock_mirror +
      "globalThis.capsFlow = null;\n"
      "globalThis.capsFlowError = null;\n"
      "(async () => {\n"
      "  const out = {};\n"
      "  out.flat = await keyboard.setLockState('capslock', 'off');\n"
      "  out.flatSettled =\n"
      "      await waitFor(() => input.getKeyState('capslock', 't') === false, 3000);\n"
      "  out.on = await keyboard.setLockState('capslock', 'on');\n"
      "  out.onSettled =\n"
      "      await waitFor(() => input.getKeyState('capslock', 't') === true, 3000);\n"
      "  out.onAgain = await keyboard.setLockState('capslock', 'on');\n"
      "  out.off = await keyboard.setLockState('capslock', 'off');\n"
      "  out.offSettled =\n"
      "      await waitFor(() => input.getKeyState('capslock', 't') === false, 3000);\n"
      "  out.mixedCase = await keyboard.setLockState('CapsLock', 'on');\n"
      "  out.mixedSettled =\n"
      "      await waitFor(() => input.getKeyState('capslock', 't') === true, 3000);\n"
      "  out.mixedAgain = await keyboard.setLockState('CapsLock', 'on');\n"
      "  return out;\n"
      "})().then(v => { globalThis.capsFlow = v; },\n"
      "          e => { globalThis.capsFlowError = String(e); });",
      "input-setlock-caps.mjs");
  assert(runtime.settle(25000ms).ok());
  run(runtime,
      "if (globalThis.capsFlowError)\n"
      "  throw new Error('capslock flow failed: ' + globalThis.capsFlowError);\n"
      "const caps = globalThis.capsFlow;\n"
      "if (!caps) throw new Error('capslock flow never settled');\n"
      "if (typeof caps.flat.changed !== 'boolean')\n"
      "  throw new Error('setLockState must resolve { changed: boolean }');\n"
      "if (caps.flatSettled !== true) throw new Error('capslock must settle to off');\n"
      "if (caps.on.changed !== true)\n"
      "  throw new Error('first on must report changed: ' + JSON.stringify(caps.on));\n"
      "if (caps.onSettled !== true) throw new Error('capslock must settle to on');\n"
      "if (caps.onAgain.changed !== false)\n"
      "  throw new Error('second on must report unchanged: ' + JSON.stringify(caps.onAgain));\n"
      "if (caps.off.changed !== true)\n"
      "  throw new Error('off must report changed: ' + JSON.stringify(caps.off));\n"
      "if (caps.offSettled !== true) throw new Error('capslock must settle back to off');\n"
      "if (caps.mixedCase.changed !== true)\n"
      "  throw new Error('the CapsLock spelling must be accepted: ' +\n"
      "                  JSON.stringify(caps.mixedCase));\n"
      "if (caps.mixedSettled !== true) throw new Error('capslock must settle to on');\n"
      "if (caps.mixedAgain.changed !== false)\n"
      "  throw new Error('the second CapsLock on must be unchanged');\n",
      "input-setlock-caps-check.mjs");
  restore_lock("capslock", VK_CAPITAL, caps_original, "input-setlock-caps-restore.mjs");

  // Held key (scrolllock): a lock key that is down cannot move its LED until
  // the injected steps release it first (AHK's KEYUP-then-DOWNANDUP,
  // keyboard_mouse.cpp:2976-2990). The foreign press itself toggled the LED,
  // so the target is whatever the toggle is not after it lands - which makes
  // changed===true deterministic.
  send_key_state(VK_SCROLL, true);
  run(runtime,
      "import { input } from 'rime:input';\n" + lock_mirror +
      "globalThis.heldLock = null;\n"
      "globalThis.heldLockError = null;\n"
      "(async () => {\n"
      "  const out = {};\n"
      "  out.held = await waitFor(() => input.getKeyState('scrolllock', 'l'), 3000);\n"
      "  const target = !input.getKeyState('scrolllock', 't');\n"
      "  out.target = target;\n"
      "  out.result = await keyboard.setLockState('scrolllock', target ? 'on' : 'off');\n"
      "  out.settled = await waitFor(\n"
      "      () => input.getKeyState('scrolllock', 't') === out.target, 3000);\n"
      "  out.up = await waitFor(() => !input.getKeyState('scrolllock', 'l'), 3000);\n"
      "  return out;\n"
      "})().then(v => { globalThis.heldLock = v; },\n"
      "          e => { globalThis.heldLockError = String(e); });",
      "input-setlock-held.mjs");
  assert(runtime.settle(25000ms).ok());
  // Release before asserting so a failed tap can never leave the key stuck
  // down on this machine.
  send_key_state(VK_SCROLL, false);
  run(runtime,
      "if (globalThis.heldLockError)\n"
      "  throw new Error('held scrolllock flow failed: ' + globalThis.heldLockError);\n"
      "const held = globalThis.heldLock;\n"
      "if (!held) throw new Error('held scrolllock flow never settled');\n"
      "if (!held.held) throw new Error('scrolllock must read down while it is held');\n"
      "if (held.result.changed !== true)\n"
      "  throw new Error('a held lock key tap must report changed: ' +\n"
      "                  JSON.stringify(held.result));\n"
      "if (!held.settled)\n"
      "  throw new Error('the release-then-tap sequence must flip the toggle');\n"
      "if (!held.up)\n"
      "  throw new Error('the injected steps must release the key first');\n",
      "input-setlock-held-check.mjs");
  restore_lock("scrolllock", VK_SCROLL, scrolllock_original,
               "input-setlock-held-restore.mjs");

  // alwaysOn + suppression end to end (numlock): arm the force, prove a
  // foreign tap cannot move the toggle, release it through the "" form and
  // prove the next foreign tap can - so the suppression really came from the
  // force and not from something else.
  run(runtime,
      "import { input } from 'rime:input';\n" + lock_mirror +
      "globalThis.numArm = null;\n"
      "globalThis.numArmError = null;\n"
      "(async () => {\n"
      "  const out = {};\n"
      "  out.arm = await keyboard.setLockState('numlock', 'alwaysOn');\n"
      "  out.on = await waitFor(() => input.getKeyState('numlock', 't') === true, 3000);\n"
      "  return out;\n"
      "})().then(v => { globalThis.numArm = v; },\n"
      "          e => { globalThis.numArmError = String(e); });",
      "input-setlock-numlock-arm.mjs");
  assert(runtime.settle(25000ms).ok());
  run(runtime,
      "if (globalThis.numArmError)\n"
      "  throw new Error('numlock alwaysOn failed: ' + globalThis.numArmError);\n"
      "if (!globalThis.numArm || globalThis.numArm.on !== true)\n"
      "  throw new Error('numlock must settle to on under alwaysOn');\n",
      "input-setlock-numlock-arm-check.mjs");
  send_foreign_key(VK_NUMLOCK);
  std::this_thread::sleep_for(400ms);
  run(runtime,
      "globalThis.numSwallowed = input.getKeyState('numlock', 't');\n"
      "keyboard.setLockState('numlock', '');\n",
      "input-setlock-numlock-neutral.mjs");
  run(runtime,
      "if (globalThis.numSwallowed !== true)\n"
      "  throw new Error('alwaysOn must swallow a foreign tap, toggle was ' +\n"
      "                  globalThis.numSwallowed);\n",
      "input-setlock-numlock-swallow-check.mjs");
  send_foreign_key(VK_NUMLOCK);
  run(runtime,
      "globalThis.numReleased = 'pending';\n"
      "waitFor(() => input.getKeyState('numlock', 't') === false, 3000)\n"
      "  .then(v => { globalThis.numReleased = v; });",
      "input-setlock-numlock-release.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.numReleased !== true)\n"
      "  throw new Error('a neutral numlock must flip on a foreign tap');\n",
      "input-setlock-numlock-release-check.mjs");
  restore_lock("numlock", VK_NUMLOCK, numlock_original, "input-setlock-numlock-restore.mjs");

  // alwaysOff (scrolllock), the mirror image: armed, a foreign tap cannot
  // turn the LED on; neutral, the next one can.
  run(runtime,
      "import { input } from 'rime:input';\n" + lock_mirror +
      "globalThis.scrollArm = null;\n"
      "globalThis.scrollArmError = null;\n"
      "(async () => {\n"
      "  const out = {};\n"
      "  out.arm = await keyboard.setLockState('scrolllock', 'alwaysOff');\n"
      "  out.off =\n"
      "      await waitFor(() => input.getKeyState('scrolllock', 't') === false, 3000);\n"
      "  return out;\n"
      "})().then(v => { globalThis.scrollArm = v; },\n"
      "          e => { globalThis.scrollArmError = String(e); });",
      "input-setlock-scrolllock-arm.mjs");
  assert(runtime.settle(25000ms).ok());
  run(runtime,
      "if (globalThis.scrollArmError)\n"
      "  throw new Error('scrolllock alwaysOff failed: ' + globalThis.scrollArmError);\n"
      "if (!globalThis.scrollArm || globalThis.scrollArm.off !== true)\n"
      "  throw new Error('scrolllock must settle to off under alwaysOff');\n",
      "input-setlock-scrolllock-arm-check.mjs");
  send_foreign_key(VK_SCROLL);
  std::this_thread::sleep_for(400ms);
  run(runtime,
      "globalThis.scrollSwallowed = input.getKeyState('scrolllock', 't');\n"
      "keyboard.setLockState('scrolllock', '');\n",
      "input-setlock-scrolllock-neutral.mjs");
  run(runtime,
      "if (globalThis.scrollSwallowed !== false)\n"
      "  throw new Error('alwaysOff must swallow a foreign tap, toggle was ' +\n"
      "                  globalThis.scrollSwallowed);\n",
      "input-setlock-scrolllock-swallow-check.mjs");
  send_foreign_key(VK_SCROLL);
  run(runtime,
      "globalThis.scrollReleased = 'pending';\n"
      "waitFor(() => input.getKeyState('scrolllock', 't') === true, 3000)\n"
      "  .then(v => { globalThis.scrollReleased = v; });",
      "input-setlock-scrolllock-release.mjs");
  assert(runtime.settle(5000ms).ok());
  run(runtime,
      "if (globalThis.scrollReleased !== true)\n"
      "  throw new Error('a neutral scrolllock must flip on a foreign tap');\n",
      "input-setlock-scrolllock-release-check.mjs");
  restore_lock("scrolllock", VK_SCROLL, scrolllock_original,
               "input-setlock-scrolllock-restore.mjs");

  // Segment tail: pull all three back once more (each is a no-op when the
  // per-section restore already landed) and prove they read the snapshot.
  run(runtime,
      "import { input } from 'rime:input';\n" + lock_mirror +
      "globalThis.finalError = null;\n"
      "(async () => {\n"
      "  await keyboard.setLockState('capslock', " + (caps_original ? "'on'" : "'off'") + ");\n"
      "  await keyboard.setLockState('numlock', " + (numlock_original ? "'on'" : "'off'") +
      ");\n"
      "  await keyboard.setLockState('scrolllock', " +
      (scrolllock_original ? "'on'" : "'off'") + ");\n"
      "})().catch(e => { globalThis.finalError = String(e); });",
      "input-setlock-final.mjs");
  assert(runtime.settle(15000ms).ok());
  run(runtime,
      "if (globalThis.finalError)\n"
      "  throw new Error('final lock restore failed: ' + globalThis.finalError);\n",
      "input-setlock-final-check.mjs");
  assert(wait_for([&] {
    return rime::win32::read_key_state(VK_CAPITAL, rime::win32::KeyStateType::Toggle) ==
               caps_original &&
           rime::win32::read_key_state(VK_NUMLOCK, rime::win32::KeyStateType::Toggle) ==
               numlock_original &&
           rime::win32::read_key_state(VK_SCROLL, rime::win32::KeyStateType::Toggle) ==
               scrolllock_original;
  }));

  // Lifetime: a live subscription may outlive the runtime; the host event
  // queue closes on teardown so producers become no-ops.
  run(runtime, "globalThis.sid2 = input.subscribe(() => {});", "input-resubscribe.mjs");
  assert(runtime.stop().ok());
  assert(runtime.stop().ok());

  // setLockForce's capability gate, on a runtime that holds inject+read but
  // NOT windows.hook.global: alwaysOn/alwaysOff must fail synchronously with
  // the capability name (the force is validated before any state read, so it
  // fails first) while plain on/off keeps working - those need no hook gate.
  // The toggle ends on the value the M3 segment snapshotted.
  {
    rime::action::Kernel gate_kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
        std::unordered_set<std::string>{"windows.input.inject", "windows.input.read"}));
    rime::action::Dispatcher gate_dispatcher(gate_kernel,
                                             rime::action::default_dispatch_policy());
    // send() still queues through the kernel, so the gate runtime needs the
    // same executor the main runtime uses.
    assert(gate_kernel.register_executor("input.send", input_executor).ok());
    std::atomic<std::uint64_t> gate_next_action_id{0};
    rime::win32::InputModuleBinding gate_binding;
    gate_binding.service = &service;
    gate_binding.window_service = &window_service;
    gate_binding.kernel = &gate_kernel;
    gate_binding.dispatcher = &gate_dispatcher;
    gate_binding.next_action_id = &gate_next_action_id;
    rime::js::Runtime gate_runtime;
    assert(rime::win32::register_input_module(gate_runtime, &gate_binding).ok());
    assert(gate_runtime.start().ok());
    run(gate_runtime,
        "import { input } from 'rime:input';\n"
        "import { runtime } from 'rime:runtime';\n" + lock_mirror +
        "globalThis.lockPoll = async (predicate, timeoutMs) => {\n"
        "  const deadline = Date.now() + timeoutMs;\n"
        "  while (Date.now() < deadline) {\n"
        "    if (predicate()) return true;\n"
        "    await runtime.delay(20, null);\n"
        "  }\n"
        "  return predicate();\n"
        "};\n"
        "globalThis.gate = {};\n"
        "try { keyboard.setLockState('capslock', 'alwaysOn'); }\n"
        "catch (e) { globalThis.gate.hookOn = e.message; }\n"
        "try { keyboard.setLockState('capslock', 'alwaysOff'); }\n"
        "catch (e) { globalThis.gate.hookOff = e.message; }\n"
        "globalThis.gate.out = null;\n"
        "globalThis.gate.err = null;\n"
        "(async () => {\n"
        "  const out = {};\n"
        "  out.on = await keyboard.setLockState('capslock', 'on');\n"
        "  out.settled = await globalThis.lockPoll(\n"
        "      () => input.getKeyState('capslock', 't') === true, 3000);\n"
        "  out.back = await keyboard.setLockState('capslock', " +
        (caps_original ? "'on'" : "'off'") + ");\n"
        "  out.backSettled = await globalThis.lockPoll(\n"
        "      () => input.getKeyState('capslock', 't') === " +
        (caps_original ? "true" : "false") + ", 3000);\n"
        "  return out;\n"
        "})().then(v => { globalThis.gate.out = v; },\n"
        "          e => { globalThis.gate.err = String(e); });",
        "input-setlock-gate.mjs");
    assert(gate_runtime.settle(25000ms).ok());
    run(gate_runtime,
        "if (!globalThis.gate.hookOn ||\n"
        "    !globalThis.gate.hookOn.includes('windows.hook.global'))\n"
        "  throw new Error('alwaysOn must be denied with the capability name: ' +\n"
        "                  globalThis.gate.hookOn);\n"
        "if (!globalThis.gate.hookOff ||\n"
        "    !globalThis.gate.hookOff.includes('windows.hook.global'))\n"
        "  throw new Error('alwaysOff must be denied with the capability name: ' +\n"
        "                  globalThis.gate.hookOff);\n"
        "if (globalThis.gate.err)\n"
        "  throw new Error('on/off must work without the hook gate: ' + globalThis.gate.err);\n"
        "const out = globalThis.gate.out;\n"
        "if (!out) throw new Error('the ungated on/off flow never settled');\n"
        "if (typeof out.on.changed !== 'boolean')\n"
        "  throw new Error('on must resolve { changed: boolean }');\n"
        "if (out.settled !== true) throw new Error('capslock must settle to on');\n"
        "if (out.backSettled !== true) throw new Error('the pull-back must settle');\n",
        "input-setlock-gate-check.mjs");
    assert(gate_runtime.stop().ok());
  }

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
    denied_binding.window_service = &window_service;
    denied_binding.kernel = &denied_kernel;
    denied_binding.dispatcher = &denied_dispatcher;
    denied_binding.next_action_id = &denied_next_action_id;
    rime::js::Runtime denied_runtime;
    assert(rime::win32::register_input_module(denied_runtime, &denied_binding).ok());
    assert(denied_runtime.start().ok());
    run(denied_runtime,
        "import { input } from 'rime:input';\n" + lock_mirror +
        "globalThis.denied = null;\n"
        "globalThis.deniedBind = null;\n"
        "try { input.subscribe(() => {}); }\n"
        "catch (e) { globalThis.denied = e.message; }\n"
        "try { input.bind('f24', { type: 'probe.chord', capability: 'probe.chord',\n"
        "                          target: { kind: 'chord', id: 'main' } }); }\n"
        "catch (e) { globalThis.deniedBind = e.message; }\n"
        "globalThis.deniedSend = null;\n"
        "try { input.send([{ vk: 135, down: true }]); }\n"
        "catch (e) { globalThis.deniedSend = e.message; }\n"
        "globalThis.deniedMouse = null;\n"
        "try { input.mouse({ steps: [{ action: 'move', x: 1, y: 1 }] }); }\n"
        "catch (e) { globalThis.deniedMouse = e.message; }\n"
        "globalThis.deniedMods = null;\n"
        "try { input.modifiers(); }\n"
        "catch (e) { globalThis.deniedMods = e.message; }\n"
        "globalThis.deniedPos = null;\n"
        "input.mouseGetPos().then(\n"
        "  () => { globalThis.deniedPos = 'resolved'; },\n"
        "  e => { globalThis.deniedPos = { code: e.code, message: e.message }; });\n"
        "globalThis.deniedState = null;\n"
        "try { input.getKeyState('f24'); }\n"
        "catch (e) { globalThis.deniedState = e.message; }\n"
        "globalThis.deniedHistory = null;\n"
        "try { input.keyHistory(); }\n"
        "catch (e) { globalThis.deniedHistory = e.message; }\n"
        "globalThis.deniedBlock = null;\n"
        "try { input.blockInput('on'); }\n"
        "catch (e) { globalThis.deniedBlock = e.message; }\n"
        "globalThis.deniedKeyWait = null;\n"
        "input.keyWait('f24').then(\n"
        "  () => { globalThis.deniedKeyWait = 'resolved'; },\n"
        "  e => { globalThis.deniedKeyWait = { code: e.code, message: e.message }; });\n"
        "globalThis.deniedLock = null;\n"
        "try { keyboard.setLockState('capslock', 'on'); }\n"
        "catch (e) { globalThis.deniedLock = e.message; }",
        "input-deny.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    run(denied_runtime,
        "if (!globalThis.denied || !globalThis.denied.includes('windows.hook.global'))\n"
        "  throw new Error('subscribe must be denied with the capability name: ' +\n"
        "                  globalThis.denied);\n"
        "if (!globalThis.deniedBind || !globalThis.deniedBind.includes('windows.hook.global'))\n"
        "  throw new Error('bind must be denied with the capability name: ' +\n"
        "                  globalThis.deniedBind);\n"
        "if (!globalThis.deniedSend || !globalThis.deniedSend.includes('windows.input.inject'))\n"
        "  throw new Error('send must be denied with the capability name: ' +\n"
        "                  globalThis.deniedSend);\n"
        "if (!globalThis.deniedMouse || !globalThis.deniedMouse.includes('windows.input.inject'))\n"
        "  throw new Error('mouse must be denied with the capability name: ' +\n"
        "                  globalThis.deniedMouse);\n"
        "if (!globalThis.deniedMods || !globalThis.deniedMods.includes('windows.input.inject'))\n"
        "  throw new Error('modifiers must be denied with the capability name: ' +\n"
        "                  globalThis.deniedMods);\n"
        "if (!globalThis.deniedPos || globalThis.deniedPos === 'resolved' ||\n"
        "    globalThis.deniedPos.code !== 'capability_denied' ||\n"
        "    !globalThis.deniedPos.message.includes('windows.input.read'))\n"
        "  throw new Error('getPos must reject with capability_denied naming ' +\n"
        "                  'windows.input.read: ' + JSON.stringify(globalThis.deniedPos));\n"
        "if (!globalThis.deniedState || !globalThis.deniedState.includes('windows.input.read'))\n"
        "  throw new Error('getKeyState must be denied with the capability name: ' +\n"
        "                  globalThis.deniedState);\n"
        "if (!globalThis.deniedHistory || !globalThis.deniedHistory.includes('windows.input.read'))\n"
        "  throw new Error('keyHistory must be denied with the capability name: ' +\n"
        "                  globalThis.deniedHistory);\n"
        "if (!globalThis.deniedBlock || !globalThis.deniedBlock.includes('windows.input.inject'))\n"
        "  throw new Error('blockInput must be denied with the capability name: ' +\n"
        "                  globalThis.deniedBlock);\n"
        "if (!globalThis.deniedKeyWait || globalThis.deniedKeyWait === 'resolved' ||\n"
        "    globalThis.deniedKeyWait.code !== 'capability_denied' ||\n"
        "    !globalThis.deniedKeyWait.message.includes('windows.input.read'))\n"
        "  throw new Error('keyWait must reject with capability_denied naming ' +\n"
        "                  'windows.input.read: ' +\n"
        "                  JSON.stringify(globalThis.deniedKeyWait));\n"
        "if (!globalThis.deniedLock || !globalThis.deniedLock.includes('windows.input.read'))\n"
        "  throw new Error('setLockState must be denied with the capability name: ' +\n"
        "                  globalThis.deniedLock);",
        "input-deny-check.mjs");
    // The denied blockInput must stop at the capability gate: the desktop
    // is never left blocked by a rejected call. setLockForce is ungated for
    // 'neutral', so the denied setLockState got as far as the LED read -
    // nothing was armed on the way out (every force direction would need
    // windows.hook.global, which this policy does not grant).
    assert(!service.blocked());
    assert(denied_runtime.stop().ok());
  }

  assert(window_service.stop().ok());
  assert(service.stop().ok());
  assert(service.stop().ok());
  return 0;
}
