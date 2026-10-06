#include "rime/win32/clipboard.hpp"

#include "utf.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// ClipboardAll blob layout: a header identifying the format, then the record
// sequence Var::GetClipboardAll emits (rime-research .../var.cpp:312): for
// each format, [UINT32 format][UINT32 size][size bytes], closed by a zero
// format. Sizes are 32-bit even on x64, as in AHK, so a blob stays portable
// between the two word sizes.
constexpr std::uint8_t kSnapshotMagic[4] = {'R', 'I', 'M', 'B'};
constexpr std::uint32_t kSnapshotVersion = 1;
constexpr std::size_t kSnapshotHeaderBytes = sizeof(kSnapshotMagic) + 8;  // magic + version + reserved
constexpr std::uint32_t kMaxRecordBytes = 0x3FFFFFFFu;                     // records stay under 1GiB

void append_u32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

// Returns false (leaving cursor where it was) when fewer than four bytes
// remain, so a caller can never read past the end of a truncated blob.
bool read_u32(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint32_t& value) {
  if (cursor > end || static_cast<std::size_t>(end - cursor) < sizeof(std::uint32_t)) return false;
  value = static_cast<std::uint32_t>(cursor[0]) | (static_cast<std::uint32_t>(cursor[1]) << 8) |
          (static_cast<std::uint32_t>(cursor[2]) << 16) |
          (static_cast<std::uint32_t>(cursor[3]) << 24);
  cursor += 4;
  return true;
}

// Full structural check, without opening the clipboard, so a foreign or
// corrupted buffer is rejected before EmptyClipboard() discards anything.
// Every caller therefore walks a blob that is known to be well formed.
bool snapshot_is_well_formed(const std::uint8_t* begin, const std::uint8_t* end,
                             std::string& error) {
  const auto fail = [&error](std::string_view message) {
    error.assign(message);
    return false;
  };
  if (!begin || end < begin || static_cast<std::size_t>(end - begin) < kSnapshotHeaderBytes) {
    return fail("clipboard snapshot is truncated");
  }
  if (!std::equal(begin, begin + sizeof(kSnapshotMagic), kSnapshotMagic)) {
    return fail("not a clipboard snapshot");
  }
  const std::uint8_t* cursor = begin + sizeof(kSnapshotMagic);
  std::uint32_t version = 0;
  std::uint32_t reserved = 0;
  if (!read_u32(cursor, end, version) || !read_u32(cursor, end, reserved)) {
    return fail("clipboard snapshot header is truncated");
  }
  if (version != kSnapshotVersion) return fail("unsupported clipboard snapshot version");
  for (;;) {
    std::uint32_t format = 0;
    if (!read_u32(cursor, end, format)) return fail("clipboard snapshot has no terminator");
    if (format == 0) return true;
    std::uint32_t size = 0;
    if (!read_u32(cursor, end, size)) return fail("clipboard snapshot record header is truncated");
    if (size > kMaxRecordBytes || cursor > end ||
        static_cast<std::size_t>(end - cursor) < size) {
      return fail("clipboard snapshot record runs past the end");
    }
    cursor += size;
  }
}


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

Error ClipboardService::save_all(std::vector<std::uint8_t>& out) const {
  out.clear();
  out.reserve(kSnapshotHeaderBytes);
  out.insert(out.end(), kSnapshotMagic, kSnapshotMagic + sizeof(kSnapshotMagic));
  append_u32(out, kSnapshotVersion);
  append_u32(out, 0);  // reserved
  if (!open_clipboard()) return {Code::ExecutionFailed, "clipboard is busy"};
  ClipboardGuard guard;
  // AHK skips the formats whose handle is not safe to GlobalSize, the text
  // formats synthesis always reconstructs from CF_UNICODETEXT, and one half
  // of CF_DIB/CF_DIBV5 - the rest is copied byte for byte (var.cpp:312).
  UINT omit_dib_format = 0;
  for (UINT format = EnumClipboardFormats(0); format != 0;
       format = EnumClipboardFormats(format)) {
    switch (format) {
      case CF_BITMAP:
      case CF_ENHMETAFILE:
      case CF_DSPENHMETAFILE:
      case CF_TEXT:
      case CF_OEMTEXT:
        continue;
      default:
        break;
    }
    if (format == omit_dib_format) continue;
    if (format == CF_DIB) {
      omit_dib_format = CF_DIBV5;
    } else if (format == CF_DIBV5) {
      omit_dib_format = CF_DIB;
    }
    const HANDLE data = GetClipboardData(format);
    // A failed read skips only this format: AHK treats a partially saved
    // clipboard as far better than abandoning the snapshot.
    if (!data) continue;
    const SIZE_T bytes = GlobalSize(data);
    if (bytes > kMaxRecordBytes) continue;
    const auto* locked = static_cast<const std::uint8_t*>(bytes ? GlobalLock(data) : nullptr);
    if (bytes != 0 && !locked) continue;
    append_u32(out, format);
    append_u32(out, static_cast<std::uint32_t>(bytes));
    if (bytes != 0) {
      out.insert(out.end(), locked, locked + bytes);
      GlobalUnlock(data);
    }
  }
  append_u32(out, 0);  // terminator
  return Error::none();
}

Error ClipboardService::restore_all(const std::vector<std::uint8_t>& blob,
                                    std::uint32_t& formats_restored) const {
  formats_restored = 0;
  if (blob.size() < kSnapshotHeaderBytes) {
    return {Code::InvalidContract, "clipboard snapshot is truncated"};
  }
  const std::uint8_t* const begin = blob.data();
  const std::uint8_t* const end = begin + blob.size();
  std::string error;
  if (!snapshot_is_well_formed(begin, end, error)) return {Code::InvalidContract, error};
  if (!open_clipboard()) return {Code::ExecutionFailed, "clipboard is busy"};
  ClipboardGuard guard;
  if (!EmptyClipboard()) return {Code::ExecutionFailed, "cannot clear the clipboard"};
  const std::uint8_t* cursor = begin + kSnapshotHeaderBytes;
  for (;;) {
    std::uint32_t format = 0;
    if (!read_u32(cursor, end, format)) return {Code::InvalidContract, "clipboard snapshot has no terminator"};
    if (format == 0) break;
    // The record header is known to be complete: snapshot_is_well_formed
    // walked the same bytes first.
    std::uint32_t size = 0;
    if (!read_u32(cursor, end, size)) {
      return {Code::InvalidContract, "clipboard snapshot record header is truncated"};
    }
    // Zero-length records still get a non-zero HGLOBAL: SetClipboardData
    // rejects an empty buffer on current Windows (AHK v1.1.16).
    const HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, static_cast<SIZE_T>(size) + (size == 0));
    if (!memory) return {Code::ExecutionFailed, "cannot allocate a clipboard buffer"};
    if (size != 0) {
      void* target = GlobalLock(memory);
      if (!target) {
        GlobalFree(memory);
        return {Code::ExecutionFailed, "cannot lock the clipboard buffer"};
      }
      std::memcpy(target, cursor, size);
      GlobalUnlock(memory);
      cursor += size;
    }
    if (!SetClipboardData(format, memory)) {
      GlobalFree(memory);
      return {Code::ExecutionFailed, "cannot set clipboard data"};
    }
    ++formats_restored;  // The system owns the buffer now.
  }
  self_write_.store(true, std::memory_order_release);
  return Error::none();
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
