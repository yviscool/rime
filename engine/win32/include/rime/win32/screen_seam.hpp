#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace rime::win32 {

// Test-only OS substitution seam (AGENTS testing rules: a stub may replace
// the environment - this one screen read - never the unit under test). A
// healthy desktop shows arbitrary pixels, so exact color and search results
// can only be asserted against a synthetic framebuffer injected at the OS
// boundary while the whole real capture -> scan -> Action path still runs.
//
// Process-global and unsynchronized beyond the atomic store: set it before
// the operation and restore it (nullptr) right after. ctest runs serially,
// so no concurrent consumer exists. See screen_seam.cpp for the call site.
namespace screen_seam {

// Reads `bounds` (virtual-desktop coordinates) into a top-down 32bpp BGRA
// buffer of `width * height` pixels, 4 bytes each, stride `width * 4`. Row 0
// is the top row. Returns false when the rectangle could not be read.
using CaptureFn = bool (*)(const RECT& bounds, std::vector<std::uint8_t>& bgra, int& width,
                           int& height);

// nullptr restores the real BitBlt/GetDIBits capture.
void set_capture(CaptureFn capture);

}  // namespace screen_seam

// The capture every screen read goes through: the seam when one is
// installed, the real capture otherwise. Out parameters are only meaningful
// on success.
bool capture_rect(const RECT& bounds, std::vector<std::uint8_t>& bgra, int& width, int& height);

}  // namespace rime::win32
