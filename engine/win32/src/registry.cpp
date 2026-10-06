#include "rime/win32/registry.hpp"

#include "utf.hpp"

#include <windows.h>

#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

// Fixed English failure texts. JS, the SDK, the tests and docs/api/registry.md
// match on these strings, so nothing here interpolates localised Win32 text.
// Only the explicitly numbered cases append a Win32 status number.
constexpr const char* kEmptyKey = "registry key must not be empty";
constexpr const char* kUnknownHive = "registry key must start with HKLM, HKCU, HKCR, HKU or HKCC";
constexpr const char* kEmptySegment = "registry key must not contain an empty path segment";
constexpr const char* kBadView = "registry view must be \"default\", \"64\" or \"32\"";
constexpr const char* kHasSubkeysPrefix = "registry key still has subkeys: ";
constexpr const char* kHiveRootPrefix = "registry hive root cannot be deleted: ";
constexpr const char* kDwordRange = "registry dword value is out of range: 0..4294967295";
constexpr const char* kMalformed = "registry read returned a malformed value";
constexpr const char* kEmbeddedNul = "registry value must not contain an embedded NUL";
constexpr const char* kEmptyMultiElement = "registry multi_sz element must not be empty";

struct Hive {
  const char* alias;
  const char* long_form;
  HKEY root;
};

