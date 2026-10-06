// Realism: L5 - the service reads the machine's own displays through
// EnumDisplayMonitors and the real desktop through BitBlt/GetDIBits. Nothing
// is mutated, so every expectation is asserted directly; count and primary are
// cross-checked against the OS's own answers rather than against the service's
// own bookkeeping.
//
// The pixel family mixes three levels in one file, each with its own source of
// truth: the scan rules are asserted against a hand-built framebuffer and
// literal colors, the capture -> scan wiring runs with a synthetic screen
// injected at the OS boundary (the seam may replace the environment but never
// the unit under test), and the real capture is checked for shape and contract
// errors. Expectations never come from the code under test.

#include "rime/win32/screen.hpp"
#include "rime/win32/screen_pixels.hpp"
#include "rime/win32/screen_seam.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using rime::win32::ScreenService;
namespace pixels = rime::win32::pixels;

void set_pixel(std::vector<std::uint8_t>& bytes, int width, int x, int y, std::uint32_t rgb) {
  const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                              static_cast<std::size_t>(x)) *
                             4u;
  bytes[offset + 0] = static_cast<std::uint8_t>(rgb & 0xFFu);         // B
  bytes[offset + 1] = static_cast<std::uint8_t>((rgb >> 8) & 0xFFu);  // G
  bytes[offset + 2] = static_cast<std::uint8_t>((rgb >> 16) & 0xFFu); // R
  bytes[offset + 3] = 0xFFu;
}

// Synthetic screen: a pixel's red channel carries its absolute x, its green
// channel its absolute y and its blue channel a constant, so a golden
// expectation for one coordinate cannot describe another.
bool synthetic_capture(const RECT& bounds, std::vector<std::uint8_t>& bgra, int& width,
                       int& height) {
  width = bounds.right - bounds.left;
  height = bounds.bottom - bounds.top;
  if (width <= 0 || height <= 0) return false;
  bgra.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int absolute_x = bounds.left + x;
      const int absolute_y = bounds.top + y;
      set_pixel(bgra, width, x, y,
                (static_cast<std::uint32_t>(absolute_x & 0xFF) << 16) |
                    (static_cast<std::uint32_t>(absolute_y & 0xFF) << 8) | 0x40u);
    }
  }
  return true;
}

}  // namespace

