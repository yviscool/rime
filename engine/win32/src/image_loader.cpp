#include "rime/win32/image_loader.hpp"

#include "utf.hpp"

#include <windows.h>

// gdiplus.h needs COM declarations (PROPID and friends) that WIN32_LEAN_AND_MEAN
// leaves out of windows.h; objidl.h supplies them on both toolchains.
#include <objidl.h>
#include <gdiplus.h>

#include <cstring>
#include <new>
#include <string>

namespace rime::win32 {
namespace {

using Error = rime::core::Error;
using Code = rime::core::Error::Code;

// GDI+ is process-wide state: a token has to be taken before any Bitmap is
// touched and released afterwards. Holding it in an object for the duration of
// one decode keeps decoded pixels free of lifetime questions - nothing is left
// running when this function returns, so there is nothing for shutdown to leak.
class GdiplusSession final {
 public:
  explicit GdiplusSession(Gdiplus::Status& status) {
    Gdiplus::GdiplusStartupInput input{};
    status = Gdiplus::GdiplusStartup(&token_, &input, nullptr);
  }
  ~GdiplusSession() {
    if (token_ != 0) Gdiplus::GdiplusShutdown(token_);
  }
  GdiplusSession(const GdiplusSession&) = delete;
  GdiplusSession& operator=(const GdiplusSession&) = delete;

 private:
  ULONG_PTR token_{0};
};

// A needle is a few thousand pixels; anything past this ceiling is a wrong
// argument rather than a big image, and refusing it here keeps the decode from
// sizing a buffer the process could not hand out anyway.
constexpr std::uint64_t kMaxPixels = 64ull * 1024ull * 1024ull;

}  // namespace

Error load_image_file(const std::string& path_utf8, ImageBuffer& out) {
  out = ImageBuffer{};
  if (path_utf8.empty()) return {Code::InvalidContract, "image path must not be empty"};
  const std::wstring wide = from_utf8(path_utf8);

  Gdiplus::Status status = Gdiplus::Ok;
  GdiplusSession session(status);
  if (status != Gdiplus::Ok) return {Code::ExecutionFailed, "cannot start the image decoder"};

  // The filename overload on both toolchains; the second parameter is an
  // embedded-colour-management flag, not a stream.
  Gdiplus::Bitmap source(wide.c_str());
  if (source.GetLastStatus() != Gdiplus::Ok) {
    return {Code::InvalidContract, "image file could not be loaded: " + path_utf8};
  }
  const auto width = source.GetWidth();
  const auto height = source.GetHeight();
  if (width == 0 || height == 0) {
    return {Code::InvalidContract, "image file has no pixels: " + path_utf8};
  }
  if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxPixels) {
    return {Code::InvalidContract, "image file is too large to search: " + path_utf8};
  }

  // Convert through a fresh 32bpp target instead of locking the source: the
  // source pixel format is whatever the file stored (indexed, 24bpp, gray),
  // and the matcher below only speaks the capture seam's 32bpp BGRA layout.
  // GDI+ calls that layout PixelFormat32bppARGB - blue lives in the lowest
  // byte, which is the byte order capture_rect hands back.
  Gdiplus::Bitmap decoded(width, height, PixelFormat32bppARGB);
  if (decoded.GetLastStatus() != Gdiplus::Ok) {
    return {Code::ExecutionFailed, "cannot allocate the decoded image"};
  }
  {
    Gdiplus::Graphics graphics(&decoded);
    if (graphics.GetLastStatus() != Gdiplus::Ok ||
        graphics.DrawImage(&source, 0, 0, static_cast<INT>(width), static_cast<INT>(height)) !=
            Gdiplus::Ok) {
      return {Code::ExecutionFailed, "cannot convert the image to 32bpp BGRA"};
    }
  }

  Gdiplus::BitmapData data{};
  Gdiplus::Rect region(0, 0, static_cast<INT>(width), static_cast<INT>(height));
  if (decoded.LockBits(&region, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) !=
      Gdiplus::Ok) {
    return {Code::ExecutionFailed, "cannot read the decoded image"};
  }

  const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
  const int stride = data.Stride;
  // A decoder that hands back a negative stride (bottom-up) or a stride
  // narrower than one row cannot be copied row by row without guessing, so it
  // is reported instead of being read out of place.
  if (stride < 0 || static_cast<std::size_t>(stride) < row_bytes) {
    decoded.UnlockBits(&data);
    return {Code::ExecutionFailed, "cannot read the decoded image"};
  }
  const auto* base = static_cast<const std::uint8_t*>(data.Scan0);
  out.width = static_cast<int>(width);
  out.height = static_cast<int>(height);
  try {
    out.bgra.assign(row_bytes * static_cast<std::size_t>(height), 0u);
    for (UINT row = 0; row < height; ++row) {
      std::memcpy(out.bgra.data() + row_bytes * row,
                  base + static_cast<std::ptrdiff_t>(row) * stride, row_bytes);
    }
  } catch (const std::bad_alloc&) {
    decoded.UnlockBits(&data);
    out = ImageBuffer{};
    return {Code::ExecutionFailed, "cannot allocate the decoded image"};
  }
  decoded.UnlockBits(&data);
  return Error::none();
}

}  // namespace rime::win32
