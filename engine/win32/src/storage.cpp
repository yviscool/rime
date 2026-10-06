#include "rime/win32/storage.hpp"

#include "handle_guard.hpp"
#include "utf.hpp"

#include "rime/win32/ui_thread.hpp"

#include <windows.h>

#include <objbase.h>
#include <objidl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

// Stable English failure texts. The Action result, the tests and
// docs/api/storage.md all match on these strings, so nothing here
// interpolates localised Win32 text; only Win32 and HRESULT failures append
// a status number.
constexpr const char* kHandleNotOpen = "file handle is not open";
constexpr const char* kNoUiThread = "storage selector has no UI thread";
constexpr const char* kCancelledBeforeStart = "selection was cancelled before it started";
constexpr const char* kCancelledByUser = "selection was cancelled";
constexpr const char* kReadLimitPrefix = "file is larger than the 1 GiB read limit: ";
constexpr const char* kReadCountLimit = "file read count exceeds the 64 MiB limit";
constexpr const char* kDriveNotFoundPrefix = "drive not found: ";

// Reading caps (docs/api/storage.md): whole-file reads are bounded so one
// action cannot pin the worker lane with a multi-gigabyte copy, and a handle
// read is bounded so the caller pages a long stream itself.
constexpr std::uint64_t kMaxWholeFileRead = 1ull << 30;  // 1 GiB
constexpr std::uint32_t kMaxHandleRead = 64u << 20;      // 64 MiB
constexpr std::uint32_t kDownloadChunk = 64u << 10;      // 64 KiB
// Largest unix millisecond value that still maps into the year 9999, the
// range FILETIME conversion accepts from callers.
constexpr std::int64_t kMaxUnixMs = 253402300799999ll;

Error contract_error(std::string message) { return {Code::InvalidContract, std::move(message)}; }

Error fs_error(const char* operation, const std::string& path, const DWORD status) {
  if (status == ERROR_FILE_NOT_FOUND) return {Code::ExecutionFailed, "file not found: " + path};
  if (status == ERROR_PATH_NOT_FOUND) return {Code::ExecutionFailed, "path not found: " + path};
  return {Code::ExecutionFailed, std::string(operation) + " failed for " + path +
                                     " (Win32 error " + std::to_string(status) + ")"};
}

Error win32_error(const char* operation, const DWORD status) {
  return {Code::ExecutionFailed, std::string("storage ") + operation + " failed (Win32 error " +
                                     std::to_string(status) + ")"};
}

Error hresult_error(const char* operation, const HRESULT status) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X",
                static_cast<unsigned int>(static_cast<std::uint32_t>(status)));
  return {Code::ExecutionFailed, std::string("storage ") + operation + " failed (HRESULT " + text +
                                     ")"};
}

// ASCII-only case folding: the storage vocabulary (encodings, modes, drive
// names, INI fields) is fixed ASCII, so neither <algorithm> nor locale
// machinery is involved.
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

bool iequals(const std::wstring_view left, const std::wstring_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto narrow = static_cast<char>(left[index]);
    if (narrow != left[index] || ascii_lower(narrow) != ascii_lower(static_cast<char>(right[index]))) {
      return false;
    }
  }
  return true;
}

bool starts_with_ci(const std::string& text, const std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    if (ascii_lower(text[index]) != ascii_lower(prefix[index])) return false;
  }
  return true;
}

std::int64_t now_unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// UTF-8 -> UTF-16 for a path, name or environment variable. An embedded NUL
// would silently truncate at the Win32 boundary (the wide APIs are
// NUL-terminated), so it is rejected instead of resolving to a shorter path.
Error wide_path(const std::string& text, const char* what, std::wstring& out) {
  if (text.empty()) return contract_error(std::string("storage ") + what + " must not be empty");
  out = from_utf8(text);
  if (out.empty()) {
    return contract_error(std::string("storage ") + what + " is not valid UTF-8: " + text);
  }
  if (out.find(L'\0') != std::wstring::npos) {
    return contract_error(std::string("storage ") + what + " must not contain a NUL character");
  }
  return Error::none();
}

// ---- attributes -----------------------------------------------------------

// Attribute letters follow AHK's FileAttribToStr (source/util.cpp): R A S H N
// D O C T L in that fixed order with no separators.
std::string attrib_to_string(const DWORD attributes) {
  std::string out;
  if (attributes & FILE_ATTRIBUTE_READONLY) out.push_back('R');
  if (attributes & FILE_ATTRIBUTE_ARCHIVE) out.push_back('A');
  if (attributes & FILE_ATTRIBUTE_SYSTEM) out.push_back('S');
  if (attributes & FILE_ATTRIBUTE_HIDDEN) out.push_back('H');
  if (attributes & FILE_ATTRIBUTE_NORMAL) out.push_back('N');
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) out.push_back('D');
  if (attributes & FILE_ATTRIBUTE_OFFLINE) out.push_back('O');
  if (attributes & FILE_ATTRIBUTE_COMPRESSED) out.push_back('C');
  if (attributes & FILE_ATTRIBUTE_TEMPORARY) out.push_back('T');
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) out.push_back('L');
  return out;
}

// ---- time -----------------------------------------------------------------

void filetime_to_unix_ms(const FILETIME& time, std::int64_t& out) {
  ULARGE_INTEGER value{};
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  // 100 ns ticks between 1601-01-01 and 1970-01-01.
  constexpr std::uint64_t kEpochDiff = 116444736000000000ull;
  if (value.QuadPart < kEpochDiff) {
    out = 0;
    return;
  }
  out = static_cast<std::int64_t>((value.QuadPart - kEpochDiff) / 10000ull);
}

Error unix_ms_to_filetime(const std::int64_t unix_ms, FILETIME& out) {
  if (unix_ms < 0 || unix_ms > kMaxUnixMs) {
    return contract_error("setTime unix_ms must be between 0 and 253402300799999");
  }
  constexpr std::uint64_t kEpochDiff = 116444736000000000ull;
  const std::uint64_t ticks = static_cast<std::uint64_t>(unix_ms) * 10000ull + kEpochDiff;
  out.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFull);
  out.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return Error::none();
}

// ---- session encoding -----------------------------------------------------

enum class Encoding { Utf8, Utf8Bom, Utf16, Utf16Be, Cp0, Cp1252, Latin1 };

constexpr const char* kEncodingNames[] = {"utf-8",    "utf-8-bom", "utf-16", "utf-16-be",
                                          "cp0",      "cp1252",    "latin1"};

Error parse_encoding(const std::string& text, Encoding& out) {
  for (std::size_t index = 0; index < sizeof(kEncodingNames) / sizeof(kEncodingNames[0]); ++index) {
    if (iequals(text, kEncodingNames[index])) {
      out = static_cast<Encoding>(index);
      return Error::none();
    }
  }
  return contract_error(
      "encoding must be one of utf-8, utf-8-bom, utf-16, utf-16-be, cp0, cp1252, latin1");
}

const char* encoding_name(const Encoding encoding) {
  return kEncodingNames[static_cast<std::size_t>(encoding)];
}

std::vector<std::uint8_t> bom_of(const Encoding encoding) {
  switch (encoding) {
    case Encoding::Utf8Bom:
      return {0xEF, 0xBB, 0xBF};
    case Encoding::Utf16:
      return {0xFF, 0xFE};
    case Encoding::Utf16Be:
      return {0xFE, 0xFF};
    default:
      return {};
  }
}

std::wstring utf16_bytes_to_wide(const std::uint8_t* data, const std::size_t size,
                                 const bool big_endian) {
  std::wstring out;
  out.reserve(size / 2);
  for (std::size_t index = 0; index + 1 < size; index += 2) {
    const auto high = static_cast<std::uint16_t>(data[index]);
    const auto low = static_cast<std::uint16_t>(data[index + 1]);
    const char16_t unit = big_endian ? static_cast<char16_t>((high << 8) | low)
                                     : static_cast<char16_t>((low << 8) | high);
    out.push_back(static_cast<wchar_t>(unit));
  }
  return out;
}

std::vector<std::uint8_t> wide_to_utf16_bytes(const std::wstring& wide, const bool big_endian) {
  std::vector<std::uint8_t> out;
  out.reserve(wide.size() * 2);
  for (const wchar_t unit : wide) {
    const auto value = static_cast<char16_t>(unit);
    if (big_endian) {
      out.push_back(static_cast<std::uint8_t>(value >> 8));
      out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    } else {
      out.push_back(static_cast<std::uint8_t>(value & 0xFF));
      out.push_back(static_cast<std::uint8_t>(value >> 8));
    }
  }
  return out;
}

