#include "rime/win32/js_sound.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

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

// The trailing options object of the five endpoint calls (getVolume,
// setVolume, getMute, setMute, getName): AHK's component and device
// arguments plus the ActionOptions every native call shares. Omitted
// entirely it is AHK's own default - the endpoint's master control on the
// default render device - so an unqualified getVolume() is SoundGetVolume()
// with no arguments. `component`/`device` keep AHK's string grammar (parse_*
// in SoundService), so "", "2" and "Wave:2" mean exactly what they mean in a
// script; anything that is not a string throws instead of being stringified.
struct EndpointSpec {
  SoundService::ComponentSpec component;
  SoundService::DeviceSpec device;
  ActionOptions action;
};

bool parse_string_spec(JSContext* context, JSValueConst object, const char* name,
                       std::string& raw, bool& present) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  present = false;
  if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
    if (!JS_IsString(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "options.%s must be a string", name);
      return false;
    }
    const char* text = JS_ToCString(context, property);
    if (!text) {
      JS_FreeValue(context, property);
      return false;
    }
    raw = text;
    present = true;
    JS_FreeCString(context, text);
  }
  JS_FreeValue(context, property);
  return true;
}

bool parse_endpoint_options(JSContext* context, JSValueConst value, EndpointSpec& out) {
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  if (!JS_IsObject(value)) {
    JS_ThrowTypeError(context, "options must be an object");
    return false;
  }
  std::string raw;
  bool present = false;
  if (!parse_string_spec(context, value, "component", raw, present)) return false;
  if (present) out.component = SoundService::parse_component(raw);
  if (!parse_string_spec(context, value, "device", raw, present)) return false;
  if (present) out.device = SoundService::parse_device(raw);
  return parse_action_options(context, value, out.action);
}

