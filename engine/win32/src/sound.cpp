#include "rime/win32/sound.hpp"

// utf.hpp owns the Win32 include (windows.h with NOMINMAX/WIN32_LEAN_AND_MEAN
// already applied by rime_win32); mmsystem.h has to follow it, because
// WIN32_LEAN_AND_MEAN keeps windows.h from pulling the multimedia API in.
#include "utf.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <limits>
#include <string>

namespace rime::win32 {
namespace {

// One alias per process, the way AHK keeps one too (script.h:2489). Everything
// that touches the alias goes through here so open/close/status can never
// disagree about which name is in use.
constexpr const wchar_t* kPlayAlias = L"RimeSoundPlay";

// mciSendString returns 0 on success (an MCIERROR of no error). The SDKs do
// not agree on a name for that zero, so it is named here once instead of
// sprinkling a bare literal through every comparison.
constexpr MCIERROR kMciOk = 0;

// Runs one MCI command. `reported` receives whatever the device answered and
// stays empty when the command fails - which is how an idle alias reads, the
// same way AHK's own status probe treats an untouched buffer as "nothing is
// open" (lib/sound.cpp:559).
MCIERROR mci(const std::wstring& command, std::wstring& reported) {
  wchar_t buffer[512] = {};
  reported.clear();
  const MCIERROR result = mciSendStringW(command.c_str(), buffer, 512, nullptr);
  if (result == kMciOk) reported = buffer;
  return result;
}

// Failure text for an MCI call: what was attempted plus the device's own
// message, localised or not - the text is the machine's, the prefix is ours,
// so a script can tell an MCI rejection from a Win32 one.
rime::core::Error mci_failure(const MCIERROR result, const char* what) {
  wchar_t text[256] = {};
  std::string message = std::string("MCI ") + what + " failed";
  if (mciGetErrorStringW(result, text, 256)) {
    const std::string detail = to_utf8(text);
    if (!detail.empty()) message += ": " + detail;
  }
  return {rime::core::Error::Code::ExecutionFailed, std::move(message)};
}

}  // namespace

SoundService::BeepSpec SoundService::resolve_beep(const std::optional<int>& frequency,
                                                  const std::optional<int>& duration) {
  // AHK: duration defaults to 150 and only a negative value falls back to it
  // (a zero duration stays zero - see the comment above Beep in sound.cpp).
  const int resolved_duration =
      duration.has_value() && *duration >= 0 ? *duration : 150;
  return {frequency.value_or(523), resolved_duration};
}

rime::core::Error SoundService::beep(const std::optional<int>& frequency,
                                     const std::optional<int>& duration) {
  const BeepSpec spec = resolve_beep(frequency, duration);
  if (::Beep(spec.frequency, spec.duration) == 0) {
    const DWORD code = ::GetLastError();
    return {rime::core::Error::Code::ExecutionFailed,
            "Beep failed (win32 error " + std::to_string(code) + ")"};
  }
  return rime::core::Error::none();
}

rime::core::Error SoundService::message_beep(const unsigned int type) {
  if (::MessageBeep(type) == 0) {
    const DWORD code = ::GetLastError();
    return {rime::core::Error::Code::ExecutionFailed,
            "MessageBeep failed (win32 error " + std::to_string(code) + ")"};
  }
  return rime::core::Error::none();
}

rime::core::Error SoundService::play(const std::string& path) {
  if (path.empty()) {
    return {rime::core::Error::Code::ExecutionFailed, "play(file): file is empty"};
  }
  // One open sound at a time: whatever the alias still holds goes first, so a
  // second play cannot queue behind or fail on the busy alias.
  close_play();

  const std::wstring wide_path = from_utf8(path);
  if (wide_path.empty()) {
    return {rime::core::Error::Code::ExecutionFailed, "play(file): path is not valid UTF-8"};
  }
  const std::wstring open_command = L"open \"" + wide_path + L"\" alias " + kPlayAlias;

  std::wstring reported;
  if (const MCIERROR result = mci(open_command, reported); result != kMciOk) {
    return mci_failure(result, "open");
  }
  open_ = true;
  if (const MCIERROR result = mci(std::wstring(L"play ") + kPlayAlias, reported);
      result != kMciOk) {
    // Opened but never started: hand the alias back so the next attempt
    // starts from a clean device instead of a stuck "open".
    close_play();
    return mci_failure(result, "play");
  }
  return rime::core::Error::none();
}

std::string SoundService::play_mode() const {
  if (!open_) return {};
  std::wstring reported;
  if (mci(std::wstring(L"status ") + kPlayAlias + L" mode", reported) != kMciOk) {
    return {};
  }
  return to_utf8(reported);
}

void SoundService::close_play() {
  if (!open_) return;
  std::wstring reported;
  (void)mci(std::wstring(L"close ") + kPlayAlias, reported);
  open_ = false;
}

SoundService::~SoundService() { close_play(); }

}  // namespace rime::win32
