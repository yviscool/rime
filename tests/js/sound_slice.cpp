// Realism: L5 - production JS wiring (rime:sound module, real SoundService)
// drives this machine's own audio device: the beep, the MessageBeep path and
// the MCI playback are all real, the wait promise is timed against the file's
// own length, so a facade that resolves as soon as it returned cannot pass,
// and the endpoint controls write volume/mute to this machine's default
// endpoint and read them back (the original state is restored by C++ before
// any assertion, since abort() runs no destructors). A machine without a
// render endpoint proves the same five calls through their documented
// target_gone miss instead - a branch that still asserts, never a skip - and
// the beep and the waited play through the rejection the native service
// returned ("Beep failed (win32 error ...)" / "MCI open|play failed"), so a
// facade that swallowed a device failure fails here too. MessageBeep's `*`
// branch is the one call only made where a device exists: user32 documents
// nothing for a machine without one, so there is no expectation to assert
// there rather than a reason to invent one.
// Nothing dispatches an Action, which the empty trace proves. A second
// capability-less runtime proves the gate. The WAV the device plays is
// written by this test and removed by the fixture on every path.

#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/js_sound.hpp"
#include "rime/win32/sound.hpp"
#include "../sound_wav_fixture.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_set>

namespace {

using namespace std::chrono_literals;
using rime::win32::SoundModuleBinding;
using rime::win32::SoundService;

// Fails with the error a native call returned instead of a bare assertion,
// so a start/stop regression reports its code and text.
void require_ok(const rime::core::Error& error, const char* what) {
  if (!error.ok()) {
    std::fprintf(stderr, "%s failed: %s: %s\n", what,
                 rime::core::error_code_name(error.code), error.message.c_str());
    std::fflush(stderr);
    std::abort();
  }
}

// Evaluates an assertion script; failures abort with the engine's message.
void check(rime::js::Runtime& runtime, const std::string& source, const std::string& filename) {
  const auto error = runtime.evaluate_module(source, filename).get();
  if (!error.ok()) {
    std::fprintf(stderr, "js check failed (%s): %s\n", filename.c_str(), error.message.c_str());
    std::abort();
  }
}

// Paths travel into JS as JSON strings so backslashes survive both the C++
// literal and the JS source.
std::string json_string(const std::string& text) {
  return rime::core::json::stringify(rime::core::json::Value::string(text));
}

constexpr const char* kMissingFile = "C:\\Windows\\__rime_no_such_sound__.wav";

}  // namespace

