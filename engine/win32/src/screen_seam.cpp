#include "rime/win32/screen_seam.hpp"

#include <atomic>

namespace rime::win32 {
namespace {

std::atomic<screen_seam::CaptureFn> g_capture{nullptr};

// Real capture: BitBlt the screen rectangle into a memory bitmap, then ask
// GetDIBits for a top-down 32bpp DIB so the bytes arrive as BGRA rows in y
// order. The memory DC matters: reading the screen DC's own bitmap directly
// can return the whole screen instead of the requested rectangle (the same
// note AHK carries in pixel.cpp).
bool real_capture(const RECT& bounds, std::vector<std::uint8_t>& bgra, int& width, int& height) {
  const int capture_width = bounds.right - bounds.left;
  const int capture_height = bounds.bottom - bounds.top;
  if (capture_width <= 0 || capture_height <= 0) return false;

  HDC screen = GetDC(nullptr);
  if (!screen) return false;
  bool ok = false;
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = memory ? CreateCompatibleBitmap(screen, capture_width, capture_height) : nullptr;
  HGDIOBJ previous = nullptr;
  std::vector<std::uint8_t> bytes;
  if (bitmap) previous = SelectObject(memory, bitmap);
  if (previous && previous != HGDI_ERROR) {
    if (BitBlt(memory, 0, 0, capture_width, capture_height, screen, bounds.left, bounds.top,
               SRCCOPY)) {
      BITMAPINFO info{};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = capture_width;
      info.bmiHeader.biHeight = -capture_height;  // top-down
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      bytes.resize(static_cast<std::size_t>(capture_width) *
                   static_cast<std::size_t>(capture_height) * 4u);
      ok = GetDIBits(memory, bitmap, 0, static_cast<UINT>(capture_height), bytes.data(), &info,
                     DIB_RGB_COLORS) != 0;
      if (ok) {
        width = capture_width;
        height = capture_height;
        bgra = std::move(bytes);
      }
    }
  }
  if (previous && previous != HGDI_ERROR) (void)SelectObject(memory, previous);
  if (bitmap) (void)DeleteObject(bitmap);
  if (memory) (void)DeleteDC(memory);
  (void)ReleaseDC(nullptr, screen);
  return ok;
}

}  // namespace

namespace screen_seam {

void set_capture(const CaptureFn capture) { g_capture.store(capture, std::memory_order_release); }

}  // namespace screen_seam

bool capture_rect(const RECT& bounds, std::vector<std::uint8_t>& bgra, int& width, int& height) {
  if (const screen_seam::CaptureFn seam = g_capture.load(std::memory_order_acquire)) {
    return seam(bounds, bgra, width, height);
  }
  return real_capture(bounds, bgra, width, height);
}

}  // namespace rime::win32