std::string cp_to_utf8(const std::uint8_t* data, const std::size_t size, const UINT codepage) {
  if (size == 0) return {};
  if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
  const int wide_size = MultiByteToWideChar(codepage, 0, reinterpret_cast<const char*>(data),
                                            static_cast<int>(size), nullptr, 0);
  if (wide_size <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
  if (MultiByteToWideChar(codepage, 0, reinterpret_cast<const char*>(data), static_cast<int>(size),
                          wide.data(), wide_size) <= 0) {
    return {};
  }
  return to_utf8(wide);
}

std::vector<std::uint8_t> utf8_to_cp(const std::string& utf8, const UINT codepage) {
  const std::wstring wide = from_utf8(utf8);
  if (wide.empty()) return {};
  if (wide.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
  // "?" replaces code points the target code page cannot hold, so an encode
  // never silently truncates the payload.
  const int size = WideCharToMultiByte(codepage, 0, wide.data(), static_cast<int>(wide.size()),
                                       nullptr, 0, "?", nullptr);
  if (size <= 0) return {};
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
  if (WideCharToMultiByte(codepage, 0, wide.data(), static_cast<int>(wide.size()),
                          reinterpret_cast<char*>(out.data()), size, "?", nullptr) <= 0) {
    return {};
  }
  return out;
}

// Session encoding -> UTF-8. Invalid byte sequences convert best-effort (the
// same rule as utf.hpp), so a corrupt file still reads back instead of
// failing a whole action.
std::string decode_text(const std::vector<std::uint8_t>& bytes, const Encoding encoding) {
  switch (encoding) {
    case Encoding::Utf8:
    case Encoding::Utf8Bom: {
      std::size_t offset = 0;
      if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        offset = 3;
      }
      if (offset >= bytes.size()) return {};
      return std::string(reinterpret_cast<const char*>(bytes.data() + offset), bytes.size() - offset);
    }
    case Encoding::Utf16:
    case Encoding::Utf16Be: {
      if (bytes.empty()) return {};
      std::size_t offset = 0;
      bool big_endian = encoding == Encoding::Utf16Be;
      if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        big_endian = false;
        offset = 2;
      } else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        big_endian = true;
        offset = 2;
      }
      const std::wstring wide =
          utf16_bytes_to_wide(bytes.data() + offset, bytes.size() - offset, big_endian);
      return to_utf8(wide);
    }
    case Encoding::Cp0:
      return cp_to_utf8(bytes.data(), bytes.size(), GetACP());
    case Encoding::Cp1252:
      return cp_to_utf8(bytes.data(), bytes.size(), 1252);
    case Encoding::Latin1: {
      std::wstring wide;
      wide.reserve(bytes.size());
      for (const std::uint8_t value : bytes) wide.push_back(static_cast<wchar_t>(value));
      return to_utf8(wide);
    }
  }
  return {};
}

// UTF-8 -> session encoding. The byte-order mark is produced separately by
// bom_of() so a writer places it only where the contract allows.
std::vector<std::uint8_t> encode_text(const std::string& utf8, const Encoding encoding) {
  if (utf8.empty()) return {};
  switch (encoding) {
    case Encoding::Utf8:
    case Encoding::Utf8Bom:
      return std::vector<std::uint8_t>(utf8.begin(), utf8.end());
    case Encoding::Utf16:
      return wide_to_utf16_bytes(from_utf8(utf8), false);
    case Encoding::Utf16Be:
      return wide_to_utf16_bytes(from_utf8(utf8), true);
    case Encoding::Cp0:
      return utf8_to_cp(utf8, GetACP());
    case Encoding::Cp1252:
      return utf8_to_cp(utf8, 1252);
    case Encoding::Latin1: {
      const std::wstring wide = from_utf8(utf8);
      std::vector<std::uint8_t> out;
      out.reserve(wide.size());
      for (const wchar_t unit : wide) {
        out.push_back(unit <= 0xFF ? static_cast<std::uint8_t>(unit)
                                   : static_cast<std::uint8_t>('?'));
      }
      return out;
    }
  }
  return {};
}

// ---- handles --------------------------------------------------------------

// Owner for the HANDLE that StorageService::file_open hands to the handle
// table. handle_guard.hpp's HandleGuard closes on scope exit and offers no
// release(), while file_open must transfer the raw value without closing it
// on exactly one path - hence this sibling with release().
class OpenFile final {
 public:
  OpenFile() = default;
  explicit OpenFile(HANDLE handle) : handle_(handle) {}

  OpenFile(const OpenFile&) = delete;
  OpenFile& operator=(const OpenFile&) = delete;

  ~OpenFile() { reset(); }

  [[nodiscard]] HANDLE get() const { return handle_; }

  [[nodiscard]] explicit operator bool() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  void reset() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }

  [[nodiscard]] HANDLE release() {
    const HANDLE released = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return released;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

// FindFirstFileW handles close with FindClose, not CloseHandle, so the
// generic HandleGuard cannot own them.
class FindGuard final {
 public:
  FindGuard() = default;
  explicit FindGuard(HANDLE handle) : handle_(handle) {}

  FindGuard(const FindGuard&) = delete;
  FindGuard& operator=(const FindGuard&) = delete;

  ~FindGuard() { reset(); }

  [[nodiscard]] HANDLE get() const { return handle_; }

  [[nodiscard]] explicit operator bool() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  void reset() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) FindClose(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

template <typename T>
class ComPtr final {
 public:
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ~ComPtr() { reset(); }

  [[nodiscard]] T* get() const { return ptr_; }
  [[nodiscard]] T* operator->() const { return ptr_; }
  [[nodiscard]] explicit operator bool() const { return ptr_ != nullptr; }

  [[nodiscard]] T** put() {
    reset();
    return &ptr_;
  }

  void reset() {
    if (ptr_ != nullptr) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

 private:
  T* ptr_{nullptr};
};

// Balances CoInitializeEx for the calling scope: a first-time or same-model
// initialization is released on exit, while RPC_E_CHANGED_MODE (COM already
// present in the other apartment) is adopted without an unbalance.
class ComApartment final {
 public:
  explicit ComApartment(const DWORD model) {
    hr_ = CoInitializeEx(nullptr, model);
    owns_ = (hr_ == S_OK || hr_ == S_FALSE);
    available_ = SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
  }

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  ~ComApartment() {
    if (owns_) CoUninitialize();
  }

  [[nodiscard]] bool ok() const { return available_; }
  [[nodiscard]] HRESULT hr() const { return hr_; }

 private:
  HRESULT hr_{S_OK};
  bool owns_{false};
  bool available_{false};
};

class WinHttpHandle final {
 public:
  WinHttpHandle() = default;
  explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}

  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;

  ~WinHttpHandle() { reset(); }

  [[nodiscard]] HINTERNET get() const { return handle_; }
  [[nodiscard]] explicit operator bool() const { return handle_ != nullptr; }

  void reset() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
      handle_ = nullptr;
    }
  }

 private:
  HINTERNET handle_{nullptr};
};

// ---- file IO --------------------------------------------------------------

Error read_file_bytes(const std::wstring& path, const std::uint64_t cap, const char* operation,
                      const std::string& label, std::vector<std::uint8_t>& out) {
  out.clear();
  HandleGuard file(CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) return fs_error(operation, label, GetLastError());
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size)) return fs_error(operation, label, GetLastError());
  if (size.QuadPart < 0) return fs_error(operation, label, ERROR_NEGATIVE_SEEK);
  if (static_cast<std::uint64_t>(size.QuadPart) > cap) {
    return contract_error(kReadLimitPrefix + label);
  }
  out.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t total = 0;
  while (total < out.size()) {
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - total, 1u << 30));
    DWORD got = 0;
    if (!ReadFile(file.get(), out.data() + total, want, &got, nullptr)) {
      return fs_error(operation, label, GetLastError());
    }
    if (got == 0) break;
    total += got;
  }
  out.resize(total);
  return Error::none();
}

Error write_all(HANDLE file, const std::vector<std::uint8_t>& bytes, const char* operation,
                const std::string& label) {
  std::size_t total = 0;
  while (total < bytes.size()) {
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - total, 1u << 20));
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + total, want, &written, nullptr)) {
      return fs_error(operation, label, GetLastError());
    }
    if (written == 0) return {Code::ExecutionFailed, std::string(operation) + " wrote no data"};
    total += written;
  }
  return Error::none();
}

// ---- directories ----------------------------------------------------------

std::wstring parent_of(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  std::wstring parent = path.substr(0, slash);
  if (!parent.empty() && parent.back() == L':') parent.push_back(L'\\');
  return parent;
}

// Creates `path` and every missing parent (AHK DirCreate). An existing
// directory is success, an existing file is not.
Error create_directories(const std::wstring& path, const std::string& label) {
  std::vector<std::wstring> chain;
  std::wstring current = path;
  for (;;) {
    if (CreateDirectoryW(current.c_str(), nullptr)) break;
    const DWORD status = GetLastError();
    if (status == ERROR_ALREADY_EXISTS) {
      const DWORD attributes = GetFileAttributesW(current.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) break;
      return {Code::ExecutionFailed, "path already exists as a file: " + label};
    }
    if (status != ERROR_PATH_NOT_FOUND) return fs_error("create directory", label, status);
    const std::wstring parent = parent_of(current);
    if (parent.empty() || parent == current) return fs_error("create directory", label, status);
    chain.push_back(current);
    current = parent;
  }
  for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator) {
    if (CreateDirectoryW(iterator->c_str(), nullptr)) continue;
    const DWORD status = GetLastError();
    if (status == ERROR_ALREADY_EXISTS) continue;
    return fs_error("create directory", label, status);
  }
  return Error::none();
}

// Recursive delete. Reparse points are removed as links and never walked
// through, so a junction cannot delete content outside the tree. Every entry
// is attempted even after a failure (the first failure is reported) so a
// partially deleted tree still makes progress.
Error delete_tree(const std::wstring& path, const std::string& label) {
  std::wstring pattern = path;
  if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
  pattern.push_back(L'*');
  WIN32_FIND_DATAW found{};
  FindGuard search(FindFirstFileW(pattern.c_str(), &found));
  if (!search) {
    const DWORD status = GetLastError();
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return Error::none();
    return fs_error("delete directory", label, status);
  }
  Error first_error = Error::none();
  do {
    if (std::wcscmp(found.cFileName, L".") == 0 || std::wcscmp(found.cFileName, L"..") == 0) {
      continue;
    }
    std::wstring child = path;
    if (!child.empty() && child.back() != L'\\') child.push_back(L'\\');
    child.append(found.cFileName);
    const bool is_dir = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool is_link = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    Error entry_error = Error::none();
    if (is_dir && !is_link) {
      entry_error = delete_tree(child, label);
    } else if (is_dir) {
      if (!RemoveDirectoryW(child.c_str())) {
        entry_error = fs_error("delete directory", label, GetLastError());
      }
    } else if (!DeleteFileW(child.c_str())) {
      entry_error = fs_error("delete file", label, GetLastError());
    }
    if (!entry_error.ok() && first_error.ok()) first_error = entry_error;
  } while (FindNextFileW(search.get(), &found));
  if (!first_error.ok()) return first_error;
  if (!RemoveDirectoryW(path.c_str())) return fs_error("delete directory", label, GetLastError());
  return Error::none();
}

