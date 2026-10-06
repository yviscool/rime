// Realism: L5 - winmm drives this machine's own audio device: Beep and
// MessageBeep are real calls, and SoundPlay opens a real MCI device on a WAV
// this test writes itself, so "playing" and "stopped" come from Windows
// rather than from the service's own bookkeeping. The AHK defaults are pinned
// with hand-written literals taken from lib/sound.cpp:594-600 (523 Hz, 150
// ms, negative falls back) instead of being recomputed by the code under test,
// and the file the device plays is deleted by the fixture on every path.

#include "rime/win32/sound.hpp"
#include "../sound_wav_fixture.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

namespace {

using rime::win32::SoundService;

void section(const char* name) {
  std::printf("---- %s ----\n", name);
  std::fflush(stdout);
}

// The one path that must fail: MCI's own text after our prefix is localised,
// so only the prefix we build ourselves is pinned here.
constexpr const char* kMissingFile = "C:\\Windows\\__rime_no_such_sound__.wav";

}  // namespace

int main() {
  section("SoundBeep defaults");
  assert(SoundService::resolve_beep(std::nullopt, std::nullopt).frequency == 523);
  assert(SoundService::resolve_beep(std::nullopt, std::nullopt).duration == 150);
  assert(SoundService::resolve_beep(880, std::nullopt).frequency == 880);
  assert(SoundService::resolve_beep(880, std::nullopt).duration == 150);
  // Only a negative duration falls back; zero is what AHK would pass through.
  assert(SoundService::resolve_beep(std::nullopt, -5).duration == 150);
  assert(SoundService::resolve_beep(440, 0).duration == 0);
  assert(SoundService::resolve_beep(1000, 250).duration == 250);

  section("SoundBeep");
  assert(SoundService::beep(std::nullopt, std::nullopt).ok());
  assert(SoundService::beep(1000, 1).ok());
  assert(SoundService::message_beep(0).ok());

  section("SoundPlay");
  SoundService sound;
  // A file that does not exist must be reported, never swallowed, and a
  // failed open leaves the service holding no alias.
  const auto missing = sound.play(kMissingFile);
  assert(!missing.ok());
  assert(missing.code == rime::core::Error::Code::ExecutionFailed);
  assert(missing.message.rfind("MCI open failed", 0) == 0);
  assert(sound.play_mode().empty());

  rime::test::TempWavPath wav;
  assert(wav.usable());
  assert(rime::test::write_wav(wav.path(), 1500));

  // The device is the oracle: it reports the sound as playing right after the
  // play and as stopped once its own 1500 ms of audio ran out.
  assert(sound.play(wav.path()).ok());
  assert(sound.play_mode() == "playing");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (sound.play_mode() == "playing" && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(sound.play_mode() == "stopped");
  sound.close_play();
  assert(sound.play_mode().empty());

  // One alias, one sound: a second play closes whatever the first still holds
  // before it opens, so a failing open does not leave audio running behind it.
  assert(sound.play(wav.path()).ok());
  assert(sound.play_mode() == "playing");
  const auto superseded = sound.play(kMissingFile);
  assert(!superseded.ok());
  assert(sound.play_mode().empty());

  std::printf("sound tests passed\n");
  return 0;
}
