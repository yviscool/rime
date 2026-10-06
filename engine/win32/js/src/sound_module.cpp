#include "rime/win32/js_sound.hpp"

#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rime::win32 {
namespace {

SoundModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<SoundModuleBinding*>(host->module_data("rime:sound"));
}

// Capability name checked by this module. Pointed at from
// contracts/registry/actions.json (capabilities.media.sound), so the literal
// lives here at the top of the file rather than inline in the bodies.
constexpr const char* kMediaSoundCapability = "media.sound";

// Optional numeric argument for beep(). Missing/undefined/null leaves AHK's
// default in place; anything else that is not an exact integer in int range
// throws, the same no-silent-truncation rule every numeric parameter in these
// modules follows (js_int64_strict rejects fractions and non-finite values).
bool optional_int_arg(JSContext* context, JSValueConst value, const char* what,
                      std::optional<int>& out) {
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  std::int64_t raw = 0;
  if (!js_int64_strict(context, value, raw, what)) return false;
  if (raw < static_cast<std::int64_t>((std::numeric_limits<int>::min)()) ||
      raw > static_cast<std::int64_t>((std::numeric_limits<int>::max)())) {
    JS_ThrowRangeError(context, "%s is out of range", what);
    return false;
  }
  out = static_cast<int>(raw);
  return true;
}

