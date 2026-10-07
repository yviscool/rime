// Realism: L5 - winmm drives this machine's own audio device: Beep and
// MessageBeep are real calls, and SoundPlay opens a real MCI device on a WAV
// this test writes itself, so "playing" and "stopped" come from Windows
// rather than from the service's own bookkeeping. A runner without a render
// endpoint (GitHub's headless windows-latest, where kernel32 Beep fails)
// takes the other half of each two-branch pair: the same calls must fail
// through the documented wrapper text - "Beep failed (win32 error ...)" and
// "MCI open/play failed" - with no alias left behind, which is asserted
// rather than skipped. The AHK defaults are pinned
// with hand-written literals taken from lib/sound.cpp:594-600 (523 Hz, 150
// ms, negative falls back) instead of being recomputed by the code under test,
// and the file the device plays is deleted by the fixture on every path.
//
// The endpoint sections stay L5: volume and mute are written to this
// machine's real default endpoint and read back, with a fixture that restores
// both values on every path. The parse_* sections inside this file are L2 -
// pure functions over string literals whose expectations are written out by
// hand from AHK's own rules (IsNumeric util.cpp:326-436, ParseInteger
// util.cpp:951-981, SoundConvertComponent lib/sound.cpp:146-160,
// SoundSetGet_GetDevice lib/sound.cpp:56-126), never recomputed by the code
// under test.

#include "rime/win32/sound.hpp"
#include "../sound_wav_fixture.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
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

// Expectations are the literal shapes AHK's own code produces, spelled out
// per call - none of them is derived from what the parser returns.
void expect_volume(const std::string& raw, const double scalar, const bool adjust) {
  SoundService::VolumeSetting setting;
  assert(SoundService::parse_volume_setting(raw, setting));
  assert(setting.scalar == scalar);
  assert(setting.adjust == adjust);
}

void expect_component(const std::string& raw, const bool master, const std::string& name,
                      const int instance) {
  const auto spec = SoundService::parse_component(raw);
  assert(spec.master == master);
  assert(spec.name == name);
  assert(spec.instance == instance);
}

