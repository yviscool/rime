#pragma once

#include <windows.h>

#include <string>

namespace rime::win32 {

// Win32 boundary conversions. Invalid sequences are converted best-effort;
// JS and JSON boundaries are always UTF-8.

inline std::string to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                          nullptr, nullptr);
  if (size <= 0) return {};
  std::string text(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size,
                      nullptr, nullptr);
  return text;
}

inline std::wstring from_utf8(const std::string& text) {
  if (text.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

}  // namespace rime::win32
