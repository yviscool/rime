// Realism: L5 - production JS wiring (rime:storage module, storage.write
// executor, real StorageService) runs a real filesystem roundtrip: mkdir,
// write, append, iniWrite and env go through the traced Action kernel and are
// read back through readText/readBytes/stat/list/iniRead/envGet/driveGet,
// with the environment variable independently re-read from the OS. Then the
// handle family (open/handleWrite/fileRead/fileSeek/fileStat/fileClose), the
// executor's payload refusals, the headless selector refusal, download's URL
// contract and a capability-less runtime that proves both gates. The fixture
// tree is removed before the asserts that could abort.

#include "rime/action/dispatcher.hpp"
#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/js_storage.hpp"
#include "rime/win32/storage.hpp"
#include "rime/win32/storage_executor.hpp"

#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;
using rime::win32::StorageService;

// Evaluates an assertion script; failures abort with the engine's message.
void check(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js check failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

// Paths travel into JS as JSON strings so backslashes survive both the C++
// literal and the JS source (same trick as registry_slice's key_json).
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

// The fixture path must survive as UTF-8 into JS; .string() on Windows is the
// ANSI code page, so the conversion is explicit.
std::string to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                          nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), needed,
                      nullptr, nullptr);
  return out;
}

bool os_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

int main() {
  // The executor claims nothing itself: WorkerService owns Lane::Worker, so
  // claiming it here would make every storage.write fail require_lane.
  wchar_t temp[MAX_PATH];
  const DWORD temp_length = GetTempPathW(MAX_PATH, temp);
  assert(temp_length > 0 && temp_length < MAX_PATH);
  const std::wstring fixture = std::wstring(temp, temp_length) + L"rime_storage_slice_" +
                               std::to_wstring(GetCurrentProcessId());
  const std::wstring denied_fixture =
      std::wstring(temp, temp_length) + L"rime_storage_slice_denied_" +
      std::to_wstring(GetCurrentProcessId());
  // A crashed earlier run with the same pid must not poison this one.
  std::error_code ignored;
  std::filesystem::remove_all(fixture, ignored);
  std::filesystem::remove_all(denied_fixture, ignored);
  assert(!os_exists(fixture));
  assert(!os_exists(denied_fixture));

  const std::string fixture_u8 = to_utf8(fixture);
  const std::string dir_json = json_string(fixture_u8);
  const std::string hello_json = json_string(fixture_u8 + "\\hello.txt");
  const std::string ini_json = json_string(fixture_u8 + "\\data.ini");
  const std::string link_json = json_string(fixture_u8 + "\\links\\target.lnk");
  wchar_t sys_dir[MAX_PATH] = {0};
  assert(GetSystemDirectoryW(sys_dir, MAX_PATH) != 0);
  const std::string kernel_json = json_string(to_utf8(sys_dir) + "\\kernel32.dll");
  const std::string never_json = json_string(fixture_u8 + "\\never.bin");
  const std::string denied_json = json_string(to_utf8(denied_fixture));

  const std::string env_name = "RIME_STORAGE_SLICE_" + std::to_string(GetCurrentProcessId());
  const std::string env_json = json_string(env_name);
  const std::string env_value = "slice-value";
  const std::string env_value_json = json_string(env_value);
  // Never set: envGet must read an unset name back as "" rather than failing.
  const std::string env_unset_json = json_string(env_name + "_UNSET");

  StorageService storage_service;

  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
                                  std::unordered_set<std::string>{"filesystem.read",
                                                                  "filesystem.write"}),
                              trace);
  const auto executor = std::make_shared<rime::win32::StorageExecutor>(storage_service);
  assert(kernel.register_executor("storage.write", executor).ok());

  std::atomic<std::uint64_t> next_action_id{0};
  rime::action::Dispatcher dispatcher(kernel, rime::action::default_dispatch_policy());
  rime::win32::StorageModuleBinding binding{&storage_service, &kernel, &dispatcher,
                                             &next_action_id};

  rime::js::Runtime runtime;
  assert(rime::win32::register_storage_module(runtime, &binding).ok());
  assert(runtime.start().ok());

  // Segment 1: the module surface is exactly the 18 documented exports, arity
  // and shape mistakes are synchronous TypeErrors rather than rejections, and
  // runSteps is the sequencer every later segment drives its chain with (one
  // recorded failure stops the chain instead of leaving an unhandled
  // rejection behind).
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "const expected = ['readText','readBytes','stat','shortcut','version','list','envGet','iniRead','driveGet',\n"
        "'open','fileRead','fileSeek','fileStat','fileClose','write','encoding','setEncoding',\n"
        "'download','selectFile','selectDir'];\n"
        "globalThis.missingSurface = expected.filter(n => typeof storage[n] !== 'function');\n"
        "globalThis.extraSurface = Object.keys(storage).filter(n => expected.indexOf(n) < 0);\n"
        "globalThis.runSteps = (holder, steps) => steps.reduce(\n"
        "  (chain, step) => chain.then(() => {\n"
        "    if (holder.error !== null) return null;\n"
        "    return Promise.resolve().then(step).catch(e => { holder.error = String(e); });\n"
        "  }), Promise.resolve());\n"
        "globalThis.syncErrors = 0;\n"
        "try { storage.readText(42); } catch (e) { if (e instanceof TypeError) globalThis"
        ".syncErrors++; }\n"
        "try { storage.open('a', 'b'); } catch (e) { if (e instanceof TypeError) globalThis"
        ".syncErrors++; }\n"
        "try { storage.write([]); } catch (e) { if (e instanceof TypeError) globalThis"
        ".syncErrors++; }\n"
        "try { storage.setEncoding('bogus'); } catch (e) { if (e instanceof TypeError) globalThis"
        ".syncErrors++; }\n"
        "try { storage.encoding(1); } catch (e) { if (e instanceof TypeError) globalThis"
        ".syncErrors++; }\n"
        "try { storage.fileRead(0, 1); } catch (e) { if (e instanceof TypeError) globalThis"
        ".syncErrors++; }\n"
        "try { storage.fileSeek(1, 1.5, 0); } catch (e) { if (e instanceof TypeError) globalThis"
        ".syncErrors++; }",
        "storage-surface.mjs");
  check(runtime,
        "if (globalThis.missingSurface.length)\n"
        "  throw new Error('missing exports: ' + globalThis.missingSurface.join(','));\n"
        "if (globalThis.extraSurface.length)\n"
        "  throw new Error('unexpected exports: ' + globalThis.extraSurface.join(','));\n"
        "if (globalThis.syncErrors !== 7)\n"
        "  throw new Error('expected 7 sync TypeErrors, got ' + globalThis.syncErrors);",
        "storage-surface-check.mjs");

  // Segment 2: the session encoding is synchronous service state; an unknown
  // name is a TypeError that leaves the session untouched.
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "globalThis.enc = [];\n"
        "globalThis.enc.push(storage.encoding().encoding);\n"
        "globalThis.enc.push(storage.setEncoding('latin1').encoding);\n"
        "globalThis.enc.push(storage.encoding().encoding);\n"
        "globalThis.encError = null;\n"
        "try { storage.setEncoding('bogus'); }\n"
        "  catch (e) { globalThis.encError = e instanceof TypeError ? String(e) : 'not a "
        "TypeError'; }\n"
        "globalThis.enc.push(storage.encoding().encoding);\n"
        "storage.setEncoding('utf-8');\n"
        "globalThis.enc.push(storage.encoding().encoding);",
        "storage-encoding.mjs");
  check(runtime,
        "const expected = ['utf-8', 'latin1', 'latin1', 'latin1', 'utf-8'];\n"
        "if (JSON.stringify(globalThis.enc) !== JSON.stringify(expected))\n"
        "  throw new Error('encoding sequence mismatch: ' + JSON.stringify(globalThis.enc));\n"
        "if (!globalThis.encError || !globalThis.encError.includes('encoding must be one of'))\n"
        "  throw new Error('bad encoding must be a TypeError: ' + globalThis.encError);",
        "storage-encoding-check.mjs");

  // Segment 3: five storage.write Actions land on disk, then the read family
  // answers from the same files.
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "const rt = globalThis.rt = {error: null, ops: []};\n"
        "const push = r => { rt.ops.push(r.op); };\n"
        "globalThis.roundtrip = globalThis.runSteps(rt, [\n"
        "  () => storage.write({op: 'mkdir', path: " + dir_json + "}).then(push),\n"
        "  () => storage.write({op: 'write', path: " + hello_json +
            ", text: 'rime storage slice'}).then(push),\n"
        "  () => storage.write({op: 'append', path: " + hello_json + ", text: ' plus "
            "append'}).then(push),\n"
        "  () => storage.write({op: 'iniWrite', path: " + ini_json +
            ", section: 'sec', key: 'key', value: 'val'}).then(push),\n"
        "  () => storage.write({op: 'env', name: " + env_json + ", value: " + env_value_json +
            "}).then(push),\n"
        "  () => storage.readText(" + hello_json + ").then(r => { rt.readBack = r.text; }),\n"
        "  () => storage.readBytes(" + hello_json + ").then(r => { rt.bytes = r.bytes; }),\n"
        "  () => storage.stat(" + hello_json + ").then(r => { rt.helloStat = r; }),\n"
        "  () => storage.list(" + dir_json + ").then(r => { rt.entries = r.entries; }),\n"
        "  () => storage.iniRead(" + ini_json + ", 'sec', 'key').then(r => { rt.iniValue = "
            "r.value; }),\n"
        "  () => storage.envGet(" + env_json + ").then(r => { rt.envValue = r.value; }),\n"
        "  () => storage.envGet(" + env_unset_json + ").then(r => { rt.envMissing = r.value; })\n"
        "]);",
        "storage-roundtrip.mjs");
  assert(runtime.settle(10000ms).ok());
  check(runtime,
        "const rt = globalThis.rt;\n"
        "if (rt.error !== null) throw new Error('roundtrip failed: ' + rt.error);\n"
        "if (rt.ops.join(',') !== 'mkdir,write,append,iniWrite,env')\n"
        "  throw new Error('op sequence mismatch: ' + rt.ops.join(','));\n"
        "if (rt.readBack !== 'rime storage slice plus append')\n"
        "  throw new Error('readText mismatch: ' + JSON.stringify(rt.readBack));\n"
        "if (rt.helloStat.size !== 30 || rt.helloStat.isDir)\n"
        "  throw new Error('stat mismatch: ' + JSON.stringify(rt.helloStat));\n"
        "if (!rt.helloStat.attrib || typeof rt.helloStat.mtimeMs !== 'number')\n"
        "  throw new Error('stat must report attrib and mtimeMs');\n"
        "if (rt.bytes.length !== rt.helloStat.size)\n"
        "  throw new Error('readBytes length must equal stat size');\n"
        "const head = 'rime storage slice';\n"
        "for (let i = 0; i < head.length; i++) {\n"
        "  if (rt.bytes[i] !== head.charCodeAt(i))\n"
        "    throw new Error('leading byte ' + i + ' is ' + rt.bytes[i]);\n"
        "}\n"
        "const tail = ' plus append';\n"
        "for (let i = 0; i < tail.length; i++) {\n"
        "  const at = rt.bytes.length - tail.length + i;\n"
        "  if (rt.bytes[at] !== tail.charCodeAt(i))\n"
        "    throw new Error('trailing byte ' + i + ' is ' + rt.bytes[at]);\n"
        "}\n"
        "if (rt.entries.length !== 2)\n"
        "  throw new Error('expected 2 entries, got ' + rt.entries.length);\n"
        "const names = rt.entries.map(e => e.name).sort();\n"
        "if (names[0] !== 'data.ini' || names[1] !== 'hello.txt')\n"
        "  throw new Error('listing mismatch: ' + names.join(','));\n"
        "if (rt.entries.some(e => e.isDir))\n"
        "  throw new Error('fixture entries must not be directories');\n"
        "if (rt.iniValue !== 'val')\n"
        "  throw new Error('iniRead mismatch: ' + JSON.stringify(rt.iniValue));\n"
        "if (rt.envValue !== " + env_value_json + ")\n"
        "  throw new Error('envGet mismatch: ' + JSON.stringify(rt.envValue));\n"
        "if (rt.envMissing !== '')\n"
        "  throw new Error('unset env must read back as an empty string');",
        "storage-roundtrip-check.mjs");

  // Segment: shortcut() decodes the fixture link; version() reads a system
  // binary; both reject shape mistakes synchronously. The .lnk is created
  // here (not in setup) so the roundtrip listing above keeps asserting
  // exactly 2 top-level entries.
  {
    const HRESULT co_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    assert(co_init == S_OK || co_init == S_FALSE || co_init == RPC_E_CHANGED_MODE);
    IShellLinkW* link = nullptr;
    assert(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW, reinterpret_cast<void**>(&link))));
    IPersistFile* persist = nullptr;
    assert(SUCCEEDED(
        link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist))));
    wchar_t link_sys[MAX_PATH] = {0};
    assert(GetSystemDirectoryW(link_sys, MAX_PATH) != 0);
    const std::wstring target_exe = std::wstring(link_sys) + L"\\notepad.exe";
    const std::wstring links_dir = fixture + L"\\links";
    std::error_code link_dir_error;
    std::filesystem::create_directories(links_dir, link_dir_error);
    assert(!link_dir_error);
    const std::wstring link_path = links_dir + L"\\target.lnk";
    assert(SUCCEEDED(link->SetPath(target_exe.c_str())));
    assert(SUCCEEDED(link->SetArguments(L"--slice")));
    assert(SUCCEEDED(persist->Save(link_path.c_str(), TRUE)));
    persist->Release();
    link->Release();
    CoUninitialize();
  }
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "globalThis.linkOut = null;\n"
        "globalThis.verOut = null;\n"
        "storage.shortcut(" +
            link_json +
            ").then(r => { globalThis.linkOut = r; }, e => { globalThis.linkOut = String(e); });\n"
            "storage.version(" +
            kernel_json +
            ").then(r => { globalThis.verOut = r; }, e => { globalThis.verOut = String(e); });\n"
            "globalThis.linkShape = false;\n"
            "globalThis.verShape = false;\n"
            "try { storage.shortcut(42); } catch (e) { globalThis.linkShape = e instanceof TypeError; }\n"
            "try { storage.version(); } catch (e) { globalThis.verShape = e instanceof TypeError; }\n",
        "storage-linkver.mjs");
  assert(runtime.settle(10000ms).ok());
  check(runtime,
        "if (!globalThis.linkShape || !globalThis.verShape)\n"
        "  throw new Error('shape mistakes must be TypeErrors');\n"
        "if (typeof globalThis.linkOut === 'string') throw new Error(globalThis.linkOut);\n"
        "if (!globalThis.linkOut || !globalThis.linkOut.target.endsWith('notepad.exe'))\n"
        "  throw new Error('shortcut target mismatch: ' + JSON.stringify(globalThis.linkOut));\n"
        "if (!globalThis.linkOut || globalThis.linkOut.args !== '--slice')\n"
        "  throw new Error('shortcut args mismatch');\n"
        "if (typeof globalThis.verOut === 'string') throw new Error(globalThis.verOut);\n"
        "if (!globalThis.verOut || !/^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$/.test(globalThis.verOut.version))\n"
        "  throw new Error('version shape mismatch: ' + JSON.stringify(globalThis.verOut));\n",
        "storage-linkver-check.mjs");
  assert(runtime.settle(10000ms).ok());
  {
    std::error_code links_cleanup_error;
    std::filesystem::remove_all(fixture + L"\\links", links_cleanup_error);
    assert(!links_cleanup_error);
  }
  // Independent observation: the Action that set the variable is also visible
  // through the process environment, not only through the JS read.
  {
    std::string observed(64, '\0');
    const DWORD got = GetEnvironmentVariableA(env_name.c_str(), observed.data(),
                                              static_cast<DWORD>(observed.size()));
    assert(got == env_value.size());
    observed.resize(got);
    assert(observed == env_value);
    assert(GetEnvironmentVariableA((env_name + "_UNSET").c_str(), nullptr, 0) == 0);
  }

  // Segment 4: driveGet answers from the OS drive table; an unknown field is
  // refused by contract before any OS call.
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "globalThis.drive = {error: null, list: null};\n"
        "globalThis.driveChain = globalThis.runSteps(globalThis.drive, [\n"
        "  () => storage.driveGet('list').then(r => { globalThis.drive.list = r.list; }),\n"
        "  () => storage.driveGet('nonsense').then(\n"
        "      () => { throw new Error('unknown drive field resolved'); },\n"
        "      e => { globalThis.drive.field = e.code + ':' + e.message; })\n"
        "]);",
        "storage-drive.mjs");
  assert(runtime.settle(10000ms).ok());
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "const d = globalThis.drive;\n"
        "if (d.error !== null) throw new Error(d.error);\n"
        "if (!Array.isArray(d.list) || d.list.length < 1)\n"
        "  throw new Error('drive list must not be empty: ' + JSON.stringify(d.list));\n"
        "if (d.list.some(x => !/^[A-Z]$/.test(x)))\n"
        "  throw new Error('drive list entries must be single letters: ' + JSON"
        ".stringify(d.list));\n"
        "if (!d.field.startsWith('invalid_contract:'))\n"
        "  throw new Error('unknown drive field must carry invalid_contract: ' + d.field);\n"
        "if (!d.field.includes('drive field must be one of'))\n"
        "  throw new Error('unknown drive field must name the rule: ' + d.field);\n"
        "globalThis.driveLetter = d.list[0];\n"
        "globalThis.driveType = {error: null};\n"
        "globalThis.driveTypeChain = globalThis.runSteps(globalThis.driveType, [\n"
        "  () => storage.driveGet('type', globalThis.driveLetter).then(\n"
        "      r => { globalThis.driveType.info = r; })\n"
        "]);",
        "storage-drive-check.mjs");
  assert(runtime.settle(10000ms).ok());
  check(runtime,
        "const t = globalThis.driveType;\n"
        "if (t.error !== null) throw new Error(t.error);\n"
        "if (!t.info || typeof t.info !== 'object')\n"
        "  throw new Error('driveGet(type) must resolve an object');\n"
        "if (t.info.letter !== globalThis.driveLetter + ':')\n"
        "  throw new Error('driveGet(type) must echo the letter: ' + JSON.stringify(t.info));\n"
        "const types = ['Unknown', 'Removable', 'Fixed', 'Network', 'CDROM', 'RAMDisk'];\n"
        "if (types.indexOf(t.info.type) < 0)\n"
        "  throw new Error('unknown drive type: ' + t.info.type);",
        "storage-drive-type-check.mjs");

  // Segment 5: the handle family. Mode "a" starts at end of file, handleWrite
  // appends through the Action pipeline, then seek/read/stat answer from the
  // same OS file.
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "const h = globalThis.h = {error: null};\n"
        "globalThis.handleChain = globalThis.runSteps(h, [\n"
        "  () => storage.open(" + hello_json + ", 'a').then(r => { h.opened = r; }),\n"
        "  () => storage.fileStat(h.opened.handle).then(r => { h.stat0 = r; }),\n"
        "  () => storage.write({op: 'handleWrite', handle: h.opened.handle, data: [0x21]})\n"
        "        .then(r => { h.writeResult = r; }),\n"
        "  () => storage.fileStat(h.opened.handle).then(r => { h.stat1 = r; }),\n"
        "  () => storage.readText(" + hello_json + ").then(r => { h.readBack = r.text; }),\n"
        "  () => storage.fileSeek(h.opened.handle, 0, 0).then(r => { h.seek0 = r; }),\n"
        "  () => storage.fileRead(h.opened.handle, 4).then(r => { h.firstFour = r; }),\n"
        "  () => storage.fileStat(h.opened.handle).then(r => { h.stat2 = r; }),\n"
        "  () => storage.fileRead(h.opened.handle, 100).then(r => { h.rest = r; })\n"
        "]);",
        "storage-handle.mjs");
  assert(runtime.settle(10000ms).ok());
  check(runtime,
        "const h = globalThis.h;\n"
        "if (h.error !== null) throw new Error('handle chain failed: ' + h.error);\n"
        "if (!h.opened || h.opened.length !== 30)\n"
        "  throw new Error('open must report the 30-byte length: ' + JSON.stringify(h.opened));\n"
        "if (h.stat0.pos !== 30 || h.stat0.length !== 30)\n"
        "  throw new Error('append mode must start at end of file: ' + JSON.stringify(h.stat0));\n"
        "if (!h.writeResult || h.writeResult.op !== 'handleWrite')\n"
        "  throw new Error('handleWrite must resolve {op}');\n"
        "if (h.stat1.pos !== 31 || h.stat1.length !== 31)\n"
        "  throw new Error('handleWrite must append one byte: ' + JSON.stringify(h.stat1));\n"
        "if (h.readBack !== 'rime storage slice plus append!')\n"
        "  throw new Error('handleWrite must be visible to readText: ' + JSON"
        ".stringify(h.readBack));\n"
        "if (h.seek0.pos !== 0)\n"
        "  throw new Error('fileSeek must report the new position');\n"
        "const want = [0x72, 0x69, 0x6d, 0x65];\n"
        "if (JSON.stringify(h.firstFour.bytes) !== JSON.stringify(want))\n"
        "  throw new Error('first four bytes mismatch: ' + JSON.stringify(h.firstFour.bytes));\n"
        "if (h.firstFour.eof)\n"
        "  throw new Error('a full 4-byte read at position 4 is not at eof');\n"
        "if (h.stat2.pos !== 4 || h.stat2.length !== 31)\n"
        "  throw new Error('fileStat must report pos 4 of 31: ' + JSON.stringify(h.stat2));\n"
        "if (!h.rest.eof || h.rest.bytes.length !== 27)\n"
        "  throw new Error('reading past the end must report eof: ' + JSON.stringify(h.rest));",
        "storage-handle-check.mjs");

  // Segment 6: the native read-count cap, then close and the two stale-handle
  // refusals it produces.
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "const c = globalThis.c = {error: null, closes: [], stale: []};\n"
        "globalThis.closeChain = globalThis.runSteps(c, [\n"
        "  () => storage.fileRead(globalThis.h.opened.handle, 100000000).then(\n"
        "      () => { throw new Error('oversized read resolved'); },\n"
        "      e => { c.cap = e.code + ':' + e.message; }),\n"
        "  () => storage.fileClose(globalThis.h.opened.handle).then(r => { c.closes.push(r); }),\n"
        "  () => storage.fileClose(globalThis.h.opened.handle).then(\n"
        "      () => { throw new Error('stale close resolved'); },\n"
        "      e => { c.stale.push(e.code + ':' + e.message); }),\n"
        "  () => storage.fileRead(globalThis.h.opened.handle, 1).then(\n"
        "      () => { throw new Error('stale read resolved'); },\n"
        "      e => { c.stale.push(e.code + ':' + e.message); })\n"
        "]);",
        "storage-handle-close.mjs");
  assert(runtime.settle(10000ms).ok());
  check(runtime,
        "const c = globalThis.c;\n"
        "if (c.error !== null) throw new Error(c.error);\n"
        "if (c.cap !== 'invalid_contract:file read count exceeds the 64 MiB limit')\n"
        "  throw new Error('read count cap must be an invalid_contract: ' + c.cap);\n"
        "if (c.closes.length !== 1)\n"
        "  throw new Error('fileClose must resolve once');\n"
        "if (c.stale.length !== 2)\n"
        "  throw new Error('expected 2 stale-handle refusals, got ' + c.stale.length);\n"
        "for (const failure of c.stale) {\n"
        "  if (failure !== 'invalid_state:file handle is not open')\n"
        "    throw new Error('stale handle must report invalid_state: ' + failure);\n"
        "}",
        "storage-handle-close-check.mjs");

  // Segment 7: the executor refuses payloads the contract does not define,
  // with each refusal inside the trace instead of a silent local throw.
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "const x = globalThis.x = {error: null, refusals: []};\n"
        "const refuse = payload => storage.write(payload).then(\n"
        "  () => { throw new Error('refused payload resolved'); },\n"
        "  e => { x.refusals.push(e.code + ':' + e.message); });\n"
        "globalThis.refuseChain = globalThis.runSteps(x, [\n"
        "  () => refuse({op: 'rename', path: " + hello_json + "}),\n"
        "  () => refuse({op: 'write', path: " + hello_json + ", text: 'x', extra: 1}),\n"
        "  () => refuse({op: 'handleWrite', handle: 0, data: [1]})\n"
        "]);",
        "storage-refusals.mjs");
  assert(runtime.settle(10000ms).ok());
  check(runtime,
        "const x = globalThis.x;\n"
        "if (x.error !== null) throw new Error(x.error);\n"
        "if (x.refusals.length !== 3)\n"
        "  throw new Error('expected 3 refusals, got ' + x.refusals.length);\n"
        "const [rename, extra, handle] = x.refusals;\n"
        "if (!rename.startsWith('invalid_contract:') ||\n"
        "    !rename.includes('does not support op: rename'))\n"
        "  throw new Error('unknown op refusal wrong: ' + rename);\n"
        "if (!extra.startsWith('invalid_contract:') ||\n"
        "    !extra.includes('does not accept field: extra'))\n"
        "  throw new Error('extra field refusal wrong: ' + extra);\n"
        "if (!handle.startsWith('invalid_contract:') ||\n"
        "    !handle.includes('requires an integer handle between 1 and 9223372036854775807'))\n"
        "  throw new Error('handle range refusal wrong: ' + handle);",
        "storage-refusals-check.mjs");

  // Segment 8: the selectors refuse headless (the cancellation check runs
  // first, then the UI pump) and download refuses a non-http URL before any
  // network work.
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "const p = globalThis.p = {error: null, refusals: []};\n"
        "const capture = e => { p.refusals.push(e.code + ':' + e.message); };\n"
        "globalThis.pickerChain = globalThis.runSteps(p, [\n"
        "  () => storage.selectFile({filter: '*.*'}).then(\n"
        "      () => { throw new Error('file selector resolved'); }, capture),\n"
        "  () => storage.selectDir().then(\n"
        "      () => { throw new Error('dir selector resolved'); }, capture),\n"
        "  () => storage.download('ftp://example.test/a', " + never_json + ").then(\n"
        "      () => { throw new Error('download resolved'); }, capture)\n"
        "]);",
        "storage-pickers.mjs");
  assert(runtime.settle(10000ms).ok());
  check(runtime,
        "const p = globalThis.p;\n"
        "if (p.error !== null) throw new Error(p.error);\n"
        "if (p.refusals.length !== 3)\n"
        "  throw new Error('expected 3 refusals, got ' + p.refusals.length);\n"
        "const [file, dir, download] = p.refusals;\n"
        "if (file !== 'invalid_state:storage selector has no UI thread')\n"
        "  throw new Error('file selector refusal wrong: ' + file);\n"
        "if (dir !== 'invalid_state:storage selector has no UI thread')\n"
        "  throw new Error('directory selector refusal wrong: ' + dir);\n"
        "if (!download.startsWith('invalid_contract:') ||\n"
        "    !download.includes('download URL must start with http:// or https://'))\n"
        "  throw new Error('download refusal wrong: ' + download);",
        "storage-pickers-check.mjs");
  {
    // The refused download never created its destination.
    std::error_code code;
    assert(!std::filesystem::exists(std::filesystem::path(fixture_u8 + "\\never.bin"), code));
  }

  // Segment 9: teardown through the same pipeline, verified before the final
  // asserts, then the runtime stops so the fixture can be removed.
  check(runtime,
        "import { storage } from 'rime:storage';\n"
        "const t = globalThis.t = {error: null, ops: [], gone: []};\n"
        "const remove = payload => storage.write(payload).then(r => { t.ops.push(r.op); });\n"
        "globalThis.teardownChain = globalThis.runSteps(t, [\n"
        "  () => remove({op: 'delete', path: " + hello_json + "}),\n"
        "  () => remove({op: 'delete', path: " + ini_json + "}),\n"
        "  () => remove({op: 'rmdir', path: " + dir_json + "}),\n"
        "  () => storage.stat(" + dir_json + ").then(\n"
        "      () => { throw new Error('removed directory still exists'); },\n"
        "      e => { t.gone.push(e.code); }),\n"
        "  () => storage.readText(" + hello_json + ").then(\n"
        "      () => { throw new Error('removed file still exists'); },\n"
        "      e => { t.gone.push(e.code); })\n"
        "]);",
        "storage-teardown.mjs");
  assert(runtime.settle(10000ms).ok());
  check(runtime,
        "const t = globalThis.t;\n"
        "if (t.error !== null) throw new Error('teardown failed: ' + t.error);\n"
        "if (t.ops.join(',') !== 'delete,delete,rmdir')\n"
        "  throw new Error('teardown op sequence mismatch: ' + t.ops.join(','));\n"
        "if (t.gone.join(',') !== 'execution_failed,execution_failed')\n"
        "  throw new Error('removed paths must report execution_failed: ' + t.gone.join(','));",
        "storage-teardown-check.mjs");

  assert(runtime.stop().ok());
  // The fixture is gone before any assert that could abort: from here on a
  // failure can no longer leave temp state behind.
  std::filesystem::remove_all(fixture, ignored);
  assert(!os_exists(fixture));

  // Twelve storage.write Actions reached the kernel: mkdir, write, append,
  // iniWrite, env, handleWrite, the three payload refusals and the three
  // teardown ops. Every one was Accepted by the queue policy (no coalescing:
  // each payload differs and each was awaited), and every one Started and
  // Finished. The read/view paths are direct service calls and never enter
  // the trace.
  assert(trace_count(trace, "storage.write", rime::core::TraceKind::ActionAccepted) == 12);
  assert(trace_count(trace, "storage.write", rime::core::TraceKind::ActionRefused) == 0);
  assert(trace_count(trace, "storage.write", rime::core::TraceKind::ActionStarted) == 12);
  assert(trace_count(trace, "storage.write", rime::core::TraceKind::ActionFinished) == 12);

  // Capability gate: an empty policy refuses both gates by name, the read
  // never reaches the trace, and the refused write records only Finished (the
  // Kernel's fail() path records no Started). The JS lane is process-wide, so
  // this runtime only starts after the first one released it.
  {
    auto denied_trace = std::make_shared<rime::core::InMemoryTrace>();
    rime::action::Kernel denied_kernel(
        std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{}),
        denied_trace);
    rime::action::Dispatcher denied_dispatcher(denied_kernel,
                                               rime::action::default_dispatch_policy());
    rime::win32::StorageModuleBinding denied_binding{&storage_service, &denied_kernel,
                                                      &denied_dispatcher, &next_action_id};
    rime::js::Runtime denied_runtime;
    assert(rime::win32::register_storage_module(denied_runtime, &denied_binding).ok());
    assert(denied_runtime.start().ok());
    check(denied_runtime,
          "import { storage } from 'rime:storage';\n"
          // A fresh Runtime has its own global, so the sequencer defined for
          // the first runtime above is not visible here.
          "globalThis.runSteps = (holder, steps) => steps.reduce(\n"
          "  (chain, step) => chain.then(() => {\n"
          "    if (holder.error !== null) return null;\n"
          "    return Promise.resolve().then(step).catch(e => { holder.error = String(e); });\n"
          "  }), Promise.resolve());\n"
          "const g = globalThis.g = {error: null, read: null, write: null};\n"
          "globalThis.denyChain = globalThis.runSteps(g, [\n"
          "  () => storage.readText(" +
              hello_json +
              ").then(\n"
              "      () => { throw new Error('denied read resolved'); },\n"
              "      e => { g.read = e.code + ':' + e.message; }),\n"
          "  () => storage.write({op: 'mkdir', path: " +
              denied_json +
              "}).then(\n"
              "      () => { throw new Error('denied write resolved'); },\n"
              "      e => { g.write = e.code + ':' + e.message; })\n"
          "]);",
          "storage-deny.mjs");
    assert(denied_runtime.settle(10000ms).ok());
    check(denied_runtime,
          "const g = globalThis.g;\n"
          "if (g.read !== 'capability_denied:required capability was not granted: "
          "filesystem.read')\n"
          "  throw new Error('read refusal wrong: ' + g.read);\n"
          "if (g.write !== 'capability_denied:required capability was not granted: "
          "filesystem.write')\n"
          "  throw new Error('write refusal wrong: ' + g.write);",
          "storage-deny-check.mjs");
    assert(denied_runtime.stop().ok());
    // The denied write never reached the executor, so the directory it
    // targeted was never created.
    assert(!os_exists(denied_fixture));
    assert(trace_count(denied_trace, "storage.write", rime::core::TraceKind::ActionAccepted) == 1);
    assert(trace_count(denied_trace, "storage.write", rime::core::TraceKind::ActionStarted) == 0);
    assert(trace_count(denied_trace, "storage.write", rime::core::TraceKind::ActionFinished) == 1);
  }

  std::filesystem::remove_all(denied_fixture, ignored);
  SetEnvironmentVariableA(env_name.c_str(), nullptr);
  assert(GetEnvironmentVariableA(env_name.c_str(), nullptr, 0) == 0);
  return 0;
}