// Recursive copy (AHK DirCopy). `overwrite` governs existing destination
// files; an existing destination directory is merged.
Error copy_tree(const std::wstring& src, const std::wstring& dst, const bool overwrite,
                const std::string& label) {
  const DWORD src_attributes = GetFileAttributesW(src.c_str());
  if (src_attributes == INVALID_FILE_ATTRIBUTES) {
    return fs_error("copy directory", label, GetLastError());
  }
  if (!(src_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return {Code::ExecutionFailed, "path is not a directory: " + label};
  }
  const DWORD dst_attributes = GetFileAttributesW(dst.c_str());
  if (dst_attributes != INVALID_FILE_ATTRIBUTES && !(dst_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return {Code::ExecutionFailed, "destination already exists as a file: " + label};
  }
  if (const Error create_error = create_directories(dst, label); !create_error.ok()) {
    return create_error;
  }

  std::wstring pattern = src;
  if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
  pattern.push_back(L'*');
  WIN32_FIND_DATAW found{};
  FindGuard search(FindFirstFileW(pattern.c_str(), &found));
  if (!search) return fs_error("copy directory", label, GetLastError());
  do {
    if (std::wcscmp(found.cFileName, L".") == 0 || std::wcscmp(found.cFileName, L"..") == 0) {
      continue;
    }
    std::wstring child_src = src;
    if (!child_src.empty() && child_src.back() != L'\\') child_src.push_back(L'\\');
    child_src.append(found.cFileName);
    std::wstring child_dst = dst;
    if (!child_dst.empty() && child_dst.back() != L'\\') child_dst.push_back(L'\\');
    child_dst.append(found.cFileName);
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (const Error child_error = copy_tree(child_src, child_dst, overwrite, label);
          !child_error.ok()) {
        return child_error;
      }
      continue;
    }
    if (CopyFileW(child_src.c_str(), child_dst.c_str(), overwrite ? FALSE : TRUE)) continue;
    const DWORD status = GetLastError();
    if (status == ERROR_ALREADY_EXISTS || status == ERROR_FILE_EXISTS) {
      return {Code::ExecutionFailed, "destination already exists: " + label};
    }
    return fs_error("copy", label, status);
  } while (FindNextFileW(search.get(), &found));
  return Error::none();
}

// ---- drives ---------------------------------------------------------------

// Accepts "C", "C:" and "C:\" in any case and returns the canonical root
// "C:\" plus the uppercase letter; anything else is a contract error.
Error drive_root(const std::string& letter, std::wstring& root, char& canonical) {
  const auto reject = [&letter] {
    return contract_error("drive must be a letter, \"X:\" or \"X:\\\": " + letter);
  };
  if (letter.empty()) return contract_error("drive must not be empty");
  char head = letter[0];
  if (head >= 'a' && head <= 'z') head = static_cast<char>(head - 'a' + 'A');
  if (head < 'A' || head > 'Z') return reject();
  const bool bare = letter.size() == 1;
  const bool colon = letter.size() == 2 && letter[1] == ':';
  const bool root_form = letter.size() == 3 && letter[1] == ':' && (letter[2] == '\\' || letter[2] == '/');
  if (!bare && !colon && !root_form) return reject();
  canonical = head;
  root.clear();
  root.push_back(static_cast<wchar_t>(head));
  root.append(L":\\");
  return Error::none();
}

bool drive_exists(const std::wstring& root) {
  return GetDriveTypeW(root.c_str()) != DRIVE_NO_ROOT_DIR;
}

const char* drive_type_name(const UINT type) {
  switch (type) {
    case DRIVE_REMOVABLE:
      return "Removable";
    case DRIVE_FIXED:
      return "Fixed";
    case DRIVE_REMOTE:
      return "Network";
    case DRIVE_CDROM:
      return "CDROM";
    case DRIVE_RAMDISK:
      return "RAMDisk";
    default:
      return "Unknown";
  }
}

Error drive_missing(const char canonical) {
  return {Code::TargetGone, std::string(kDriveNotFoundPrefix) + canonical};
}

// ---- INI (Win32 profile API, exactly like AHK's IniRead/IniWrite) ---------

constexpr wchar_t kIniMissing[] = L"\x01RIME-INI-MISSING";

Error ini_full_path(const std::string& path, const char* operation, std::wstring& out) {
  if (const Error path_error = wide_path(path, "path", out); !path_error.ok()) return path_error;
  const DWORD need = GetFullPathNameW(out.c_str(), 0, nullptr, nullptr);
  if (need == 0) return win32_error(operation, GetLastError());
  std::wstring full(need, L'\0');
  const DWORD written = GetFullPathNameW(out.c_str(), need, full.data(), nullptr);
  if (written == 0 || written >= need) return win32_error(operation, GetLastError());
  full.resize(written);
  out = std::move(full);
  return Error::none();
}

bool ini_file_exists(const std::wstring& full) {
  const DWORD attributes = GetFileAttributesW(full.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool ini_section_exists(const std::wstring& full, const std::wstring& section) {
  std::wstring buffer(4096, L'\0');
  for (;;) {
    const DWORD written =
        GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), full.c_str());
    if (written == 0) return false;
    if (written < buffer.size()) break;
    if (buffer.size() >= (1u << 20)) return false;
    buffer.resize(buffer.size() * 2, L'\0');
  }
  for (const wchar_t* entry = buffer.c_str(); *entry != L'\0'; entry += std::wcslen(entry) + 1) {
    if (iequals(std::wstring_view(entry), section)) return true;
  }
  return false;
}

// Reads one key through GetPrivateProfileStringW. `found` separates a missing
// key from an empty value; a real value that starts with the sentinel byte
// would be misread, which is documented in docs/api/storage.md.
Error ini_read_value(const std::wstring& full, const std::wstring& section, const std::wstring& key,
                     std::wstring& out, bool& found) {
  out.clear();
  found = false;
  std::wstring buffer(4096, L'\0');
  DWORD written = 0;
  for (;;) {
    written = GetPrivateProfileStringW(section.c_str(), key.c_str(), kIniMissing,
                                                   buffer.data(),
                                                   static_cast<DWORD>(buffer.size()), full.c_str());
    if (written == 0) return Error::none();
    if (written + 1 < buffer.size()) break;
    if (buffer.size() >= (1u << 20)) return {Code::ExecutionFailed, "ini value is too large"};
    buffer.resize(buffer.size() * 2, L'\0');
  }
  const std::wstring_view value(buffer.data(), written);
  const std::wstring_view sentinel(kIniMissing);
  if (value.size() >= sentinel.size() && value.substr(0, sentinel.size()) == sentinel) {
    return Error::none();
  }
  found = true;
  out.assign(buffer.data(), written);
  return Error::none();
}

// ---- download -------------------------------------------------------------

struct HttpUrl {
  std::wstring host;
  INTERNET_PORT port{0};
  std::wstring object;
  bool secure{false};
};

Error parse_http_url(const std::string& url, HttpUrl& out) {
  const bool is_http = starts_with_ci(url, "http://");
  const bool is_https = starts_with_ci(url, "https://");
  if (!is_http && !is_https) {
    return contract_error("download URL must start with http:// or https://: " + url);
  }
  const std::wstring wide = from_utf8(url);
  if (wide.empty()) return contract_error("download URL is not valid UTF-8: " + url);

  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components)) {
    return contract_error("download URL could not be parsed: " + url);
  }
  if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
    return contract_error("download URL must be http or https: " + url);
  }
  if (components.dwHostNameLength == 0) {
    return contract_error("download URL must include a host: " + url);
  }
  out.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
  out.host.assign(components.lpszHostName, components.dwHostNameLength);
  out.port = components.nPort != 0
                 ? components.nPort
                 : (out.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
  if (components.lpszUrlPath != nullptr) {
    out.object.assign(components.lpszUrlPath, components.dwUrlPathLength);
  }
  if (components.lpszExtraInfo != nullptr) {
    out.object.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  if (out.object.empty()) out.object = L"/";
  return Error::none();
}

bool is_loopback_host(const std::wstring& host) {
  if (iequals(host, std::wstring_view(L"localhost"))) return true;
  if (iequals(host, std::wstring_view(L"::1")) || iequals(host, std::wstring_view(L"[::1]"))) {
    return true;
  }
  return host.size() >= 4 && host.compare(0, 4, L"127.") == 0;
}

// ---- selectors ------------------------------------------------------------

std::string shell_item_path(IShellItem* item) {
  if (item == nullptr) return {};
  PWSTR raw = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) return {};
  std::string path = to_utf8(raw);
  CoTaskMemFree(raw);
  return path;
}

