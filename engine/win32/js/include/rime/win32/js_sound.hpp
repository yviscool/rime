#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/sound.hpp"

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring a sound module needs: the winmm service and the Action kernel the
// capability gate is read from. There is no dispatcher and no action id
// source - SoundBeep/SoundPlay are direct service calls by design (the M5 plan
// routes them without an Action), so nothing in this module builds one.
// The owner keeps the binding alive for the whole host/runtime lifetime.
struct SoundModuleBinding {
  SoundService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
};

// Registers `rime:sound` (exports `sound`) on a host. Every call is an async
// write gated on `media.sound`.
rime::core::Error register_sound_module(rime::js::Host& host, SoundModuleBinding* binding);
// Same for a Runtime (applies to every host it creates; pre-start only).
rime::core::Error register_sound_module(rime::js::Runtime& runtime, SoundModuleBinding* binding);

}  // namespace rime::win32