// AHK SoundBeep: beep(frequency?, duration?, options?). The defaults and the
// negative-duration fallback are SoundService::resolve_beep's (they are AHK's,
// not the module's), so the native test pins them in one place. Capability
// media.sound is read inside the worker body, so a denied call never touches
// winmm; no Action is built, so the trace stays empty.
JSValue sound_beep(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  SoundModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:sound is not wired");
  }
  if (argc > 3) return JS_ThrowTypeError(context, "beep(frequency?, duration?, options?)");
  std::optional<int> frequency;
  std::optional<int> duration;
  if (argc >= 1 && !optional_int_arg(context, argv[0], "frequency", frequency)) {
    return JS_EXCEPTION;
  }
  if (argc >= 2 && !optional_int_arg(context, argv[1], "duration", duration)) {
    return JS_EXCEPTION;
  }
  ActionOptions options;
  if (argc >= 3 && !parse_action_options(context, argv[2], options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  return start_async(
      context,
      [kernel, frequency, duration]() -> AsyncOutcome {
        if (!kernel->allows(kMediaSoundCapability)) {
          return capability_denied(kMediaSoundCapability);
        }
        if (const auto error = SoundService::beep(frequency, duration); !error.ok()) {
          return async_failure(error);
        }
        return async_success("null");
      },
      options.cancellation_id);
}

// `*`-prefixed files are AHK's MessageBeep path: the digits after the star are
// the MB_* type (`*` alone is 0), anything non-numeric stops the parse, and an
// absurd value saturates instead of wrapping (lib/sound.cpp:547-555).
unsigned int message_beep_type(const std::string& file) {
  unsigned int type = 0;
  for (std::size_t index = 1; index < file.size(); ++index) {
    const char symbol = file[index];
    if (symbol < '0' || symbol > '9') break;
    const unsigned int digit = static_cast<unsigned int>(symbol - '0');
    if (type > (std::numeric_limits<unsigned int>::max() - digit) / 10u) break;
    type = type * 10u + digit;
  }
  return type;
}

// sound.play({wait:true}): the wait-family skeleton shared with windows.wait
// and clipboard.wait (async_task.hpp). Capability and cancellation are
// re-read on every slice, the MCI status is polled instead of pinning a
// worker for the whole playback, and the single terminal path closes the
// alias so no MCI handle outlives the promise. The first slice opens and
// starts the sound; later slices watch it until it reports "stopped".
struct SoundPlayLoop {
  rime::js::Host* host;
  SoundService* service;
  rime::action::Kernel* kernel;
  std::uint64_t token{0};
  std::uint64_t cancellation_id{0};
  std::int64_t deadline_unix_ms{0};  // absolute system ms since the epoch
  std::int64_t budget_ms{0};         // the requested deadlineMs (error text)
  std::string file;
  bool started{false};

  std::optional<AsyncOutcome> evaluate() {
    if (!kernel->allows(kMediaSoundCapability)) {
      return capability_denied(kMediaSoundCapability);
    }
    if (cancellation_id != 0 && host->is_cancelled(cancellation_id)) {
      return async_failure("cancelled", "play cancelled");
    }
    if (!started) {
      if (const auto error = service->play(file); !error.ok()) {
        return async_failure(error);
      }
      started = true;
    }
    const std::string mode = service->play_mode();
    if (mode == "stopped") return async_success("null");
    if (mode.empty()) {
      // The alias is ours but MCI stopped answering for it. Treating this as
      // success would resolve a promise for a sound that is not playing, so it
      // terminates as a failure (AHK silently breaks out of its loop here).
      return async_failure("execution_failed", "MCI lost the open sound device");
    }
    if (now_unix_ms() >= deadline_unix_ms) {
      return async_failure("timeout",
                           "play wait timed out after " + std::to_string(budget_ms) + "ms");
    }
    return std::nullopt;
  }

  void on_terminal(const AsyncOutcome&) { service->close_play(); }
};

// AHK SoundPlay: play(file, options?). `options.wait` waits for the sound to
// finish (AHK's second argument) using sliced polling; without it the promise
// resolves as soon as MCI accepted the play. A `*` file is MessageBeep and
// never waits, exactly like AHK's early return. Capability: media.sound.
JSValue sound_play(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  SoundModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:sound is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "play(file, options?)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "play(file): file must be a string");
  }
  const char* text = JS_ToCString(context, argv[0]);
  if (!text) return JS_EXCEPTION;
  std::string file(text);
  JS_FreeCString(context, text);

  ActionOptions options;
  bool wait = false;
  if (argc >= 2) {
    if (!parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
      JSValue flag = JS_GetPropertyStr(context, argv[1], "wait");
      if (JS_IsException(flag)) return JS_EXCEPTION;
      if (!JS_IsUndefined(flag) && !JS_IsNull(flag)) {
        if (!JS_IsBool(flag)) {
          JS_FreeValue(context, flag);
          return JS_ThrowTypeError(context, "play(options).wait must be a boolean");
        }
        wait = JS_ToBool(context, flag);
      }
      JS_FreeValue(context, flag);
    }
  }
  if (argc > 2) return JS_ThrowTypeError(context, "play(file, options?)");

  rime::action::Kernel* kernel = binding->kernel;
  if (!file.empty() && file[0] == '*') {
    const unsigned int type = message_beep_type(file);
    return start_async(
        context,
        [kernel, type]() -> AsyncOutcome {
          if (!kernel->allows(kMediaSoundCapability)) {
            return capability_denied(kMediaSoundCapability);
          }
          if (const auto error = SoundService::message_beep(type); !error.ok()) {
            return async_failure(error);
          }
          return async_success("null");
        },
        options.cancellation_id);
  }

  SoundService* service = binding->service;
  if (!wait) {
    return start_async(
        context,
        [service, kernel, file]() -> AsyncOutcome {
          if (!kernel->allows(kMediaSoundCapability)) {
            return capability_denied(kMediaSoundCapability);
          }
          if (const auto error = service->play(file); !error.ok()) {
            return async_failure(error);
          }
          return async_success("null");
        },
        options.cancellation_id);
  }

  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return JS_ThrowInternalError(context, "runtime host is gone");
  JSValue promise = JS_UNDEFINED;
  std::uint64_t token = 0;
  if (const auto error = host->begin_async(context, promise, token, options.cancellation_id);
      !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  auto loop = std::make_shared<SoundPlayLoop>(SoundPlayLoop{
      host, service, kernel, token, options.cancellation_id,
      wait_deadline_unix_ms(options.deadline_ms), static_cast<std::int64_t>(options.deadline_ms),
      std::move(file)});
  host->schedule_worker(token, [loop] { slice_wait_step(loop); });
  return promise;
}

int sound_module_init(JSContext* context, JSModuleDef* module) {
  SoundModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:sound requires a sound module binding");
    return -1;
  }
  JSValue sound = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue fn = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(fn)) {
      JS_FreeValue(context, sound);
      return false;
    }
    // JS_SetPropertyStr consumes `fn` on both success and failure.
    if (JS_SetPropertyStr(context, sound, name, fn) < 0) {
      JS_FreeValue(context, sound);
      return false;
    }
    return true;
  };
  if (!add("beep", sound_beep, 0) || !add("play", sound_play, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "sound", sound);
}

JSModuleDef* create_sound_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:sound", sound_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "sound") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const SoundModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:sound requires a sound service and kernel"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_sound_module(rime::js::Host& host, SoundModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:sound", binding);
  if (const auto error = host.modules().add_native(
          "rime:sound", [](JSContext* context) { return create_sound_module(context); });
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error register_sound_module(rime::js::Runtime& runtime,
                                        SoundModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:sound", [](JSContext* context) { return create_sound_module(context); }, binding);
}

}  // namespace rime::win32