// Short alias first: parse_registry_key canonicalises to it, so every lookup
// afterwards is an exact ASCII compare. The predefined HKEY handles are not
// constexpr-able (they are integer-to-pointer casts), hence const, not
// constexpr.
const Hive kHives[] = {
    {"HKLM", "HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE},
    {"HKCU", "HKEY_CURRENT_USER", HKEY_CURRENT_USER},
    {"HKCR", "HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT},
    {"HKU", "HKEY_USERS", HKEY_USERS},
    {"HKCC", "HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG},
};

// ASCII-only case folding: hive aliases are a fixed uppercase vocabulary, so
// neither <algorithm> nor the locale machinery is involved.
char ascii_lower(const char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool iequals(const std::string_view left, const std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (ascii_lower(left[index]) != ascii_lower(right[index])) return false;
  }
  return true;
}

int hive_index(const std::string_view token) {
  for (std::size_t index = 0; index < sizeof(kHives) / sizeof(kHives[0]); ++index) {
    if (iequals(token, kHives[index].alias) || iequals(token, kHives[index].long_form)) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

HKEY root_hkey(const std::string& hive) {
  for (const Hive& entry : kHives) {
    if (hive == entry.alias) return entry.root;
  }
  return nullptr;
}

// RAII for the HKEYs this file opens. RegCloseKey runs on every scope exit,
// including the error paths, so no handle can outlive the call that made it.
class RegKeyGuard final {
 public:
  RegKeyGuard() = default;

  RegKeyGuard(const RegKeyGuard&) = delete;
  RegKeyGuard& operator=(const RegKeyGuard&) = delete;

  explicit RegKeyGuard(HKEY key) : key_(key) {}
  RegKeyGuard(RegKeyGuard&& other) noexcept : key_(std::exchange(other.key_, nullptr)) {}
  RegKeyGuard& operator=(RegKeyGuard&& other) noexcept {
    if (this != &other) {
      reset();
      key_ = std::exchange(other.key_, nullptr);
    }
    return *this;
  }

  ~RegKeyGuard() { reset(); }

  void reset() {
    if (key_) {
      RegCloseKey(key_);
      key_ = nullptr;
    }
  }

  [[nodiscard]] HKEY get() const { return key_; }

 private:
  HKEY key_{nullptr};
};

// Session view -> WOW64 flag applied to every open in this call. "default"
// means the OS default (64-bit view for a 64-bit process), hence no flag.
REGSAM view_flag(const std::string& view) {
  if (view == "64") return KEY_WOW64_64KEY;
  if (view == "32") return KEY_WOW64_32KEY;
  return 0;
}

Error missing_key(const std::string& key) {
  return {Code::TargetGone, "registry key does not exist: " + key};
}

Error missing_value(const std::string& name) {
  const std::string label = name.empty() ? std::string("(default)") : name;
  return {Code::TargetGone, "registry value does not exist: " + label};
}

Error unsupported_type(const std::string& token) {
  return {Code::Unsupported, "registry value type is not supported: " + token};
}

Error win32_error(const char* operation, const LONG status) {
  return {Code::ExecutionFailed, std::string("registry ") + operation +
                                     " failed (Win32 error " + std::to_string(status) + ")"};
}

// UTF-8 -> UTF-16 for a key, name or value. from_utf8 returns an empty string
// for empty input *and* for a failed conversion, so a non-empty input that
// produced nothing is rejected instead of silently resolving to a shorter
// path (an empty subkey would address the hive root itself).
Error wide_path(const std::string& text, const char* what, std::wstring& out) {
  out = from_utf8(text);
  if (out.empty() && !text.empty()) {
    return {Code::InvalidContract,
            std::string("registry ") + what + " is not valid UTF-8: " + text};
  }
  return Error::none();
}

// Opens key + subkey with `access` plus the current session view. A missing
// key or path becomes the stable "key does not exist" text; every other
// status carries its Win32 number.
Error open_key(const HKEY root, const std::wstring& subkey, const REGSAM access, const REGSAM view,
               const std::string& key, HKEY& out) {
  out = nullptr;
  const LONG status =
      RegOpenKeyExW(root, subkey.empty() ? nullptr : subkey.c_str(), 0, access | view, &out);
  if (status == ERROR_SUCCESS) return Error::none();
  out = nullptr;
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return missing_key(key);
  return win32_error("open", status);
}

// REG_* constant for a type name this runtime carries. Every name is ASCII so
// the comparison is a plain string equality.
bool win32_type(const std::string& type, DWORD& out) {
  if (type == "sz") {
    out = REG_SZ;
    return true;
  }
  if (type == "expand_sz") {
    out = REG_EXPAND_SZ;
    return true;
  }
  if (type == "binary") {
    out = REG_BINARY;
    return true;
  }
  if (type == "dword") {
    out = REG_DWORD;
    return true;
  }
  if (type == "multi_sz") {
    out = REG_MULTI_SZ;
    return true;
  }
  if (type == "qword") {
    out = REG_QWORD;
    return true;
  }
  return false;
}

// Encodes a RegValue into the byte buffer RegSetValueExW takes. Runs before
// the key is opened so an impossible payload is rejected with no side effect.
Error encode_value(const RegValue& value, DWORD& type, std::vector<BYTE>& data) {
  data.clear();
  if (!win32_type(value.type, type)) return unsupported_type(value.type);

  if (value.type == "sz" || value.type == "expand_sz") {
    const std::wstring wide = from_utf8(value.value_sz);
    if (wide.empty() && !value.value_sz.empty()) {
      return {Code::InvalidContract,
              std::string("registry value is not valid UTF-8: ") + value.value_sz};
    }
    if (wide.find(L'\0') != std::wstring::npos) return {Code::InvalidContract, kEmbeddedNul};
    // resize value-initialises, so the terminating NUL(s) are already zeroed.
    data.resize((wide.size() + 1) * sizeof(wchar_t));
    if (!wide.empty()) std::memcpy(data.data(), wide.data(), wide.size() * sizeof(wchar_t));
    return Error::none();
  }

  if (value.type == "dword") {
    if (value.value_int > 0xFFFFFFFFull) return {Code::InvalidContract, kDwordRange};
    const DWORD dword = static_cast<DWORD>(value.value_int);
    data.resize(sizeof(DWORD));
    std::memcpy(data.data(), &dword, sizeof(dword));
    return Error::none();
  }

  if (value.type == "qword") {
    data.resize(sizeof(std::uint64_t));
    std::memcpy(data.data(), &value.value_int, sizeof(std::uint64_t));
    return Error::none();
  }

  if (value.type == "multi_sz") {
    std::wstring wide;
    for (const std::string& item : value.value_multi) {
      const std::wstring part = from_utf8(item);
      if (part.empty() && !item.empty()) {
        return {Code::InvalidContract,
                std::string("registry value is not valid UTF-8: ") + item};
      }
      // An empty element would read back as the list terminator, and an
      // embedded NUL as a shorter element: both lose data on round-trip.
      if (part.empty()) return {Code::InvalidContract, kEmptyMultiElement};
      if (part.find(L'\0') != std::wstring::npos) return {Code::InvalidContract, kEmbeddedNul};
      wide.append(part);
      wide.push_back(L'\0');
    }
    wide.push_back(L'\0');
    data.resize(wide.size() * sizeof(wchar_t));
    if (!wide.empty()) std::memcpy(data.data(), wide.data(), data.size());
    return Error::none();
  }

  // binary: the payload bytes are stored verbatim (an empty list writes a
  // zero-length value, which RegSetValueExW accepts with a null pointer).
  data.assign(value.value_bin.begin(), value.value_bin.end());
  return Error::none();
}

// Turns a queried registry value into the RegValue shape JS reads. Type names
// are lowercase and fixed; a type this runtime does not carry is Unsupported
// rather than a lossy conversion.
Error decode_value(const DWORD type, const std::vector<BYTE>& buffer, RegValue& out) {
  out = RegValue{};
  const auto malformed = [] { return Error{Code::ExecutionFailed, kMalformed}; };

  if (type == REG_SZ || type == REG_EXPAND_SZ) {
    if (buffer.size() % sizeof(wchar_t) != 0) return malformed();
    out.type = type == REG_SZ ? "sz" : "expand_sz";
    // Copy into std::wstring first: the byte buffer carries no wchar_t
    // alignment guarantee, and the view must not outlive it anyway.
    std::wstring text(buffer.size() / sizeof(wchar_t), L'\0');
    if (!text.empty()) std::memcpy(text.data(), buffer.data(), buffer.size());
    // The stored value is NUL-terminated; drop the padding so an empty string
    // reads back as "" rather than a run of NULs.
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    out.value_sz = to_utf8(text);
    return Error::none();
  }

  if (type == REG_MULTI_SZ) {
    if (buffer.size() % sizeof(wchar_t) != 0) return malformed();
    out.type = "multi_sz";
    std::wstring text(buffer.size() / sizeof(wchar_t), L'\0');
    if (!text.empty()) std::memcpy(text.data(), buffer.data(), buffer.size());
    std::size_t start = 0;
    while (start < text.size()) {
      const std::size_t end = text.find(L'\0', start);
      if (end == std::wstring::npos) {
        out.value_multi.push_back(to_utf8(text.substr(start)));
        break;
      }
      if (end == start) break;  // empty segment: end of the list
      out.value_multi.push_back(to_utf8(text.substr(start, end - start)));
      start = end + 1;
    }
    return Error::none();
  }

  if (type == REG_DWORD) {
    if (buffer.size() != sizeof(DWORD)) return malformed();
    out.type = "dword";
    DWORD dword = 0;
    std::memcpy(&dword, buffer.data(), sizeof(dword));
    out.value_int = dword;
    return Error::none();
  }

  if (type == REG_QWORD) {
    if (buffer.size() != sizeof(std::uint64_t)) return malformed();
    out.type = "qword";
    std::uint64_t qword = 0;
    std::memcpy(&qword, buffer.data(), sizeof(qword));
    out.value_int = qword;
    return Error::none();
  }

  if (type == REG_BINARY) {
    out.type = "binary";
    out.value_bin.assign(buffer.begin(), buffer.end());
    return Error::none();
  }

  return {Code::Unsupported,
          std::string("registry value type is not supported: ") + std::to_string(type)};
}

}  // namespace

Error parse_registry_key(const std::string_view key, RegKeyParts& out) {
  out = RegKeyParts{};
  if (key.empty()) return {Code::InvalidContract, kEmptyKey};

  const std::size_t first_slash = key.find('\\');
  const int hive = hive_index(key.substr(0, first_slash));
  if (hive < 0) return {Code::InvalidContract, kUnknownHive};
  out.hive = kHives[hive].alias;
  if (first_slash == std::string_view::npos) return Error::none();

  // Walk the remainder segment by segment. A doubled or trailing backslash
  // yields an empty segment, which is rejected instead of being collapsed -
  // "HKCU\\\\Software" and "HKCU\\Software" must not mean the same key.
  std::string subkey;
  std::size_t start = first_slash + 1;
  for (;;) {
    const std::size_t slash = key.find('\\', start);
    const std::size_t length = (slash == std::string_view::npos ? key.size() : slash) - start;
    if (length == 0) return {Code::InvalidContract, kEmptySegment};
    if (!subkey.empty()) subkey.push_back('\\');
    subkey.append(key.substr(start, length));
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  out.subkey = std::move(subkey);
  return Error::none();
}

Error RegistryService::read(const std::string& key, const std::string& name,
                             RegValue& out) const {
  out = RegValue{};
  RegKeyParts parts;
  if (const auto error = parse_registry_key(key, parts); !error.ok()) return error;
  const HKEY root = root_hkey(parts.hive);
  if (!root) return {Code::InvalidContract, kUnknownHive};
  std::wstring subkey;
  if (const auto error = wide_path(parts.subkey, "key", subkey); !error.ok()) return error;
  std::wstring value_name;
  if (const auto error = wide_path(name, "name", value_name); !error.ok()) return error;

  HKEY raw = nullptr;
  if (const auto error =
          open_key(root, subkey, KEY_QUERY_VALUE, view_flag(view_snapshot()), key, raw);
      !error.ok()) {
    return error;
  }
  RegKeyGuard handle(raw);

  DWORD type = 0;
  DWORD size = 0;
  LONG status = ERROR_SUCCESS;
  std::vector<BYTE> buffer;
  // First call sizes the buffer, second reads it. A value that grows between
  // the two shows up as ERROR_MORE_DATA and is retried with the reported size
  // instead of returning a truncated value.
  for (int attempt = 0; attempt < 4; ++attempt) {
    status = RegQueryValueExW(handle.get(), value_name.c_str(), nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS) break;
    buffer.assign(size, static_cast<BYTE>(0));
    DWORD capacity = size;
    status = RegQueryValueExW(handle.get(), value_name.c_str(), nullptr, &type,
                              size == 0 ? nullptr : buffer.data(), &capacity);
    if (status == ERROR_SUCCESS) {
      buffer.resize(capacity);
      break;
    }
    if (status != ERROR_MORE_DATA) break;
    size = capacity;
  }
  // The key was opened above, so a not-found here is the value, not the key.
  if (status == ERROR_FILE_NOT_FOUND) return missing_value(name);
  if (status != ERROR_SUCCESS) return win32_error("read", status);
  return decode_value(type, buffer, out);
}

Error RegistryService::write_value(const std::string& key, const std::string& name,
                                    const RegValue& value) const {
  RegKeyParts parts;
  if (const auto error = parse_registry_key(key, parts); !error.ok()) return error;
  const HKEY root = root_hkey(parts.hive);
  if (!root) return {Code::InvalidContract, kUnknownHive};
  std::wstring subkey;
  if (const auto error = wide_path(parts.subkey, "key", subkey); !error.ok()) return error;
  std::wstring value_name;
  if (const auto error = wide_path(name, "name", value_name); !error.ok()) return error;
  DWORD type = 0;
  std::vector<BYTE> data;
  // Encode first: a payload that cannot be stored must not open the key.
  if (const auto error = encode_value(value, type, data); !error.ok()) return error;

  HKEY raw = nullptr;
  if (const auto error =
          open_key(root, subkey, KEY_SET_VALUE, view_flag(view_snapshot()), key, raw);
      !error.ok()) {
    return error;
  }
  RegKeyGuard handle(raw);
  const LONG status =
      RegSetValueExW(handle.get(), value_name.c_str(), 0, type,
                     data.empty() ? nullptr : data.data(), static_cast<DWORD>(data.size()));
  if (status != ERROR_SUCCESS) return win32_error("write", status);
  return Error::none();
}

Error RegistryService::create_key(const std::string& key) const {
  RegKeyParts parts;
  if (const auto error = parse_registry_key(key, parts); !error.ok()) return error;
  const HKEY root = root_hkey(parts.hive);
  if (!root) return {Code::InvalidContract, kUnknownHive};
  std::wstring subkey;
  if (const auto error = wide_path(parts.subkey, "key", subkey); !error.ok()) return error;
  // The hive root exists by definition; creating it is not a state change.
  if (subkey.empty()) return Error::none();

  HKEY created = nullptr;
  const LONG status = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                       KEY_READ | KEY_SET_VALUE | view_flag(view_snapshot()),
                                       nullptr, &created, nullptr);
  if (status != ERROR_SUCCESS) return win32_error("create", status);
  RegKeyGuard handle(created);
  return Error::none();
}

Error RegistryService::delete_value(const std::string& key, const std::string& name) const {
  RegKeyParts parts;
  if (const auto error = parse_registry_key(key, parts); !error.ok()) return error;
  const HKEY root = root_hkey(parts.hive);
  if (!root) return {Code::InvalidContract, kUnknownHive};
  std::wstring subkey;
  if (const auto error = wide_path(parts.subkey, "key", subkey); !error.ok()) return error;
  std::wstring value_name;
  if (const auto error = wide_path(name, "name", value_name); !error.ok()) return error;

  HKEY raw = nullptr;
  if (const auto error =
          open_key(root, subkey, KEY_SET_VALUE, view_flag(view_snapshot()), key, raw);
      !error.ok()) {
    return error;
  }
  RegKeyGuard handle(raw);
  const LONG status = RegDeleteValueW(handle.get(), value_name.c_str());
  if (status == ERROR_FILE_NOT_FOUND) return missing_value(name);
  if (status != ERROR_SUCCESS) return win32_error("delete", status);
  return Error::none();
}

Error RegistryService::delete_key(const std::string& key) const {
  RegKeyParts parts;
  if (const auto error = parse_registry_key(key, parts); !error.ok()) return error;
  const HKEY root = root_hkey(parts.hive);
  if (!root) return {Code::InvalidContract, kUnknownHive};
  std::wstring subkey;
  if (const auto error = wide_path(parts.subkey, "key", subkey); !error.ok()) return error;
  if (subkey.empty()) return {Code::InvalidContract, kHiveRootPrefix + key};

  const REGSAM view = view_flag(view_snapshot());
  HKEY raw = nullptr;
  if (const auto error = open_key(root, subkey, KEY_READ, view, key, raw); !error.ok()) {
    return error;
  }
  // Enumerate index 0 first: the Win32 delete refuses a non-empty key, but
  // its own message ("access denied") does not say why, and the outcome must
  // not depend on which child would be found first.
  {
    RegKeyGuard handle(raw);
    wchar_t name[256];
    DWORD length = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
    const LONG status =
        RegEnumKeyExW(handle.get(), 0, name, &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_SUCCESS || status == ERROR_MORE_DATA) {
      return {Code::ExecutionFailed, std::string(kHasSubkeysPrefix) + key};
    }
    if (status != ERROR_NO_MORE_ITEMS) return win32_error("delete", status);
  }

  const LONG status = RegDeleteKeyW(root, subkey.c_str());
  if (status == ERROR_ACCESS_DENIED) return {Code::ExecutionFailed, std::string(kHasSubkeysPrefix) + key};
  if (status == ERROR_FILE_NOT_FOUND) return missing_key(key);
  if (status != ERROR_SUCCESS) return win32_error("delete", status);
  return Error::none();
}

std::string RegistryService::view() const {
  std::lock_guard lock(view_mutex_);
  return view_;
}

Error RegistryService::set_view(const std::string& view_name) {
  if (view_name != "default" && view_name != "64" && view_name != "32") {
    return {Code::InvalidContract, kBadView};
  }
  std::lock_guard lock(view_mutex_);
  view_ = view_name;
  return Error::none();
}

std::string RegistryService::view_snapshot() const {
  std::lock_guard lock(view_mutex_);
  return view_;
}

}  // namespace rime::win32
