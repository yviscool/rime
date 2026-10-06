#pragma once

// Screen-image fixture shared by the two screen test binaries: a 24bpp BMP
// written byte by byte, so every needle pixel is known before a decoder sees
// it, plus a temp path that removes itself when the guard goes out of scope.
// Both tests create and delete the file inside the test process, so nothing
// depends on an asset committed to the repository (AGENTS realism rules).

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace rime::test {

// BMP headers are fixed-layout on-disk structures; anything else would change
// what the file contains.
static_assert(sizeof(BITMAPFILEHEADER) == 14, "BITMAPFILEHEADER must be packed to 14 bytes");
static_assert(sizeof(BITMAPINFOHEADER) == 40, "BITMAPINFOHEADER must be 40 bytes");

inline std::string to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (length <= 0) return {};
  std::string text(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), length,
                      nullptr, nullptr);
  return text;
}

// A unique file in %TEMP%, deleted by the destructor whether the test passed
// or aborted on an assertion.
class TempImagePath final {
 public:
  TempImagePath() {
    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    if (length == 0 || length >= MAX_PATH) return;
    wchar_t name[MAX_PATH]{};
    // uUnique == 0 makes the OS create the file and pick a name that does not
    // collide with another test running in parallel.
    if (GetTempFileNameW(directory, L"rscr", 0, name) == 0) return;
    path_ = to_utf8(name);
  }
  ~TempImagePath() {
    if (!path_.empty()) DeleteFileA(path_.c_str());
  }
  TempImagePath(const TempImagePath&) = delete;
  TempImagePath& operator=(const TempImagePath&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] bool usable() const { return !path_.empty(); }

 private:
  std::string path_;
};

// Writes a 24bpp uncompressed BMP. `rgb` is row-major (top row first) with one
// 0xRRGGBB value per pixel; the byte order and the bottom-up row order are the
// file's problem, not the caller's.
inline bool write_bmp_24(const std::string& path, int width, int height,
                         const std::vector<std::uint32_t>& rgb) {
  if (width <= 0 || height <= 0) return false;
  if (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) != rgb.size()) return false;
  const int stride = (width * 3 + 3) & ~3;
  BITMAPINFOHEADER info{};
  info.biSize = sizeof(info);
  info.biWidth = width;
  info.biHeight = height;  // positive: rows are stored bottom-up
  info.biPlanes = 1;
  info.biBitCount = 24;
  info.biCompression = BI_RGB;
  info.biSizeImage = static_cast<DWORD>(stride) * static_cast<DWORD>(height);

  BITMAPFILEHEADER header{};
  header.bfType = 0x4D42;  // 'BM'
  header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  header.bfSize = header.bfOffBits + info.biSizeImage;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  out.write(reinterpret_cast<const char*>(&info), sizeof(info));
  std::vector<char> row(static_cast<std::size_t>(stride), 0);
  for (int y = height - 1; y >= 0; --y) {
    for (int x = 0; x < width; ++x) {
      const std::uint32_t pixel = rgb[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                      static_cast<std::size_t>(x)];
      row[static_cast<std::size_t>(x) * 3 + 0] = static_cast<char>(pixel & 0xFFu);         // B
      row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<char>((pixel >> 8) & 0xFFu);  // G
      row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<char>((pixel >> 16) & 0xFFu);  // R
    }
    std::memset(row.data() + static_cast<std::size_t>(width) * 3, 0,
                static_cast<std::size_t>(stride) - static_cast<std::size_t>(width) * 3);
    out.write(row.data(), static_cast<std::streamsize>(row.size()));
  }
  return out.good();
}

}  // namespace rime::test
