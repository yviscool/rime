#pragma once

#include "rime/win32/window.hpp"

#include <windows.h>

#include <string_view>

namespace rime::win32::detail {

Rect to_rect(const RECT& rectangle);

bool work_area_for(HWND window, RECT& work);

bool resolve_placement(const RECT& work, const std::string_view placement, Rect& out);

}  // namespace rime::win32::detail
