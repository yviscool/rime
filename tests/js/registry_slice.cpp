// Realism: L5 - production JS wiring (rime:registry module, registry.write
// executor, real RegistryService) performs a real HKCU roundtrip: create, set
// and read back a value that is also observed through raw advapi32, then
// delete it again. The fixture tree is removed before the final asserts, and a
// second capability-less runtime proves both gates.

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/js_registry.hpp"
#include "rime/win32/registry.hpp"
#include "rime/win32/registry_executor.hpp"

#include <windows.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::win32::RegistryService;

// Evaluates an assertion script; failures abort with the engine's message.
void check(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js check failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

// The fixture key is pure ASCII, so the conversion stays trivial.
std::wstring ascii_wide(const std::string& text) {
  std::wstring wide;
  wide.reserve(text.size());
  for (const char value : text) {
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(value)));
  }
  return wide;
}

// Key paths travel into JS as JSON strings so backslashes survive both the
// C++ literal and the JS source (same trick as breadth_slice's command_json).
std::string json_string(const std::string& text) {
  return rime::core::json::stringify(rime::core::json::Value::string(text));
}

std::size_t trace_count(const std::shared_ptr<rime::core::InMemoryTrace>& trace,
                        const std::string& subject, const rime::core::TraceKind kind) {
  std::size_t count = 0;
  for (const auto& entry : trace->snapshot()) {
    if (entry.subject == subject && entry.kind == kind) ++count;
  }
  return count;
}

// Independent observation: reads HKCU\<path> straight from the OS so a value
// the JS layer claims to have written is never only self-reported.
struct OsValue {
  bool found{false};
  DWORD type{0};
  std::vector<BYTE> data;
};

OsValue os_read(const std::wstring& path, const wchar_t* name) {
  OsValue out;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return out;
  }
  DWORD size = 0;
  if (RegQueryValueExW(key, name, nullptr, &out.type, nullptr, &size) == ERROR_SUCCESS) {
    out.data.assign(size, static_cast<BYTE>(0));
    DWORD capacity = size;
    if (RegQueryValueExW(key, name, nullptr, &out.type,
                         size == 0 ? nullptr : out.data.data(), &capacity) == ERROR_SUCCESS) {
      out.data.resize(capacity);
      out.found = true;
    }
  }
  RegCloseKey(key);
  return out;
}

bool os_key_exists(const std::wstring& path) {
  HKEY key = nullptr;
  const LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
  if (status == ERROR_SUCCESS) RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

// Deletes only the per-pid subtree; Software\Rime may be shared, so it stays.
void remove_fixture(const std::wstring& path) { RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }

}  // namespace

