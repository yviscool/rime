#include "rime/win32/screen_pixels.hpp"

#include <cstdlib>

namespace rime::win32::pixels {
namespace {

int channel(const std::uint32_t color, const int shift) {
  return static_cast<int>((color >> shift) & 0xFFu);
}

}  // namespace

bool color_at(const Framebuffer& frame, const int x, const int y, std::uint32_t& rgb) {
  if (!frame.bgra || x < 0 || y < 0 || x >= frame.width || y >= frame.height) return false;
  const std::size_t offset =
      (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) +
       static_cast<std::size_t>(x)) *
      4u;
  const std::uint8_t* const pixel = frame.bgra + offset;
  rgb = (static_cast<std::uint32_t>(pixel[2]) << 16) | (static_cast<std::uint32_t>(pixel[1]) << 8) |
        static_cast<std::uint32_t>(pixel[0]);
  return true;
}

bool color_matches(const std::uint32_t pixel_rgb, const std::uint32_t target_rgb,
                   const int variation) {
  const int limit = variation < 0 ? 0 : (variation > 255 ? 255 : variation);
  if (limit == 0) return pixel_rgb == target_rgb;
  return abs(channel(pixel_rgb, 16) - channel(target_rgb, 16)) <= limit &&
         abs(channel(pixel_rgb, 8) - channel(target_rgb, 8)) <= limit &&
         abs(channel(pixel_rgb, 0) - channel(target_rgb, 0)) <= limit;
}

bool search(const Framebuffer& frame, int left, int top, int right, int bottom,
            const std::uint32_t target_rgb, const int variation, int& out_x, int& out_y) {
  if (!frame.bgra) return false;
  if (left > right) {
    const int swap = left;
    left = right;
    right = swap;
  }
  if (top > bottom) {
    const int swap = top;
    top = bottom;
    bottom = swap;
  }
  if (left < 0) left = 0;
  if (top < 0) top = 0;
  if (right >= frame.width) right = frame.width - 1;
  if (bottom >= frame.height) bottom = frame.height - 1;
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      std::uint32_t rgb = 0;
      if (!color_at(frame, x, y, rgb)) continue;
      if (!color_matches(rgb, target_rgb, variation)) continue;
      out_x = x;
      out_y = y;
      return true;
    }
  }
  return false;
}

}  // namespace rime::win32::pixels
