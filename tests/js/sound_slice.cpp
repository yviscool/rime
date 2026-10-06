// Realism: L5 - production JS wiring (rime:sound module, real SoundService)
// drives this machine's own audio device: the beep, the MessageBeep path and
// the MCI playback are all real, and the wait promise is timed against the
// file's own length, so a facade that resolves as soon as it returned cannot
// pass. Nothing dispatches an Action, which the empty trace proves. A second
// capability-less runtime proves the gate. The WAV the device plays is written
// by this test and removed by the fixture on every path.

#include "rime/action/kernel.hpp"
#include "rime/core/json.hpp"
#include "rime/core/trace.hpp"
#include "rime/js/runtime.hpp"
#include "rime/win32/js_sound.hpp"
#include "rime/win32/sound.hpp"
#include "../sound_wav_fixture.hpp"

#include <cassert>
#include <chrono>
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

  // beep resolves null (a void write) and a fractional frequency is rejected
  // synchronously, before any winmm call can happen.
  check(runtime,
        "import { sound } from 'rime:sound';\n"
        "globalThis.beep = 'pending';\n"
        "globalThis.beepTypeError = 'none';\n"
        "try { sound.beep(1.5); } catch (e) { globalThis.beepTypeError = e.constructor.name; }\n"
        "sound.beep(1000, 1).then(r => { globalThis.beep = r; },\n"
        "                         e => { globalThis.beep = String(e.code) + ':' + e.message; });",
        "sound-beep.mjs");
  assert(runtime.settle(5000ms).ok());
  check(runtime,
        "if (globalThis.beepTypeError !== 'TypeError')\n"
        "  throw new Error('a fractional frequency must be a TypeError: ' +\n"
        "                   JSON.stringify(globalThis.beepTypeError));\n"
        "if (globalThis.beep !== null)\n"
        "  throw new Error('beep must resolve null: ' + JSON.stringify(globalThis.beep));",
        "sound-beep-check.mjs");

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

  // The wait: the file is 1500 ms of real audio, so a promise that resolved
  // as soon as the play was accepted would report well under a second.
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

  // Beep and play build no Action, so the trace stays empty across all of it.
  assert(trace->snapshot().empty());
  assert(runtime.stop().ok());

  {
    // Capability-less runtime: both writes are denied on their first worker
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
          "sound.play(" + missing_json + ").then(resolved('play'), rejected('play'));",
          "sound-denied.mjs");
    assert(denied_runtime.settle(5000ms).ok());
    check(denied_runtime,
          "if (globalThis.denied.length !== 2)\n"
          "  throw new Error('both calls must settle: ' + JSON.stringify(globalThis.denied));\n"
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
