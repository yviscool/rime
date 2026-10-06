#include "rime/win32/clipboard.hpp"

#include "utf.hpp"

#include <windows.h>

#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

struct ClipboardGuard {
  ClipboardGuard() = default;

  ClipboardGuard(const ClipboardGuard&) = delete;
  ClipboardGuard& operator=(const ClipboardGuard&) = delete;

  ClipboardGuard(ClipboardGuard&& other) noexcept
      : owns(std::exchange(other.owns, false)) {}

  ClipboardGuard& operator=(ClipboardGuard&& other) noexcept {
    if (this != &other) {
      reset();
      owns = std::exchange(other.owns, false);
    }
    return *this;
  }

  ~ClipboardGuard() { reset(); }

  void reset() {
    if (owns) {
      CloseClipboard();
      owns = false;
    }
  }

 private:
  bool owns{true};
};

constexpr int kOpenAttempts = 5;

// OpenClipboard requires a thread with a message queue and fails while
// another window holds the clipboard open. Callers must route through
// UiThread::call (or another message-queue thread); the short Sleep(10)
// retry loop is kept intentionally to avoid behavior churn.
// TODO: centralize this retry/throttle policy in the scheduler instead of
// scattering Sleep-based retries across modules.
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
  // Bound the view with GlobalSize instead of trusting NUL termination: the
  // clipboard owner could hand us an unterminated buffer.
  const SIZE_T bytes = GlobalSize(data);
  if (bytes == 0 || bytes % sizeof(wchar_t) != 0) {
    return {Code::ExecutionFailed, "cannot size clipboard text"};
  }
  const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
  if (!text) return {Code::ExecutionFailed, "cannot lock clipboard text"};
  std::wstring_view view(text, bytes / sizeof(wchar_t));
  // CF_UNICODETEXT is conventionally NUL-terminated; drop trailing NULs so an
  // "empty" clipboard reads back as an empty string.
  while (!view.empty() && view.back() == L'\0') view.remove_suffix(1);
  // utf.hpp only takes std::wstring, so the bounded view is materialized here
  // instead of changing its signature.
  out = to_utf8(std::wstring(view));
  GlobalUnlock(data);
  return Error::none();
}

Error ClipboardService::write_text(const std::string& utf8_text) const {
  const std::wstring wide = from_utf8(utf8_text);
  if (!open_clipboard()) return {Code::ExecutionFailed, "clipboard is busy"};
  ClipboardGuard guard;
  if (!EmptyClipboard()) return {Code::ExecutionFailed, "cannot clear the clipboard"};
  // An empty string leaves the clipboard truly empty: no CF_UNICODETEXT at
  // all, which is what AHK's `Clipboard := ""` does (source/clipboard.cpp
  // spells out that it wants a truly empty clipboard "for use with functions
  // such as ClipWait"). Storing a lone NUL instead would keep the text format
  // available, so IsClipboardFormatAvailable would report text forever and
  // ClipWait could never observe an empty clipboard.
  if (wide.empty()) {
    self_write_.store(true, std::memory_order_release);
    return Error::none();
  }
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
  // On success the system owns the buffer. Mark the change as ours so the
  // next WM_CLIPBOARDUPDATE fans out with from_self=true (AHK type 1).
  self_write_.store(true, std::memory_order_release);
  return Error::none();
}

bool ClipboardService::has_wait_data(bool any_data) const {
  if (any_data) return CountClipboardFormats() != 0;
  // AHK's ClipWait predicate (wait.cpp:78 uses CF_NATIVETEXT, which is
  // CF_UNICODETEXT on every platform we target - the Windows SDK does not
  // define the alias): a file drop counts as content too, because the
  // implicit CF_HDROP -> text conversion would make it usable as text.
  return IsClipboardFormatAvailable(CF_UNICODETEXT) ||
         IsClipboardFormatAvailable(CF_HDROP);
}

std::uint64_t ClipboardService::add_change_listener(ChangeListener listener) {
  if (!listener) return 0;
  std::lock_guard lock(listeners_mutex_);
  const std::uint64_t id = next_listener_id_.fetch_add(1, std::memory_order_relaxed);
  listeners_.push_back(Listener{id, std::move(listener)});
  return id;
}

bool ClipboardService::remove_change_listener(const std::uint64_t id) {
  std::lock_guard lock(listeners_mutex_);
  for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
    if (it->id == id) {
      listeners_.erase(it);
      return true;
    }
  }
  return false;
}

std::size_t ClipboardService::change_listener_count() const {
  std::lock_guard lock(listeners_mutex_);
  return listeners_.size();
}

void ClipboardService::notify_change() {
  // Consume the self flag once per update so every listener of this update
  // sees the same type, and a stale flag cannot leak into a later foreign
  // change.
  const bool from_self = self_write_.exchange(false, std::memory_order_acq_rel);
  std::vector<ChangeListener> snapshot;
  {
    std::lock_guard lock(listeners_mutex_);
    snapshot.reserve(listeners_.size());
    for (const auto& listener : listeners_) snapshot.push_back(listener.fn);
  }
  for (const auto& listener : snapshot) {
    try {
      listener(from_self);
    } catch (...) {
      // Listeners cross the UI-thread boundary; they must not throw out.
    }
  }
}

}  // namespace rime::win32
