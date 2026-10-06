#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/screen.hpp"

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring a screen module needs: the display queries and the Action kernel the
// capability gate is read from. The monitor family is read-only, so there is
// no dispatcher and no action id source - nothing here builds an Action.
// The owner keeps the binding alive for the whole host/runtime lifetime.
struct ScreenModuleBinding {
  ScreenService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
};

// Registers `rime:screen` (exports `screen`) on a host. Every call is an
// async read gated on `screen.capture`.
rime::core::Error register_screen_module(rime::js::Host& host, ScreenModuleBinding* binding);
// Same for a Runtime (applies to every host it creates; pre-start only).
rime::core::Error register_screen_module(rime::js::Runtime& runtime, ScreenModuleBinding* binding);

}  // namespace rime::win32
