#pragma once

#include "rime/core/types.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rime::win32 {

// One stored registry value in the shape the JS/JSON layer speaks. `type` is
// the lowercase registry type name: sz | expand_sz | dword | qword |
// multi_sz | binary. Text lands in value_sz, integers in value_int, string
// lists in value_multi and raw bytes in value_bin; unused members stay empty.
// Every type name is ASCII so payloads compare as plain strings.
struct RegValue {
  std::string type;
  std::string value_sz;
  std::uint64_t value_int{0};
  std::vector<std::string> value_multi;
  std::vector<std::uint8_t> value_bin;
};

// A registry key split into its hive and the path below it. `hive` is always
// one of the five canonical short aliases (HKLM, HKCU, HKCR, HKU, HKCC) and
// `subkey` has no leading backslash; the hive root itself is an empty subkey.
// HKEY types never appear here so this stays a plain, testable data type.
struct RegKeyParts {
  std::string hive;
  std::string subkey;
};

// Splits "HKCU\Software\Rime" (or the long HKEY_CURRENT_USER form, in any
// case) into hive + subkey. Pure: no OS call, no locale, ASCII-only case
// folding. The three rejection texts are fixed English strings and are the
// contract scripts and tests match on.
[[nodiscard]] rime::core::Error parse_registry_key(std::string_view key, RegKeyParts& out);

// Typed operations over the Win32 registry. Every call opens exactly what it
// needs and closes it again (RAII in the .cpp); no HKEY ever leaves the
// service. Errors are stable: a missing key or value is TargetGone, a type
// this runtime does not carry is Unsupported, everything else that reaches
// the caller is InvalidContract for payload problems or ExecutionFailed for
// Win32 failures (with the Win32 status number appended).
//
// `view` is the 32/64-bit registry view (AHK's SetRegView) held per service
// instance: reads and writes take one snapshot per call so a concurrent
// set_view() cannot split a single operation across views.
class RegistryService final {
 public:
  // Fills `out` with the stored type and value. `name` is "" for the default
  // value. A missing key or value returns TargetGone; an unsupported registry
  // type (e.g. REG_LINK) returns Unsupported.
  [[nodiscard]] rime::core::Error read(const std::string& key, const std::string& name,
                                        RegValue& out) const;

  // Stores `value` under `name` ("" = default value). The key must already
  // exist: set deliberately does not create keys, so the effect of an Action
  // is decided by its payload alone (create the key first with create_key).
  [[nodiscard]] rime::core::Error write_value(const std::string& key, const std::string& name,
                                               const RegValue& value) const;

  // Creates the key and any missing intermediate key. Existing keys succeed
  // (the Win32 create is idempotent), which is what RegCreateKey means.
  [[nodiscard]] rime::core::Error create_key(const std::string& key) const;

  // Deletes one value ("" = default value); the key itself is untouched.
  [[nodiscard]] rime::core::Error delete_value(const std::string& key,
                                                const std::string& name) const;

  // Deletes an empty key. A key with subkeys is rejected with
  // ExecutionFailed instead of relying on the Win32 partial-delete rule, so
  // the outcome never depends on which child was found first.
  [[nodiscard]] rime::core::Error delete_key(const std::string& key) const;

  // Current view: "default" (native), "64" or "32".
  [[nodiscard]] std::string view() const;

  // Selects the view for subsequent operations. Values outside
  // default|64|32 are InvalidContract.
  rime::core::Error set_view(const std::string& view);

 private:
  // Locked snapshot used by one operation; never held across an OS call.
  [[nodiscard]] std::string view_snapshot() const;

  mutable std::mutex view_mutex_;
  std::string view_{"default"};
};

}  // namespace rime::win32
