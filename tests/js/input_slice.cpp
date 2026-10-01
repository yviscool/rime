#include "rime/js/runtime.hpp"
#include "rime/win32/input.hpp"
#include "rime/win32/js_input.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

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
  assert(SendInput(2, inputs, sizeof(INPUT)) == 2);
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
  assert(SendInput(1, &input, sizeof(INPUT)) == 1);
}

void run(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js step failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

}  // namespace

int main() {
  InputService service;
  assert(service.start().ok());

  rime::win32::InputModuleBinding binding;
  binding.service = &service;
  rime::js::Runtime runtime;
  assert(rime::win32::register_input_module(runtime, &binding).ok());
  assert(runtime.start().ok());

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
  assert(service.stop().ok());
  assert(service.stop().ok());
  return 0;
}
