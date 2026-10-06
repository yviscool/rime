#pragma once

#include "rime/core/types.hpp"

#include <optional>
#include <string>

namespace rime::win32 {

// winmm-backed audio output: AHK SoundBeep and SoundPlay
// (rime-research/AutoHotkey-alpha/source/lib/sound.cpp:546-600).
//
// Both are fire-and-forget writes through one process-wide MCI alias - the
// single-alias rule AHK keeps with its own "AHK_PlayMe" name (script.h:2489):
// a new play closes the previous sound first, so one service owns at most one
// open sound. Nothing here builds an Action, so neither call produces an
// Action Trace; the module reads capability `media.sound` in the worker body
// that calls in, and the wait loop slices instead of pinning a thread.
class SoundService {
 public:
  // The values AHK's SoundBeep actually emits: 523 Hz and 150 ms are the
  // defaults, and only a negative duration falls back to 150 ms (so it never
  // becomes a very long beep); 0 stays 0 (lib/sound.cpp:594-600). Split out
  // because the defaults are pure input mapping - the native test pins them
  // with hand-written literals instead of listening to a beep.
  struct BeepSpec {
    int frequency;
    int duration;
  };
  static BeepSpec resolve_beep(const std::optional<int>& frequency,
                               const std::optional<int>& duration);

  // AHK SoundBeep: kernel32 Beep with resolve_beep()'s values.
  static rime::core::Error beep(const std::optional<int>& frequency,
                                const std::optional<int>& duration);

  // AHK SoundPlay's `*` branch (lib/sound.cpp:547-555): MessageBeep(type),
  // where `*` alone is type 0 (MB_OK). MessageBeep reports success even with
  // no sound device, which is exactly the behaviour AHK relies on.
  static rime::core::Error message_beep(unsigned int type);

  // Opens `path` on this service's MCI alias and starts it. Anything already
  // open on the alias is closed first (lib/sound.cpp:558-562). A failure
  // never leaves the alias owned: an open that did not start is closed again.
  rime::core::Error play(const std::string& path);

  // The MCI mode for the alias this service opened: "playing", "stopped", or
  // "" when nothing is open - the exact query AHK's wait loop polls every
  // 20 ms (lib/sound.cpp:576-584).
  std::string play_mode() const;

  // Closes the alias if this service opened one; a no-op otherwise.
  void close_play();

  ~SoundService();

 private:
  bool open_{false};
};

}  // namespace rime::win32
