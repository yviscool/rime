#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/gui.hpp"

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Wiring a ui module needs: the GuiService (dialogs, tooltip, tray) and the
// Action kernel the capability gate is read from. These five calls are direct
// service calls by design - the M6 batch-1 plan routes them without an Action -
// so like rime:sound there is no dispatcher and no action id source here.
// The owner keeps the binding alive for the whole host/runtime lifetime.
struct GuiModuleBinding {
  GuiService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
};

// Registers `rime:ui` (exports `ui`) on a host. Every call is an async call
// gated on `ui.create`.
rime::core::Error register_ui_module(rime::js::Host& host, GuiModuleBinding* binding);
// Same for a Runtime (applies to every host it creates; pre-start only).
rime::core::Error register_ui_module(rime::js::Runtime& runtime, GuiModuleBinding* binding);

}  // namespace rime::win32