int main() {
  rime::test::TempWavPath wav;
  assert(wav.usable());
  assert(rime::test::write_wav(wav.path(), 1500));
  const std::string wav_json = json_string(wav.path());
  const std::string missing_json = json_string(kMissingFile);

  SoundService sound_service;
  auto trace = std::make_shared<rime::core::InMemoryTrace>();
  rime::action::Kernel kernel(std::make_shared<rime::action::StaticCapabilityPolicy>(
                                  std::unordered_set<std::string>{"media.sound"}),
                              trace);
  SoundModuleBinding binding{&sound_service, &kernel};

  rime::js::Runtime runtime;
  assert(rime::win32::register_sound_module(runtime, &binding).ok());
  assert(runtime.start().ok());

  // One probe read picks the branch every device-driving call below runs in -
  // the same two-asserted-branches shape the endpoint section already uses,
  // never a skip. It is read once, up here, so the beep, the waited play and
  // the endpoint round-trip all describe the same machine state.
  const SoundService::ComponentSpec master;
  const SoundService::DeviceSpec device;
  double before_percent = -1;
  bool before_muted = false;
  const auto probe = SoundService::get_volume(master, device, before_percent);
  const bool probed_mute = SoundService::get_mute(master, device, before_muted).ok();

  // The argument contract needs no device, so it runs everywhere: a
  // fractional frequency is rejected synchronously, before any winmm call
  // can happen.
  check(runtime,
        "import { sound } from 'rime:sound';\n"
        "globalThis.beepTypeError = 'none';\n"
        "try { sound.beep(1.5); } catch (e) { globalThis.beepTypeError = e.constructor.name; }",
        "sound-beep-args.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.beepTypeError !== 'TypeError')\n"
        "  throw new Error('a fractional frequency must be a TypeError: ' +\n"
        "                   JSON.stringify(globalThis.beepTypeError));",
        "sound-beep-args-check.mjs");

  if (probe.ok()) {
    // beep resolves null (a void write) where a device can answer it.
    check(runtime,
          "import { sound } from 'rime:sound';\n"
          "globalThis.beep = 'pending';\n"
          "sound.beep(1000, 1).then(r => { globalThis.beep = r; },\n"
          "                         e => { globalThis.beep = String(e.code) + ':' + e.message; });",
          "sound-beep.mjs");
    assert(runtime.settle(5000ms).ok());
    check(runtime,
          "if (globalThis.beep !== null)\n"
          "  throw new Error('beep must resolve null: ' + JSON.stringify(globalThis.beep));",
          "sound-beep-check.mjs");
  } else {
    // No endpoint to beep through: the promise must reject with the exact
    // execution failure the native call returned, because a facade that
    // swallowed it would resolve null and be indistinguishable from a beep
    // that actually sounded.
    check(runtime,
          "import { sound } from 'rime:sound';\n"
          "globalThis.beep = null;\n"
          "sound.beep(1000, 1).then(r => { globalThis.beep = 'resolved:' + JSON.stringify(r); },\n"
          "                         e => { globalThis.beep = e; });",
          "sound-beep.mjs");
    assert(runtime.settle(5000ms).ok());
    check(runtime,
          "const failure = globalThis.beep;\n"
          "if (typeof failure === 'string')\n"
          "  throw new Error('beep must reject where no endpoint exists: ' + failure);\n"
          "if (!failure || failure.code !== 'execution_failed')\n"
          "  throw new Error('beep must reject execution_failed: ' +\n"
          "                   JSON.stringify(failure && failure.code));\n"
          "if (!/^Beep failed \\(win32 error /.test(failure.message))\n"
          "  throw new Error('the failure must carry the Beep prefix: ' + failure.message);",
          "sound-beep-check.mjs");
  }

  // A missing file is reported with our own prefix (the text after it belongs
  // to MCI and is localised), and the message survives the promise boundary.
  check(runtime,
        "import { sound } from 'rime:sound';\n"
        "globalThis.playFailure = null;\n"
        "sound.play(" + missing_json + ")\n"
        "  .then(r => { globalThis.playFailure = 'resolved:' + JSON.stringify(r); },\n"
        "        e => { globalThis.playFailure = e; });",
        "sound-play-missing.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "const failure = globalThis.playFailure;\n"
        "if (typeof failure === 'string') throw new Error('play must reject: ' + failure);\n"
        "if (!failure || failure.code !== 'execution_failed')\n"
        "  throw new Error('a missing file must reject execution_failed: ' +\n"
        "                   JSON.stringify(failure && failure.code));\n"
        "if (!/^MCI open failed/.test(failure.message))\n"
        "  throw new Error('the failure must carry the MCI prefix: ' + failure.message);",
        "sound-play-missing-check.mjs");

  // AHK's `*` branch: MessageBeep never waits and resolves like the others.
  // Only where a render endpoint exists: user32 documents no behaviour for a
  // machine without one, so there is no honest expectation to assert there -
  // the branch runs the call, it just does not invent a failure for it.
  if (probe.ok()) {
    check(runtime,
          "import { sound } from 'rime:sound';\n"
          "globalThis.star = 'pending';\n"
          "sound.play('*0').then(r => { globalThis.star = r; },\n"
          "                      e => { globalThis.star = String(e.code) + ':' + e.message; });",
          "sound-play-star.mjs");
    assert(runtime.settle(5000ms).ok());
    check(runtime,
          "if (globalThis.star !== null)\n"
          "  throw new Error('MessageBeep must resolve null: ' + JSON.stringify(globalThis.star));",
          "sound-play-star-check.mjs");
  }

  // The wait: the file is 1500 ms of real audio, so a promise that resolved
  // as soon as the play was accepted would report well under a second.
  if (probe.ok()) {
    check(runtime,
          "import { sound } from 'rime:sound';\n"
          "globalThis.waited = 'pending';\n"
          "globalThis.startedAt = Date.now();\n"
          "sound.play(" + wav_json + ", { wait: true, deadlineMs: 20000 })\n"
          "  .then(r => { globalThis.waited = r; },\n"
          "        e => { globalThis.waited = String(e.code) + ':' + e.message; });",
          "sound-play-wait.mjs");
    assert(runtime.settle(30000ms).ok());
    check(runtime,
          "if (globalThis.waited !== null)\n"
          "  throw new Error('a waited play must resolve null: ' + JSON.stringify(globalThis.waited));\n"
          "const elapsed = Date.now() - globalThis.startedAt;\n"
          "if (elapsed < 1000)\n"
          "  throw new Error('the wait must outlast the 1500ms file: ' + elapsed + 'ms');\n"
          "if (elapsed >= 19000)\n"
          "  throw new Error('the wait must respect its deadline: ' + elapsed + 'ms');",
          "sound-play-wait-check.mjs");
  } else {
    // Nothing to wait for: the play itself must reject through the MCI
    // vocabulary the native service uses, and a rejection must not leave the
    // facade pretending a play is in flight.
    check(runtime,
          "import { sound } from 'rime:sound';\n"
          "globalThis.waited = null;\n"
          "sound.play(" + wav_json + ", { wait: true, deadlineMs: 20000 })\n"
          "  .then(r => { globalThis.waited = 'resolved:' + JSON.stringify(r); },\n"
          "        e => { globalThis.waited = e; });",
          "sound-play-wait.mjs");
    assert(runtime.settle(30000ms).ok());
    check(runtime,
          "const outcome = globalThis.waited;\n"
          "if (outcome === null)\n"
          "  throw new Error('the play must settle');\n"
          "if (typeof outcome === 'string')\n"
          "  throw new Error('a play must reject where no wave device exists: ' + outcome);\n"
          "if (outcome.code !== 'execution_failed')\n"
          "  throw new Error('the rejection must be execution_failed: ' +\n"
          "                   JSON.stringify(outcome && outcome.code));\n"
          "if (!/^MCI (open|play) failed/.test(outcome.message))\n"
          "  throw new Error('the rejection must carry an MCI stage prefix: ' + outcome.message);",
          "sound-play-wait-check.mjs");
  }

  // ---- endpoint controls (volume / mute / name) ----
  //
  // Argument contract first: it needs no device, so it runs on every machine
  // and proves the TypeError boundary - a setting AHK would reject, a mute
  // flag that is not a boolean and a malformed options object never reach a
  // worker, let alone the audio endpoint.
  check(runtime,
        "import { sound } from 'rime:sound';\n"
        "globalThis.endpointArgs = 'pending';\n"
        "(async () => {\n"
        "  const failures = {};\n"
        "  const capture = async (key, call) => {\n"
        "    try { await call(); failures[key] = 'resolved'; }\n"
        "    catch (e) { failures[key] = e.constructor.name; }\n"
        "  };\n"
        "  await capture('badVolume', () => sound.setVolume('abc'));\n"
        "  await capture('infiniteVolume', () => sound.setVolume(Infinity));\n"
        "  await capture('badMute', () => sound.setMute(1));\n"
        "  await capture('badOptions', () => sound.getVolume('Wave'));\n"
        "  await capture('badComponent', () => sound.getVolume({ component: 2 }));\n"
        "  globalThis.endpointArgs = failures;\n"
        "})().catch(e => { globalThis.endpointArgs = 'error:' + e; });",
        "sound-endpoint-args.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "const failures = globalThis.endpointArgs;\n"
        "if (typeof failures !== 'object')\n"
        "  throw new Error('the argument contract must report: ' + JSON.stringify(failures));\n"
        "const expected = ['badVolume', 'infiniteVolume', 'badMute', 'badOptions',\n"
        "                  'badComponent'];\n"
        "for (const key of expected) {\n"
        "  if (failures[key] !== 'TypeError')\n"
        "    throw new Error(key + ' must reject with a TypeError: ' +\n"
        "                     JSON.stringify(failures[key]));\n"
        "}",
        "sound-endpoint-args-check.mjs");

  // The machine decides which of the two branches below runs, but neither is a
  // skip: with a render endpoint the values are written and read back, without
  // one every call must report the documented target_gone miss. The original
  // volume and mute were captured by the probe read at the top and are
  // restored by C++ straight after settle - before any JS assertion can abort
  // - because abort() runs no destructors.
  if (probe.ok() && probed_mute) {
    check(runtime,
          "import { sound } from 'rime:sound';\n"
          "globalThis.endpoint = 'pending';\n"
          "(async () => {\n"
          "  const start = await sound.getVolume();\n"
          "  const startMute = await sound.getMute();\n"
          "  const name = await sound.getName();\n"
          "  await sound.setVolume(42);\n"
          "  const absolute = await sound.getVolume();\n"
          "  await sound.setVolume('+10');\n"
          "  const raised = await sound.getVolume();\n"
          "  await sound.setVolume(-5);\n"
          "  const lowered = await sound.getVolume();\n"
          "  await sound.setMute(!startMute);\n"
          "  const toggled = await sound.getMute();\n"
          "  const master = await sound.getVolume({ component: '' });\n"
          "  let deviceFailure = null;\n"
          "  try { await sound.getVolume({ device: '__rime_no_such_device__' }); }\n"
          "  catch (e) { deviceFailure = { code: e.code, message: e.message }; }\n"
          "  let componentFailure = null;\n"
          "  try { await sound.getVolume({ component: '__rime_no_such_component__' }); }\n"
          "  catch (e) { componentFailure = { code: e.code, message: e.message }; }\n"
          "  globalThis.endpoint = { start, startMute, name, absolute, raised,\n"
          "                           lowered, toggled, master,\n"
          "                           deviceFailure, componentFailure };\n"
          "})().catch(e => { globalThis.endpoint = 'error:' +\n"
          "                  String(e && (e.code || e.message) || e); });",
          "sound-endpoint.mjs");
    const auto settled = runtime.settle(20000ms);

    // Restore before the first assertion: the machine is back to the captured
    // state whether the JS above passed or exploded.
    (void)SoundService::set_volume({before_percent / 100.0, false}, master, device);
    (void)SoundService::set_mute(before_muted, master, device);
    // ...and the restore is observable rather than assumed, so a cleanup that
    // silently failed cannot pass as restored.
    double restored_percent = -1;
    const bool restored_read = SoundService::get_volume(master, device, restored_percent).ok();
    bool restored_muted = !before_muted;
    const bool restored_mute_read = SoundService::get_mute(master, device, restored_muted).ok();
    assert(settled.ok());
    assert(restored_read);
    assert(std::fabs(restored_percent - before_percent) < 0.01);
    assert(restored_mute_read);
    assert(restored_muted == before_muted);

    check(runtime,
          "const got = globalThis.endpoint;\n"
          "if (typeof got !== 'object')\n"
          "  throw new Error('the endpoint round-trip must report: ' + JSON.stringify(got));\n"
          "if (typeof got.start !== 'number' || got.start < 0 || got.start > 100)\n"
          "  throw new Error('getVolume must be a percentage: ' + JSON.stringify(got.start));\n"
          "if (typeof got.startMute !== 'boolean')\n"
          "  throw new Error('getMute must be a boolean: ' + JSON.stringify(got.startMute));\n"
          "if (typeof got.name !== 'string' || got.name.length === 0)\n"
          "  throw new Error('getName must be a non-empty name: ' + JSON.stringify(got.name));\n"
          // 42 was written and read back through the device: float32 costs a
          // few 1e-6, a facade that never wrote costs 42 percent.
          "if (Math.abs(got.absolute - 42) > 0.01)\n"
          "  throw new Error('setVolume(42) must read back: ' + JSON.stringify(got.absolute));\n"
          "if (Math.abs(got.raised - (got.absolute + 10)) > 0.01)\n"
          "  throw new Error('\"+10\" must raise by 10: ' + JSON.stringify(got.raised));\n"
          // The negative number form is relative too (AHK decides from the
          // first character of the stringified argument).
          "if (Math.abs(got.lowered - (got.raised - 5)) > 0.01)\n"
          "  throw new Error('setVolume(-5) must lower by 5: ' + JSON.stringify(got.lowered));\n"
          "if (got.toggled !== !got.startMute)\n"
          "  throw new Error('setMute must invert the captured state: ' +\n"
          "                   JSON.stringify([got.startMute, got.toggled]));\n"
          // An explicit empty component is AHK's default, so it must agree
          // with the unqualified read taken at the same volume - not with the
          // value the device had before any of the writes above.
          "if (Math.abs(got.master - got.lowered) > 0.01)\n"
          "  throw new Error('component: \"\" must be the master control: ' +\n"
          "                   JSON.stringify([got.master, got.lowered]));\n"
          "const expectMiss = (what, failure, message) => {\n"
          "  if (!failure || failure.code !== 'target_gone' || failure.message !== message)\n"
          "    throw new Error(what + ' must report AHK\\'s miss: ' + JSON.stringify(failure));\n"
          "};\n"
          // A device that matches nothing fails before any component is
          // looked at; a component that matches nothing on a real device is
          // AHK's other wording (script.h:216-218).
          "expectMiss('a missing device', got.deviceFailure, 'Device not found');\n"
          "expectMiss('a missing component', got.componentFailure, 'Component not found');",
          "sound-endpoint-check.mjs");
  } else {
    // No render endpoint: the same five calls, asserted through their
    // documented failure instead of a write nobody can observe. AHK raises
    // "Device not found" (script.h:216) when there is no default endpoint, so
    // our mapping is what a machine without audio has to prove.
    assert(probe.code == rime::core::Error::Code::TargetGone);
    assert(probe.message == "Device not found");
    check(runtime,
          "import { sound } from 'rime:sound';\n"
          "globalThis.noEndpoint = 'pending';\n"
          "(async () => {\n"
          "  const calls = {\n"
          "    getVolume: () => sound.getVolume(),\n"
          "    setVolume: () => sound.setVolume(42),\n"
          "    getMute: () => sound.getMute(),\n"
          "    setMute: () => sound.setMute(true),\n"
          "    getName: () => sound.getName(),\n"
          "  };\n"
          "  const outcomes = {};\n"
          "  for (const key of Object.keys(calls)) {\n"
          "    try { const value = await calls[key]();\n"
          "          outcomes[key] = { resolved: JSON.stringify(value) }; }\n"
          "    catch (e) { outcomes[key] = { code: e.code, message: e.message }; }\n"
          "  }\n"
          "  globalThis.noEndpoint = outcomes;\n"
          "})().catch(e => { globalThis.noEndpoint = 'error:' + e; });",
          "sound-no-endpoint.mjs");
    assert(runtime.settle(10000ms).ok());
    check(runtime,
          "const outcomes = globalThis.noEndpoint;\n"
          "if (typeof outcomes !== 'object')\n"
          "  throw new Error('the endpoint calls must report: ' + JSON.stringify(outcomes));\n"
          "for (const key of ['getVolume', 'setVolume', 'getMute', 'setMute', 'getName']) {\n"
          "  const outcome = outcomes[key];\n"
          "  if (!outcome || outcome.code !== 'target_gone' ||\n"
          "      outcome.message !== 'Device not found')\n"
          "    throw new Error(key + ' must report AHK\\'s miss: ' + JSON.stringify(outcome));\n"
          "}",
          "sound-no-endpoint-check.mjs");
  }

  // Beep, play and the endpoint controls build no Action, so the trace stays
  // empty across all of it.
  assert(trace->snapshot().empty());
  assert(runtime.stop().ok());

  {
    // Capability-less runtime: all four calls are denied on their first worker
    // slice. The JS lane is process-wide, so this runtime only starts after
    // the first one released it (the same rule the screen slice follows).
    // slice, and the message names the capability so a script can tell
    // "not granted" from "the device refused".
    rime::action::Kernel denied_kernel(
        std::make_shared<rime::action::StaticCapabilityPolicy>(std::unordered_set<std::string>{}),
        std::make_shared<rime::core::InMemoryTrace>());
    SoundModuleBinding denied_binding{&sound_service, &denied_kernel};
    rime::js::Runtime denied_runtime;
    require_ok(rime::win32::register_sound_module(denied_runtime, &denied_binding),
               "denied module registration");
    require_ok(denied_runtime.start(), "denied runtime start");
    check(denied_runtime,
          "import { sound } from 'rime:sound';\n"
          "globalThis.denied = [];\n"
          "const resolved = where => value =>\n"
          "  globalThis.denied.push({ where, code: 'resolved', message: JSON.stringify(value) });\n"
          "const rejected = where => e =>\n"
          "  globalThis.denied.push({ where, code: e.code, message: e.message });\n"
          "sound.beep().then(resolved('beep'), rejected('beep'));\n"
          "sound.play(" + missing_json + ").then(resolved('play'), rejected('play'));\n"
          // A read and a write through the endpoint path: the capability is
          // read in the worker body, so neither can reach an audio device.
          "sound.getVolume().then(resolved('getVolume'), rejected('getVolume'));\n"
          "sound.setMute(true).then(resolved('setMute'), rejected('setMute'));",
          "sound-denied.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    check(denied_runtime,
          "if (globalThis.denied.length !== 4)\n"
          "  throw new Error('all four calls must settle: ' + JSON.stringify(globalThis.denied));\n"
          "for (const entry of globalThis.denied) {\n"
          "  if (entry.code !== 'capability_denied')\n"
          "    throw new Error(entry.where + ' must be denied: ' + JSON.stringify(entry));\n"
          "  if (entry.message.indexOf('media.sound') === -1)\n"
          "    throw new Error(entry.where + ' must name media.sound: ' + entry.message);\n"
          "}",
          "sound-denied-check.mjs");
    assert(denied_runtime.stop().ok());
  }

  std::printf("sound slice passed\n");
  return 0;
}
