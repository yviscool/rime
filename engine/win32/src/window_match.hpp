#pragma once

#include "rime/win32/window.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rime::win32::detail {

// TargetGone message for "the id behind this window is gone". The text is
// observable (executor errors, JS rejections, tests), so every call site in
// every translation unit shares this one spelling.
inline constexpr const char* kWindowGoneMessage = "window no longer exists";

// Shared by every WindowService entry point: a non-positive timeout is
// rejected before any UI work is queued.
rime::core::Error expired_deadline();

std::wstring window_text(HWND window);

// Image identity behind `pid`: one OpenProcess + QueryFullProcessImageNameW
// yields both the full path (WinGetProcessPath) and the final path segment
// (processName / ahk_exe matching). Both parts are empty when the process
// cannot be opened (protected/system processes stay readable as windows).
struct ProcessImage {
  std::wstring path;     // full image path, empty when unreadable
  std::wstring basename;  // final path segment (extension kept)
};

// Per-enumeration pid -> image cache. A single query() enum touches every
// top-level window; without this each window pays OpenProcess plus a 64KB path
// buffer even when dozens share one pid. The snapshot reads processName and
// processPath per window, so the cache only dedupes repeated pids; callers
// without a shared cache (single-window probes and reads) pass nullptr and go
// straight to process_image.
using PidImageCache = std::unordered_map<std::uint32_t, ProcessImage>;

// Process-wide generation handed out once per registry, so ids issued by one
// WindowService can never resolve inside a newer one: an id is
// [generation:32][sequence:32], generation only grows, and lookups reject
// foreign generations before scanning. That turns a stale id held across a
// service restart into target_gone instead of a different window with the
// same sequence number.
inline std::atomic<std::uint32_t> next_window_generation{1};

// UI-thread-only mapping from stable ids to live HWNDs. Stale entries are
// dropped on lookup; ids are never recycled inside one service (the
// sequence is masked to its 32 bits, which cannot wrap in a service's
// lifetime).
class WindowRegistry final {
 public:
  std::uint64_t id_for(HWND window) {
    prune();
    for (const auto& [id, hwnd] : entries_) {
      if (hwnd == window) return id;
    }
    const std::uint64_t id = (static_cast<std::uint64_t>(generation_) << 32) |
                             (++next_id_ & 0xFFFFFFFFull);
    entries_.emplace_back(id, window);
    return id;
  }

  HWND hwnd_for(const std::uint64_t id) {
    if (static_cast<std::uint32_t>(id >> 32) != generation_) return nullptr;
    prune();
    for (const auto& [entry_id, hwnd] : entries_) {
      if (entry_id == id) return hwnd;
    }
    return nullptr;
  }

 private:
  void prune() {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (!IsWindow(it->second)) {
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::vector<std::pair<std::uint64_t, HWND>> entries_;
  const std::uint32_t generation_{next_window_generation.fetch_add(1)};
  std::uint64_t next_id_{0};
};

// WinGroup state (AHK WinGroup): the named query-spec lists plus the shared
// visited-window cycle driving activate/deactivate/close. UI-thread only, so
// no lock protects it - every accessor runs inside ui.call. It lives beside
// WindowRegistry rather than in window.hpp because the complete type needs
// HWND; WindowService::Impl (window.cpp) owns one, window_groups.cpp owns
// every mutation, and window.hpp only forward-declares it.
struct WindowGroups {
  std::unordered_map<std::string, std::vector<WindowQuery>> groups;
  std::string last_group;
  std::vector<HWND> visited;
};

rime::core::Error build_info(WindowRegistry& registry, HWND window, WindowInfo& out);

// Compiles one AHK-style regex pattern: an optional run of option letters
// followed by ')' (AHK's `i)`/`m)`/`s)` prefix) selects case/line/dot
// semantics, everything else is the pattern body. AHK's regex default is
// case-sensitive, which std::wregex already gives us. Only `i)` maps onto a
// std::wregex flag (icase); `m)` and `s)` would need multiline/dotall, which
// MSVC's std::regex does not provide, so they fail loudly (Unsupported)
// instead of silently changing match semantics. An unknown option letter or
// an invalid pattern fails with a typed error, never silently ignored.
rime::core::Error compile_window_regex(const std::string& utf8,
                                       std::optional<std::wregex>& out);

// A query with the global defaults resolved and any regex compiled once for
// the whole request (never per window). Built on the caller thread before
// the UI hop: std::wregex touches no HWND, and an invalid pattern then
// fails the service call without queueing UI work.
struct ResolvedQuery {
  // Null selects the list() baseline: the DetectHiddenWindows visibility
  // filter carried in `include_hidden` plus the untitled-window skip, and no
  // WinTitle selectors at all.
  const WindowQuery* query{nullptr};
  TitleMatchMode mode{TitleMatchMode::Contains};
  bool include_hidden{false};
  std::optional<std::wregex> title_regex;
  std::optional<std::wregex> class_regex;
  std::optional<std::wregex> exe_regex;
};

rime::core::Error resolve_query(const WindowQuery& query, const WindowSettings& settings,
                                ResolvedQuery& out);

bool has_selectors(const WindowQuery& query);

// WinTitle-style matching evaluated on the UI lane only. A null
// `resolved.query` is the list() baseline (see ResolvedQuery): visibility
// plus untitled-window filter, no selectors. `cache` dedupes
// pid -> basename lookups within one enumeration; pass nullptr for single
// foreground lookups. Title matching is case-sensitive in every mode
// (AHK WinTitle rule; only RegEx `i)` opts back in), class stays
// case-insensitive (AHK v2.1) and exe keeps the ASCII-fold rule.
bool matches_query(WindowRegistry& registry, HWND window, const ResolvedQuery& resolved,
                   PidImageCache* cache);

// EnumWindows callback context for query() and list(). `resolved` is never
// null: the list() baseline passes a ResolvedQuery with a null `query` (the
// DetectHiddenWindows visibility flag lives there too, so the baseline has
// one filter path). `images` is null on the baseline, so build_info falls
// back to an uncached process-image read.
struct EnumContext {
  WindowRegistry* registry;
  const ResolvedQuery* resolved;
  bool selectors;
  std::vector<WindowInfo>* windows;
  PidImageCache* images;
};

BOOL CALLBACK collect_matching(HWND window, LPARAM parameter);

// Shared ClassNN numbering behind WinGetControls and the MouseGetPos control
// probe: instance numbers run per class across the whole enumeration, matched
// case-insensitively (CompareStringOrdinal) and capped at AHK's 99999. One
// implementation so the two paths can never drift apart.
struct ClassNNCounter {
  // (class, instances seen so far)
  std::vector<std::pair<std::wstring, int>> counts;
};

struct ClassNNInstance {
  std::wstring class_name;
  int number{0};  // 0: skip the control (no class name, or past the cap)
};

// Bumps the per-class count for `control` and returns its 1-based instance.
ClassNNInstance next_class_nn(ClassNNCounter& counter, HWND control);

}  // namespace rime::win32::detail
