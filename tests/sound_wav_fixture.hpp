#pragma once

// Sound fixture shared by the two sound test binaries: a 16-bit mono PCM WAV
// written byte by byte, so the playback length is known before MCI ever opens
// it, plus a temp path that removes itself when the guard goes out of scope.
// Both tests create and delete the file inside the test process, so nothing
// depends on an asset committed to the repository (AGENTS realism rules).

#include <windows.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace rime::test {

inline std::string wav_to_utf8(const std::wstring& wide) {
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
// or aborted on an assertion. The name carries a `.wav` extension on purpose:
// MCI chooses the device by extension, so the same bytes under GetTempFileName's
// `.tmp` are rejected with "device not open or not recognized" (263) while a
// `.wav` plays - the probe that proved it is why the rename happens here.
class TempWavPath final {
 public:
  TempWavPath() {
    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    if (length == 0 || length >= MAX_PATH) return;
    wchar_t name[MAX_PATH]{};
    if (GetTempFileNameW(directory, L"rswv", 0, name) == 0) return;
    std::wstring wav_name(name);
    const std::size_t dot = wav_name.find_last_of(L'.');
    const std::size_t slash = wav_name.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
      DeleteFileW(name);
      return;
    }
    wav_name.resize(dot + 1);  // keep the dot
    wav_name += L".wav";
    if (MoveFileW(name, wav_name.c_str()) == 0) {
      DeleteFileW(name);
      return;
    }
    path_ = wav_to_utf8(wav_name);
  }
  ~TempWavPath() {
    if (!path_.empty()) DeleteFileA(path_.c_str());
  }
  TempWavPath(const TempWavPath&) = delete;
  TempWavPath& operator=(const TempWavPath&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] bool usable() const { return !path_.empty(); }

 private:
  std::string path_;
};

// Writes a canonical PCM WAV (44-byte header, 8 kHz, 16-bit, mono) holding
// `milliseconds` of a 440 Hz square wave. The header is written field by
// field in little-endian order, so the file does not depend on struct packing.
inline bool write_wav(const std::string& path, int milliseconds) {
  if (milliseconds <= 0) return false;
  constexpr int kSampleRate = 8000;
  const std::uint32_t samples =
      static_cast<std::uint32_t>(kSampleRate) * static_cast<std::uint32_t>(milliseconds) / 1000u;
  if (samples == 0) return false;
  const std::uint32_t data_bytes = samples * 2u;

  std::vector<char> bytes;
  auto push = [&bytes](std::uint32_t value, int count) {
    for (int index = 0; index < count; ++index) {
      bytes.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
    }
  };
  const auto tag = [&bytes](const char* text) { bytes.insert(bytes.end(), text, text + 4); };

  tag("RIFF");
  push(36u + data_bytes, 4);
  tag("WAVE");
  tag("fmt ");
  push(16u, 4);                                 // PCM fmt chunk size
  push(1u, 2);                                  // format = PCM
  push(1u, 2);                                  // channels
  push(static_cast<std::uint32_t>(kSampleRate), 4);
  push(static_cast<std::uint32_t>(kSampleRate) * 2u, 4);  // byte rate
  push(2u, 2);                                  // block align
  push(16u, 2);                                 // bits per sample
  tag("data");
  push(data_bytes, 4);
  for (std::uint32_t index = 0; index < samples; ++index) {
    const std::int16_t amplitude = ((index / 18u) % 2u) == 0u ? 12000 : -12000;
    push(static_cast<std::uint16_t>(amplitude), 2);
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

}  // namespace rime::test
