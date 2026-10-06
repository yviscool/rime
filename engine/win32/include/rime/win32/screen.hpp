#pragma once

#include "rime/core/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rime::win32 {

// Display geometry: AHK's Monitor* family (rime-research .../lib/env.cpp:92).
//
// Enumeration uses EnumDisplayMonitors, never GetSystemMetrics(SM_CMONITORS):
// the counter only counts display monitors while the enumerator also reports
// non-display pseudo-monitors, and the two disagree - AHK spells out that trap
// in env.cpp:143 and we keep the same choice. Indexes are 1-based like AHK's,
// and index 0 selects the primary monitor, which is what AHK's omitted
// argument means.
//
// The pixel family (PixelGetColor / PixelSearch) reads the same virtual
// desktop through capture_rect (screen_seam.hpp) and scans the result with
// the pure helpers in screen_pixels.hpp, so every exact color assertion runs
// against an injected framebuffer instead of whatever the desktop shows.
class ScreenService final {
 public:
  struct Monitor {
    int index{0};   // 1-based position in enumeration order
    bool primary{false};
    int left{0};
    int top{0};
    int right{0};
    int bottom{0};
    int work_left{0};
    int work_top{0};
    int work_right{0};
    int work_bottom{0};
    std::string name;  // device name, e.g. \\.\DISPLAY1
  };

  rime::core::Error monitor_count(int& out) const;
  // `index` 0 selects the primary monitor. An index with no monitor behind it
  // is an invalid contract, never a silent fallback to some other display.
  rime::core::Error monitor_at(int index, Monitor& out) const;

  // One pixel as 0xRRGGBB in virtual-desktop coordinates.
  rime::core::Error pixel_color(int x, int y, std::uint32_t& rgb) const;
  // First matching pixel in the rectangle, scanning top row first and left to
  // right; `found` stays false when nothing matched. Reversed corners are
  // accepted and normalized.
  rime::core::Error pixel_search(int left, int top, int right, int bottom, std::uint32_t color,
                                 int variation, bool& found, int& out_x, int& out_y) const;

  // First position in the rectangle where `image_path` fits, scanning like
  // pixel_search. The file is decoded through image_loader before the screen
  // is touched, so a bad path is reported without reading the desktop.
  rime::core::Error image_search(int left, int top, int right, int bottom,
                                 const std::string& image_path, int variation, bool& found,
                                 int& out_x, int& out_y) const;

  // AHK CaretGetPos (rime-research .../lib/vars.cpp:991): the caret of the
  // foreground window's thread, converted to screen pixels. Win32 exposes a
  // thread's caret only through GetGUIThreadInfo, and a caret belongs to the
  // focused control, so the foreground thread is the only place to look -
  // a desktop with no foreground window or no caret is a plain `found:false`,
  // the same blank answer AHK writes into its output variables, never an
  // error. x/y are meaningless while `found` is false.
  struct Caret {
    bool found{false};
    int x{0};
    int y{0};
  };
  Caret caret() const;

 private:
  rime::core::Error collect(std::vector<Monitor>& out) const;
};

}  // namespace rime::win32