// AHK's first SoundSetVolume argument: a percentage number, or the setting
// string itself. Both are handed to the same native parser the AHK path uses,
// so JS can never accept a setting the script side rejects - and a negative
// number stays relative for the same reason AHK's "-5" does: the stringified
// first character is what decides (lib/sound.cpp:331-342).
bool volume_setting_arg(JSContext* context, JSValueConst value, SoundService::VolumeSetting& out) {
  std::string text;
  if (JS_IsString(value)) {
    const char* raw = JS_ToCString(context, value);
    if (!raw) return false;
    text.assign(raw);
    JS_FreeCString(context, raw);
  } else if (JS_IsNumber(value)) {
    double number = 0;
    if (JS_ToFloat64(context, &number, value)) return false;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.10g", number);
    text = buffer;
  } else {
    JS_ThrowTypeError(context, "setVolume(value): value must be a number or a string");
    return false;
  }
  if (!SoundService::parse_volume_setting(text, out)) {
    JS_ThrowTypeError(context,
                      "setVolume(value): value must be a percentage like 50, \"+5\" or \"-5\"");
    return false;
  }
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

// AHK SoundGetVolume: getVolume(options?) resolves to the percentage AHK
// returns (0..100, float32 round-trip so a read-back can differ in the last
// bits). Capability media.sound is read inside the worker body, so a denied
// call never opens an endpoint; no Action is built, so the trace stays empty.
JSValue sound_get_volume(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  SoundModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:sound is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "getVolume(options?)");
  EndpointSpec spec;
  if (argc >= 1 && !parse_endpoint_options(context, argv[0], spec)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  return start_async(
      context,
      [kernel, spec]() -> AsyncOutcome {
        if (!kernel->allows(kMediaSoundCapability)) {
          return capability_denied(kMediaSoundCapability);
        }
        double percent = 0;
        if (const auto error = SoundService::get_volume(spec.component, spec.device, percent);
            !error.ok()) {
          return async_failure(error);
        }
        char text[48];
        std::snprintf(text, sizeof(text), "%.10g", percent);
        return async_success(text);
      },
      spec.action.cancellation_id);
}

// AHK SoundSetVolume: setVolume(value, options?). `value` is a percentage, or
// "+5"/"-5" to adjust the current level - a negative number is treated as the
// latter because AHK decides `adjust` from the first character of the
// stringified argument (lib/sound.cpp:331-342). Unparseable input throws a
// TypeError before any worker starts, matching how every numeric parameter
// in these modules fails early.
JSValue sound_set_volume(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                         void*) {
  SoundModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:sound is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "setVolume(value, options?)");
  SoundService::VolumeSetting setting;
  if (!volume_setting_arg(context, argv[0], setting)) return JS_EXCEPTION;
  EndpointSpec spec;
  if (argc >= 2 && !parse_endpoint_options(context, argv[1], spec)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  return start_async(
      context,
      [kernel, setting, spec]() -> AsyncOutcome {
        if (!kernel->allows(kMediaSoundCapability)) {
          return capability_denied(kMediaSoundCapability);
        }
        if (const auto error = SoundService::set_volume(setting, spec.component, spec.device);
            !error.ok()) {
          return async_failure(error);
        }
        return async_success("null");
      },
      spec.action.cancellation_id);
}

// AHK SoundGetMute: getMute(options?) resolves to a boolean, where AHK
// reports 1/0.
JSValue sound_get_mute(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  SoundModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:sound is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "getMute(options?)");
  EndpointSpec spec;
  if (argc >= 1 && !parse_endpoint_options(context, argv[0], spec)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  return start_async(
      context,
      [kernel, spec]() -> AsyncOutcome {
        if (!kernel->allows(kMediaSoundCapability)) {
          return capability_denied(kMediaSoundCapability);
        }
        bool muted = false;
        if (const auto error = SoundService::get_mute(spec.component, spec.device, muted);
            !error.ok()) {
          return async_failure(error);
        }
        return async_success(muted ? "true" : "false");
      },
      spec.action.cancellation_id);
}

// AHK SoundSetMute: setMute(muted, options?). AHK's relative form
// (SoundSetMute("+1") toggles) is a string convention that has no honest
// boolean spelling, so this API takes an absolute boolean and a caller who
// wants a toggle writes setMute(!await getMute()) - the deviation is
// recorded in docs/api/sound.md rather than smuggled into the type.
JSValue sound_set_mute(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  SoundModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:sound is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "setMute(muted, options?)");
  if (!JS_IsBool(argv[0])) {
    return JS_ThrowTypeError(context, "setMute(muted): muted must be a boolean");
  }
  const bool muted = JS_ToBool(context, argv[0]);
  EndpointSpec spec;
  if (argc >= 2 && !parse_endpoint_options(context, argv[1], spec)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  return start_async(
      context,
      [kernel, muted, spec]() -> AsyncOutcome {
        if (!kernel->allows(kMediaSoundCapability)) {
          return capability_denied(kMediaSoundCapability);
        }
        if (const auto error = SoundService::set_mute(muted, spec.component, spec.device);
            !error.ok()) {
          return async_failure(error);
        }
        return async_success("null");
      },
      spec.action.cancellation_id);
}

// AHK SoundGetName: getName(options?) resolves to the endpoint's friendly
// name, JSON-stringified by the shared encoder so a name with a quote or a
// backslash survives the trip instead of breaking the payload.
JSValue sound_get_name(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  SoundModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:sound is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "getName(options?)");
  EndpointSpec spec;
  if (argc >= 1 && !parse_endpoint_options(context, argv[0], spec)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  return start_async(
      context,
      [kernel, spec]() -> AsyncOutcome {
        if (!kernel->allows(kMediaSoundCapability)) {
          return capability_denied(kMediaSoundCapability);
        }
        std::string name;
        if (const auto error = SoundService::get_name(spec.component, spec.device, name);
            !error.ok()) {
          return async_failure(error);
        }
        return async_success(json::stringify(json::Value::string(name)));
      },
      spec.action.cancellation_id);
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
  if (!add("beep", sound_beep, 0) || !add("play", sound_play, 1) ||
      !add("getVolume", sound_get_volume, 0) || !add("setVolume", sound_set_volume, 1) ||
      !add("getMute", sound_get_mute, 0) || !add("setMute", sound_set_mute, 1) ||
      !add("getName", sound_get_name, 0)) {
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
