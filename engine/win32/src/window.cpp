#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

namespace lane = rime::core;

std::wstring window_text(HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) return {};
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(window, text.data(), length + 1);
  if (copied <= 0) return {};
  text.resize(static_cast<std::size_t>(copied));
  return text;
}

Rect to_rect(const RECT& rectangle) {
  return {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom};
}

bool work_area_for(HWND window, RECT& work) {
  const HMONITOR monitor =
      MonitorFromWindow(window != nullptr ? window : GetDesktopWindow(),
                        MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) return false;
  work = info.rcWork;
  return true;
}

bool resolve_placement(const RECT& work, const std::string_view placement, Rect& out) {
  const Rect area = to_rect(work);
  const long half_width = area.width() / 2;
  const long half_height = area.height() / 2;
  if (placement == "left") {
    out = {area.left, area.top, area.left + half_width, area.bottom};
  } else if (placement == "right") {
    out = {area.left + half_width, area.top, area.right, area.bottom};
  } else if (placement == "top") {
    out = {area.left, area.top, area.right, area.top + half_height};
  } else if (placement == "bottom") {
    out = {area.left, area.top + half_height, area.right, area.bottom};
  } else if (placement == "full") {
    out = area;
  } else {
    return false;
  }
  return true;
}

// Case-insensitive ASCII folding for ahk_exe basename compares. The rule is
// ASCII-only by design: bytes >= 0x80 compare unchanged, so non-ASCII image
// names fall back to exact-byte equality after folding. This documents the
// matching behavior; it does not normalize Unicode case.
[[nodiscard]] std::string fold_ascii(std::string_view text) {
  std::string folded(text);
  for (char& character : folded) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  return folded;
}

// File-local RAII for process handles (mirrors process.cpp HandleGuard style;
// kept local so window.cpp owns its lifetime explicitly, no cross-file reuse).
struct ProcessHandleGuard {
  explicit ProcessHandleGuard(HANDLE raw) : handle(raw) {}
  ProcessHandleGuard(const ProcessHandleGuard&) = delete;
  ProcessHandleGuard& operator=(const ProcessHandleGuard&) = delete;
  ~ProcessHandleGuard() {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
  [[nodiscard]] HANDLE get() const { return handle; }
  HANDLE handle = nullptr;
};

// Raw image basename behind `pid` as UTF-16 (extension kept); empty when it
// cannot be read.
[[nodiscard]] std::wstring process_image_basename_w(const DWORD process_id) {
  if (process_id == 0) return {};
  ProcessHandleGuard process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
  if (!process.get()) return {};
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size) || size == 0) {
    return {};
  }
  path.resize(size);
  const std::size_t slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos) path = path.substr(slash + 1);
  return path;
}

// Per-enumeration pid -> basename cache. A single query() enum touches every
// top-level window; without this each window pays OpenProcess plus a 64KB path
// buffer even when dozens share one pid. The snapshot still reads processName
// per window, so the cache only dedupes repeated pids.
using PidNameCache = std::unordered_map<std::uint32_t, std::wstring>;

[[nodiscard]] std::wstring process_basename_cached(const DWORD process_id,
                                                  PidNameCache* cache) {
  if (process_id == 0) return {};
  if (cache == nullptr) return process_image_basename_w(process_id);
  const auto found = cache->find(process_id);
  if (found != cache->end()) return found->second;
  std::wstring name = process_image_basename_w(process_id);
  cache->emplace(process_id, name);
  return name;
}

// UTF-16 class name for direct ordinal compares; callers convert once via
// to_utf8 instead of round-tripping through UTF-8.
[[nodiscard]] std::wstring window_class_name_w(HWND window) {
  wchar_t class_name[256] = {};
  const int copied = GetClassNameW(window, class_name, 256);
  if (copied <= 0) return {};
  return std::wstring(class_name, static_cast<std::size_t>(copied));
}

// Process-wide generation handed out once per registry, so ids issued by one
// WindowService can never resolve inside a newer one: an id is
// [generation:32][sequence:32], generation only grows, and lookups reject
// foreign generations before scanning. That turns a stale id held across a
// service restart into target_gone instead of a different window with the
// same sequence number.
std::atomic<std::uint32_t> next_window_generation{1};

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

