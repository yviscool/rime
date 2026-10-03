#pragma once

#include "rime/action/kernel.hpp"
#include "rime/core/scheduler_policy.hpp"
#include "rime/core/types.hpp"
#include "rime/win32/context_watcher.hpp"
#include "rime/win32/input.hpp"

#include "quickjs.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rime::action {
class Dispatcher;
}
namespace rime::js {
class Host;
class Runtime;
}  // namespace rime::js

namespace rime::win32 {

class ClipboardService;
class WindowService;

// One `input.bind` registration. The binding owns it for its own lifetime;
// the JS closure delivered to by the host event queue only borrows it, so
// teardown order (remove the callback, then erase the registration) can
// never race a finalizer. Filled once at bind time and read-only on the
// trigger path.
struct ChordBinding {
  std::uint32_t vk{0};
  std::uint8_t mask{0};  // exact modifier mask: bit 0 alt, 1 control, 2 shift, 3 super
  std::string type;
  std::string capability;
  std::string target_kind;
  std::string target_id;
  std::string payload;  // JSON object text for the built Action
};

// One action template shared by the M2-C exports (hotkey/hotstring observer
// forms dispatch actions exactly like input.bind does).
struct EventAction {
  std::string type;
  std::string capability;
  std::string target_kind;
  std::string target_id;
  std::string payload;  // JSON object text
};

// Wiring for `rime:input`. The owner keeps the binding alive for the whole
// host/runtime lifetime. The `callbacks`, `chords` and `next_chord_id` members
// are touched on the JS thread only. `kernel` gates hook subscriptions behind
// `windows.hook.global`; `dispatcher` and `next_action_id` turn chord bindings
// into queued, traceable actions.
struct InputModuleBinding {
  InputService* service{nullptr};
  // Window reads for `input.mouseGetPos` (window + control under the cursor).
  // Null when the host runs without a window service; the JS entry then
  // refuses with an explicit internal error instead of dereferencing null.
  WindowService* window_service{nullptr};
  // Clipboard change notifications for `input.onClipboardChange`. Null when
  // the host runs without a clipboard service; the export refuses with an
  // explicit internal error.
  ClipboardService* clipboard_service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  rime::action::Dispatcher* dispatcher{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
  std::unordered_map<std::uint64_t, std::uint64_t> callbacks;  // service id -> host callback id

  // Chord bindings: binding id -> delivery chain plus the owned context.
  struct ChordRegistration {
    std::uint64_t subscription_id{0};
    std::uint64_t host_callback_id{0};
    std::unique_ptr<ChordBinding> binding;
  };
  std::unordered_map<std::uint64_t, ChordRegistration> chords;
  std::uint64_t next_chord_id{1};  // monotonic, JS thread only

  // M2-C event state (Hotkey/Hotstring/SetTimer/OnMessage/...). Created
  // lazily when the module initializes, destroyed with the binding; the host
  // teardown registered at creation closes everything exactly once.
  std::shared_ptr<struct EventsState> events;
};

// State behind the M2-C event exports. Every member is touched on the JS
// thread only (the shared dispatch closure, timer-thread pushes and UI
// observers hop through the host event queue first); the few native counters
// are read by tests after stop()/teardown().
struct EventsState {
  // ---- wiring (set once when the module initializes) ----
  rime::js::Host* host{nullptr};
  InputService* service{nullptr};
  WindowService* window_service{nullptr};
  ClipboardService* clipboard_service{nullptr};
  rime::action::Kernel* kernel{nullptr};
  rime::action::Dispatcher* dispatcher{nullptr};
  std::atomic<std::uint64_t>* next_action_id{nullptr};
  // Stable for the host's lifetime; used only to JS_FreeValue owned values.
  JSContext* js_context{nullptr};

  // ---- shared hook dispatch (created on first hotkey/hotstring) ----
  std::uint64_t dispatch_callback{0};     // host callback invoking events_dispatch
  std::uint64_t dispatch_subscription{0}; // InputService subscription
  std::uint64_t dispatch_users{0};        // open hotkey + hotstring registrations

  // ---- M2-D dispatch policy (the one place scheduling rules live) ----
  // Consulted by hotkey/hotstring admission, the timer tick merge, onMessage
  // instance caps, HotIf evaluation and input.suspend(); never re-invented
  // inside a callback implementation (docs/api/directives-and-syntax.md).
  rime::core::SchedulerPolicy policy{rime::core::SchedulerPolicy::events()};
  // input.suspend(): hotkey and hotstring delivery is skipped unless the
  // registration set suspendExempt. Timers, onMessage and InputHook capture
  // are unaffected (AHK's Suspend only disables hotkey recognition).
  bool suspended{false};
  // Registrations currently running a callback on this thread; the policy
  // caps re-entrant delivery (#MaxThreads / #MaxThreadsPerHotkey).
  std::uint32_t running_deliveries{0};

