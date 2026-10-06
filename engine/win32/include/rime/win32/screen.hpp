#pragma once

#include "rime/core/types.hpp"

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

 private:
  rime::core::Error collect(std::vector<Monitor>& out) const;
};

}  // namespace rime::win32
