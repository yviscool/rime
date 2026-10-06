#pragma once

#include <cstdint>

namespace rime::win32::pixels {

// One captured framebuffer: 32bpp BGRA bytes, 4 bytes per pixel, row 0 on
// top, no padding (stride is always width * 4 because the capture that
// produces it asks GetDIBits for exactly 32bpp).
struct Framebuffer {
  int width{0};
  int height{0};
  const std::uint8_t* bgra{nullptr};
};

// Reads one pixel as 0xRRGGBB. Returns false when (x, y) is outside the
// buffer - the caller turns that into an invalid_contract, never a guess.
bool color_at(const Framebuffer& frame, int x, int y, std::uint32_t& rgb);

// Per-channel match: each of R, G and B may differ from the target by at
// most `variation` (clamped to 0..255, compared without wrapping so a low
// component cannot wrap around to a bright one - AHK's rule in pixel.cpp).
bool color_matches(std::uint32_t pixel_rgb, std::uint32_t target_rgb, int variation);

// Scans the frame-local rectangle (normalized: left <= right, top <= bottom)
// in row-major order, top row first and left to right, and reports the first
// pixel that matches. Returns false when nothing matches; `out_x`/`out_y`
// are then left untouched. Reversed corners are accepted and normalized, so
// the reported coordinate is the first hit in scan order rather than in the
// caller's corner order (a documented deviation from AHK's direction-aware
// scan).
bool search(const Framebuffer& frame, int left, int top, int right, int bottom,
            std::uint32_t target_rgb, int variation, int& out_x, int& out_y);

}  // namespace rime::win32::pixels