lane::Error build_info_impl(WindowRegistry& registry, HWND window, WindowInfo& out,
                            PidNameCache* names) {
  if (!window || !IsWindow(window)) {
    return {lane::Error::Code::TargetGone, "window no longer exists"};
  }
  out = WindowInfo{};
  out.id = registry.id_for(window);
  out.title = to_utf8(window_text(window));
  out.class_name = to_utf8(window_class_name_w(window));
  RECT rectangle{};
  if (GetWindowRect(window, &rectangle)) out.rect = to_rect(rectangle);
  out.visible = IsWindowVisible(window) != FALSE;
  out.minimized = IsIconic(window) != FALSE;
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  out.process_id = process_id;
  out.process_name = to_utf8(process_basename_cached(process_id, names));

  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  const bool maximized =
      GetWindowPlacement(window, &placement) && placement.showCmd == SW_SHOWMAXIMIZED;
  if (out.minimized) {
    out.state = "minimized";
  } else if (!out.visible) {
    out.state = "hidden";
  } else if (maximized) {
    out.state = "maximized";
  } else {
    out.state = "normal";
  }
  return lane::Error::none();
}

lane::Error build_info(WindowRegistry& registry, HWND window, WindowInfo& out) {
  return build_info_impl(registry, window, out, nullptr);
}

// WinTitle-style matching evaluated on the UI lane only. `cache` dedupes
// pid -> basename lookups within one enumeration; pass nullptr for single
// foreground lookups.
bool matches_query(WindowRegistry& registry, HWND window, const WindowQuery& query,
                   const bool has_selectors, PidNameCache* cache) {
  if (!query.include_hidden && !IsWindowVisible(window)) return false;
  if (query.id != 0 && registry.id_for(window) != query.id) return false;
  if (!query.title.empty()) {
    const std::wstring text = window_text(window);
    const std::wstring needle = from_utf8(query.title);
    // from_utf8 returns empty on invalid UTF-8; an empty needle would match
    // everything via FindStringOrdinal, so fail closed instead.
    if (needle.empty()) return false;
    if (query.exact_title) {
      if (CompareStringOrdinal(text.c_str(), -1, needle.c_str(), -1, TRUE) != CSTR_EQUAL) {
        return false;
      }
    } else {
      constexpr std::size_t kIntMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());
      if (text.size() > kIntMax || needle.size() > kIntMax) return false;
      if (FindStringOrdinal(FIND_FROMSTART, text.c_str(), static_cast<int>(text.size()),
                            needle.c_str(), static_cast<int>(needle.size()), TRUE) < 0) {
        return false;
      }
    }
  }
  if (!query.class_name.empty()) {
    // Same ordinal rule as title: case-insensitive CompareStringOrdinal on
    // UTF-16, no ASCII-fold round-trip through UTF-8.
    const std::wstring actual = window_class_name_w(window);
    const std::wstring expected = from_utf8(query.class_name);
    if (expected.empty() || actual.empty()) return false;
    if (CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL) {
      return false;
    }
  }
  if (!query.process_name.empty()) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    const std::string actual = to_utf8(process_basename_cached(process_id, cache));
    if (fold_ascii(actual) != fold_ascii(query.process_name)) return false;
  }
  // A selector-less query keeps the list() convention of skipping untitled
  // windows so an empty query never dumps the whole desktop enum.
  if (!has_selectors && window_text(window).empty()) return false;
  return true;
}

bool has_selectors(const WindowQuery& query) {
  return query.active || !query.title.empty() || !query.class_name.empty() ||
         !query.process_name.empty() || query.id != 0;
}

struct EnumContext {
  WindowRegistry* registry;
  const WindowQuery* query;
  bool selectors;
  std::vector<WindowInfo>* windows;
  PidNameCache* names;  // per-query pid cache; never null on the enum path
};

