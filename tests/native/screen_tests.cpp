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
//
// ImageSearch adds a fourth ingredient, the file decoder: its needles are BMP
// fixtures this test writes byte by byte (tests/screen_image_fixture.hpp) and
// deletes again, so the pixels it searches for are known before GDI+ ever sees
// them.
//
// CaretGetPos is the one section that has to arrange the desktop first: the
// caret can only be read from the foreground thread, so this test gives the
// desktop a foreground window of its own with a caret at a known offset and
// uses the production focus ladder to activate it. The expected point is the
// window's own position plus the offset the test chose - never a second read
// through the query under test.

#include "rime/win32/screen.hpp"
#include "rime/win32/screen_pixels.hpp"
#include "rime/win32/screen_seam.hpp"
#include "rime/win32/window.hpp"
#include "../screen_image_fixture.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
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

  // ---- Template matching, on a hand-built field ---------------------------
  // The needle is planted at (4, 2); the pixel at (1, 0) is a decoy that
  // matches the needle's first pixel alone, so a scan reporting (1, 0) would
  // be reporting a prefilter hit as if it were a match.
  std::vector<std::uint8_t> field_bytes(6 * 4 * 4u, 0);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 6; ++x) set_pixel(field_bytes, 6, x, y, 0x101010);
  }
  set_pixel(field_bytes, 6, 1, 0, 0x202020);  // decoy
  set_pixel(field_bytes, 6, 4, 2, 0x202020);
  set_pixel(field_bytes, 6, 5, 2, 0x303030);
  const pixels::Framebuffer field{6, 4, field_bytes.data()};

  std::vector<std::uint8_t> needle_bytes(2 * 1 * 4u, 0);
  set_pixel(needle_bytes, 2, 0, 0, 0x202020);
  set_pixel(needle_bytes, 2, 1, 0, 0x303030);
  const pixels::Framebuffer needle{2, 1, needle_bytes.data()};

  found_x = -1;
  found_y = -1;
  assert(pixels::image_search(field, needle, 0, found_x, found_y));
  assert(found_x == 4 && found_y == 2);

  // A needle whose second pixel is wrong does not match anywhere, and the
  // coordinates stay as they were.
  std::vector<std::uint8_t> absent_bytes(2 * 1 * 4u, 0);
  set_pixel(absent_bytes, 2, 0, 0, 0x202020);
  set_pixel(absent_bytes, 2, 1, 0, 0x999999);
  const pixels::Framebuffer absent{2, 1, absent_bytes.data()};
  found_x = 7;
  found_y = 7;
  assert(!pixels::image_search(field, absent, 0, found_x, found_y));
  assert(found_x == 7 && found_y == 7);

  // A needle wider than the field cannot fit anywhere in it: a miss, not a
  // truncated search along its left edge.
  std::vector<std::uint8_t> wide_bytes(7 * 1 * 4u, 0);
  const pixels::Framebuffer too_wide{7, 1, wide_bytes.data()};
  assert(!pixels::image_search(field, too_wide, 0, found_x, found_y));
  assert(found_x == 7 && found_y == 7);

  // Variation widens the template match the same way it widens a single
  // pixel: the field's background is 0x101010, so a black needle needs
  // exactly 16 of slack per channel to accept it.
  std::vector<std::uint8_t> black_bytes(1 * 1 * 4u, 0);
  set_pixel(black_bytes, 1, 0, 0, 0x000000);
  const pixels::Framebuffer black{1, 1, black_bytes.data()};
  found_x = -1;
  found_y = -1;
  assert(pixels::image_search(field, black, 0x10, found_x, found_y));
  assert(found_x == 0 && found_y == 0);
  found_x = 7;
  found_y = 7;
  assert(!pixels::image_search(field, black, 0x0F, found_x, found_y));
  assert(found_x == 7 && found_y == 7);

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

  // ---- ImageSearch over the synthetic screen ------------------------------
  // The fixture is a 2x1 strip of the two pixels the synthetic screen shows at
  // (2, 3) and (3, 3). The red channel names the column and the green channel
  // the row, so that corner is the only place the strip fits.
  rime::test::TempImagePath needle_path;
  assert(needle_path.usable());
  assert(rime::test::write_bmp_24(needle_path.path(), 2, 1, {0x00020340, 0x00030340}));

  found = false;
  found_x = -1;
  found_y = -1;
  assert(service.image_search(0, 0, 7, 7, needle_path.path(), 0, found, found_x, found_y).ok());
  assert(found && found_x == 2 && found_y == 3);

  // A rectangle narrow enough that the strip cannot fit is a miss, and one
  // wide enough but without the pixels is a miss too.
  found = true;
  found_x = 7;
  found_y = 7;
  assert(service.image_search(0, 0, 0, 7, needle_path.path(), 0, found, found_x, found_y).ok());
  assert(!found && found_x == 7 && found_y == 7);
  found = true;
  found_x = 7;
  found_y = 7;
  assert(service.image_search(4, 0, 7, 7, needle_path.path(), 0, found, found_x, found_y).ok());
  assert(!found && found_x == 7 && found_y == 7);

  // A file that cannot be decoded is the caller's contract, and it is
  // reported before the screen is read - the same order every search uses
  // (variation, then area, then file, then capture).
  const auto missing_file = service.image_search(0, 0, 7, 7, needle_path.path() + ".missing", 0,
                                                 found, found_x, found_y);
  assert(missing_file.code == rime::core::Error::Code::InvalidContract);
  assert(missing_file.message.find("could not be loaded") != std::string::npos);
  const auto empty_path =
      service.image_search(0, 0, 7, 7, "", 0, found, found_x, found_y);
  assert(empty_path.code == rime::core::Error::Code::InvalidContract);
  assert(empty_path.message.find("must not be empty") != std::string::npos);

  // The rectangle and variation rules are shared with pixelSearch.
  const auto image_outside = service.image_search(10'000'000, 10'000'000, 10'000'001,
                                                  10'000'001, needle_path.path(), 0, found, found_x,
                                                  found_y);
  assert(image_outside.code == rime::core::Error::Code::InvalidContract);
  assert(image_outside.message.find("does not intersect") != std::string::npos);
  const auto image_bad_variation = service.image_search(
      0, 0, 7, 7, needle_path.path(), -1, found, found_x, found_y);
  assert(image_bad_variation.code == rime::core::Error::Code::InvalidContract);
  assert(image_bad_variation.message.find("0..255") != std::string::npos);

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

  // ImageSearch end to end on the real desktop: the needle is cut out of a
  // real capture of a 4x4 patch and searched for in exactly that patch, so
  // there is precisely one candidate position and the answer does not depend
  // on what the desktop shows. The patch is at the virtual-screen origin,
  // which pixel_color(0, 0) above already proved is on a real display.
  const pixels::Framebuffer real_frame{real_width, real_height, real_bytes.data()};
  std::vector<std::uint32_t> real_needle_rgb;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      std::uint32_t pixel = 0;
      assert(pixels::color_at(real_frame, x, y, pixel));
      real_needle_rgb.push_back(pixel);
    }
  }
  rime::test::TempImagePath real_needle_path;
  assert(real_needle_path.usable());
  assert(rime::test::write_bmp_24(real_needle_path.path(), 4, 4, real_needle_rgb));
  found = false;
  found_x = -1;
  found_y = -1;
  assert(service.image_search(0, 0, 3, 3, real_needle_path.path(), 0, found, found_x, found_y).ok());
  assert(found && found_x == 0 && found_y == 0);

  // Flip one channel of one needle pixel: the very patch the needle came from
  // can no longer hold it, which is the other half of the contract.
  real_needle_rgb.back() ^= 0x00FFFFFFu;
  assert(rime::test::write_bmp_24(real_needle_path.path(), 4, 4, real_needle_rgb));
  found = true;
  found_x = 7;
  found_y = 7;
  assert(service.image_search(0, 0, 3, 3, real_needle_path.path(), 0, found, found_x, found_y).ok());
  assert(!found && found_x == 7 && found_y == 7);

  // CaretGetPos end to end on a real desktop: a popup of this process (no
  // non-client area, so its client origin is its window rect) with a caret at
  // a known offset, activated through the production focus ladder. The
  // expectation is window position + offset - the window position comes from
  // GetWindowRect, which observes the window, not the caret query. Window and
  // caret both live on the service's UI thread: a window owned by this thread
  // would never pump its queue, so the desktop would show it as an
  // unresponsive black box and the activation ladder would block on it.
  rime::win32::WindowService windows;
  assert(windows.start().ok());

  HWND caret_window = nullptr;
  assert(windows.ui()
             .call([&] {
               caret_window =
                   CreateWindowExW(0, L"STATIC", L"Rime Caret Fixture", WS_POPUP | WS_VISIBLE, 40,
                                   60, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr),
                                   nullptr);
             })
             .ok());
  assert(caret_window != nullptr);

  std::vector<rime::win32::WindowInfo> listed;
  assert(windows.list(listed).ok());
  std::optional<std::uint64_t> caret_window_id;
  for (const auto& window : listed) {
    if (window.title.find("Rime Caret Fixture") != std::string::npos) caret_window_id = window.id;
  }
  assert(caret_window_id.has_value());

  // The desktop keeps stealing the foreground back, so re-ask against a
  // deadline (condition poll, no blind sleep) and fail outright with the
  // reason instead of degrading to a weaker assertion.
  rime::core::Error focus_result = rime::core::Error::none();
  bool focused = false;
  const auto focus_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  do {
    focus_result = windows.focus(*caret_window_id);
    focused = focus_result.ok();
    if (!focused) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  } while (!focused && std::chrono::steady_clock::now() < focus_deadline);
  if (!focused) {
    std::fprintf(stderr, "caret fixture focus was never granted: %s - %s\n",
                 rime::core::error_code_name(focus_result.code), focus_result.message.c_str());
    std::fflush(stderr);
  }
  assert(focused);

  // The caret is created on the window's own thread - the thread
  // GetGUIThreadInfo is later asked about - by that thread itself.
  BOOL created_caret = FALSE;
  BOOL moved_caret = FALSE;
  BOOL shown_caret = FALSE;
  assert(windows.ui()
             .call([&] {
               created_caret = CreateCaret(caret_window, nullptr, 4, 16);
               moved_caret = created_caret ? SetCaretPos(5, 7) : FALSE;
               shown_caret = moved_caret ? ShowCaret(caret_window) : FALSE;
             })
             .ok());
  assert(created_caret && moved_caret && shown_caret);

  RECT fixture_rect{};
  assert(GetWindowRect(caret_window, &fixture_rect));
  const int expected_x = fixture_rect.left + 5;
  const int expected_y = fixture_rect.top + 7;

  // Foreground is the query's precondition, so it is checked rather than
  // assumed: otherwise a wrong coordinate could always be explained away as
  // "some other window was focused".
  assert(GetForegroundWindow() == caret_window);
  const ScreenService::Caret caret = service.caret();
  assert(caret.found);
  assert(caret.x == expected_x);
  assert(caret.y == expected_y);

  // The other half of the contract: with no caret there is no coordinate to
  // report. Our own window is still the foreground one, so `found:false`
  // cannot be some other window's missing caret. Both calls run on the
  // thread that owns the caret.
  BOOL hidden_caret = FALSE;
  BOOL destroyed_caret = FALSE;
  assert(windows.ui()
             .call([&] {
               hidden_caret = HideCaret(caret_window);
               destroyed_caret = DestroyCaret();
             })
             .ok());
  assert(hidden_caret && destroyed_caret);
  assert(GetForegroundWindow() == caret_window);
  const ScreenService::Caret no_caret = service.caret();
  assert(!no_caret.found);

  BOOL destroyed_window = FALSE;
  assert(windows.ui()
             .call([&] { destroyed_window = DestroyWindow(caret_window); })
             .ok());
  assert(destroyed_window);
  assert(windows.stop().ok());

  return 0;
}
