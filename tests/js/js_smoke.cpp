// Realism: L4 - the real QuickJS host loads and evaluates modules
// in process with deterministic exit and asserted failure messages; no OS
// side effects beyond the script's own process exit codes.

#include "rime/js/host.hpp"

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

void drain_until_idle(rime::js::Host& host, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!host.idle() && std::chrono::steady_clock::now() < deadline) {
    host.drain();
    std::this_thread::sleep_for(5ms);
  }
  host.drain();
  assert(host.idle());
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc >= 2);
  const std::string fixture_root = argv[1];

  rime::js::Host host;

  // The registry refuses empty and duplicate native registrations with
  // InvalidContract instead of letting load()'s first-match lookup silently
  // shadow the second one ("rime:runtime" is already registered by the Host
  // constructor).
  const auto empty_registration =
      host.modules().add_native("", [](JSContext*) -> JSModuleDef* { return nullptr; });
  assert(!empty_registration.ok());
  assert(empty_registration.code == rime::core::Error::Code::InvalidContract);
  const auto null_factory_registration = host.modules().add_native("rime:test:nullfactory", {});
  assert(!null_factory_registration.ok());
  assert(null_factory_registration.code == rime::core::Error::Code::InvalidContract);
  const auto duplicate_registration =
      host.modules().add_native("rime:runtime", [](JSContext*) -> JSModuleDef* {
        return nullptr;
      });
  assert(!duplicate_registration.ok());
  assert(duplicate_registration.code == rime::core::Error::Code::InvalidContract);
  assert(duplicate_registration.message.find("rime:runtime") != std::string::npos);

  // Native module binding through the module loader.
  assert(host.eval_module("import { runtime } from 'rime:runtime';\n"
                          "if (runtime.ping() !== 'pong') throw new Error('bad native binding');")
             .ok());

  // Exceptions surface with location and message and are recorded for inspect.
  const auto error = host.eval("throw new Error('expected failure');", "smoke.mjs");
  assert(error.code == rime::core::Error::Code::ExecutionFailed);
  assert(error.message.find("expected failure") != std::string::npos);
  assert(host.errors().size() == 1);
  assert(host.errors()[0].where == "smoke.mjs");

  // Host::record is the diagnostic channel for failures that have no promise
  // to settle: it returns an ExecutionFailed error and lands in errors() and
  // inspect so the CLI can read what the pump could not report.
  const std::size_t record_mark = host.error_count();
  const auto recorded = host.record("rime:test.channel", "no promise to settle");
  assert(recorded.code == rime::core::Error::Code::ExecutionFailed);
  assert(recorded.message == "no promise to settle");
  assert(host.error_count() == record_mark + 1);
  assert(host.new_error_since(record_mark) == "no promise to settle");
  assert(host.inspect(R"({"kind":"errors"})").find("rime:test.channel") != std::string::npos);

  // File-based ES modules are confined to the configured root.
  assert(host.modules().set_file_root(fixture_root).ok());
  assert(host.eval_module("import { answer } from './answer.mjs';\n"
                          "globalThis.fromModule = answer;",
                          "entry.mjs")
             .ok());
  assert(host.eval("if (fromModule !== 42) throw new Error('file module mismatch');")
             .ok());
  assert(host.modules().loaded_files().size() == 1);

  // Escaping the file root is refused by the normalizer.
  const auto escape = host.eval_module("import '../outside.mjs';", "escape.mjs");
  assert(!escape.ok());
  assert(escape.message.find("file root") != std::string::npos);

  // async delay resolves through the completion queue.
  assert(host.eval_module("import { runtime } from 'rime:runtime';\n"
                          "runtime.delay(20, { ok: true }).then(v => { globalThis.delayed = v.ok; });",
                          "delay.mjs")
             .ok());
  assert(!host.idle());
  drain_until_idle(host, 2s);
  assert(host.eval("if (delayed !== true) throw new Error('delay not applied');").ok());

  // cancellation rejects the promise and disarms its timer.
  assert(host.eval_module("import { runtime } from 'rime:runtime';\n"
                          "const id = runtime.cancellation();\n"
                          "globalThis.cancelled = 'pending';\n"
                          "runtime.delay(500, 1, id).then(\n"
                          "  () => { globalThis.cancelled = false; },\n"
                          "  () => { globalThis.cancelled = true; });\n"
                          "runtime.cancel(id);",
                          "cancel.mjs")
             .ok());
  drain_until_idle(host, 2s);
  assert(host.eval("if (cancelled !== true) throw new Error('cancellation failed');").ok());
  assert(host.timers().pending() == 0);

  // subscription lifecycle is tracked and unsubscribe removes it.
  assert(host.eval_module("import { runtime } from 'rime:runtime';\n"
                          "globalThis.sid = runtime.subscribe(() => {});",
                          "subscribe.mjs")
             .ok());
  assert(host.subscriptions().size() == 1);
  assert(host.eval_module("import { runtime } from 'rime:runtime';\n"
                          "globalThis.unsub = (id) => runtime.unsubscribe(id);",
                          "unsubscribe.mjs")
             .ok());
  assert(host.eval("if (!unsub(sid)) throw new Error('unsubscribe failed');").ok());
  assert(host.subscriptions().size() == 0);

  // inspect reports live state without executing script.
  const std::string report = host.inspect(R"({"kind":"all"})");
  assert(report.find("\"rime:runtime\"") != std::string::npos);
  assert(report.find("answer.mjs") != std::string::npos);
  assert(report.find("\"subscriptions\"") != std::string::npos);
  assert(report.find("expected failure") != std::string::npos);

  return 0;
}