BOOL CALLBACK collect_matching(HWND window, LPARAM parameter) {
  auto* context = reinterpret_cast<EnumContext*>(parameter);
  // EnumWindows contract: parameter always carries the query context above.
  assert(context != nullptr);
  assert(context->registry != nullptr);
  assert(context->query != nullptr);
  assert(context->windows != nullptr);
  assert(context->names != nullptr);
  if (!matches_query(*context->registry, window, *context->query, context->selectors,
                     context->names)) {
    return TRUE;
  }
  WindowInfo info;
  if (!build_info_impl(*context->registry, window, info, context->names).ok()) return TRUE;
  context->windows->push_back(std::move(info));
  return TRUE;
}

lane::Error expired_deadline() {
  return {lane::Error::Code::Timeout, "operation deadline exceeded"};
}

}  // namespace

struct WindowService::Impl {
  UiThread ui;
  WindowRegistry registry;  // UI-thread only
};

WindowService::WindowService() : impl_(std::make_unique<Impl>()) {}
WindowService::~WindowService() { stop(); }

rime::core::json::Value window_info_json(const WindowInfo& info) {
  namespace json = rime::core::json;
  json::Value rect = json::Value::object();
  rect.set("left", json::Value::number(info.rect.left));
  rect.set("top", json::Value::number(info.rect.top));
  rect.set("right", json::Value::number(info.rect.right));
  rect.set("bottom", json::Value::number(info.rect.bottom));
  json::Value value = json::Value::object();
  value.set("id", json::Value::number(static_cast<double>(info.id)));
  value.set("title", json::Value::string(info.title));
  value.set("className", json::Value::string(info.class_name));
  value.set("processName", json::Value::string(info.process_name));
  value.set("rect", std::move(rect));
  value.set("visible", json::Value::boolean(info.visible));
  value.set("minimized", json::Value::boolean(info.minimized));
  value.set("state", json::Value::string(info.state));
  value.set("processId", json::Value::number(static_cast<double>(info.process_id)));
  return value;
}

UiThread& WindowService::ui() { return impl_->ui; }
UiThreadState WindowService::state() const { return impl_->ui.state(); }

rime::core::Error WindowService::start() { return impl_->ui.start(); }

rime::core::Error WindowService::stop() { return impl_->ui.stop(); }