// Parses the selector filter. Two accepted spellings: "Name (*.txt)" (the AHK
// form) and a bare pattern list such as "*.txt;*.ini".
Error parse_selector_filter(const std::string& filter, std::wstring& name, std::wstring& spec) {
  if (filter.empty()) return contract_error("file selector filter must not be empty");
  const std::size_t open = filter.rfind('(');
  if (open != std::string::npos && filter.back() == ')') {
    const std::string label = filter.substr(0, open);
    const std::string patterns = filter.substr(open + 1, filter.size() - open - 2);
    if (label.empty() || patterns.empty()) {
      return contract_error("file selector filter must be \"Name (*.patterns)\" or a pattern list");
    }
    name = from_utf8(label);
    spec = from_utf8(patterns);
    if (name.empty() || spec.empty()) {
      return contract_error("file selector filter is not valid UTF-8: " + filter);
    }
    return Error::none();
  }
  spec = from_utf8(filter);
  if (spec.empty()) return contract_error("file selector filter is not valid UTF-8: " + filter);
  name = L"Matched files";
  return Error::none();
}

Error run_file_dialog(const std::wstring& filter_name, const std::wstring& filter_spec,
                      const std::wstring& default_name, const bool multi, const HWND owner,
                      std::vector<std::string>& out) {
  ComApartment apartment(COINIT_APARTMENTTHREADED);
  if (!apartment.ok()) return hresult_error("selector COM initialization", apartment.hr());

  ComPtr<IFileOpenDialog> dialog;
  HRESULT status = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IFileOpenDialog, reinterpret_cast<void**>(dialog.put()));
  if (FAILED(status)) return hresult_error("selector create", status);

  FILEOPENDIALOGOPTIONS options = 0;
  if (FAILED(status = dialog->GetOptions(&options))) return hresult_error("selector options", status);
  options |= FOS_FORCEFILESYSTEM;
  if (multi) options |= FOS_ALLOWMULTISELECT;
  if (FAILED(status = dialog->SetOptions(options))) return hresult_error("selector options", status);

  const COMDLG_FILTERSPEC one{filter_name.c_str(), filter_spec.c_str()};
  if (FAILED(status = dialog->SetFileTypes(1, &one))) return hresult_error("selector filter", status);
  if (!default_name.empty() && FAILED(status = dialog->SetFileName(default_name.c_str()))) {
    return hresult_error("selector default name", status);
  }

  status = dialog->Show(owner);
  if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {Code::Cancelled, kCancelledByUser};
  if (FAILED(status)) return hresult_error("selector show", status);

  if (!multi) {
    ComPtr<IShellItem> item;
    if (FAILED(status = dialog->GetResult(item.put()))) {
      return hresult_error("selector result", status);
    }
    std::string path = shell_item_path(item.get());
    if (path.empty()) return {Code::ExecutionFailed, "selector returned an unreadable path"};
    out.push_back(std::move(path));
    return Error::none();
  }

  ComPtr<IShellItemArray> items;
  if (FAILED(status = dialog->GetResults(items.put()))) return hresult_error("selector results", status);
  DWORD count = 0;
  if (FAILED(status = items->GetCount(&count))) return hresult_error("selector results", status);
  for (DWORD index = 0; index < count; ++index) {
    ComPtr<IShellItem> item;
    if (FAILED(status = items->GetItemAt(index, item.put()))) {
      return hresult_error("selector results", status);
    }
    std::string path = shell_item_path(item.get());
    if (path.empty()) return {Code::ExecutionFailed, "selector returned an unreadable path"};
    out.push_back(std::move(path));
  }
  return Error::none();
}

Error run_dir_dialog(const std::wstring& caption, const HWND owner, std::string& out) {
  ComApartment apartment(COINIT_APARTMENTTHREADED);
  if (!apartment.ok()) return hresult_error("selector COM initialization", apartment.hr());

  ComPtr<IFileOpenDialog> dialog;
  HRESULT status = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IFileOpenDialog, reinterpret_cast<void**>(dialog.put()));
  if (FAILED(status)) return hresult_error("selector create", status);

  FILEOPENDIALOGOPTIONS options = 0;
  if (FAILED(status = dialog->GetOptions(&options))) return hresult_error("selector options", status);
  options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM;
  if (FAILED(status = dialog->SetOptions(options))) return hresult_error("selector options", status);
  if (!caption.empty() && FAILED(status = dialog->SetTitle(caption.c_str()))) {
    return hresult_error("selector caption", status);
  }

  status = dialog->Show(owner);
  if (status == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {Code::Cancelled, kCancelledByUser};
  if (FAILED(status)) return hresult_error("selector show", status);

  ComPtr<IShellItem> item;
  if (FAILED(status = dialog->GetResult(item.put()))) return hresult_error("selector result", status);
  out = shell_item_path(item.get());
  if (out.empty()) return {Code::ExecutionFailed, "selector returned an unreadable path"};
  return Error::none();
}

}  // namespace

StorageService::~StorageService() { (void)stop(); }

// ---- reads ----------------------------------------------------------------

Error StorageService::read_text(const std::string& path, std::string& out) const {
  out.clear();
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  std::vector<std::uint8_t> bytes;
  if (const Error read_error = read_file_bytes(wide, kMaxWholeFileRead, "read", path, bytes);
      !read_error.ok()) {
    return read_error;
  }
  Encoding encoding = Encoding::Utf8;
  (void)parse_encoding(current_encoding(), encoding);
  out = decode_text(bytes, encoding);
  return Error::none();
}

Error StorageService::read_bytes(const std::string& path, std::vector<std::uint8_t>& out) const {
  out.clear();
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  return read_file_bytes(wide, kMaxWholeFileRead, "read", path, out);
}

Error StorageService::stat(const std::string& path, FileStatInfo& out) const {
  out = FileStatInfo{};
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
    return fs_error("stat", path, GetLastError());
  }
  out.attrib_bits = data.dwFileAttributes;
  out.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  out.attrib_string = attrib_to_string(data.dwFileAttributes);
  filetime_to_unix_ms(data.ftLastWriteTime, out.mtime_unix_ms);
  if (!out.is_dir) {
    ULARGE_INTEGER size{};
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    out.size = size.QuadPart;
  }
  return Error::none();
}

Error StorageService::list(const std::string& path, std::vector<DirEntryInfo>& out) const {
  out.clear();
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return fs_error("list", path, GetLastError());
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return {Code::ExecutionFailed, "path is not a directory: " + path};
  }

  std::wstring pattern = wide;
  if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
  pattern.push_back(L'*');
  WIN32_FIND_DATAW found{};
  FindGuard search(FindFirstFileW(pattern.c_str(), &found));
  if (!search) return fs_error("list", path, GetLastError());
  do {
    if (std::wcscmp(found.cFileName, L".") == 0 || std::wcscmp(found.cFileName, L"..") == 0) {
      continue;
    }
    DirEntryInfo entry;
    entry.name = to_utf8(found.cFileName);
    entry.is_dir = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out.push_back(std::move(entry));
  } while (FindNextFileW(search.get(), &found));
  // Byte order over UTF-8 keeps the listing reproducible across runs and
  // locales; "." and ".." are never reported.
  std::sort(out.begin(), out.end(),
            [](const DirEntryInfo& left, const DirEntryInfo& right) { return left.name < right.name; });
  return Error::none();
}

Error StorageService::env_get(const std::string& name, std::string& out) const {
  out.clear();
  if (name.empty()) return contract_error("environment variable name must not be empty");
  std::wstring wide;
  if (const Error name_error = wide_path(name, "name", wide); !name_error.ok()) return name_error;

  const DWORD needed = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
  // Unset and empty both read back as an empty string (AHK EnvGet).
  if (needed == 0) return Error::none();
  if (needed > 32768) return {Code::ExecutionFailed, "environment variable is too large: " + name};
  std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = GetEnvironmentVariableW(wide.c_str(), buffer.data(), needed);
  if (written == 0) {
    if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return Error::none();
    return win32_error("environment read", GetLastError());
  }
  buffer.resize(written);
  out = to_utf8(buffer);
  return Error::none();
}

Error StorageService::ini_read(const std::string& path, const std::string& section,
                               const std::string& key, std::string& out) const {
  out.clear();
  if (section.empty()) return contract_error("ini section must not be empty");
  if (key.empty()) return contract_error("ini key must not be empty");
  std::wstring full;
  if (const Error path_error = ini_full_path(path, "ini read", full); !path_error.ok()) {
    return path_error;
  }
  if (!ini_file_exists(full)) return {Code::ExecutionFailed, "file not found: " + path};

  const std::wstring wide_section = from_utf8(section);
  const std::wstring wide_key = from_utf8(key);
  if (!ini_section_exists(full, wide_section)) {
    return {Code::ExecutionFailed, "ini section not found: " + section};
  }
  std::wstring value;
  bool found = false;
  if (const Error read_error = ini_read_value(full, wide_section, wide_key, value, found);
      !read_error.ok()) {
    return read_error;
  }
  if (!found) return {Code::ExecutionFailed, "ini key not found: " + section + "/" + key};
  out = to_utf8(value);
  return Error::none();
}

