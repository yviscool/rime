#include "window_geometry.hpp"

namespace rime::win32::detail {

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

}  // namespace rime::win32::detail
