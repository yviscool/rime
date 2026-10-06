#include "rime/win32/screen.hpp"

#include "utf.hpp"

#include <windows.h>

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

}  // namespace rime::win32
