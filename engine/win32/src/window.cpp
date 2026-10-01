#include "rime/win32/window.hpp"

#include "rime/core/lane.hpp"

#include "utf.hpp"

#include <windows.h>

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
  RECT rectangle{};
  if (GetWindowRect(window, &rectangle)) out.rect = to_rect(rectangle);
  out.visible = IsWindowVisible(window) != FALSE;
  out.minimized = IsIconic(window) != FALSE;
  DWORD process_id = 0;
  GetWindowThreadProcessId(window, &process_id);
  out.process_id = process_id;
  return lane::Error::none();
}

struct EnumContext {
  WindowRegistry* registry;
  std::vector<WindowInfo>* windows;
};

BOOL CALLBACK collect_window(HWND window, LPARAM parameter) {
  auto* context = reinterpret_cast<EnumContext*>(parameter);
  if (!IsWindowVisible(window)) return TRUE;
  if (window_text(window).empty()) return TRUE;
  WindowInfo info;
  if (!build_info(*context->registry, window, info).ok()) return TRUE;
  context->windows->push_back(std::move(info));
  return TRUE;
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
  value.set("rect", std::move(rect));
  value.set("visible", json::Value::boolean(info.visible));
  value.set("minimized", json::Value::boolean(info.minimized));
  value.set("processId", json::Value::number(static_cast<double>(info.process_id)));
  return value;
}

UiThread& WindowService::ui() { return impl_->ui; }
UiThreadState WindowService::state() const { return impl_->ui.state(); }

rime::core::Error WindowService::start() { return impl_->ui.start(); }

rime::core::Error WindowService::stop() { return impl_->ui.stop(); }

rime::core::Error WindowService::list(std::vector<WindowInfo>& out) {
  rime::core::Error result = rime::core::Error::none();
  std::vector<WindowInfo> windows;
  const auto call_error = impl_->ui.call([&] {
    if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
      result = lane_error;
      return;
    }
    EnumContext context{&impl_->registry, &windows};
    EnumWindows(collect_window, reinterpret_cast<LPARAM>(&context));
  });
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(windows);
  return rime::core::Error::none();
}

rime::core::Error WindowService::active(std::optional<WindowInfo>& out) {
  rime::core::Error result = rime::core::Error::none();
  std::optional<WindowInfo> found;
  const auto call_error = impl_->ui.call([&] {
    if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
      result = lane_error;
      return;
    }
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return;
    WindowInfo info;
    if (build_info(impl_->registry, foreground, info).ok()) found = std::move(info);
  });
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = found;
  return rime::core::Error::none();
}

rime::core::Error WindowService::info(const std::uint64_t id, WindowInfo& out) {
  rime::core::Error result = rime::core::Error::none();
  WindowInfo info;
  const auto call_error = impl_->ui.call([&] {
    if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
      result = lane_error;
      return;
    }
    result = build_info(impl_->registry, impl_->registry.hwnd_for(id), info);
  });
  if (!call_error.ok()) return call_error;
  if (!result.ok()) return result;
  out = std::move(info);
  return rime::core::Error::none();
}

rime::core::Error WindowService::move(const std::uint64_t id, const std::string_view placement) {
  const std::string placement_text(placement);
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call([&] {
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
      result = {rime::core::Error::Code::ExecutionFailed, "cannot read the monitor work area"};
      return;
    }
    Rect target{};
    if (!resolve_placement(work, placement_text, target)) {
      result = {rime::core::Error::Code::InvalidContract,
                "unknown window placement: " + placement_text};
      return;
    }
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    if (!SetWindowPos(window, nullptr, target.left, target.top, target.width(), target.height(),
                      SWP_NOZORDER | SWP_NOACTIVATE)) {
      result = {rime::core::Error::Code::ExecutionFailed, "SetWindowPos failed"};
    }
  });
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::move_rect(const std::uint64_t id, const Rect& rect) {
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call([&] {
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
  });
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::focus(const std::uint64_t id) {
  rime::core::Error result = rime::core::Error::none();
  const auto call_error = impl_->ui.call([&] {
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
  });
  if (!call_error.ok()) return call_error;
  return result;
}

rime::core::Error WindowService::placement_rect(const std::string_view placement, Rect& out) {
  const std::string placement_text(placement);
  rime::core::Error result = rime::core::Error::none();
  Rect resolved{};
  const auto call_error = impl_->ui.call([&] {
    if (const auto lane_error = lane::require_lane(lane::Lane::Ui); !lane_error.ok()) {
      result = lane_error;
      return;
    }
    RECT work{};
    if (!work_area_for(nullptr, work)) {
      result = {rime::core::Error::Code::ExecutionFailed, "cannot read the monitor work area"};
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
