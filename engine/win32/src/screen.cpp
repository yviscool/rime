#include "rime/win32/screen.hpp"

#include "rime/win32/image_loader.hpp"
#include "rime/win32/screen_pixels.hpp"
#include "rime/win32/screen_seam.hpp"
#include "utf.hpp"

// winsock2.h first: windows.h is compiled with WIN32_LEAN_AND_MEAN here and
// therefore brings no socket types at all, while AF_INET and sockaddr_in
// (the shape GetAdaptersAddresses hands back) come from the winsock headers.
#include <winsock2.h>
#include <windows.h>

#include <iphlpapi.h>

#include <algorithm>
#include <cstdio>

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

// Normalizes the caller's rectangle and clips it to the virtual screen. Every
// screen read shares this so pixelSearch and ImageSearch cannot drift apart on
// what "partly outside the desktop" means: reversed corners are accepted, the
// visible remainder is searched, and a rectangle that misses the desktop
// entirely is an invalid contract instead of a quiet capture of nothing.
Error clip_area(const int left, const int top, const int right, const int bottom, RECT& out) {
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
  out.left = area_left;
  out.top = area_top;
  out.right = area_right;
  out.bottom = area_bottom;
  return Error::none();
}

// The half-open capture rectangle for a clipped area: capture_rect takes
// exclusive edges, the screen API reports inclusive ones.
RECT capture_rect_of(const RECT& area) {
  return RECT{area.left, area.top, area.right + 1, area.bottom + 1};
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
  RECT area{};
  if (const auto error = clip_area(left, top, right, bottom, area); !error.ok()) return error;

  std::vector<std::uint8_t> bgra;
  int width = 0;
  int height = 0;
  if (!capture_rect(capture_rect_of(area), bgra, width, height)) {
    return {Code::ExecutionFailed, "cannot capture the screen area"};
  }
  const pixels::Framebuffer frame{width, height, bgra.data()};
  int local_x = 0;
  int local_y = 0;
  found = pixels::search(frame, 0, 0, width - 1, height - 1, color, variation, local_x, local_y);
  if (found) {
    out_x = area.left + local_x;
    out_y = area.top + local_y;
  }
  return Error::none();
}

Error ScreenService::image_search(const int left, const int top, const int right, const int bottom,
                                  const std::string& image_path, const int variation, bool& found,
                                  int& out_x, int& out_y) const {
  if (variation < 0 || variation > 255) {
    return {Code::InvalidContract, "variation must be in 0..255"};
  }
  RECT area{};
  if (const auto error = clip_area(left, top, right, bottom, area); !error.ok()) return error;

  ImageBuffer image;
  if (const auto error = load_image_file(image_path, image); !error.ok()) return error;

  std::vector<std::uint8_t> bgra;
  int width = 0;
  int height = 0;
  if (!capture_rect(capture_rect_of(area), bgra, width, height)) {
    return {Code::ExecutionFailed, "cannot capture the screen area"};
  }
  const pixels::Framebuffer frame{width, height, bgra.data()};
  int local_x = 0;
  int local_y = 0;
  found = pixels::image_search(frame, image.view(), variation, local_x, local_y);
  if (found) {
    out_x = area.left + local_x;
    out_y = area.top + local_y;
  }
  return Error::none();
}

ScreenService::Caret ScreenService::caret() const {
  Caret out;
  const HWND foreground = GetForegroundWindow();
  if (!foreground) return out;
  const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
  if (thread == 0) return out;
  GUITHREADINFO info{};
  info.cbSize = sizeof(info);
  if (!GetGUIThreadInfo(thread, &info) || !info.hwndCaret) return out;
  // rcCaret is in the caret window's client coordinates; the conversion is the
  // one AHK performs (vars.cpp:1012). A window that vanished between the two
  // calls would leave screen coordinates that mean nothing, so it is reported
  // as "no caret" rather than as a wrong point.
  POINT point{info.rcCaret.left, info.rcCaret.top};
  if (!ClientToScreen(info.hwndCaret, &point)) return out;
  out.found = true;
  out.x = point.x;
  out.y = point.y;
  return out;
}

int ScreenService::system_metric(const int index) const { return GetSystemMetrics(index); }

Error ScreenService::ip_addresses(std::vector<std::string>& out) const {
  out.clear();
  // The query runs twice by design: the first call reports the size the
  // second one needs, and a short buffer would otherwise truncate the list.
  constexpr ULONG kFlags =
      GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG needed = 0;
  if (GetAdaptersAddresses(AF_INET, kFlags, nullptr, nullptr, &needed) != ERROR_BUFFER_OVERFLOW) {
    return {Code::ExecutionFailed, "cannot size the adapter address list"};
  }
  std::vector<unsigned char> buffer(needed);
  const ULONG error = GetAdaptersAddresses(
      AF_INET, kFlags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &needed);
  if (error != NO_ERROR) {
    return {Code::ExecutionFailed, "cannot read the adapter address list"};
  }
  // Loopback is an adapter of its own and its 127.0.0.1 comes back like any
  // other address, which is what the script's own list contains as well.
  for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
       adapter = adapter->Next) {
    for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      const sockaddr* address = unicast->Address.lpSockaddr;
      if (!address || address->sa_family != AF_INET) continue;
      // Duplicate-address detection is still running for a tentative address
      // and a deprecated one is on its way out: neither can be reached, so
      // neither belongs in a list a caller may bind to. This is also the set
      // GetIpAddrTable (the routing table) reflects - and AHK's resolved
      // host addresses never included a half-configured one either.
      if (unicast->DadState != IpDadStatePreferred) continue;
      const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
      // in_addr holds the four octets in wire order, so reading them as bytes
      // is the address itself - no byte swap, no Winsock conversion call.
      const auto* octets = reinterpret_cast<const std::uint8_t*>(&v4->sin_addr);
      char text[16];
      std::snprintf(text, sizeof(text), "%u.%u.%u.%u", octets[0], octets[1], octets[2],
                    octets[3]);
      out.emplace_back(text);
    }
  }
  return Error::none();
}

}  // namespace rime::win32