Error StorageService::drive_get(const std::string& field, const std::string& letter,
                                DriveInfo& out) const {
  out = DriveInfo{};
  constexpr const char* kFields[] = {"type",     "list",  "serial", "spacefree", "status",
                                     "statuscd", "label", "capacity"};
  // "filesystem" is spelled out below; the table exists so an unknown field
  // is rejected before any OS call.
  bool known = iequals(field, "filesystem");
  for (const char* candidate : kFields) {
    if (iequals(field, candidate)) {
      known = true;
      break;
    }
  }
  if (!known) {
    return contract_error(
        "drive field must be one of type, list, serial, spacefree, status, statuscd, filesystem, "
        "label, capacity");
  }

  if (iequals(field, "list")) {
    // `letter` carries the AHK DriveGetList type filter for this field.
    UINT wanted = 256;
    if (!letter.empty()) {
      if (iequals(letter, "CDROM")) {
        wanted = DRIVE_CDROM;
      } else if (iequals(letter, "Removable")) {
        wanted = DRIVE_REMOVABLE;
      } else if (iequals(letter, "Fixed")) {
        wanted = DRIVE_FIXED;
      } else if (iequals(letter, "Network")) {
        wanted = DRIVE_REMOTE;
      } else if (iequals(letter, "RAMDisk")) {
        wanted = DRIVE_RAMDISK;
      } else if (iequals(letter, "Unknown")) {
        wanted = DRIVE_UNKNOWN;
      } else {
        return contract_error(
            "drive list type must be CDROM, Removable, Fixed, Network, RAMDisk or Unknown");
      }
    }
    for (wchar_t candidate = L'A'; candidate <= L'Z'; ++candidate) {
      std::wstring root(1, candidate);
      root.append(L":\\");
      const UINT type = GetDriveTypeW(root.c_str());
      if (type == wanted || (wanted == 256 && type != DRIVE_NO_ROOT_DIR)) {
        out.list.emplace_back(1, static_cast<char>(candidate));
      }
    }
    return Error::none();
  }

  std::wstring root;
  char canonical = 0;
  if (const Error drive_error = drive_root(letter, root, canonical); !drive_error.ok()) {
    return drive_error;
  }
  if (!drive_exists(root)) return drive_missing(canonical);
  out.letter = std::string(1, canonical) + ":";

  if (iequals(field, "type")) {
    out.type = drive_type_name(GetDriveTypeW(root.c_str()));
    return Error::none();
  }
  if (iequals(field, "status") || iequals(field, "statuscd")) {
    // `statuscd` mirrors `status`: AHK resolves it through MCI/winmm, which
    // this runtime deliberately does not link (documented in
    // docs/api/storage.md).
    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters = 0;
    if (GetDiskFreeSpaceW(root.c_str(), &sectors_per_cluster, &bytes_per_sector, &free_clusters,
                          &total_clusters)) {
      out.status = "Ready";
      return Error::none();
    }
    switch (GetLastError()) {
      case ERROR_FILE_NOT_FOUND:
      case ERROR_PATH_NOT_FOUND:
        out.status = "Invalid";
        break;
      case ERROR_NOT_READY:
        out.status = "NotReady";
        break;
      case ERROR_WRITE_PROTECT:
        out.status = "ReadOnly";
        break;
      default:
        out.status = "Unknown";
        break;
    }
    return Error::none();
  }
  if (iequals(field, "serial")) {
    DWORD serial = 0;
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
      return win32_error("volume serial", GetLastError());
    }
    out.serial = serial;
    return Error::none();
  }
  if (iequals(field, "filesystem") || iequals(field, "label")) {
    std::wstring buffer(261, L'\0');
    const BOOL ok =
        iequals(field, "filesystem")
            ? GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, buffer.data(),
                                    static_cast<DWORD>(buffer.size()))
            : GetVolumeInformationW(root.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()),
                                    nullptr, nullptr, nullptr, nullptr, 0);
    if (!ok) return win32_error("volume information", GetLastError());
    buffer.resize(std::wcslen(buffer.c_str()));
    if (iequals(field, "filesystem")) out.filesystem = to_utf8(buffer);
    else out.label = to_utf8(buffer);
    return Error::none();
  }

  // spacefree and capacity.
  ULARGE_INTEGER available{};
  ULARGE_INTEGER total{};
  if (!GetDiskFreeSpaceExW(root.c_str(), &available, &total, nullptr)) {
    return win32_error("disk space", GetLastError());
  }
  out.free_bytes = available.QuadPart;
  if (iequals(field, "capacity")) {
    out.total_bytes = total.QuadPart;
    if (total.QuadPart == 0) {
      out.capacity_percent = 0;
    } else {
      const std::uint64_t used = total.QuadPart - available.QuadPart;
      out.capacity_percent = static_cast<int>((used * 100ull) / total.QuadPart);
    }
  }
  return Error::none();
}

// ---- file handles ---------------------------------------------------------

Error StorageService::file_open(const std::string& path, const std::string& mode,
                                std::uint64_t& handle_out, std::uint64_t& length_out) {
  handle_out = 0;
  length_out = 0;
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;

  DWORD access = 0;
  DWORD disposition = 0;
  bool seek_end = false;
  if (mode == "r") {
    access = GENERIC_READ;
    disposition = OPEN_EXISTING;
  } else if (mode == "a") {
    // AHK's TextFile::Open gives APPEND both rights (TextIO.cpp), so an
    // append handle can be read back from after a seek.
    access = GENERIC_WRITE | GENERIC_READ;
    disposition = OPEN_ALWAYS;
    seek_end = true;
  } else if (mode == "w") {
    access = GENERIC_WRITE;
    disposition = CREATE_ALWAYS;
  } else {
    return contract_error("file mode must be \"r\", \"a\" or \"w\"");
  }

  OpenFile file(CreateFileW(wide.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) return fs_error("open", path, GetLastError());

  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(file.get(), &info)) return fs_error("open", path, GetLastError());
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    return {Code::ExecutionFailed, "cannot open a directory: " + path};
  }
  if (seek_end) {
    LARGE_INTEGER end{};
    if (!SetFilePointerEx(file.get(), end, nullptr, FILE_END)) {
      return fs_error("open", path, GetLastError());
    }
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size)) return fs_error("open", path, GetLastError());

  std::lock_guard<std::mutex> lock(handles_mutex_);
  const std::uint64_t id = next_handle_id_++;
  handles_.emplace(id, file.release());
  handle_out = id;
  length_out = static_cast<std::uint64_t>(size.QuadPart);
  return Error::none();
}

Error StorageService::file_read(const std::uint64_t handle, const std::uint32_t count,
                                std::vector<std::uint8_t>& out, bool& eof) const {
  out.clear();
  eof = false;
  if (count > kMaxHandleRead) return contract_error(kReadCountLimit);
  // The lock also covers ReadFile: file_close() must not CloseHandle() under
  // a read that is still in flight on another lane.
  std::lock_guard<std::mutex> lock(handles_mutex_);
  const auto iterator = handles_.find(handle);
  if (iterator == handles_.end()) return {Code::InvalidState, kHandleNotOpen};
  const auto raw = static_cast<HANDLE>(iterator->second);
  if (count == 0) return Error::none();
  out.resize(count);
  DWORD got = 0;
  if (!ReadFile(raw, out.data(), count, &got, nullptr)) {
    const DWORD status = GetLastError();
    out.clear();
    return win32_error("file read", status);
  }
  out.resize(got);
  eof = got < count;
  return Error::none();
}

Error StorageService::file_seek(const std::uint64_t handle, const std::int64_t offset,
                                const int whence, std::uint64_t& pos) {
  pos = 0;
  if (whence < 0 || whence > 2) return contract_error("file seek whence must be 0, 1 or 2");
  std::lock_guard<std::mutex> lock(handles_mutex_);
  const auto iterator = handles_.find(handle);
  if (iterator == handles_.end()) return {Code::InvalidState, kHandleNotOpen};
  const auto raw = static_cast<HANDLE>(iterator->second);
  LARGE_INTEGER distance{};
  distance.QuadPart = offset;
  LARGE_INTEGER target{};
  if (!SetFilePointerEx(raw, distance, &target, static_cast<DWORD>(whence))) {
    return win32_error("file seek", GetLastError());
  }
  if (target.QuadPart < 0) {
    return {Code::ExecutionFailed, "file seek moved before the start of the file"};
  }
  pos = static_cast<std::uint64_t>(target.QuadPart);
  return Error::none();
}

Error StorageService::file_stat(const std::uint64_t handle, std::uint64_t& pos,
                                std::uint64_t& length) const {
  pos = 0;
  length = 0;
  std::lock_guard<std::mutex> lock(handles_mutex_);
  const auto iterator = handles_.find(handle);
  if (iterator == handles_.end()) return {Code::InvalidState, kHandleNotOpen};
  const auto raw = static_cast<HANDLE>(iterator->second);
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(raw, &size)) return win32_error("file stat", GetLastError());
  LARGE_INTEGER current{};
  if (!SetFilePointerEx(raw, current, &current, FILE_CURRENT)) {
    return win32_error("file stat", GetLastError());
  }
  length = static_cast<std::uint64_t>(size.QuadPart);
  pos = static_cast<std::uint64_t>(current.QuadPart);
  return Error::none();
}

Error StorageService::file_close(const std::uint64_t handle) {
  std::lock_guard<std::mutex> lock(handles_mutex_);
  const auto iterator = handles_.find(handle);
  if (iterator == handles_.end()) return {Code::InvalidState, kHandleNotOpen};
  const auto raw = static_cast<HANDLE>(iterator->second);
  handles_.erase(iterator);
  if (raw != nullptr) CloseHandle(raw);
  return Error::none();
}

std::size_t StorageService::open_handle_count() const {
  std::lock_guard<std::mutex> lock(handles_mutex_);
  return handles_.size();
}

std::size_t StorageService::stop() {
  std::lock_guard<std::mutex> lock(handles_mutex_);
  const std::size_t closed = handles_.size();
  for (const auto& entry : handles_) {
    const auto raw = static_cast<HANDLE>(entry.second);
    if (raw != nullptr) CloseHandle(raw);
  }
  handles_.clear();
  return closed;
}