  // ---- hotkeys (registration order = first-match order) ----
  struct Hotkey {
    std::uint64_t sub{0};
    std::uint32_t vk{0};
    std::uint8_t mask{0};
    std::uint64_t criterion{0};  // 0 = unconditional
    bool enabled{true};
    // #SuspendExempt: keeps delivering while input.suspend() is on.
    bool suspend_exempt{false};
    // Registration-time #InputLevel: events below this level never match.
    std::uint32_t input_level{0};
    // In-flight callbacks for this registration (re-entrancy guard).
    std::uint32_t running{0};
    bool via_action{false};
    std::uint64_t observer{0};  // host callback when !via_action
    EventAction action;
    std::string name;  // original spelling, kept for diagnostics
  };
  std::vector<Hotkey> hotkeys;

  // ---- hotstrings ----
  struct Hotstring {
    std::uint64_t sub{0};
    std::string trigger;
    std::string replacement;
    std::uint64_t criterion{0};
    bool enabled{true};
    // #SuspendExempt: keeps firing while input.suspend() is on.
    bool suspend_exempt{false};
    // Registration-time #InputLevel: events below this level never match.
    std::uint32_t input_level{0};
    // In-flight callbacks for this registration (re-entrancy guard).
    std::uint32_t running{0};
    // Options (Hotstring::ParseOptions letters, see docs/api/hotkey-events.md):
    bool wildcard{false};       // *: fire without an end char
    bool inside_word{false};    // ?: fire when the trigger is a word suffix
    bool do_backspace{true};    // B / B0
    bool case_sensitive{false}; // C
    bool conform_case{true};    // C0 (default) / C1
    bool omit_end_char{false};  // O
    bool do_reset{true};        // Z / Z0
    bool execute{false};        // X (function replaces the replacement)
    bool via_action{false};
    std::uint64_t observer{0};
    EventAction action;
    // Dedupe key: criterion + case + inside-word + trigger (AHK also lets
    // wildcard variants share one hotstring; options replace on re-register).
    std::string key;
  };
  std::vector<Hotstring> hotstrings;
  // Typing stream for hotstring matching (letters/digits since last reset).
  std::string typed;
  // Global hotstring settings (hotstring("EndChars"/"MouseReset"/"Reset")).
  std::string end_chars;
  bool mouse_reset{true};
  // Options-only registrations (:*: ...) feed these defaults.
  Hotstring option_defaults;  // only the option flags are read

  // ---- InputHook (M2-D) ----
  // One runtime-owned InputHook object (`input.createInputHook`). The ref
  // lives here for the host's lifetime - QuickJS may collect the JS object
  // at any time, so the owning state must not depend on a finalizer - and
  // every member call resolves its ref through the object's `id`. While in
  // progress the hook holds a host subscription, so HostAbi::unload refuses
  // to tear it down; teardown stops every hook before the JS context dies.
  struct InputHook {
    enum KeyFlag : std::uint8_t {
      kEndKey = 1u << 0,       // KeyOpt "E": this key can end the input
      kIgnoreText = 1u << 1,   // KeyOpt "I": do not collect its text
      kNotify = 1u << 2,       // KeyOpt "N": notify OnKeyDown/OnKeyUp
      // End-key shift state (single non-alpha end keys only; AHK's
      // END_KEY_WITH_SHIFT/OUT pair). Neither set = end either way.
      kEndKeyShift = 1u << 3,
      kEndKeyNoShift = 1u << 4,
      // Transient: the key-down produced text (AHK INPUT_KEY_IS_TEXT),
      // tracked only while NotifyNonText is on, exactly like the hook does.
      kHadText = 1u << 5,
      kKeyOptionMask = kEndKey | kIgnoreText | kNotify,  // KeyOpt "Z" resets these
    };
    std::uint64_t id{0};   // stable object id (the JS object's `id` property)
    std::uint64_t sub{0};  // host subscription while in progress, 0 otherwise
    bool in_progress{false};

    // Options (__New plus the settable properties).
    bool backspace_undo{true};
    bool case_sensitive{false};
    bool find_anywhere{false};
    bool notify_non_text{false};
    bool visible_non_text{true};
    bool visible_text{false};
    std::uint32_t min_send_level{0};
    std::int64_t timeout_ms{0};
    std::int64_t buffer_max{1023};  // AHK INPUTHOOK_BUFFER_SIZE - 1

    // Collection state (JS thread only).
    std::string buffer;
    std::vector<std::string> match_list;
    std::array<std::uint8_t, 256> key_flags{};
    std::string end_key;
    std::string end_mods;
    std::string end_reason{"Stopped"};
    std::string match;