int main() {
  const std::string pid = std::to_string(GetCurrentProcessId());
  const std::string root_key = "HKCU\\Software\\Rime\\RegistryTest_" + pid;
  const std::string exec_key = root_key + "\\JsExec";
  const std::wstring root_path = ascii_wide("Software\\Rime\\RegistryTest_" + pid);
  const std::wstring exec_path = root_path + L"\\JsExec";

  const std::string root_json = json_string(root_key);
  const std::string exec_json = json_string(exec_key);
  const std::string missing_key_json = json_string(root_key + "\\NoSuchValue");

  RegistryService registry_service;
  // The fixture root is created before the runtime starts: `set` never
  // creates keys, so the payload alone decides whether a write can land.
  assert(registry_service.create_key(root_key).ok());

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
                                  std::unordered_set<std::string>{"registry.read",
                                                                  "registry.write"}),
                              trace);
  const auto executor = std::make_shared<rime::win32::RegistryExecutor>(registry_service);
  assert(kernel.register_executor("registry.write", executor).ok());

  std::atomic<std::uint64_t> next_action_id{0};
  rime::action::Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  rime::win32::RegistryModuleBinding binding{&registry_service, &kernel, &dispatcher,
                                              &next_action_id};

  rime::js::Runtime runtime;
  assert(rime::win32::register_registry_module(runtime, &binding).ok());
  assert(runtime.start().ok());

  // Segment 1: arity and key-shape failures are synchronous TypeErrors; a
  // bad hive and a missing key reject asynchronously with the service text.
  check(runtime,
        "import { registry } from 'rime:registry';\n"
        "globalThis.arityErrors = 0;\n"
        "try { registry.read(42); } catch (e) { if (e instanceof TypeError) globalThis.arityErrors++; }\n"
        "try { registry.read(''); } catch (e) { if (e instanceof TypeError) globalThis.arityErrors++; }\n"
        "try { registry.write(42); } catch (e) { if (e instanceof TypeError) globalThis.arityErrors++; }\n"
        "try { registry.write({type: 'sz', value: 'x'}); }\n"
        "  catch (e) { if (e instanceof TypeError) globalThis.arityErrors++; }\n"
        "globalThis.hiveFailure = null;\n"
        "registry.read('Software\\\\Rime\\\\Nope').then(\n"
        "  () => { globalThis.hiveFailure = 'unexpected resolution'; },\n"
        "  e => { globalThis.hiveFailure = String(e); });\n"
        "globalThis.missingFailure = null;\n"
        "registry.read(" +
            missing_key_json +
            ").then(\n"
            "  () => { globalThis.missingFailure = 'unexpected resolution'; },\n"
            "  e => { globalThis.missingFailure = String(e); });",
        "registry-arity.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.arityErrors !== 4)\n"
        "  throw new Error('expected 4 sync TypeErrors, got ' + globalThis.arityErrors);\n"
        "if (!globalThis.hiveFailure ||\n"
        "    !globalThis.hiveFailure.includes('registry key must start with HKLM'))\n"
        "  throw new Error('bad hive must name the hive rule: ' + globalThis.hiveFailure);\n"
        "if (!globalThis.missingFailure ||\n"
        "    !globalThis.missingFailure.includes('registry key does not exist'))\n"
        "  throw new Error('missing key must reject: ' + globalThis.missingFailure);",
        "registry-arity-check.mjs");

  // Segment 2: createKey makes the child key (OS observed below).
  check(runtime,
        "import { registry } from 'rime:registry';\n"
        "globalThis.failure = null;\n"
        "globalThis.created = null;\n"
        "registry.write({op: 'createKey', key: " +
            exec_json +
            "})\n"
            "  .then(r => { globalThis.created = r; },\n"
            "        e => { globalThis.failure = String(e); });",
        "registry-create.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (!globalThis.created || globalThis.created.op !== 'createKey')\n"
        "  throw new Error('createKey must resolve {key, op}');\n"
        "if (globalThis.created.key !== " +
            exec_json + ") throw new Error('createKey must echo the key');",
        "registry-create-check.mjs");
  assert(os_key_exists(exec_path));

  // Segment 3: set writes through the Action pipeline, read answers it back.
  const std::string slice_value = "rime-slice-\xE6\xB5\x8B\xE8\xAF\x95";
  check(runtime,
        "import { registry } from 'rime:registry';\n"
        "globalThis.failure = null;\n"
        "globalThis.settled = null;\n"
        "registry.write({op: 'set', key: " +
            exec_json + ", name: 'greeting', type: 'sz', value: '" + slice_value + "'})\n"
            "  .then(r => { globalThis.settled = r; },\n"
            "        e => { globalThis.failure = String(e); });",
        "registry-set.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (!globalThis.settled || globalThis.settled.op !== 'set')\n"
        "  throw new Error('set must resolve {key, op}');\n"
        "if (globalThis.settled.key !== " + exec_json + ") throw new Error('set must echo the key');",
        "registry-set-check.mjs");
  {
    const OsValue observed = os_read(exec_path, L"greeting");
    assert(observed.found);
    assert(observed.type == REG_SZ);
  }

  check(runtime,
        "import { registry } from 'rime:registry';\n"
        "globalThis.failure = null;\n"
        "globalThis.stored = null;\n"
        "registry.read(" +
            exec_json + ", 'greeting')\n"
            "  .then(r => { globalThis.stored = r; },\n"
            "        e => { globalThis.failure = String(e); });",
        "registry-read.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (!globalThis.stored || globalThis.stored.type !== 'sz')\n"
        "  throw new Error('read must report type sz: ' + JSON.stringify(globalThis.stored));\n"
        "if (globalThis.stored.value !== '" +
            slice_value + "') throw new Error('read value mismatch');",
        "registry-read-check.mjs");

  // Segment 4: view is a synchronous service state; an unknown view is a
  // TypeError before it can change anything.
  check(runtime,
        "import { registry } from 'rime:registry';\n"
        "globalThis.viewError = null;\n"
        "globalThis.views = [];\n"
        "globalThis.views.push(registry.view().view);\n"
        "globalThis.views.push(registry.setView('64').view);\n"
        "globalThis.views.push(registry.view().view);\n"
        "try { registry.setView('native'); }\n"
        "  catch (e) { globalThis.viewError = e instanceof TypeError ? String(e) : 'not type error'; }\n"
        "registry.setView('default');\n"
        "globalThis.views.push(registry.view().view);",
        "registry-view.mjs");
  check(runtime,
        "const expected = ['default', '64', '64', 'default'];\n"
        "if (JSON.stringify(globalThis.views) !== JSON.stringify(expected))\n"
        "  throw new Error('view sequence mismatch: ' + JSON.stringify(globalThis.views));\n"
        "if (!globalThis.viewError || !globalThis.viewError.includes('setView'))\n"
        "  throw new Error('bad view must be a TypeError: ' + globalThis.viewError);",
        "registry-view-check.mjs");

  // Segment 5: executor payload validation rejects as an invalid_contract
  // Action result (code and message both readable from JS), with no state
  // change behind it.
  check(runtime,
        "import { registry } from 'rime:registry';\n"
        "globalThis.opFailure = null;\n"
        "globalThis.opCode = null;\n"
        "registry.write({op: 'rename', key: " +
            exec_json +
            "})\n"
            "  .then(() => { globalThis.opFailure = 'unexpected resolution'; },\n"
            "        e => { globalThis.opFailure = String(e);\n"
            "               globalThis.opCode = e.code; });",
        "registry-bad-op.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (!globalThis.opFailure ||\n"
        "    !globalThis.opFailure.includes('does not support op: rename'))\n"
        "  throw new Error('wrong rejection: ' + globalThis.opFailure);\n"
        "if (globalThis.opCode !== 'invalid_contract')\n"
        "  throw new Error('payload refusal must carry invalid_contract: ' + globalThis.opCode);",
        "registry-bad-op-check.mjs");

  // Segment 6: delete the value, observe target_gone on read, then deleteKey.
  check(runtime,
        "import { registry } from 'rime:registry';\n"
        "globalThis.failure = null;\n"
        "globalThis.deleted = null;\n"
        "registry.write({op: 'delete', key: " +
            exec_json + ", name: 'greeting'})\n"
            "  .then(r => { globalThis.deleted = r; },\n"
            "        e => { globalThis.failure = String(e); });",
        "registry-delete.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (!globalThis.deleted || globalThis.deleted.op !== 'delete')\n"
        "  throw new Error('delete must resolve {key, op}');",
        "registry-delete-check.mjs");
  assert(!os_read(exec_path, L"greeting").found);

  check(runtime,
        "import { registry } from 'rime:registry';\n"
        "globalThis.goneFailure = null;\n"
        "registry.read(" +
            exec_json + ", 'greeting')\n"
            "  .then(() => { globalThis.goneFailure = 'unexpected resolution'; },\n"
            "        e => { globalThis.goneFailure = String(e); });",
        "registry-gone.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (!globalThis.goneFailure ||\n"
        "    !globalThis.goneFailure.includes('registry value does not exist: greeting'))\n"
        "  throw new Error('deleted value must report target gone: ' + globalThis.goneFailure);",
        "registry-gone-check.mjs");

  check(runtime,
        "import { registry } from 'rime:registry';\n"
        "globalThis.failure = null;\n"
        "globalThis.removed = null;\n"
        "registry.write({op: 'deleteKey', key: " +
            exec_json +
            "})\n"
            "  .then(r => { globalThis.removed = r; },\n"
            "        e => { globalThis.failure = String(e); });",
        "registry-delete-key.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.failure) throw new Error(globalThis.failure);\n"
        "if (!globalThis.removed || globalThis.removed.op !== 'deleteKey')\n"
        "  throw new Error('deleteKey must resolve {key, op}');",
        "registry-delete-key-check.mjs");
  assert(!os_key_exists(exec_path));

  // Fixture teardown before the asserts that could abort: from here on a
  // failure can no longer leave registry state behind.
  remove_fixture(root_path);

  // Five registry.write Actions reached the kernel: createKey, set, the
  // rejected rename, delete and deleteKey. The read/view paths are direct
  // service calls and never enter the trace (documented in registry.md).
  assert(trace_count(trace, "registry.write", rime::core::TraceKind::ActionStarted) == 5);
  assert(trace_count(trace, "registry.write", rime::core::TraceKind::ActionFinished) == 5);

  assert(runtime.stop().ok());

  // Capability gate: an empty policy rejects both gates by name. The JS lane
  // is process-wide, so this runtime only starts after the first one released
  // it (same rule as the window slice).
  {
    rime::action::Kernel denied_kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
        std::unordered_set<std::string>{}));
    rime::action::Dispatcher denied_dispatcher(denied_kernel,
                                               rime::action::default_dispatch_policy());
    rime::win32::RegistryModuleBinding denied_binding{&registry_service, &denied_kernel,
                                                       &denied_dispatcher, &next_action_id};
    rime::js::Runtime denied_runtime;
    assert(rime::win32::register_registry_module(denied_runtime, &denied_binding).ok());
    assert(denied_runtime.start().ok());
    check(denied_runtime,
          "import { registry } from 'rime:registry';\n"
          "globalThis.readDenied = null;\n"
          "globalThis.writeDenied = null;\n"
          "registry.read(" +
              root_json +
              ").then(() => {}, e => { globalThis.readDenied = String(e); });\n"
              "registry.write({op: 'createKey', key: " +
              exec_json +
              "}).then(() => {}, e => { globalThis.writeDenied = String(e); });",
          "registry-deny.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    check(denied_runtime,
          "if (!globalThis.readDenied ||\n"
          "    !globalThis.readDenied.includes('registry.read'))\n"
          "  throw new Error('read must name the capability: ' + globalThis.readDenied);\n"
          "if (!globalThis.writeDenied ||\n"
          "    !globalThis.writeDenied.includes('registry.write'))\n"
          "  throw new Error('write must name the capability: ' + globalThis.writeDenied);",
          "registry-deny-check.mjs");
    assert(denied_runtime.stop().ok());
    // The denied write never reached the executor, so the fixture subtree it
    // targeted is still absent (it was removed above).
    assert(!os_key_exists(exec_path));
  }
  return 0;
}