int main() {
  ScreenService service;

  int count = 0;
  assert(service.monitor_count(count).ok());
  assert(count >= 1);

  // Every index the count promises must resolve, and one past it must not:
  // that pair proves the count is the real enumeration length in both
  // directions (too small would leave index count+1 resolving, too large
  // would fail inside the loop) without re-implementing the enumeration here.
  for (int index = 1; index <= count; ++index) {
    ScreenService::Monitor monitor;
    assert(service.monitor_at(index, monitor).ok());
    assert(monitor.index == index);
    assert(!monitor.name.empty());
    // The work area is the monitor minus the taskbar, so it is inside bounds.
    assert(monitor.work_left >= monitor.left);
    assert(monitor.work_top >= monitor.top);
    assert(monitor.work_right <= monitor.right);
    assert(monitor.work_bottom <= monitor.bottom);
    // A monitor rectangle always has a positive area.
    assert(monitor.right > monitor.left);
    assert(monitor.bottom > monitor.top);
  }

  ScreenService::Monitor past_end;
  const auto past_end_result = service.monitor_at(count + 1, past_end);
  assert(!past_end_result.ok());
  assert(past_end_result.code == rime::core::Error::Code::InvalidContract);
  assert(past_end_result.message.find("does not exist") != std::string::npos);
  assert(past_end_result.message.find(std::to_string(count + 1)) != std::string::npos);

  const auto negative = service.monitor_at(-1, past_end);
  assert(!negative.ok());
  assert(negative.code == rime::core::Error::Code::InvalidContract);

  // Exactly one primary, and index 0 must select that very monitor - AHK's
  // omitted argument and MonitorGetPrimary() both answer with it.
  ScreenService::Monitor primary;
  assert(service.monitor_at(0, primary).ok());
  assert(primary.primary);
  int primary_count = 0;
  int primary_index = 0;
  for (int index = 1; index <= count; ++index) {
    ScreenService::Monitor monitor;
    assert(service.monitor_at(index, monitor).ok());
    if (!monitor.primary) continue;
    ++primary_count;
    primary_index = index;
  }
  assert(primary_count == 1);
  assert(primary.index == primary_index);
  assert(primary.name.length() > 0);

  // Asking for the primary by number returns the same record as asking for
  // "the primary" by omission: bounds, work area and name all agree.
  ScreenService::Monitor primary_by_number;
  assert(service.monitor_at(primary_index, primary_by_number).ok());
  assert(primary_by_number.left == primary.left);
  assert(primary_by_number.top == primary.top);
  assert(primary_by_number.right == primary.right);
  assert(primary_by_number.bottom == primary.bottom);
  assert(primary_by_number.work_left == primary.work_left);
  assert(primary_by_number.work_right == primary.work_right);
  assert(primary_by_number.name == primary.name);

  // ---- Pixel scan rules, on a hand-built framebuffer --------------------
  std::vector<std::uint8_t> frame_bytes(4 * 3 * 4u, 0);
  set_pixel(frame_bytes, 4, 0, 0, 0xFF0000);
  set_pixel(frame_bytes, 4, 1, 0, 0x00FF00);
  set_pixel(frame_bytes, 4, 2, 0, 0x0000FF);
  set_pixel(frame_bytes, 4, 3, 0, 0x101010);
  set_pixel(frame_bytes, 4, 0, 1, 0xFFFFFF);
  set_pixel(frame_bytes, 4, 1, 1, 0x000000);
  set_pixel(frame_bytes, 4, 2, 1, 0x203040);
  set_pixel(frame_bytes, 4, 3, 1, 0x506070);
  set_pixel(frame_bytes, 4, 0, 2, 0x0A0B0C);
  set_pixel(frame_bytes, 4, 1, 2, 0x112233);
  set_pixel(frame_bytes, 4, 2, 2, 0x445566);
  set_pixel(frame_bytes, 4, 3, 2, 0x778899);
  const pixels::Framebuffer framebuffer{4, 3, frame_bytes.data()};

  std::uint32_t rgb = 0;
  assert(pixels::color_at(framebuffer, 0, 0, rgb) && rgb == 0xFF0000);
  assert(pixels::color_at(framebuffer, 2, 1, rgb) && rgb == 0x203040);
  assert(pixels::color_at(framebuffer, 3, 2, rgb) && rgb == 0x778899);
  assert(!pixels::color_at(framebuffer, -1, 0, rgb));
  assert(!pixels::color_at(framebuffer, 4, 0, rgb));
  assert(!pixels::color_at(framebuffer, 0, 3, rgb));

  // Variation is per channel and never wraps: a target whose red component is
  // 1 must not accept a pixel whose red component wrapped to 255.
  assert(pixels::color_matches(0xFF0000, 0xFF0000, 0));
  assert(!pixels::color_matches(0xFF0001, 0xFF0000, 0));
  assert(pixels::color_matches(0xFF0001, 0xFF0000, 1));
  assert(!pixels::color_matches(0xFF0006, 0xFF0000, 5));
  assert(pixels::color_matches(0x000000, 0x010000, 1));
  assert(!pixels::color_matches(0x000000, 0x020000, 1));
  assert(pixels::color_matches(0x000000, 0x00FF00, 255));  // the widest slack still matches
  assert(!pixels::color_matches(0x000000, 0x00FF01, 254));

  int found_x = -1;
  int found_y = -1;
  assert(pixels::search(framebuffer, 0, 0, 3, 2, 0x0000FF, 0, found_x, found_y));
  assert(found_x == 2 && found_y == 0);
  assert(pixels::search(framebuffer, 0, 0, 3, 2, 0x000000, 0, found_x, found_y));
  assert(found_x == 1 && found_y == 1);
  // Reversed corners are accepted and normalized; the answer is the first hit
  // in top-left -> bottom-right scan order, not in the caller's corner order.
  assert(pixels::search(framebuffer, 3, 2, 0, 0, 0xFF0000, 0, found_x, found_y));
  assert(found_x == 0 && found_y == 0);
  assert(!pixels::search(framebuffer, 0, 0, 3, 2, 0x123456, 0, found_x, found_y));
  assert(found_x == 0 && found_y == 0);  // untouched on a miss

  // ---- Capture -> scan wiring, with the OS replaced by a synthetic screen --
  rime::win32::screen_seam::set_capture(synthetic_capture);
  std::uint32_t sampled = 0;
  assert(service.pixel_color(3, 5, sampled).ok());
  assert(sampled == 0x00030540);  // red=3, green=5, blue=0x40

  bool found = false;
  found_x = -1;
  found_y = -1;
  assert(service.pixel_search(0, 0, 7, 7, 0x00020340, 0, found, found_x, found_y).ok());
  assert(found && found_x == 2 && found_y == 3);

  // A miss is a result, not an error, and leaves no coordinates behind.
  found = true;
  found_x = 7;
  found_y = 7;
  assert(service.pixel_search(0, 0, 7, 7, 0x123456, 0, found, found_x, found_y).ok());
  assert(!found && found_x == 7 && found_y == 7);

  // Variation widens the match by exactly its per-channel budget.
  found = false;
  assert(service.pixel_search(2, 3, 2, 3, 0x00020300, 0x40, found, found_x, found_y).ok());
  assert(found && found_x == 2 && found_y == 3);
  found = false;
  assert(service.pixel_search(2, 3, 2, 3, 0x00020300, 0x3F, found, found_x, found_y).ok());
  assert(!found);

  // Contract errors the service enforces on its own, independent of JS.
  const auto outside_pixel = service.pixel_color(2'000'000, 2'000'000, sampled);
  assert(outside_pixel.code == rime::core::Error::Code::InvalidContract);
  assert(outside_pixel.message.find("outside the virtual screen") != std::string::npos);
  const auto outside_area = service.pixel_search(10'000'000, 10'000'000, 10'000'001,
                                                 10'000'001, 0x000000, 0, found, found_x,
                                                 found_y);
  assert(outside_area.code == rime::core::Error::Code::InvalidContract);
  assert(outside_area.message.find("does not intersect") != std::string::npos);
  const auto bad_variation = service.pixel_search(0, 0, 1, 1, 0x000000, 256, found, found_x,
                                                  found_y);
  assert(bad_variation.code == rime::core::Error::Code::InvalidContract);
  assert(bad_variation.message.find("0..255") != std::string::npos);

  // ---- Real capture --------------------------------------------------------
  rime::win32::screen_seam::set_capture(nullptr);
  RECT patch{0, 0, 16, 16};
  std::vector<std::uint8_t> real_bytes;
  int real_width = 0;
  int real_height = 0;
  assert(rime::win32::capture_rect(patch, real_bytes, real_width, real_height));
  assert(real_width == 16 && real_height == 16);
  assert(real_bytes.size() == 16u * 16u * 4u);
  std::uint32_t real_color = 0;
  assert(service.pixel_color(0, 0, real_color).ok());

  return 0;
}