Error StorageService::handle_write(const std::uint64_t handle,
                                   const std::vector<std::uint8_t>& bytes) {
  std::lock_guard<std::mutex> lock(handles_mutex_);
  const auto iterator = handles_.find(handle);
  if (iterator == handles_.end()) return {Code::InvalidState, kHandleNotOpen};
  const auto raw = static_cast<HANDLE>(iterator->second);
  return write_all(raw, bytes, "file write", "the open file");
}

// ---- writes ---------------------------------------------------------------

Error StorageService::append_text(const std::string& path, const std::string& utf8) const {
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  Encoding encoding = Encoding::Utf8;
  (void)parse_encoding(current_encoding(), encoding);

  OpenFile file(CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) return fs_error("append", path, GetLastError());
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size)) return fs_error("append", path, GetLastError());
  LARGE_INTEGER end{};
  if (!SetFilePointerEx(file.get(), end, nullptr, FILE_END)) {
    return fs_error("append", path, GetLastError());
  }
  // The mark is written only where the file begins, so appending never
  // inserts a byte-order mark into existing content.
  if (size.QuadPart == 0) {
    if (const Error bom_error = write_all(file.get(), bom_of(encoding), "append", path);
        !bom_error.ok()) {
      return bom_error;
    }
  }
  return write_all(file.get(), encode_text(utf8, encoding), "append", path);
}

Error StorageService::write_text(const std::string& path, const std::string& utf8) const {
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  Encoding encoding = Encoding::Utf8;
  (void)parse_encoding(current_encoding(), encoding);

  OpenFile file(CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) return fs_error("write", path, GetLastError());
  if (const Error bom_error = write_all(file.get(), bom_of(encoding), "write", path);
      !bom_error.ok()) {
    return bom_error;
  }
  return write_all(file.get(), encode_text(utf8, encoding), "write", path);
}

Error StorageService::file_copy(const std::string& src, const std::string& dst, const bool overwrite) {
  std::wstring wide_src;
  if (const Error path_error = wide_path(src, "source", wide_src); !path_error.ok()) return path_error;
  std::wstring wide_dst;
  if (const Error path_error = wide_path(dst, "destination", wide_dst); !path_error.ok()) {
    return path_error;
  }
  if (CopyFileW(wide_src.c_str(), wide_dst.c_str(), overwrite ? FALSE : TRUE)) return Error::none();
  const DWORD status = GetLastError();
  if (status == ERROR_ALREADY_EXISTS || status == ERROR_FILE_EXISTS) {
    return {Code::ExecutionFailed, "destination already exists: " + dst};
  }
  if (status == ERROR_FILE_NOT_FOUND) return {Code::ExecutionFailed, "file not found: " + src};
  if (status == ERROR_PATH_NOT_FOUND) return {Code::ExecutionFailed, "path not found: " + dst};
  return fs_error("copy", src, status);
}

Error StorageService::file_move(const std::string& src, const std::string& dst, const bool overwrite) {
  std::wstring wide_src;
  if (const Error path_error = wide_path(src, "source", wide_src); !path_error.ok()) return path_error;
  std::wstring wide_dst;
  if (const Error path_error = wide_path(dst, "destination", wide_dst); !path_error.ok()) {
    return path_error;
  }

  const DWORD existing = GetFileAttributesW(wide_dst.c_str());
  if (existing != INVALID_FILE_ATTRIBUTES && !overwrite) {
    return {Code::ExecutionFailed, "destination already exists: " + dst};
  }

  if (MoveFileExW(wide_src.c_str(), wide_dst.c_str(), overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) {
    return Error::none();
  }
  const DWORD status = GetLastError();
  if (status == ERROR_NOT_SAME_DEVICE) {
    // Cross-volume move: the OS refuses, so copy then delete (the fallback
    // documented in the header).
    if (const Error copy_error = file_copy(src, dst, true); !copy_error.ok()) return copy_error;
    return file_delete(src);
  }
  if (status == ERROR_ALREADY_EXISTS || status == ERROR_FILE_EXISTS) {
    return {Code::ExecutionFailed, "destination already exists: " + dst};
  }
  if (status == ERROR_FILE_NOT_FOUND) return {Code::ExecutionFailed, "file not found: " + src};
  if (status == ERROR_PATH_NOT_FOUND) return {Code::ExecutionFailed, "path not found: " + dst};
  return fs_error("move", src, status);
}

Error StorageService::file_delete(const std::string& path) {
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return fs_error("delete", path, GetLastError());
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    return {Code::ExecutionFailed, "path is a directory: " + path};
  }
  if (DeleteFileW(wide.c_str())) return Error::none();
  return fs_error("delete", path, GetLastError());
}

Error StorageService::dir_create(const std::string& path) {
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  return create_directories(wide, path);
}

Error StorageService::dir_delete(const std::string& path, const bool recursive) {
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return fs_error("delete directory", path, GetLastError());
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return {Code::ExecutionFailed, "path is not a directory: " + path};
  }
  if (!recursive) {
    if (RemoveDirectoryW(wide.c_str())) return Error::none();
    const DWORD status = GetLastError();
    if (status == ERROR_DIR_NOT_EMPTY) {
      return {Code::ExecutionFailed, "directory is not empty: " + path};
    }
    return fs_error("delete directory", path, status);
  }
  return delete_tree(wide, path);
}

