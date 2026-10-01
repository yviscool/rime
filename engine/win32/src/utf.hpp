#pragma once

// NOTE: windows.h is deliberately kept in this src-internal header; moving it
// would cascade through the win32 sources. Its macro pollution (min/max and
// friends) is a known hazard -- include this header after standard headers
// and rely on NOMINMAX / WIN32_LEAN_AND_MEAN from the build.
#include <windows.h>

#include <limits>
#include <string>

namespace rime::win32 {

// Win32 boundary conversions. Invalid sequences are converted best-effort;
// JS and JSON boundaries are always UTF-8.

[[nodiscard]] inline std::string to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  // WideCharToMultiByte takes an int length; refuse to truncate instead of
  // wrapping around on oversized input.
  if (wide.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                          nullptr, nullptr);
  if (size <= 0) return {};
  std::string text(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(),
                           size, nullptr, nullptr) == 0) {
    return {};
  }
  return text;
}

[[nodiscard]] inline std::wstring from_utf8(const std::string& text) {
  if (text.empty()) return {};
  // MultiByteToWideChar takes an int length; refuse to truncate instead of
  // wrapping around on oversized input.
  if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                           size) == 0) {
    return {};
  }
  return wide;
}

}  // namespace rime::win32
