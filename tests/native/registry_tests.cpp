// Realism: L5 - real HKCU registry roundtrip through the real Kernel and the
// real registry executor. The test creates a per-pid subtree under
// HKCU\Software\Rime, observes every write back through raw advapi32 calls
// (never through the service under test), and deletes the subtree before the
// first assert that could abort.

#include "rime/action/kernel.hpp"
#include "rime/core/lane.hpp"
#include "rime/win32/registry.hpp"
#include "rime/win32/registry_executor.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using rime::win32::RegValue;
using rime::win32::RegistryService;

std::uint64_t deadline_ms() {
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         5000;
}

// The fixture key is pure ASCII (HKCU\Software\Rime\RegistryTest_<pid>), so
// the conversions below stay trivial instead of pulling utf.hpp in.
std::wstring ascii_wide(const std::string& text) {
  std::wstring wide;
  wide.reserve(text.size());
  for (const char value : text) {
    wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(value)));
  }
  return wide;
}

// Escapes backslashes for a payload the fixture path is pasted into.
std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char value : text) {
    if (value == '\\') out += "\\\\";
    else out += value;
  }
  return out;
}

RegValue sz_value(std::string text) {
  RegValue value;
  value.type = "sz";
  value.value_sz = std::move(text);
  return value;
}

// Observation straight from the OS: opens HKCU\<subkey> with raw advapi32 and
// returns the value's type and bytes, so assertions about what was written do
// not round-trip through the service being tested.
struct OsValue {
  bool found{false};
  DWORD type{0};
  std::vector<BYTE> data;
};

OsValue os_read(const std::wstring& subkey, const wchar_t* name) {
  OsValue out;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
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

bool os_write_type(const std::wstring& subkey, const wchar_t* name, const DWORD type) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                      &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const DWORD payload = 0;
  const LONG status = RegSetValueExW(key, name, 0, type,
                                     reinterpret_cast<const BYTE*>(&payload), sizeof(payload));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool os_key_exists(const std::wstring& subkey) {
  HKEY key = nullptr;
  const LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &key);
  if (status == ERROR_SUCCESS) RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

// Deletes only the per-pid subtree. Software\Rime is shared with other tests
// (and may pre-exist), so it is never removed here.
void remove_fixture(const std::wstring& subkey) { RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str()); }

// Reads a value through the service, keeping only what the assert phase needs.
bool service_read(const RegistryService& service, const std::string& key, const std::string& name,
                  RegValue& out) {
  out = RegValue{};
  return service.read(key, name, out).ok();
}

}  // namespace

