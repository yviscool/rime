#include "rime/win32/screen.hpp"

#include "rime/win32/screen_pixels.hpp"
#include "rime/win32/screen_seam.hpp"
#include "utf.hpp"

#include <windows.h>

#include <algorithm>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

// Fills `out` from MONITORINFOEX. A failed GetMonitorInfo skips that monitor
// instead of aborting the enumeration: losing one display must not hide the
// others (AHK gives up on the specific monitor it was looking for, which for a
// full collection would mean an incomplete answer reported as complete).
BOOL CALLBACK collect_monitor(HMONITOR handle, HDC, LPRECT, LPARAM param) {
  auto* out = reinterpret_cast<std::vector<ScreenService::Monitor>*>(param);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(handle, &info)) return TRUE;
  const int index = static_cast<int>(out->size()) + 1;
  ScreenService::Monitor monitor;
  monitor.index = index;
  monitor.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
  monitor.left = info.rcMonitor.left;
  monitor.top = info.rcMonitor.top;
  monitor.right = info.rcMonitor.right;
  monitor.bottom = info.rcMonitor.bottom;
  monitor.work_left = info.rcWork.left;
  monitor.work_top = info.rcWork.top;
  monitor.work_right = info.rcWork.right;
  monitor.work_bottom = info.rcWork.bottom;
  monitor.name = to_utf8(std::wstring(info.szDevice));
  out->push_back(std::move(monitor));
  return TRUE;
}

}  // namespace

Error ScreenService::collect(std::vector<Monitor>& out) const {
  out.clear();
  if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitor,
                           reinterpret_cast<LPARAM>(&out))) {
    return {Code::ExecutionFailed, "cannot enumerate monitors"};
  }
  return Error::none();
}

Error ScreenService::monitor_count(int& out) const {
  std::vector<Monitor> monitors;
  if (const auto error = collect(monitors); !error.ok()) return error;
  out = static_cast<int>(monitors.size());
  return Error::none();
}

Error ScreenService::monitor_at(const int index, Monitor& out) const {
  if (index < 0) {
    return {Code::InvalidContract, "monitor index must be 0 (primary) or a positive number"};
  }
  std::vector<Monitor> monitors;
  if (const auto error = collect(monitors); !error.ok()) return error;
  if (monitors.empty()) return {Code::TargetGone, "no monitor is connected"};
  const Monitor* wanted = nullptr;
  if (index == 0) {
    for (const Monitor& monitor : monitors) {
      if (monitor.primary) {
        wanted = &monitor;
        break;
      }
    }
    // Every system with a monitor reports one primary; if the OS ever fails
    // to flag one, fall back to the first entry rather than reporting nothing.
    if (!wanted) wanted = &monitors.front();
  } else {
    if (index > static_cast<int>(monitors.size())) {
      return {Code::InvalidContract,
              "monitor index " + std::to_string(index) + " does not exist, " +
                  std::to_string(monitors.size()) + " monitor(s) connected"};
    }
    wanted = &monitors[index - 1];
  }
  out = *wanted;
  return Error::none();
}

namespace {

// The rectangle the desktop actually covers. Capture coordinates are virtual
// desktop coordinates, so a search area is clipped to this before BitBlt -
// reading outside it would silently return black instead of an error.
RECT virtual_screen_rect() {
  RECT rect{};
  rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  return rect;
}

}  // namespace

Error ScreenService::pixel_color(const int x, const int y, std::uint32_t& rgb) const {
  const RECT screen_rect = virtual_screen_rect();
  if (x < screen_rect.left || x >= screen_rect.right || y < screen_rect.top ||
      y >= screen_rect.bottom) {
    return {Code::InvalidContract,
            "pixel (" + std::to_string(x) + ", " + std::to_string(y) +
                ") is outside the virtual screen"};
  }
  RECT single{x, y, x + 1, y + 1};
  std::vector<std::uint8_t> bgra;
  int width = 0;
  int height = 0;
  if (!capture_rect(single, bgra, width, height)) {
    return {Code::ExecutionFailed, "cannot capture the screen at (" + std::to_string(x) + ", " +
                                       std::to_string(y) + ")"};
  }
  const pixels::Framebuffer frame{width, height, bgra.data()};
  if (!pixels::color_at(frame, 0, 0, rgb)) {
    return {Code::ExecutionFailed, "screen capture returned no pixel"};
  }
  return Error::none();
}

Error ScreenService::pixel_search(const int left, const int top, const int right, const int bottom,
                                  const std::uint32_t color, const int variation, bool& found,
                                  int& out_x, int& out_y) const {
  if (variation < 0 || variation > 255) {
    return {Code::InvalidContract, "variation must be in 0..255"};
  }
  int area_left = left;
  int area_top = top;
  int area_right = right;
  int area_bottom = bottom;
  if (area_left > area_right) std::swap(area_left, area_right);
  if (area_top > area_bottom) std::swap(area_top, area_bottom);
  const RECT screen_rect = virtual_screen_rect();
  area_left = std::max(area_left, static_cast<int>(screen_rect.left));
  area_top = std::max(area_top, static_cast<int>(screen_rect.top));
  area_right = std::min(area_right, static_cast<int>(screen_rect.right) - 1);
  area_bottom = std::min(area_bottom, static_cast<int>(screen_rect.bottom) - 1);
  if (area_left > area_right || area_top > area_bottom) {
    return {Code::InvalidContract, "search area does not intersect the virtual screen"};
  }

  RECT area{area_left, area_top, area_right + 1, area_bottom + 1};
  std::vector<std::uint8_t> bgra;
  int width = 0;
  int height = 0;
  if (!capture_rect(area, bgra, width, height)) {
    return {Code::ExecutionFailed, "cannot capture the screen area"};
  }
  const pixels::Framebuffer frame{width, height, bgra.data()};
  int local_x = 0;
  int local_y = 0;
  found = pixels::search(frame, 0, 0, width - 1, height - 1, color, variation, local_x, local_y);
  if (found) {
    out_x = area_left + local_x;
    out_y = area_top + local_y;
  }
  return Error::none();
}

}  // namespace rime::win32
