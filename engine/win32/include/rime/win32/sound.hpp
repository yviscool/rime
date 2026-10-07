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

  // ---- endpoint controls: AHK SoundGetVolume / SoundSetVolume /
  // SoundGetMute / SoundSetMute / SoundGetName (lib/sound.cpp:292-536) ----
  //
  // Core Audio over COM: an omitted component is the endpoint's master
  // control, a named one walks the device topology for the matching
  // connector. Each call owns its apartment and its interfaces for the span
  // of that call on the calling worker, so nothing is marshalled and no COM
  // pointer ever reaches JS. Like beep and play, these build no Action, so
  // the module body that calls in is where capability `media.sound` is read.

  // AHK's setting string, scaled by 100: a bare number is absolute, a leading
  // '+' or '-' adjusts the current value (lib/sound.cpp:331-342). The scalar
  // is already divided by 100 and clamped to [-1, 1], exactly as AHK does
  // before it ever touches a device.
  struct VolumeSetting {
    double scalar{0};
    bool adjust{false};
  };

  // "": the endpoint's master control. Otherwise "name", "name:N" (last colon
  // splits, AHK's ATOI tail so "Wave:x" means instance 0 and never matches)
  // or a bare instance number (lib/sound.cpp:146-160).
  struct ComponentSpec {
    bool master{true};
    std::string name;
    int instance{1};
  };

  // "": the default render endpoint. Otherwise "name", "name:N" or a 1-based
  // index over every endpoint including unplugged ones; `index` is the
  // 0-based Item() index, or the countdown for a case-insensitive name-prefix
  // match (lib/sound.cpp:60-126).
  struct DeviceSpec {
    bool use_default{true};
    std::string name;
    int index{0};
  };

  // Pure string parsing - no device, no clock, so tests pin the shapes with
  // hand-written literals. parse_volume_setting follows AHK's gate exactly:
  // IsNumeric(aSetting, TRUE, FALSE, TRUE) plus ATOF (util.cpp:326-436,
  // lib/sound.cpp:300-302, 331-342) means spaces and tabs are trimmed, a
  // sign/decimal point/exponent/0x prefix is legal, a suffix that is not part
  // of the number rejects the string, a blank string is rejected, the result
  // is divided by 100 and clamped to [-1, 1] ("1e999" clamps to 1, not an
  // error), and `adjust` comes from the raw first character so " +5" stays an
  // absolute 5. parse_component and parse_device share AHK's ParseInteger for
  // the index form (util.cpp:951-981): spaces/tabs, sign, decimal or 0x-hex,
  // whole string consumed, wrapping overflow - anything else is a name.
  static bool parse_volume_setting(const std::string& raw, VolumeSetting& out);
  static ComponentSpec parse_component(const std::string& raw);
  static DeviceSpec parse_device(const std::string& raw);

  // Read side resolves to `target_gone` when the device or component does not
  // exist ("Device not found" / "Component not found", AHK script.h:216-218
  // verbatim) and to `unsupported` when the component carries no such control
  // ("Component doesn't support this control type", script.h:218). Volume is
  // a percentage like AHK's return value (0..100, float32 round-trip, so a
  // read-back of a value you just set can differ in the last bits).
  static rime::core::Error get_volume(const ComponentSpec& component, const DeviceSpec& device,
                                      double& out_percent);
  static rime::core::Error set_volume(const VolumeSetting& setting, const ComponentSpec& component,
                                      const DeviceSpec& device);
  static rime::core::Error get_mute(const ComponentSpec& component, const DeviceSpec& device,
                                    bool& out_muted);
  // Mute has no relative form here: `setMute(+...)` toggling is AHK's string
  // convention, and the SDK asks for setMute(!await getMute()) instead - the
  // deviation is recorded in docs/api/sound.md.
  static rime::core::Error set_mute(bool muted, const ComponentSpec& component,
                                    const DeviceSpec& device);
  static rime::core::Error get_name(const ComponentSpec& component, const DeviceSpec& device,
                                    std::string& out_name);

 private:
  bool open_{false};
};

}  // namespace rime::win32