Error StorageService::dir_copy(const std::string& src, const std::string& dst, const bool overwrite) {
  std::wstring wide_src;
  if (const Error path_error = wide_path(src, "source", wide_src); !path_error.ok()) return path_error;
  std::wstring wide_dst;
  if (const Error path_error = wide_path(dst, "destination", wide_dst); !path_error.ok()) {
    return path_error;
  }
  // The source is validated against its own path (and before the
  // destination is created) so a bad source reports "src" and leaves no
  // new directory behind.
  const DWORD src_attributes = GetFileAttributesW(wide_src.c_str());
  if (src_attributes == INVALID_FILE_ATTRIBUTES) {
    return fs_error("copy", src, GetLastError());
  }
  if (!(src_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return {Code::ExecutionFailed, "path is not a directory: " + src};
  }
  if (GetFileAttributesW(wide_dst.c_str()) == INVALID_FILE_ATTRIBUTES) {
    if (const Error create_error = create_directories(wide_dst, dst); !create_error.ok()) {
      return create_error;
    }
  }
  // Nested failures keep reporting the caller's destination path, so the
  // error text is stable no matter how deep the copy stopped.
  return copy_tree(wide_src, wide_dst, overwrite, dst);
}

Error StorageService::dir_move(const std::string& src, const std::string& dst, const bool overwrite) {
  std::wstring wide_src;
  if (const Error path_error = wide_path(src, "source", wide_src); !path_error.ok()) return path_error;
  std::wstring wide_dst;
  if (const Error path_error = wide_path(dst, "destination", wide_dst); !path_error.ok()) {
    return path_error;
  }
  const DWORD src_attributes = GetFileAttributesW(wide_src.c_str());
  if (src_attributes == INVALID_FILE_ATTRIBUTES) {
    return fs_error("move directory", src, GetLastError());
  }
  if (!(src_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return {Code::ExecutionFailed, "path is not a directory: " + src};
  }

  const DWORD dst_attributes = GetFileAttributesW(wide_dst.c_str());
  if (dst_attributes != INVALID_FILE_ATTRIBUTES) {
    if (!overwrite) return {Code::ExecutionFailed, "destination already exists: " + dst};
    if (!(dst_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      return {Code::ExecutionFailed, "destination already exists as a file: " + dst};
    }
    // A merge cannot go through MoveFileEx, so the result is the same
    // copy-then-delete sequence a cross-volume move uses.
    if (const Error copy_error = dir_copy(src, dst, true); !copy_error.ok()) return copy_error;
    return dir_delete(src, true);
  }

  if (MoveFileExW(wide_src.c_str(), wide_dst.c_str(), 0)) return Error::none();
  const DWORD status = GetLastError();
  if (status == ERROR_NOT_SAME_DEVICE) {
    if (const Error copy_error = dir_copy(src, dst, true); !copy_error.ok()) return copy_error;
    return dir_delete(src, true);
  }
  if (status == ERROR_FILE_NOT_FOUND) return {Code::ExecutionFailed, "file not found: " + src};
  if (status == ERROR_PATH_NOT_FOUND) return {Code::ExecutionFailed, "path not found: " + dst};
  return fs_error("move directory", src, status);
}

Error StorageService::set_attrib(const std::string& path, const std::string& add,
                                 const std::string& remove) {
  if (add.empty() && remove.empty()) {
    return contract_error("setAttrib requires at least one attribute letter to add or remove");
  }
  // A R H S T N O match AHK's FileSetAttrib letters (docs/api/storage.md).
  auto parse_letters = [](const std::string& letters, DWORD& bits) -> Error {
    bits = 0;
    for (const char value : letters) {
      switch (value) {
        case 'A':
        case 'a':
          bits |= FILE_ATTRIBUTE_ARCHIVE;
          break;
        case 'R':
        case 'r':
          bits |= FILE_ATTRIBUTE_READONLY;
          break;
        case 'H':
        case 'h':
          bits |= FILE_ATTRIBUTE_HIDDEN;
          break;
        case 'S':
        case 's':
          bits |= FILE_ATTRIBUTE_SYSTEM;
          break;
        case 'T':
        case 't':
          bits |= FILE_ATTRIBUTE_TEMPORARY;
          break;
        case 'N':
        case 'n':
          bits |= FILE_ATTRIBUTE_NORMAL;
          break;
        case 'O':
        case 'o':
          bits |= FILE_ATTRIBUTE_OFFLINE;
          break;
        default:
          return contract_error(std::string("unknown attribute letter: ") + value);
      }
    }
    return Error::none();
  };

  DWORD add_bits = 0;
  if (const Error add_error = parse_letters(add, add_bits); !add_error.ok()) return add_error;
  DWORD remove_bits = 0;
  if (const Error remove_error = parse_letters(remove, remove_bits); !remove_error.ok()) {
    return remove_error;
  }

  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  const DWORD current = GetFileAttributesW(wide.c_str());
  if (current == INVALID_FILE_ATTRIBUTES) return fs_error("set attributes", path, GetLastError());

  constexpr DWORD kSettable = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL |
                              FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM |
                              FILE_ATTRIBUTE_TEMPORARY;
  DWORD next = (current | add_bits) & ~remove_bits;
  if (add_bits & FILE_ATTRIBUTE_NORMAL) {
    // NORMAL is only legal when no other attribute bit is set.
    next &= ~(kSettable & ~FILE_ATTRIBUTE_NORMAL);
  }
  next &= kSettable;
  if (next == 0) next = FILE_ATTRIBUTE_NORMAL;
  if (!SetFileAttributesW(wide.c_str(), next)) {
    return fs_error("set attributes", path, GetLastError());
  }
  return Error::none();
}

Error StorageService::set_time(const std::string& path, const std::string& which,
                               const std::int64_t unix_ms) {
  FILETIME target{};
  if (const Error time_error = unix_ms_to_filetime(unix_ms, target); !time_error.ok()) {
    return time_error;
  }
  const bool is_mtime = which == "mtime";
  const bool is_atime = which == "atime";
  const bool is_ctime = which == "ctime";
  if (!is_mtime && !is_atime && !is_ctime) {
    return contract_error("setTime which must be one of mtime, atime, ctime");
  }
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  HandleGuard file(CreateFileW(wide.c_str(), FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!file) return fs_error("set time", path, GetLastError());
  FILETIME access_time = target;
  FILETIME write_time = target;
  FILETIME create_time = target;
  if (!SetFileTime(file.get(), is_ctime ? &create_time : nullptr, is_atime ? &access_time : nullptr,
                   is_mtime ? &write_time : nullptr)) {
    return fs_error("set time", path, GetLastError());
  }
  return Error::none();
}

Error StorageService::recycle(const std::string& path) {
  std::wstring wide;
  if (const Error path_error = wide_path(path, "path", wide); !path_error.ok()) return path_error;
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return fs_error("recycle", path, GetLastError());

  // SHFileOperationW walks a double-null-terminated multi-string.
  std::wstring from = wide;
  from.push_back(L'\0');
  from.push_back(L'\0');
  SHFILEOPSTRUCTW operation{};
  operation.wFunc = FO_DELETE;
  operation.pFrom = from.c_str();
  operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  const int result = SHFileOperationW(&operation);
  if (result != 0 || operation.fAnyOperationsAborted) {
    return {Code::ExecutionFailed,
            "recycle failed for " + path + " (shell error " + std::to_string(result) + ")"};
  }
  return Error::none();
}

Error StorageService::recycle_empty(const std::string& root) {
  std::wstring wide;
  if (!root.empty()) {
    if (const Error path_error = wide_path(root, "root", wide); !path_error.ok()) return path_error;
  }
  const HRESULT status =
      SHEmptyRecycleBinW(nullptr, root.empty() ? nullptr : wide.c_str(),
                         SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
  if (FAILED(status)) return hresult_error("recycle bin empty", status);
  return Error::none();
}

Error StorageService::make_shortcut(const std::string& link_path, const std::string& target,
                                    const std::string& args, const std::string& workdir,
                                    const std::string& icon, const std::string& description) {
  if (link_path.empty()) return contract_error("shortcut path must not be empty");
  if (target.empty()) return contract_error("shortcut target must not be empty");
  std::wstring wide_link;
  if (const Error path_error = wide_path(link_path, "path", wide_link); !path_error.ok()) {
    return path_error;
  }
  std::wstring wide_target;
  if (const Error path_error = wide_path(target, "target", wide_target); !path_error.ok()) {
    return path_error;
  }

  ComApartment apartment(COINIT_MULTITHREADED);
  if (!apartment.ok()) return hresult_error("shortcut COM initialization", apartment.hr());

  ComPtr<IShellLinkW> link;
  HRESULT status = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                    reinterpret_cast<void**>(link.put()));
  if (FAILED(status)) return hresult_error("shortcut create", status);
  if (FAILED(status = link->SetPath(wide_target.c_str()))) {
    return hresult_error("shortcut target", status);
  }
  if (!args.empty()) link->SetArguments(from_utf8(args).c_str());
  if (!workdir.empty()) link->SetWorkingDirectory(from_utf8(workdir).c_str());
  if (!icon.empty()) link->SetIconLocation(from_utf8(icon).c_str(), 0);
  if (!description.empty()) link->SetDescription(from_utf8(description).c_str());

  ComPtr<IPersistFile> file;
  if (FAILED(status = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put())))) {
    return hresult_error("shortcut save", status);
  }
  if (FAILED(status = file->Save(wide_link.c_str(), TRUE))) {
    return hresult_error("shortcut save", status);
  }
  return Error::none();
}

Error StorageService::file_install(const std::string& src, const std::string& dst,
                                   const bool overwrite) {
  // AHK embeds the source into the script binary at compile time; this
  // runtime has no compile step, so the operation is a copy (documented
  // deviation in docs/api/storage.md).
  return file_copy(src, dst, overwrite);
}

Error StorageService::env_set(const std::string& name, const std::string& value) {
  if (name.empty()) return contract_error("environment variable name must not be empty");
  if (name.find('=') != std::string::npos) {
    return contract_error("environment variable name must not contain '=': " + name);
  }
  std::wstring wide_name;
  if (const Error name_error = wide_path(name, "name", wide_name); !name_error.ok()) {
    return name_error;
  }
  const std::wstring wide_value = from_utf8(value);
  if (!value.empty() && wide_value.empty()) {
    return contract_error("environment variable value is not valid UTF-8");
  }
  if (!SetEnvironmentVariableW(wide_name.c_str(), wide_value.c_str())) {
    return win32_error("environment write", GetLastError());
  }
  return Error::none();
}

Error StorageService::ini_write(const std::string& path, const std::string& section,
                                const std::string& key, const std::string& value) const {
  if (section.empty()) return contract_error("ini section must not be empty");
  if (key.empty()) return contract_error("ini key must not be empty");
  std::wstring full;
  if (const Error path_error = ini_full_path(path, "ini write", full); !path_error.ok()) {
    return path_error;
  }
  if (!ini_file_exists(full)) {
    // AHK's IniWrite creates a missing file; the containing directory must
    // already exist, which is reported as a stable "path not found".
    const std::wstring parent = parent_of(full);
    const DWORD attributes = parent.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      return {Code::ExecutionFailed, "path not found: " + path};
    }
  }
  const std::wstring wide_section = from_utf8(section);
  const std::wstring wide_key = from_utf8(key);
  const std::wstring wide_value = from_utf8(value);
  if (!WritePrivateProfileStringW(wide_section.c_str(), wide_key.c_str(), wide_value.c_str(),
                                  full.c_str())) {
    return fs_error("ini write", path, GetLastError());
  }
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, full.c_str());
  return Error::none();
}

Error StorageService::ini_delete(const std::string& path, const std::string& section,
                                 const std::string& key) const {
  if (section.empty()) return contract_error("ini section must not be empty");
  std::wstring full;
  if (const Error path_error = ini_full_path(path, "ini delete", full); !path_error.ok()) {
    return path_error;
  }
  if (!ini_file_exists(full)) return {Code::ExecutionFailed, "file not found: " + path};

  const std::wstring wide_section = from_utf8(section);
  if (!ini_section_exists(full, wide_section)) {
    return {Code::ExecutionFailed, "ini section not found: " + section};
  }
  std::wstring wide_key;
  if (!key.empty()) {
    wide_key = from_utf8(key);
    std::wstring value;
    bool found = false;
    if (const Error read_error = ini_read_value(full, wide_section, wide_key, value, found);
        !read_error.ok()) {
      return read_error;
    }
    if (!found) return {Code::ExecutionFailed, "ini key not found: " + section + "/" + key};
  }
  // A null key value deletes the whole section, which is what an empty key
  // means in this contract.
  const wchar_t* delete_key = key.empty() ? nullptr : wide_key.c_str();
  if (!WritePrivateProfileStringW(wide_section.c_str(), delete_key, nullptr, full.c_str())) {
    return fs_error("ini delete", path, GetLastError());
  }
  WritePrivateProfileStringW(nullptr, nullptr, nullptr, full.c_str());
  return Error::none();
}

Error StorageService::drive_set_label(const std::string& letter, const std::string& label) {
  std::wstring root;
  char canonical = 0;
  if (const Error drive_error = drive_root(letter, root, canonical); !drive_error.ok()) {
    return drive_error;
  }
  if (!drive_exists(root)) return drive_missing(canonical);
  const std::wstring wide_label = from_utf8(label);
  if (!label.empty() && wide_label.empty()) return contract_error("drive label is not valid UTF-8");
  if (!SetVolumeLabelW(root.c_str(), label.empty() ? nullptr : wide_label.c_str())) {
    return win32_error("set volume label", GetLastError());
  }
  return Error::none();
}

