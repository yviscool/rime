#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"

#include <windows.h>

#include <algorithm>
#include <string>
#include <utility>

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

// Case-insensitive ASCII folding for stable comparisons across UTF-8 inputs.
std::string fold_ascii(std::string_view text) {
  std::string folded(text);
  for (char& character : folded) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  return folded;
}

std::wstring fold_wide(const std::wstring& text) {
  std::wstring folded(text);
  for (wchar_t& character : folded) {
    if (character >= L'A' && character <= L'Z') {
      character = static_cast<wchar_t>(character - L'A' + L'a');
    }
  }
  return folded;
}

// Basename of the process image behind `pid`; empty when it cannot be read.
std::string process_image_name(const DWORD process_id) {
  if (process_id == 0) return {};
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (!process) return {};
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
  CloseHandle(process);
  if (!ok || size == 0) return {};
  path.resize(size);
  const std::size_t slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos) path = path.substr(slash + 1);
  return to_utf8(path);
}

std::string window_class_name(HWND window) {
  wchar_t class_name[256] = {};
  const int copied = GetClassNameW(window, class_name, 256);
  if (copied <= 0) return {};
  return to_utf8(std::wstring(class_name, static_cast<std::size_t>(copied)));
}

// UI-thread-only mapping from stable ids to live HWNDs. Stale entries are
// dropped on lookup; ids are never recycled inside one service.
class WindowRegistry final {
 public:
  std::uint64_t id_for(HWND window) {
    prune();
    for (const auto& [id, hwnd] : entries_) {
      if (hwnd == window) return id;
    }
    const std::uint64_t id = ++next_id_;
    entries_.emplace_back(id, window);
    return id;
  }

  HWND hwnd_for(const std::uint64_t id) {
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
  std::uint64_t next_id_{0};
};

lane::Error build_info(WindowRegistry& registry, HWND window, WindowInfo& out) {
  if (!window || !IsWindow(window)) {
    return {lane::Error::Code::InvalidState, "window no longer exists"};
  }
  out = WindowInfo{};
  out.id = registry.id_for(window);
  out.title = to_utf8(window_text(window));
  out.class_name = window_class_name(window);
  RECT rectangle{};
  if (GetWindowRect(window, &rectangle)) out.rect = to_rect(rectangle);
  out.visible = IsWindowVisible(window) != FALSE;
  out.minimized = IsIconic(window) != FALSE;
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  out.process_id = process_id;
  out.process_name = process_image_name(process_id);

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

// WinTitle-style matching evaluated on the UI lane only.
bool matches_query(WindowRegistry& registry, HWND window, const WindowQuery& query,
                   const bool has_selectors) {
  if (!query.include_hidden && !IsWindowVisible(window)) return false;
  if (query.id != 0 && registry.id_for(window) != query.id) return false;
  if (!query.title.empty() && query.title != "A") {
    const std::wstring text = window_text(window);
    const std::wstring needle = from_utf8(query.title);
    if (query.exact_title) {
      if (CompareStringOrdinal(text.c_str(), -1, needle.c_str(), -1, TRUE) != CSTR_EQUAL) {
        return false;
      }
    } else if (FindStringOrdinal(0, text.c_str(), static_cast<int>(text.size()),
                                 needle.c_str(), static_cast<int>(needle.size()), TRUE) < 0) {
      return false;
    }
  }
  if (!query.class_name.empty()) {
    const std::wstring actual = from_utf8(window_class_name(window));
    const std::wstring expected = from_utf8(query.class_name);
    if (fold_wide(actual) != fold_wide(expected)) return false;
  }
  if (!query.process_name.empty()) {
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    const std::string actual = process_image_name(process_id);
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
};

BOOL CALLBACK collect_matching(HWND window, LPARAM parameter) {
  auto* context = reinterpret_cast<EnumContext*>(parameter);
  if (!matches_query(*context->registry, window, *context->query, context->selectors)) {
    return TRUE;
  }
  WindowInfo info;
  if (!build_info(*context->registry, window, info).ok()) return TRUE;
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
        EnumContext context{&impl_->registry, nullptr, false, &windows};
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
        const bool foreground_selection =
            query.active || (!query.title.empty() && query.title == "A");
        if (foreground_selection) {
          const HWND foreground = GetForegroundWindow();
          if (foreground != nullptr && matches_query(impl_->registry, foreground, query, true)) {
            WindowInfo info;
            if (build_info(impl_->registry, foreground, info).ok()) {
              windows.push_back(std::move(info));
            }
          }
          return;
        }
        EnumContext context{&impl_->registry, &query, selectors, &windows};
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
          result = {rime::core::Error::Code::InvalidState, "window no longer exists"};
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
          result = {rime::core::Error::Code::InvalidState, "window no longer exists"};
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
          result = {rime::core::Error::Code::InvalidState, "window no longer exists"};
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
  if (!window) return {lane::Error::Code::InvalidState, "window no longer exists"};
  ShowWindow(window, command);
  return lane::Error::none();
}

}  // namespace

rime::core::Error WindowService::close(const std::uint64_t id,
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
          result = {rime::core::Error::Code::InvalidState, "window no longer exists"};
          return;
        }
        const auto bounded = std::clamp<std::uint64_t>(
            static_cast<std::uint64_t>(timeout.count()), 1, 0xffffffffu);
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
          result = {rime::core::Error::Code::InvalidState, "window no longer exists"};
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

rime::core::Error WindowService::placement_rect(const std::string_view placement, Rect& out) {
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
      });
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = resolved;
  return rime::core::Error::none();
}

}  // namespace rime::win32