    // Callbacks: the JS values are owned for the object's lifetime; the host
    // callback ids exist only while input is in progress, so an idle hook
    // holds no host reference and never blocks unload on its own.
    JSValue on_char{JS_UNDEFINED};
    JSValue on_end{JS_UNDEFINED};
    JSValue on_key_down{JS_UNDEFINED};
    JSValue on_key_up{JS_UNDEFINED};
    std::uint64_t on_char_cb{0};
    std::uint64_t on_end_cb{0};
    std::uint64_t on_key_down_cb{0};
    std::uint64_t on_key_up_cb{0};

    // Wait() waiters: promise tokens plus the deadline timers that resolve
    // them when the wait budget runs out before the input ends.
    std::vector<std::uint64_t> wait_tokens;
    std::vector<std::uint64_t> wait_timers;
    // Armed while input is in progress and Timeout > 0 (AHK's input timer).
    std::uint64_t timeout_timer{0};
  };
  std::vector<InputHook> input_hooks;
  std::uint64_t next_input_hook_id{1};      // monotonic object ids (JS thread)
  std::uint64_t hook_channel_callback{0};   // lazy timeout/wait relay channel

  // ---- HotIf criteria ----
  struct Criterion {
    std::uint64_t id{0};
    CriterionSpec spec{};        // window kinds; kind Function for fn criteria
    JSValue fn{JS_UNDEFINED};    // Function kind: identity + direct invocation
  };
  std::vector<Criterion> criteria;      // insertion order; linear is enough
  std::uint64_t next_criterion_id{1};   // monotonic criterion ids (JS thread)
  std::uint64_t current_criterion{0};   // active HotIf, 0 = none
  std::shared_ptr<ContextWatcher> watcher;  // created on first window criterion

  // ---- setTimer ----
  struct Timer {
    std::uint64_t sub{0};
    std::uint64_t callback{0};
    JSValue fn{JS_UNDEFINED};
    std::int64_t period_ms{250};
    std::int64_t priority{0};
    bool once{false};           // negative period: run then close
    std::uint64_t sequence{0};  // priority tie-break: registration order
    std::int64_t next_due_ms{0};
  };
  std::vector<Timer> timers;
  std::uint64_t scheduler_callback{0};  // host callback running timer ticks
  std::uint64_t scheduler_timer{0};     // armed TimerService id for the tick
  std::uint64_t next_sequence{1};
  // Set by the timer thread when a tick lands, cleared when the JS thread
  // starts handling one: at most one tick is in flight no matter how slow
  // the handlers run (coalescing).
  std::shared_ptr<std::atomic<bool>> tick_pending{std::make_shared<std::atomic<bool>>(false)};

  // ---- onMessage ----
  struct Monitor {
    std::uint64_t sub{0};
    std::uint64_t callback{0};
    std::uint32_t msg{0};
    std::int64_t max_instances{1};
    std::int64_t running{0};  // in-flight deliveries (nested pumps)
    JSValue fn{JS_UNDEFINED};
  };
  std::vector<Monitor> monitors;
  std::uint64_t ui_observer{0};           // shared UiThread message observer, 0 = off
  std::uint64_t message_channel_callback{0};  // fans messages out to monitors

  // ---- onClipboardChange / onError / onExit ----
  struct Observer {
    std::uint64_t sub{0};
    std::uint64_t callback{0};
    std::uint64_t native_id{0};  // ClipboardService listener id (0 = n/a)
  };
  std::vector<Observer> clipboard_listeners;
  std::uint64_t clipboard_ui_observer{0};  // shared WM_CLIPBOARDUPDATE relay
  bool clipboard_os_listener{false};       // AddClipboardFormatListener paired
  std::vector<Observer> error_observers;
  std::vector<Observer> exit_observers;

  bool closed{false};

  // Teardown is registered as a host teardown: closes every registry,
  // unsubscribes the shared dispatch, detaches native observers and frees
  // owned JSValues. Idempotent, never touches script code.
  void teardown();
  // Registrations still open (tests assert 0 after teardown).
  [[nodiscard]] std::size_t open_count() const;
  [[nodiscard]] bool dispatch_live() const { return dispatch_subscription != 0; }
  // Events the shared dispatch has seen (tests assert > 0 then reset).
  std::atomic<std::uint64_t> dispatched{0};
  // Deliveries the central policy rejected (suspend, per-registration cap,
  // global cap, onMessage instance cap; tests assert on this counter).
  std::atomic<std::uint64_t> dropped{0};
};

// Registers `rime:input` (exports `input`). `input.subscribe(handler)`
// installs a hook subscription whose events are delivered to `handler` on
// the JS thread through the host event queue; `input.unsubscribe(id)` closes
// the subscription and releases the handler. `input.bind(chord, action)`
// maps an exact-match chord (e.g. "ctrl+shift+k") onto an action template:
// every matching key-down builds a fresh Action and submits it to the
// dispatcher queue; `input.unbind(id)` closes the binding.
rime::core::Error register_input_module(rime::js::Host& host, InputModuleBinding* binding);
rime::core::Error register_input_module(rime::js::Runtime& runtime, InputModuleBinding* binding);

}  // namespace rime::win32