int main() {
  const std::string pid = std::to_string(GetCurrentProcessId());
  const std::string root_key = "HKCU\\Software\\Rime\\RegistryTest_" + pid;
  const std::string sub_key = root_key + "\\Sub";
  const std::wstring root_path = ascii_wide("Software\\Rime\\RegistryTest_" + pid);
  const std::wstring sub_path = root_path + L"\\Sub";
  const std::wstring exec_path = root_path + L"\\Exec";

  RegistryService service;
  // Executors require the worker lane; this harness executes on the main thread.
  assert(rime::core::LaneRegistry::instance().claim(rime::core::Lane::Worker).ok());

  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
      std::unordered_set<std::string>{"registry.write"}));
  const auto executor = std::make_shared<rime::win32::RegistryExecutor>(service);
  assert(kernel.register_executor("registry.write", executor).ok());

  // --- Capture phase ---------------------------------------------------
  // Everything below mutates the real registry WITHOUT asserting: assert()
  // aborts and runs no destructors, so a failure here would leave the fixture
  // behind. Every outcome is recorded, the fixture is deleted, and only then
  // are they asserted.

  // Key parsing is a pure function: hive aliases, long forms and case are
  // accepted; empty keys, foreign paths and empty segments are refused with
  // the three fixed messages.
  rime::win32::RegKeyParts parsed;
  const auto parse_short = rime::win32::parse_registry_key(sub_key, parsed);
  const bool parse_short_ok =
      parse_short.ok() && parsed.hive == "HKCU" &&
      parsed.subkey == "Software\\Rime\\RegistryTest_" + pid + "\\Sub";
  const auto parse_long = rime::win32::parse_registry_key("hkey_local_machine\\Software\\Rime", parsed);
  const bool parse_long_ok =
      parse_long.ok() && parsed.hive == "HKLM" && parsed.subkey == "Software\\Rime";
  const auto parse_hive = rime::win32::parse_registry_key("HKCC", parsed);
  const bool parse_hive_ok = parse_hive.ok() && parsed.hive == "HKCC" && parsed.subkey.empty();
  const auto empty_key_error = rime::win32::parse_registry_key("", parsed);
  const bool empty_key_rejected =
      empty_key_error.code == rime::core::Error::Code::InvalidContract &&
      empty_key_error.message == "registry key must not be empty";
  const auto foreign_key_error = rime::win32::parse_registry_key("Software\\Rime", parsed);
  const bool foreign_key_rejected =
      foreign_key_error.code == rime::core::Error::Code::InvalidContract &&
      foreign_key_error.message == "registry key must start with HKLM, HKCU, HKCR, HKU or HKCC";
  const auto segment_error = rime::win32::parse_registry_key("HKCU\\\\Software", parsed);
  const bool segment_rejected =
      segment_error.code == rime::core::Error::Code::InvalidContract &&
      segment_error.message == "registry key must not contain an empty path segment";

  const bool created_root = service.create_key(root_key).ok();
  const bool created_sub = service.create_key(sub_key).ok();
  const bool created_again = service.create_key(sub_key).ok();  // create is idempotent

  const auto wrote_default = service.write_value(root_key, "", sz_value("rime-registry-default"));
  RegValue read_value;
  const bool read_default =
      service_read(service, root_key, "", read_value) && read_value.type == "sz" &&
      read_value.value_sz == "rime-registry-default";
  const OsValue os_default = os_read(root_path, L"");

  const std::string cjk = "rime-\xE6\xB5\x8B\xE8\xAF\x95-\xE2\x9C\x93";
  // Same text as UTF-16 code units, spelled with hex escapes so the source
  // file stays pure ASCII while the expected size stays independent of it.
  const std::wstring cjk_wide = L"rime-\x6D4B\x8BD5-\x2713";
  const bool wrote_cjk = service.write_value(sub_key, "text", sz_value(cjk)).ok();
  const bool read_cjk =
      service_read(service, sub_key, "text", read_value) && read_value.value_sz == cjk;
  const OsValue os_cjk = os_read(sub_path, L"text");

  RegValue expand;
  expand.type = "expand_sz";
  expand.value_sz = "%SystemRoot%";
  const bool wrote_expand = service.write_value(sub_key, "expand", expand).ok();
  const bool read_expand =
      service_read(service, sub_key, "expand", read_value) && read_value.type == "expand_sz" &&
      read_value.value_sz == "%SystemRoot%";

  RegValue dword;
  dword.type = "dword";
  dword.value_int = 42;
  const bool wrote_dword = service.write_value(sub_key, "dword", dword).ok();
  const bool read_dword =
      service_read(service, sub_key, "dword", read_value) && read_value.type == "dword" &&
      read_value.value_int == 42;
  const OsValue os_dword = os_read(sub_path, L"dword");

  RegValue qword;
  qword.type = "qword";
  qword.value_int = 4294967296ull;  // 2^32: proves qword is not narrowed to DWORD
  const bool wrote_qword = service.write_value(sub_key, "qword", qword).ok();
  const bool read_qword =
      service_read(service, sub_key, "qword", read_value) && read_value.type == "qword" &&
      read_value.value_int == 4294967296ull;
  const OsValue os_qword = os_read(sub_path, L"qword");

  RegValue multi;
  multi.type = "multi_sz";
  multi.value_multi = {"alpha", "beta"};
  const bool wrote_multi = service.write_value(sub_key, "multi", multi).ok();
  const bool read_multi =
      service_read(service, sub_key, "multi", read_value) && read_value.type == "multi_sz" &&
      read_value.value_multi == std::vector<std::string>{"alpha", "beta"};
  const OsValue os_multi = os_read(sub_path, L"multi");

  RegValue binary;
  binary.type = "binary";
  binary.value_bin = {0xDE, 0xAD, 0xBE, 0xEF};
  const bool wrote_binary = service.write_value(sub_key, "binary", binary).ok();
  const bool read_binary =
      service_read(service, sub_key, "binary", read_value) && read_value.type == "binary" &&
      read_value.value_bin == std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF};
  const OsValue os_binary = os_read(sub_path, L"binary");

  // Payload guards that refuse before touching the key.
  RegValue too_wide = dword;
  too_wide.value_int = 4294967296ull;
  const auto dword_range_error = service.write_value(sub_key, "wide", too_wide);
  RegValue empty_element = multi;
  empty_element.value_multi = {"alpha", "", "beta"};
  const auto empty_element_error = service.write_value(sub_key, "empty", empty_element);

  // Missing key / missing value both surface as TargetGone with fixed text.
  const auto missing_key_read = service.read(root_key + "\\NoSuchKey", "", read_value);
  const auto missing_value_read = service.read(sub_key, "missing", read_value);
  const auto missing_key_write =
      service.write_value(root_key + "\\NoSuchKey", "v", sz_value("x"));

  // Delete one value: first success, then TargetGone on a repeat.
  const bool deleted_value = service.delete_value(sub_key, "expand").ok();
  const auto deleted_again = service.read(sub_key, "expand", read_value);
  const auto missing_value_delete = service.delete_value(sub_key, "missing");

  // A key with subkeys is refused with an explicit message instead of the
  // Win32 partial-delete rule; an empty key deletes cleanly.
  const bool created_child = service.create_key(sub_key + "\\Child").ok();
  const auto subkeys_error = service.delete_key(sub_key);
  const bool created_leaf = service.create_key(root_key + "\\Leaf").ok();
  const bool deleted_leaf = service.delete_key(root_key + "\\Leaf").ok();
  const auto missing_key_delete = service.delete_key(root_key + "\\NoSuchKey");
  const auto hive_root_delete = service.delete_key("HKCU");

  // View selection: valid values stick, anything else is refused.
  const bool set_view_ok = service.set_view("64").ok() && service.view() == "64";
  const auto bad_view_error = service.set_view("native");
  const bool reset_view_ok = service.set_view("default").ok() && service.view() == "default";

  // A registry type this runtime does not carry: written with raw Win32 so
  // the Unsupported path is fed by the OS, not by our own encoder.
  const bool wrote_foreign_type = os_write_type(sub_path, L"foreign", 100);
  const auto foreign_type_read = service.read(sub_key, "foreign", read_value);

  // --- Executor round trip --------------------------------------------
  rime::action::Action create_action;
  create_action.id = 1;
  create_action.source = {"test", "registry_tests"};
  create_action.type = "registry.write";
  create_action.capability = "registry.write";
  create_action.target = {"registry", root_key + "\\Exec"};
  create_action.deadline_unix_ms = deadline_ms();
  create_action.payload = "{\"op\":\"createKey\",\"key\":\"" + json_escape(root_key + "\\Exec") +
                          "\"}";
  const auto create_result = kernel.execute(create_action);

  rime::action::Action set_action = create_action;
  set_action.id = 2;
  set_action.target = {"registry", root_key + "\\Exec"};
  set_action.payload = "{\"op\":\"set\",\"key\":\"" + json_escape(root_key + "\\Exec") +
                       "\",\"name\":\"via-executor\",\"type\":\"sz\",\"value\":\"executed\"}";
  const auto set_result = kernel.execute(set_action);
  const OsValue os_executed = os_read(exec_path, L"via-executor");

  // A policy without the capability refuses before the executor runs, so the
  // value stays exactly what the accepted Action wrote (observed after the
  // refusal, while the key and value still exist).
  rime::action::Kernel denied(
      std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{}));
  const bool denied_registered = denied.register_executor("registry.write", executor).ok();
  rime::action::Action denied_set = set_action;
  denied_set.id = 14;
  denied_set.payload = "{\"op\":\"set\",\"key\":\"" + json_escape(root_key + "\\Exec") +
                       "\",\"name\":\"via-executor\",\"type\":\"sz\",\"value\":\"denied-write\"}";
  const auto refused = denied.execute(denied_set);
  RegValue after_denied;
  const bool denied_untouched =
      service_read(service, root_key + "\\Exec", "via-executor", after_denied) &&
      after_denied.value_sz == "executed";

  rime::action::Action delete_value_action = create_action;
  delete_value_action.id = 3;
  delete_value_action.payload = "{\"op\":\"delete\",\"key\":\"" + json_escape(root_key + "\\Exec") +
                                "\",\"name\":\"via-executor\"}";
  const auto delete_value_result = kernel.execute(delete_value_action);
  const OsValue os_deleted = os_read(exec_path, L"via-executor");

  rime::action::Action delete_key_action = create_action;
  delete_key_action.id = 4;
  delete_key_action.payload =
      "{\"op\":\"deleteKey\",\"key\":\"" + json_escape(root_key + "\\Exec") + "\"}";
  const auto delete_key_result = kernel.execute(delete_key_action);
  const bool exec_key_gone = !os_key_exists(exec_path);

  // set never creates a key: the refusal and the untouched path are both
  // observed (the OS still has no such key afterwards).
  rime::action::Action set_missing = create_action;
  set_missing.id = 5;
  set_missing.target = {"registry", root_key + "\\Never"};
  set_missing.payload = "{\"op\":\"set\",\"key\":\"" + json_escape(root_key + "\\Never") +
                        "\",\"type\":\"sz\",\"value\":\"x\"}";
  const auto set_missing_result = kernel.execute(set_missing);
  const bool never_created = !os_key_exists(root_path + L"\\Never");

  // --- Contract violations --------------------------------------------
  rime::action::Action missing_op = create_action;
  missing_op.id = 6;
  missing_op.payload = "{\"key\":\"" + json_escape(root_key) + "\"}";
  const auto missing_op_result = kernel.execute(missing_op);

  rime::action::Action wrong_target = create_action;
  wrong_target.id = 7;
  wrong_target.target = {"registry", "some-other-key"};
  wrong_target.payload = "{\"op\":\"createKey\",\"key\":\"" + json_escape(root_key) + "\"}";
  const auto wrong_target_result = kernel.execute(wrong_target);

  // Each guard below must be the one that fires, so the target id has to agree
  // with the payload key first: the executor checks target/key agreement
  // before it ever looks at op, type or value.
  rime::action::Action unknown_op = create_action;
  unknown_op.id = 8;
  unknown_op.target = {"registry", root_key};
  unknown_op.payload = "{\"op\":\"rename\",\"key\":\"" + json_escape(root_key) + "\"}";
  const auto unknown_op_result = kernel.execute(unknown_op);

  rime::action::Action unknown_type = create_action;
  unknown_type.id = 9;
  unknown_type.target = {"registry", sub_key};
  unknown_type.payload = "{\"op\":\"set\",\"key\":\"" + json_escape(sub_key) +
                         "\",\"type\":\"nope\",\"value\":\"x\"}";
  const auto unknown_type_result = kernel.execute(unknown_type);

  rime::action::Action bad_value = create_action;
  bad_value.id = 10;
  bad_value.target = {"registry", sub_key};
  bad_value.payload = "{\"op\":\"set\",\"key\":\"" + json_escape(sub_key) +
                      "\",\"type\":\"sz\",\"value\":7}";
  const auto bad_value_result = kernel.execute(bad_value);

  rime::action::Action no_value = create_action;
  no_value.id = 11;
  no_value.target = {"registry", sub_key};
  no_value.payload = "{\"op\":\"set\",\"key\":\"" + json_escape(sub_key) +
                     "\",\"type\":\"sz\"}";
  const auto no_value_result = kernel.execute(no_value);

  rime::action::Action name_on_delete_key = create_action;
  name_on_delete_key.id = 12;
  name_on_delete_key.target = {"registry", root_key + "\\Sub"};
  name_on_delete_key.payload = "{\"op\":\"deleteKey\",\"key\":\"" + json_escape(root_key + "\\Sub") +
                               "\",\"name\":\"x\"}";
  const auto name_on_delete_key_result = kernel.execute(name_on_delete_key);

  rime::action::Action wrong_kind = create_action;
  wrong_kind.id = 13;
  wrong_kind.target = {"window", root_key};
  wrong_kind.payload = "{\"op\":\"createKey\",\"key\":\"" + json_escape(root_key) + "\"}";
  const auto wrong_kind_result = kernel.execute(wrong_kind);

  // --- Cleanup ---------------------------------------------------------
  // From here on a failing assert can no longer leave registry state behind.
  remove_fixture(root_path);

  // --- Assert phase ----------------------------------------------------
  assert(parse_short_ok);
  assert(parse_long_ok);
  assert(parse_hive_ok);
  assert(empty_key_rejected);
  assert(foreign_key_rejected);
  assert(segment_rejected);

  assert(created_root);
  assert(created_sub);
  assert(created_again);

  assert(wrote_default.ok());
  assert(read_default);
  assert(os_default.found);
  assert(os_default.type == REG_SZ);
  // The OS bytes are the UTF-16 text plus its NUL terminator.
  assert(os_default.data.size() == sizeof(L"rime-registry-default"));

  assert(wrote_cjk);
  assert(read_cjk);
  assert(os_cjk.found);
  assert(os_cjk.data.size() == (cjk_wide.size() + 1) * sizeof(wchar_t));

  assert(wrote_expand);
  assert(read_expand);

  assert(wrote_dword);
  assert(read_dword);
  assert(os_dword.found);
  assert(os_dword.type == REG_DWORD);
  assert(os_dword.data.size() == sizeof(DWORD));
  {
    DWORD observed = 0;
    assert(os_dword.data.size() == sizeof(observed));
    for (std::size_t index = 0; index < sizeof(observed); ++index) {
      observed |= static_cast<DWORD>(os_dword.data[index]) << (8 * index);
    }
    assert(observed == 42);
  }

  assert(wrote_qword);
  assert(read_qword);
  assert(os_qword.found);
  assert(os_qword.type == REG_QWORD);
  assert(os_qword.data.size() == sizeof(std::uint64_t));
  {
    std::uint64_t observed = 0;
    for (std::size_t index = 0; index < sizeof(observed); ++index) {
      observed |= static_cast<std::uint64_t>(os_qword.data[index]) << (8 * index);
    }
    assert(observed == 4294967296ull);
  }

  assert(wrote_multi);
  assert(read_multi);
  assert(os_multi.found);
  assert(os_multi.type == REG_MULTI_SZ);
  assert(os_multi.data.size() == (sizeof(L"alpha") + sizeof(L"beta")) + sizeof(wchar_t));
  {
    // "alpha\0beta\0\0" as UTF-16 bytes, rebuilt independently of the service.
    const wchar_t expected[] = L"alpha\0beta";
    std::vector<BYTE> want;
    want.reserve(sizeof(expected));
    const auto* bytes = reinterpret_cast<const BYTE*>(expected);
    want.insert(want.end(), bytes, bytes + sizeof(expected));
    want.insert(want.end(), sizeof(wchar_t), 0);
    assert(os_multi.data == want);
  }

  assert(wrote_binary);
  assert(read_binary);
  assert(os_binary.found);
  assert(os_binary.type == REG_BINARY);
  assert(os_binary.data == std::vector<BYTE>({0xDE, 0xAD, 0xBE, 0xEF}));

  assert(!dword_range_error.ok());
  assert(dword_range_error.code == rime::core::Error::Code::InvalidContract);
  assert(dword_range_error.message == "registry dword value is out of range: 0..4294967295");
  assert(!empty_element_error.ok());
  assert(empty_element_error.code == rime::core::Error::Code::InvalidContract);
  assert(empty_element_error.message == "registry multi_sz element must not be empty");

  assert(!missing_key_read.ok());
  assert(missing_key_read.code == rime::core::Error::Code::TargetGone);
  assert(missing_key_read.message == "registry key does not exist: " + root_key + "\\NoSuchKey");
  assert(!missing_value_read.ok());
  assert(missing_value_read.code == rime::core::Error::Code::TargetGone);
  assert(missing_value_read.message == "registry value does not exist: missing");
  assert(!missing_key_write.ok());
  assert(missing_key_write.code == rime::core::Error::Code::TargetGone);

  assert(deleted_value);
  assert(!deleted_again.ok());
  assert(deleted_again.code == rime::core::Error::Code::TargetGone);
  assert(deleted_again.message == "registry value does not exist: expand");
  assert(!missing_value_delete.ok());
  assert(missing_value_delete.code == rime::core::Error::Code::TargetGone);

  assert(created_child);
  assert(!subkeys_error.ok());
  assert(subkeys_error.code == rime::core::Error::Code::ExecutionFailed);
  assert(subkeys_error.message == "registry key still has subkeys: " + sub_key);
  assert(created_leaf);
  assert(deleted_leaf);
  assert(!os_key_exists(root_path + L"\\Leaf"));
  assert(!missing_key_delete.ok());
  assert(missing_key_delete.code == rime::core::Error::Code::TargetGone);
  assert(!hive_root_delete.ok());
  assert(hive_root_delete.code == rime::core::Error::Code::InvalidContract);
  assert(hive_root_delete.message == "registry hive root cannot be deleted: HKCU");

  assert(set_view_ok);
  assert(!bad_view_error.ok());
  assert(bad_view_error.code == rime::core::Error::Code::InvalidContract);
  assert(bad_view_error.message == "registry view must be \"default\", \"64\" or \"32\"");
  assert(reset_view_ok);

  assert(wrote_foreign_type);
  assert(!foreign_type_read.ok());
  assert(foreign_type_read.code == rime::core::Error::Code::Unsupported);
  assert(foreign_type_read.message == "registry value type is not supported: 100");

  assert(create_result.succeeded);
  assert(set_result.succeeded);
  assert(set_result.detail == "registry updated");
  assert(os_executed.found);
  assert(os_executed.type == REG_SZ);
  assert(delete_value_result.succeeded);
  assert(!os_deleted.found);
  assert(delete_key_result.succeeded);
  assert(exec_key_gone);

  assert(!set_missing_result.succeeded);
  assert(set_missing_result.error.code == rime::core::Error::Code::TargetGone);
  assert(set_missing_result.error.message ==
         "registry key does not exist: " + root_key + "\\Never");
  assert(never_created);

  assert(!missing_op_result.succeeded);
  assert(missing_op_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(missing_op_result.error.message == "registry.write payload requires a string op");
  assert(!wrong_target_result.succeeded);
  assert(wrong_target_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(wrong_target_result.error.message ==
         "registry.write payload target id must match the payload key");
  assert(!unknown_op_result.succeeded);
  assert(unknown_op_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(unknown_op_result.error.message == "registry.write payload does not support op: rename");
  assert(!unknown_type_result.succeeded);
  assert(unknown_type_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(unknown_type_result.error.message == "registry.write payload does not support type: nope");
  assert(!bad_value_result.succeeded);
  assert(bad_value_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(bad_value_result.error.message ==
         "registry.write payload value must be a string for type sz");
  assert(!no_value_result.succeeded);
  assert(no_value_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(no_value_result.error.message == "registry.write payload requires a value for op set");
  assert(!name_on_delete_key_result.succeeded);
  assert(name_on_delete_key_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(name_on_delete_key_result.error.message ==
         "registry.write payload op deleteKey does not accept name, value or type");
  assert(!wrong_kind_result.succeeded);
  assert(wrong_kind_result.error.code == rime::core::Error::Code::InvalidContract);
  assert(wrong_kind_result.error.message.find("target kind 'registry'") != std::string::npos);

  assert(denied_registered);
  assert(!refused.succeeded);
  assert(refused.error.code == rime::core::Error::Code::CapabilityDenied);
  assert(denied_untouched);
  return 0;
}
