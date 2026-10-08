#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/gui.hpp"

#include "quickjs.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

// Capability every rime:ui call is gated on - the five batch-1 dialogs, the
// batch-2 `ui.createGui` factory and every Gui/GuiControl member body.
// contracts/registry/actions.json (capabilities.ui.create) points at this
// declaration, so the literal lives in exactly one place.
inline constexpr const char* kUiCreateCapability = "ui.create";

// ---- batch-2 state (gui-menu.md §4.1) --------------------------------------
//
// Everything below is touched on the JS thread only - the pump only ever sees
// stable ids and plain-value specs - so it needs no lock. JSValue members are
// released by teardown(), which the host runs before JS_FreeContext. The
// shared_ptr lets the host teardown reach the state through a weak_ptr after
// the runtime is gone, and lets tests inspect the containers directly.

// One registered event handler. `fn` is either a function or a string naming
// a method on the Gui's eventObj (gui-menu.md §4.3, script_gui.cpp:346-351).
struct GuiJsHandler {
  JSValue fn{JS_UNDEFINED};
  std::string name;   // event name, kept for diagnostics and removal matching
  int add_remove{1};  // 1 = call first, -1 = call last (0 is the removal form)
};

// One key of the handler table (gui-menu.md §4.3). Structured rather than a
// concatenated string because the same components feed the pump-side
// GuiEventInterest snapshot and the dispatch-time lookup, so parsing them out
// of a string twice would be two more places to get wrong.
//
//   kind 'e'  event      selector unused, target 0 = the Gui window / else a
//                        control id, event = "Close"|"Resize"|"Click"|"Change"
//   kind 'm'  message    selector = the raw message number, target as above
//   kind 'c'  WM_COMMAND selector = notify code, target = control id
//   kind 'n'  WM_NOTIFY   selector = notify code, target = control id
struct GuiJsRoute {
  char kind{'e'};
  // Signed because a WM_NOTIFY code is a signed value (TVN_FIRST-407 and
  // friends are negative); WM_COMMAND codes and message numbers are not.
  std::int64_t selector{0};
  std::uint64_t target{0};
  std::string event;

  bool operator<(const GuiJsRoute& other) const {
    return std::tie(kind, selector, target, event) <
           std::tie(other.kind, other.selector, other.target, other.event);
  }
};

// One control as the JS side knows it. The Win32 child is created by the
// pump; this entry exists so `g["name"]`, ClassNN and Gui resolve without a
// round trip.
struct GuiJsControl {
  std::uint64_t id{0};
  std::uint64_t gui_id{0};
  GuiService::GuiControlKind kind{GuiService::GuiControlKind::Text};
  std::string name;   // vName ("" when the control never declared one)
  int ordinal{1};     // 1-based among same-kind siblings (ClassNN)
  bool destroyed{false};
  // WeakRef of the JS control object: state must not keep it alive, or the
  // FinalizationRegistry path (§4.5) could never fire. deref() preserves
  // identity while the script still holds the object.
  JSValue weak{JS_UNDEFINED};
};

// One script-created Gui. The handler table is keyed by GuiJsRoute (see
// above); every registration also mirrors into GuiSpec::interest so the pump
// starts pushing the right events as soon as the window exists
// (gui-menu.md §4.3).
struct GuiJs {
  std::uint64_t id{0};
  std::string title;
  std::string name;
  GuiService::GuiStyleOptions style;
  std::uint64_t channel{0};  // host callback id of this Gui's event channel
  bool initialized{false};
  bool destroyed{false};
  // True once any member has issued a pump call, so a Promise reader knows
  // when the window may exist (gui-menu.md §4.1: HWND materializes on the
  // first async member call). Title reads the real window after that;
  // BackColor/MarginX/MarginY have no native getter and stay mirrors
  // (§4.2, noted as a doc deviation in the same section).
  bool materialized{false};
  std::string back_color;  // Gui.BackColor mirror: last set value as AHK's
                           // 6-hex "RRGGBB" form; "" = never set
  int margin_x{-1};        // MarginX/MarginY mirror; -1 = never set
  int margin_y{-1};
  std::map<GuiJsRoute, std::vector<GuiJsHandler>> handlers;
  std::vector<std::uint64_t> controls;  // control ids, creation order
  JSValue event_obj{JS_UNDEFINED};      // kept alive: string callbacks resolve on it
  JSValue weak{JS_UNDEFINED};           // WeakRef of the JS Gui object
};

// JS-thread state behind the batch-2 Gui/GuiControl object family. Built
// lazily by the module's first `ui.createGui` and closed by the host
// teardown.
struct GuiJsState {
  // ---- wiring (set once when the module initializes) ----
  rime::js::Host* host{nullptr};
  GuiService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  // Stable for the host's lifetime; used only to JS_FreeValue owned values.
  JSContext* js_context{nullptr};
  // Read by the event sink from the pump, so it is atomic.
  std::atomic<bool> closed{false};

  // FinalizationRegistry whose cleanup is the native GC path (§4.5). Held
  // here so Destroy can unregister a Gui that is still alive.
  JSValue registry{JS_UNDEFINED};

  std::map<std::uint64_t, GuiJs> guis;              // gui id -> state
  std::map<std::uint64_t, GuiJsControl> controls;   // control id -> state
  std::map<std::uint64_t, std::uint64_t> channels;  // channel id -> gui id
  std::uint64_t next_gui_id{1};
  std::uint64_t next_ctrl_id{1};

  void teardown();
};

// Wiring a ui module needs: the GuiService (dialogs, tooltip, tray) and the
// Action kernel the capability gate is read from. These five calls are direct
// service calls by design - the M6 batch-1 plan routes them without an Action -
// so like rime:sound there is no dispatcher and no action id source here.
// The owner keeps the binding alive for the whole host/runtime lifetime.
struct GuiModuleBinding {
  GuiService* service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  // Batch-2 Gui/GuiControl object state, built lazily by the module's first
  // `ui.createGui` and released by the host teardown.
  std::shared_ptr<GuiJsState> gui{};
};

// Registers `rime:ui` (exports `ui`) on a host. Every call is an async call
// gated on `ui.create`.
rime::core::Error register_ui_module(rime::js::Host& host, GuiModuleBinding* binding);
// Same for a Runtime (applies to every host it creates; pre-start only).
rime::core::Error register_ui_module(rime::js::Runtime& runtime, GuiModuleBinding* binding);

// Installs `ui.createGui` (the Gui object factory of gui-menu.md §4.1) on the
// exported `ui` namespace. Called from the module init in ui_module.cpp.
bool install_gui_family(struct JSContext* context, JSValue ui_namespace, GuiModuleBinding* binding);

}  // namespace rime::win32
