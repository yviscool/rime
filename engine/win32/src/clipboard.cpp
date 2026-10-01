#include "rime/win32/clipboard.hpp"

#include "utf.hpp"

#include <windows.h>

#include <cstring>
#include <string>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

struct ClipboardGuard {
  ~ClipboardGuard() { CloseClipboard(); }
};

constexpr int kOpenAttempts = 5;

bool open_clipboard() {
  for (int attempt = 0; attempt < kOpenAttempts; ++attempt) {
    if (OpenClipboard(nullptr)) return true;
    Sleep(10);
  }
  return false;
}

}  // namespace

Error ClipboardService::read_text(std::string& out) const {
  out.clear();
  if (!open_clipboard()) return {Code::ExecutionFailed, "clipboard is busy"};
  ClipboardGuard guard;
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return Error::none();
  const HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (!data) return {Code::ExecutionFailed, "cannot read clipboard text"};
  const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
  if (!text) return {Code::ExecutionFailed, "cannot lock clipboard text"};
  out = to_utf8(std::wstring(text));
  GlobalUnlock(data);
  return Error::none();
}

Error ClipboardService::write_text(const std::string& utf8_text) const {
  const std::wstring wide = from_utf8(utf8_text);
  if (!open_clipboard()) return {Code::ExecutionFailed, "clipboard is busy"};
  ClipboardGuard guard;
  if (!EmptyClipboard()) return {Code::ExecutionFailed, "cannot clear the clipboard"};
  const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
  const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!memory) return {Code::ExecutionFailed, "cannot allocate a clipboard buffer"};
  void* target = GlobalLock(memory);
  if (!target) {
    GlobalFree(memory);
    return {Code::ExecutionFailed, "cannot lock the clipboard buffer"};
  }
  std::memcpy(target, wide.c_str(), bytes);
  GlobalUnlock(memory);
  if (!SetClipboardData(CF_UNICODETEXT, memory)) {
    GlobalFree(memory);
    return {Code::ExecutionFailed, "cannot set clipboard text"};
  }
  // On success the system owns the buffer.
  return Error::none();
}

}  // namespace rime::win32
