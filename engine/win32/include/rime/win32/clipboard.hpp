#pragma once

#include "rime/core/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rime::win32 {

// Text clipboard over CF_UNICODETEXT. The clipboard is a shared critical
// section: opens are retried briefly and always closed (RAII in the .cpp).
//
// Change listening (OnClipboardChange now, ClipWait in M5): listeners are
// notified on the thread that calls notify_change() (the UI thread, from the
// WM_CLIPBOARDUPDATE observer). The OS-level listener (AddClipboardFormat-
// Listener on the pump window) is owned by the caller of the module; the
// service only fans out and reports counts so tests can assert cleanup.
class ClipboardService final {
 public:
  // `from_self` marks the first notification after write_text() in this
  // process (AHK clipboard "own change" type 1 vs foreign 0).
  using ChangeListener = std::function<void(bool from_self)>;

  // Empty `out` (success) means the clipboard holds no text format.
  rime::core::Error read_text(std::string& out) const;
  rime::core::Error write_text(const std::string& utf8_text) const;

  // Thread-safe listener registry. add returns a monotonic id; the caller
  // watches change_listener_count() to attach/detach the OS listener.
  // remove is idempotent.
  std::uint64_t add_change_listener(ChangeListener listener);
  bool remove_change_listener(std::uint64_t id);
  [[nodiscard]] std::size_t change_listener_count() const;

  // Delivers one clipboard update to every listener with the consumed
  // self-change flag. Called by the WM_CLIPBOARDUPDATE relay; a write with
  // no listener leaves the flag pending until the next update.
  void notify_change();

 private:
  struct Listener {
    std::uint64_t id{0};
    ChangeListener fn;
  };
  mutable std::mutex listeners_mutex_;
  mutable std::vector<Listener> listeners_;
  mutable std::atomic<std::uint64_t> next_listener_id_{1};
  mutable std::atomic<bool> self_write_{false};
};

}  // namespace rime::win32