namespace {

// Opens the "\\\\.\\X:" device handle shared by the media lock and the
// eject/retract pair; a letter that resolves to no root is a TargetGone
// before any device is opened.
Error open_drive_device(const std::string& letter, HandleGuard& out, char& canonical) {
  std::wstring root;
  if (const Error drive_error = drive_root(letter, root, canonical); !drive_error.ok()) {
    return drive_error;
  }
  if (!drive_exists(root)) return drive_missing(canonical);
  std::wstring device = L"\\\\.\\";
  device.push_back(static_cast<wchar_t>(canonical));
  device.push_back(L':');
  out = HandleGuard(CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr));
  if (!out) return win32_error("drive open", GetLastError());
  return Error::none();
}

}  // namespace

Error StorageService::drive_lock(const std::string& letter) {
  HandleGuard drive;
  char canonical = 0;
  if (const Error open_error = open_drive_device(letter, drive, canonical); !open_error.ok()) {
    return open_error;
  }
  PREVENT_MEDIA_REMOVAL prevention{};
  prevention.PreventMediaRemoval = TRUE;
  DWORD returned = 0;
  if (!DeviceIoControl(drive.get(), IOCTL_STORAGE_MEDIA_REMOVAL, &prevention, sizeof(prevention),
                       nullptr, 0, &returned, nullptr)) {
    return win32_error("drive lock", GetLastError());
  }
  return Error::none();
}

Error StorageService::drive_unlock(const std::string& letter) {
  HandleGuard drive;
  char canonical = 0;
  if (const Error open_error = open_drive_device(letter, drive, canonical); !open_error.ok()) {
    return open_error;
  }
  PREVENT_MEDIA_REMOVAL prevention{};
  prevention.PreventMediaRemoval = FALSE;
  DWORD returned = 0;
  if (!DeviceIoControl(drive.get(), IOCTL_STORAGE_MEDIA_REMOVAL, &prevention, sizeof(prevention),
                       nullptr, 0, &returned, nullptr)) {
    return win32_error("drive unlock", GetLastError());
  }
  return Error::none();
}

Error StorageService::drive_eject(const std::string& letter) {
  HandleGuard drive;
  char canonical = 0;
  if (const Error open_error = open_drive_device(letter, drive, canonical); !open_error.ok()) {
    return open_error;
  }
  DWORD returned = 0;
  if (!DeviceIoControl(drive.get(), IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &returned,
                       nullptr)) {
    return win32_error("drive eject", GetLastError());
  }
  return Error::none();
}

Error StorageService::drive_retract(const std::string& letter) {
  HandleGuard drive;
  char canonical = 0;
  if (const Error open_error = open_drive_device(letter, drive, canonical); !open_error.ok()) {
    return open_error;
  }
  DWORD returned = 0;
  if (!DeviceIoControl(drive.get(), IOCTL_STORAGE_LOAD_MEDIA, nullptr, 0, nullptr, 0, &returned,
                       nullptr)) {
    return win32_error("drive retract", GetLastError());
  }
  return Error::none();
}

// ---- session settings -----------------------------------------------------

std::string StorageService::encoding() const {
  std::lock_guard<std::mutex> lock(encoding_mutex_);
  return encoding_;
}

std::string StorageService::current_encoding() const { return encoding(); }

Error StorageService::set_encoding(std::string enc) {
  Encoding parsed = Encoding::Utf8;
  if (const Error enc_error = parse_encoding(enc, parsed); !enc_error.ok()) return enc_error;
  std::lock_guard<std::mutex> lock(encoding_mutex_);
  encoding_ = encoding_name(parsed);
  return Error::none();
}

// ---- download -------------------------------------------------------------

Error StorageService::download(const std::string& url, const std::string& path,
                               rime::core::CancellationToken cancel,
                               const std::int64_t deadline_unix_ms, std::uint64_t& bytes_out) {
  bytes_out = 0;
  HttpUrl parsed;
  if (const Error url_error = parse_http_url(url, parsed); !url_error.ok()) return url_error;
  if (cancel.cancelled()) return {Code::Cancelled, "download was cancelled before it started"};
  std::wstring destination_path;
  if (const Error path_error = wide_path(path, "path", destination_path); !path_error.ok()) {
    return path_error;
  }

  // Loopback transfers never consult a proxy: tests and local services must
  // not depend on the machine's proxy configuration.
  const DWORD access_type =
      is_loopback_host(parsed.host) ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
  WinHttpHandle session(WinHttpOpen(L"Rime-Storage/1.0", access_type, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) return win32_error("download session", GetLastError());
  WinHttpHandle connect(WinHttpConnect(session.get(), parsed.host.c_str(), parsed.port, 0));
  if (!connect) return win32_error("download connect", GetLastError());
  WinHttpHandle request(
      WinHttpOpenRequest(connect.get(), L"GET", parsed.object.c_str(), nullptr, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, parsed.secure ? WINHTTP_FLAG_SECURE : 0));
  if (!request) return win32_error("download request", GetLastError());
  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                          0, 0)) {
    return win32_error("download send", GetLastError());
  }
  if (!WinHttpReceiveResponse(request.get(), nullptr)) {
    return win32_error("download receive", GetLastError());
  }
  DWORD status_code = 0;
  DWORD status_size = sizeof(status_code);
  if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                           WINHTTP_NO_HEADER_INDEX)) {
    return win32_error("download status", GetLastError());
  }
  if (status_code < 200 || status_code > 299) {
    return {Code::ExecutionFailed,
            "download failed with HTTP status " + std::to_string(status_code) + ": " + url};
  }

  HandleGuard destination(CreateFileW(destination_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!destination) return fs_error("download write", path, GetLastError());

  // Any failure after the destination exists removes the partial file, so a
  // cancelled or broken transfer never leaves half a download behind.
  auto abort_transfer = [&](const Error& error) -> Error {
    destination.reset();
    DeleteFileW(destination_path.c_str());
    return error;
  };

  std::uint64_t total = 0;
  for (;;) {
    if (cancel.cancelled()) {
      return abort_transfer({Code::Cancelled, "download was cancelled"});
    }
    if (deadline_unix_ms > 0 && now_unix_ms() > deadline_unix_ms) {
      return abort_transfer({Code::Timeout, "download exceeded its deadline"});
    }
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available)) {
      return abort_transfer(win32_error("download read", GetLastError()));
    }
    if (available == 0) break;
    const DWORD want = available > kDownloadChunk ? kDownloadChunk : available;
    std::vector<std::uint8_t> buffer(want);
    DWORD got = 0;
    if (!WinHttpReadData(request.get(), buffer.data(), want, &got)) {
      return abort_transfer(win32_error("download read", GetLastError()));
    }
    if (got == 0) break;
    buffer.resize(got);
    if (const Error write_error = write_all(destination.get(), buffer, "download write", path);
        !write_error.ok()) {
      return abort_transfer(write_error);
    }
    total += got;
  }
  bytes_out = total;
  return Error::none();
}

// ---- selectors ------------------------------------------------------------

void StorageService::set_ui_thread(UiThread* ui) { ui_thread_ = ui; }

Error StorageService::select_file(const std::string& filter, const std::string& default_name,
                                  const bool multi, std::vector<std::string>& out,
                                  rime::core::CancellationToken cancel) {
  out.clear();
  if (cancel.cancelled()) return {Code::Cancelled, kCancelledBeforeStart};
  UiThread* ui = ui_thread_;
  if (ui == nullptr) return {Code::InvalidState, kNoUiThread};

  std::wstring filter_name;
  std::wstring filter_spec;
  if (const Error filter_error = parse_selector_filter(filter, filter_name, filter_spec);
      !filter_error.ok()) {
    return filter_error;
  }
  std::wstring wide_default;
  if (!default_name.empty()) {
    if (default_name.find_first_of("\\/:") != std::string::npos) {
      return contract_error("file selector default name must not contain a path separator");
    }
    wide_default = from_utf8(default_name);
    if (wide_default.empty()) {
      return contract_error("file selector default name is not valid UTF-8: " + default_name);
    }
  }

  const auto owner = reinterpret_cast<HWND>(ui->message_window());
  std::vector<std::string> collected;
  Error task_error = Error::none();
  // The queued phase is bounded by the timeout; once the pump claims the
  // task the modal dialog runs to completion (documented in the header).
  const Error call_error = ui->call(
      [&] {
        if (cancel.cancelled()) {
          task_error = {Code::Cancelled, kCancelledBeforeStart};
          return;
        }
        task_error = run_file_dialog(filter_name, filter_spec, wide_default, multi, owner, collected);
      },
      std::chrono::seconds(30), cancel);
  if (!call_error.ok()) return call_error;
  if (!task_error.ok()) return task_error;
  out = std::move(collected);
  return Error::none();
}

Error StorageService::select_dir(const std::string& caption, std::string& out,
                                 rime::core::CancellationToken cancel) {
  out.clear();
  if (cancel.cancelled()) return {Code::Cancelled, kCancelledBeforeStart};
  UiThread* ui = ui_thread_;
  if (ui == nullptr) return {Code::InvalidState, kNoUiThread};

  std::wstring wide_caption;
  if (!caption.empty()) {
    wide_caption = from_utf8(caption);
    if (wide_caption.empty()) {
      return contract_error("directory selector caption is not valid UTF-8: " + caption);
    }
  }

  const auto owner = reinterpret_cast<HWND>(ui->message_window());
  std::string selected;
  Error task_error = Error::none();
  const Error call_error = ui->call(
      [&] {
        if (cancel.cancelled()) {
          task_error = {Code::Cancelled, kCancelledBeforeStart};
          return;
        }
        task_error = run_dir_dialog(wide_caption, owner, selected);
      },
      std::chrono::seconds(30), cancel);
  if (!call_error.ok()) return call_error;
  if (!task_error.ok()) return task_error;
  out = std::move(selected);
  return Error::none();
}

}  // namespace rime::win32