rime::core::Error WindowService::list(std::vector<WindowInfo>& out,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  std::vector<WindowInfo> windows;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        EnumContext context{&impl_->registry, nullptr, false, &windows, nullptr};
        EnumWindows(
            [](HWND window, LPARAM parameter) -> BOOL {
              auto* enum_context = reinterpret_cast<EnumContext*>(parameter);
              if (!IsWindowVisible(window)) return TRUE;
              if (window_text(window).empty()) return TRUE;
              WindowInfo info;
              if (!build_info(*enum_context->registry, window, info).ok()) return TRUE;
              enum_context->windows->push_back(std::move(info));
              return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(windows);
  return rime::core::Error::none();
}

rime::core::Error WindowService::query(const WindowQuery& query, std::vector<WindowInfo>& out,
                                       const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  std::vector<WindowInfo> windows;
  const bool selectors = has_selectors(query);
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const bool foreground_selection = query.active;
        if (foreground_selection) {
          const HWND foreground = GetForegroundWindow();
          if (foreground != nullptr && matches_query(impl_->registry, foreground, query, true,
                                                     nullptr)) {
            WindowInfo info;
            if (build_info(impl_->registry, foreground, info).ok()) {
              windows.push_back(std::move(info));
            }
          }
          return;
        }
        PidNameCache names;
        EnumContext context{&impl_->registry, &query, selectors, &windows, &names};
        EnumWindows(collect_matching, reinterpret_cast<LPARAM>(&context));
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(windows);
  return rime::core::Error::none();
}

rime::core::Error WindowService::active(std::optional<WindowInfo>& out,
                                        const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  std::optional<WindowInfo> found;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND foreground = GetForegroundWindow();
        if (!foreground) return;
        WindowInfo info;
        if (build_info(impl_->registry, foreground, info).ok()) found = std::move(info);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = found;
  return rime::core::Error::none();
}

rime::core::Error WindowService::info(const std::uint64_t id, WindowInfo& out,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  WindowInfo info;
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = build_info(impl_->registry, impl_->registry.hwnd_for(id), info);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(info);
  return rime::core::Error::none();
}

rime::core::Error WindowService::move(const std::uint64_t id, const std::string_view placement,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  const std::string placement_text(placement);
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        RECT work{};
        if (!work_area_for(window, work)) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "cannot read the monitor work area"};
          return;
        }
        Rect target{};
        if (!resolve_placement(work, placement_text, target)) {
          result = {rime::core::Error::Code::InvalidContract,
                    "unknown window placement: " + placement_text};
          return;
        }
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        if (!SetWindowPos(window, nullptr, target.left, target.top, target.width(),
                          target.height(), SWP_NOZORDER | SWP_NOACTIVATE)) {
          result = {rime::core::Error::Code::ExecutionFailed, "SetWindowPos failed"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::move_rect(const std::uint64_t id, const Rect& rect,
                                           const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        if (!SetWindowPos(window, nullptr, rect.left, rect.top, rect.width(), rect.height(),
                          SWP_NOZORDER | SWP_NOACTIVATE)) {
          result = {rime::core::Error::Code::ExecutionFailed, "SetWindowPos failed"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::focus(const std::uint64_t id,
                                       const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        if (!SetForegroundWindow(window)) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "SetForegroundWindow was denied by the foreground lock"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

namespace {

// Shared body for the flag-based state mutations (hide/show/min/max/restore).
lane::Error show_window_op(WindowRegistry& registry, const std::uint64_t id, const int command) {
  const HWND window = registry.hwnd_for(id);
  if (!window) return {lane::Error::Code::TargetGone, "window no longer exists"};
  ShowWindow(window, command);
  return lane::Error::none();
}

}  // namespace

rime::core::Error WindowService::close(const std::uint64_t id,
                                       const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  // Absolute deadline: ui.call() already spends part of `timeout` while queued,
  // so recompute the remainder inside the lane instead of reusing the full
  // timeout for SendMessageTimeoutW (which would double-count the queue wait).
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto call_error = impl_->ui.call(
      [&, deadline] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
          result = expired_deadline();
          return;
        }
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        const auto bounded = std::clamp<std::uint64_t>(
            static_cast<std::uint64_t>(remaining_ms.count()), 1, 0xffffffffu);
        DWORD_PTR delivered = 0;
        SetLastError(0);
        const LRESULT sent = SendMessageTimeoutW(window, WM_CLOSE, 0, 0,
                                                 SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
                                                 static_cast<UINT>(bounded), &delivered);
        if (sent == 0 && GetLastError() == ERROR_TIMEOUT) {
          result = {rime::core::Error::Code::Timeout, "WM_CLOSE was not delivered in time"};
          return;
        }
        if (sent == 0 && IsWindow(window)) {
          result = {rime::core::Error::Code::ExecutionFailed, "WM_CLOSE was not delivered"};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::hide(const std::uint64_t id,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = show_window_op(impl_->registry, id, SW_HIDE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::show(const std::uint64_t id,
                                      const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = show_window_op(impl_->registry, id, SW_SHOW);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::minimize(const std::uint64_t id,
                                           const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = show_window_op(impl_->registry, id, SW_MINIMIZE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::maximize(const std::uint64_t id,
                                           const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        const HWND window = impl_->registry.hwnd_for(id);
        if (!window) {
          result = {rime::core::Error::Code::TargetGone, "window no longer exists"};
          return;
        }
        if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
        ShowWindow(window, SW_MAXIMIZE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::restore(const std::uint64_t id,
                                          const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        result = show_window_op(impl_->registry, id, SW_RESTORE);
      },
      timeout);
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::placement_rect(const std::string_view placement, Rect& out,
                                                 const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) return expired_deadline();
  const std::string placement_text(placement);
  rime::core::Error result = rime::core::Error::none();
  Rect resolved{};
  const auto call_error = impl_->ui.call(
      [&] {
        if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
          result = lane_error;
          return;
        }
        RECT work{};
        if (!work_area_for(nullptr, work)) {
          result = {rime::core::Error::Code::ExecutionFailed,
                    "cannot read the monitor work area"};
          return;
        }
        if (!resolve_placement(work, placement_text, resolved)) {
          result = {rime::core::Error::Code::InvalidContract,
                    "unknown window placement: " + placement_text};
        }
      },
      timeout);
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = resolved;
  return rime::core::Error::none();
}

}  // namespace rime::win32