void expect_device(const std::string& raw, const bool use_default, const std::string& name,
                   const int index) {
  const auto spec = SoundService::parse_device(raw);
  assert(spec.use_default == use_default);
  assert(spec.name == name);
  assert(spec.index == index);
}

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

  section("parse_volume_setting (L2)");
  // Straight percentages: /100 only, so these double comparisons are exact.
  expect_volume("50", 0.5, false);
  expect_volume("25", 0.25, false);
  expect_volume("100", 1.0, false);
  expect_volume("0", 0.0, false);
  expect_volume("74.5", 0.745, false);
  // Leading '-' or '+' means "adjust the current value" - AHK reads the raw
  // first character (lib/sound.cpp:335-336), so a trimmed " +5" is absolute.
  expect_volume("+25", 0.25, true);
  expect_volume("-25", -0.25, true);
  expect_volume(" +5", 0.05, false);
  expect_volume(" 50 ", 0.5, false);
  // Clamp at [-1, 1] before anything touches a device (lib/sound.cpp:331-334).
  expect_volume("200", 1.0, false);
  expect_volume("-300", -1.0, true);
  // Overflowing a double still clamps instead of failing: ATOF gives inf and
  // the same comparison pushes it to 1.
  expect_volume("1e999", 1.0, false);
  // Scientific notation is part of AHK's IsNumeric (util.cpp:409-427).
  expect_volume("1e3", 1.0, false);
  SoundService::VolumeSetting rejected;
  const char* kRejected[] = {
      "", "   ", "\t", "abc", "12abc", "++5", "1.2.3", "1e", "1e+", ".", "+", "-", "50 60", "- 5",
      "0x", "inf", "nan"};
  for (const char* raw : kRejected) {
    assert(!SoundService::parse_volume_setting(raw, rejected));
  }

  section("parse_component / parse_device (L2)");
  // Component: "" is the master, otherwise "name", "name:N" (last colon, ATOI
  // tail) or a bare index (lib/sound.cpp:146-160).
  expect_component("", true, "", 1);
  expect_component("1", false, "", 1);
  expect_component("Wave", false, "Wave", 1);
  expect_component("Wave:2", false, "Wave", 2);
  expect_component("Wave:x", false, "Wave", 0);
  expect_component("Wave:", false, "Wave", 0);
  expect_component("1:2", false, "1", 2);  // "1:2" is not an integer, so it is a name
  expect_component("-1", false, "", -1);
  expect_component(" 3", false, "", 3);
  expect_component("0x2", false, "", 2);
  // ParseInteger wraps like digitsTo<UINT64> before narrowing (util.cpp:917).
  expect_component("4294967297", false, "", 1);
  // Device: "" is the default endpoint, otherwise "name", "name:N" or a
  // 1-based index over every endpoint (lib/sound.cpp:56-126).
  expect_device("", true, "", 0);
  expect_device("2", false, "", 1);
  expect_device("0", false, "", -1);
  expect_device("Speakers", false, "Speakers", 0);
  expect_device("Speakers:2", false, "Speakers", 1);
  expect_device("Speakers:0", false, "Speakers", -1);
  // The split is on the last colon, so earlier colons stay part of the name.
  expect_device("Realtek:2:3", false, "Realtek:2", 2);
  expect_device("0x2", false, "", 1);
  expect_device("1:2", false, "1", 1);

  // One probe read picks the branch every device-driving call below runs in -
  // the same two-asserted-branches shape as the endpoint section, never a
  // skip. The call and the probe are asserted together: Beep plays through
  // the default render endpoint, so "endpoint readable" and "Beep sounded"
  // are two views of one fact and neither can pass alone.
  const SoundService::ComponentSpec master;
  const SoundService::DeviceSpec device;
  double probe_percent = -1;
  const auto probe = SoundService::get_volume(master, device, probe_percent);

  section("SoundBeep");
  if (probe.ok()) {
    assert(SoundService::beep(std::nullopt, std::nullopt).ok());
    assert(SoundService::beep(1000, 1).ok());
    assert(SoundService::message_beep(0).ok());
  } else {
    const auto silent = SoundService::beep(std::nullopt, std::nullopt);
    assert(!silent.ok());
    assert(silent.code == rime::core::Error::Code::ExecutionFailed);
    assert(silent.message.rfind("Beep failed (win32 error ", 0) == 0);
    const auto tone = SoundService::beep(1000, 1);
    assert(!tone.ok());
    assert(tone.code == rime::core::Error::Code::ExecutionFailed);
    assert(tone.message.rfind("Beep failed (win32 error ", 0) == 0);
    // MessageBeep is deliberately not called here: user32 documents no
    // behaviour for a machine with no wave device, so there is no honest
    // expectation to assert. The branch above runs it wherever one exists.
  }

  section("SoundPlay");
  SoundService sound;
  // A file that does not exist must be reported, never swallowed, and a
  // failed open leaves the service holding no alias. Device-independent: a
  // missing file cannot open on any machine, audio or not.
  const auto missing = sound.play(kMissingFile);
  assert(!missing.ok());
  assert(missing.code == rime::core::Error::Code::ExecutionFailed);
  assert(missing.message.rfind("MCI open failed", 0) == 0);
  assert(sound.play_mode().empty());

  rime::test::TempWavPath wav;
  assert(wav.usable());
  assert(rime::test::write_wav(wav.path(), 1500));

  if (probe.ok()) {
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
  } else {
    // No wave device to open: MCI has to refuse, and the refusal must carry
    // one of the service's two documented stage prefixes (sound.cpp:
    // mci_failure) with no alias left behind - a half-opened alias behind a
    // reported failure is exactly what the missing-file case above forbids.
    const auto refused = sound.play(wav.path());
    assert(!refused.ok());
    assert(refused.code == rime::core::Error::Code::ExecutionFailed);
    assert(refused.message.rfind("MCI open failed", 0) == 0 ||
           refused.message.rfind("MCI play failed", 0) == 0);
    assert(sound.play_mode().empty());
  }

  section("endpoint volume / mute / name (L5)");

  // Two asserted branches chosen by that same probe read - this is not a skip.
  // A machine with a render endpoint runs the real write/read-back/restore
  // round-trip below; a runner without one (GitHub's headless windows-latest)
  // proves that every endpoint call reports the documented "Device not found"
  // target_gone instead, so a broken device path cannot pass either way.
  // "Component not found" is only distinguishable when an endpoint exists, so
  // that mapping is asserted by the first branch alone (a limitation, since
  // only machines with audio can prove it).

  if (probe.ok()) {
    // Capture phase: this section really writes to the machine's default
    // endpoint, so - exactly like clipboard_tests.cpp - every outcome is
    // recorded WITHOUT asserting (abort() runs no destructors), the original
    // volume and mute are written back, and only then is anything asserted.
    // Assertions are safe to place after that point because the device is
    // already back to the state the capture phase read.
    double before_percent = probe_percent;
    bool before_muted = false;
    std::string endpoint_name;
    const bool read_before_mute = SoundService::get_mute(master, device, before_muted).ok();
    const bool read_name = SoundService::get_name(master, device, endpoint_name).ok();

    // Away from the value already read, so "the device reports it back"
    // cannot pass without the write having happened.
    const double target_percent = before_percent > 50.0 ? 25.0 : 75.0;
    const bool wrote_absolute =
        SoundService::set_volume({target_percent / 100.0, false}, master, device).ok();
    double absolute_percent = -1;
    const bool read_absolute = SoundService::get_volume(master, device, absolute_percent).ok();

    // +5 on top of the value just written. Both targets are at most 75, so the
    // sum stays under 100 and never hits the clamp - the expectation is exact.
    const bool wrote_relative = SoundService::set_volume({0.05, true}, master, device).ok();
    double relative_percent = -1;
    const bool read_relative = SoundService::get_volume(master, device, relative_percent).ok();

    const bool wrote_mute = SoundService::set_mute(true, master, device).ok();
    bool mute_read_back = false;
    const bool read_mute = SoundService::get_mute(master, device, mute_read_back).ok();

    // Read-only probes: no topology walk can change a device, so they are safe
    // to record before the restore as well. They get their own scratch out-
    // parameter: a failed read zeroes the value it was given (the error is
    // what matters), so reusing `absolute_percent` here would quietly erase
    // the read-back this section is about to assert on.
    double probe_scratch = -1;
    const SoundService::ComponentSpec missing_component{false, "__rime_no_such_component__", 1};
    const auto component_error =
        SoundService::get_volume(missing_component, device, probe_scratch);
    const SoundService::DeviceSpec missing_device{false, "__rime_no_such_device__", 0};
    const auto device_error = SoundService::get_volume(master, missing_device, probe_scratch);

    // Restore before the first assert, so even an abort leaves the machine as
    // it was found.
    (void)SoundService::set_volume({before_percent / 100.0, false}, master, device);
    (void)SoundService::set_mute(before_muted, master, device);

    // The restore is observable, not assumed: read the device back and prove
    // it is where the capture phase found it, so a cleanup that silently
    // failed cannot pass as "restored".
    double restored_percent = -1;
    const bool restored_read = SoundService::get_volume(master, device, restored_percent).ok();
    bool restored_muted = !before_muted;
    const bool restored_mute_read = SoundService::get_mute(master, device, restored_muted).ok();

    // Assert phase - device already restored.
    assert(restored_read);
    assert(std::fabs(restored_percent - before_percent) < 0.01);
    assert(restored_mute_read);
    assert(restored_muted == before_muted);
    assert(before_percent >= 0.0 && before_percent <= 100.0);
    assert(read_before_mute);
    assert(read_name);
    assert(!endpoint_name.empty());
    assert(wrote_absolute);
    assert(read_absolute);
    assert(std::fabs(absolute_percent - target_percent) < 0.01);
    assert(wrote_relative);
    assert(read_relative);
    assert(std::fabs(relative_percent - (target_percent + 5.0)) < 0.01);
    assert(wrote_mute);
    assert(read_mute);
    assert(mute_read_back);

    // AHK's own wording for both misses (script.h:216-218), which
    // docs/api/sound.md pins: our strings, never localised Windows text.
    assert(!component_error.ok());
    assert(component_error.code == rime::core::Error::Code::TargetGone);
    assert(component_error.message == "Component not found");
    assert(!device_error.ok());
    assert(device_error.code == rime::core::Error::Code::TargetGone);
    assert(device_error.message == "Device not found");
  } else {
    // No default endpoint: all five operations fail the same documented way,
    // and none of them can have written anything.
    assert(probe.code == rime::core::Error::Code::TargetGone);
    assert(probe.message == "Device not found");

    bool muted = false;
    const auto mute_read = SoundService::get_mute(master, device, muted);
    assert(!mute_read.ok());
    assert(mute_read.code == rime::core::Error::Code::TargetGone);
    assert(mute_read.message == "Device not found");

    std::string name;
    const auto name_read = SoundService::get_name(master, device, name);
    assert(!name_read.ok());
    assert(name_read.code == rime::core::Error::Code::TargetGone);
    assert(name_read.message == "Device not found");

    const auto volume_write = SoundService::set_volume({0.5, false}, master, device);
    assert(!volume_write.ok());
    assert(volume_write.code == rime::core::Error::Code::TargetGone);
    assert(volume_write.message == "Device not found");

    const auto mute_write = SoundService::set_mute(true, master, device);
    assert(!mute_write.ok());
    assert(mute_write.code == rime::core::Error::Code::TargetGone);
    assert(mute_write.message == "Device not found");

    // With no endpoint to open, the device itself is what is missing, so the
    // two explicit misses collapse onto the same AHK wording.
    const SoundService::ComponentSpec missing_component{false, "__rime_no_such_component__", 1};
    const auto component_error = SoundService::get_volume(missing_component, device, probe_percent);
    assert(!component_error.ok());
    assert(component_error.code == rime::core::Error::Code::TargetGone);
    assert(component_error.message == "Device not found");
    const SoundService::DeviceSpec missing_device{false, "__rime_no_such_device__", 0};
    const auto device_error = SoundService::get_volume(master, missing_device, probe_percent);
    assert(!device_error.ok());
    assert(device_error.code == rime::core::Error::Code::TargetGone);
    assert(device_error.message == "Device not found");

    std::printf("no render endpoint: the sound API reported its documented miss\n");
  }

  std::printf("sound tests passed\n");
  return 0;
}
