#pragma once

#include "rime/core/types.hpp"
#include "rime/win32/screen_pixels.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rime::win32 {

// A decoded image in the same layout the capture seam produces: 32bpp BGRA,
// row 0 on top, no padding. ImageSearch compares it against a captured
// framebuffer without ever knowing about file formats.
struct ImageBuffer {
  int width{0};
  int height{0};
  std::vector<std::uint8_t> bgra;

  [[nodiscard]] pixels::Framebuffer view() const {
    return {width, height, bgra.empty() ? nullptr : bgra.data()};
  }
};

// Decodes `path_utf8` (UTF-8) through GDI+, the decoder Windows already
// ships, so this runtime grows no format support of its own. The GDI+
// startup token is taken and released inside the call: decoded pixels are
// plain bytes and must not keep process-wide state alive.
//
// Failure is split by who is at fault - a file that cannot be decoded (missing,
// not an image, empty) is the caller's contract and returns invalid_contract;
// the decoder itself being unavailable is a system failure and returns
// execution_failed. The same distinction drives every message below.
rime::core::Error load_image_file(const std::string& path_utf8, ImageBuffer& out);

}  // namespace rime::win32
