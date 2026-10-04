import type { ActionOptions, NativeActionOptions } from "../action";
import type { WindowControl, WindowSnapshot, WindowsListOptions } from "../window";
import type { SendKeyStep } from "../send";
import type {
  ChordActionTemplate,
  ClipboardChangeEvent,
  DispatchPolicy,
  ErrorEvent,
  EventControlWord,
  EventSubscription,
  ExitEvent,
  HotIfCondition,
  HotIfDescriptor,
  HotkeyAction,
  HotkeyListReport,
  HotstringAction,
  MessageEvent,
  RegistrationOptions,
} from "./events";
import type { InputHook } from "./hooks";

export interface KeyEvent {
  kind: "key";
  sequence: number;
  timestamp: number;
  injected: boolean;
  /** True only for input this process sent through `send()`. */
  selfInjected: boolean;
  down: boolean;
  vk: number;
  scan: number;
  alt: boolean;
  control: boolean;
  shift: boolean;
  super: boolean;
}

export interface MouseEvent {
  kind: "mouse";
  sequence: number;
  timestamp: number;
  injected: boolean;
  /** True only for input this process sent through `send()`. */
  selfInjected: boolean;
  action: "move" | "down" | "up" | "wheel";
  x: number;
  y: number;
  /** 1 left, 2 right, 3 middle (down/up only). */
  button: number;
  wheelDelta: number;
}

export type InputEvent = KeyEvent | MouseEvent;

/**
 * Live per-side modifier state from `input.modifiers()`: one boolean per
 * physical side (Ctrl/Shift/Alt/Win), plus the CapsLock LED state. Reflects
 * what the OS currently holds, including physical keys the user is holding.
 */
export interface ModifiersSnapshot {
  lcontrol: boolean;
  rcontrol: boolean;
  lshift: boolean;
  rshift: boolean;
  lalt: boolean;
  ralt: boolean;
  lwin: boolean;
  rwin: boolean;
  capsLock: boolean;
}

/** Mouse buttons accepted by the mouse family: 1 left, 2 right, 3 middle (AHK X1/X2 and wheel are out of scope). */
export type MouseButton = 1 | 2 | 3;

/** One `input.mouse` step: an absolute move, a relative move, or a button transition. */
export type MouseStep =
  | { action: "move"; x: number; y: number }
  | { action: "relmove"; dx: number; dy: number }
  | { action: "down"; button: MouseButton }
  | { action: "up"; button: MouseButton };

/** Payload of the `input.mouse` action: ordered steps plus the AHK speed knob. */
export interface MousePayload {
  steps: MouseStep[];
  /**
   * AHK MouseMove/MouseClick speed 0..100 (0 = instant). Validated end to
   * end, forwarded for contract fidelity, then ignored at injection: SendInput
   * moves are instantaneous — AHK does the same in SendInput/Play mode
   * (`keyboard_mouse.cpp:2369` comment and the `aSpeed == 0 || sSendMode ==
   * SM_INPUT` gate at `keyboard_mouse.cpp:2477`; stepped `DoIncrementalMouseMove`
   * at `script_autoit.cpp:1862` only runs in SM_EVENT).
   */
  speed?: number;
}

/** Cursor position plus the window/control under it (`MouseGetPos`). */
export interface MouseGetPosResult {
  x: number;
  y: number;
  /** The window under the cursor (via `WindowFromPoint` + `GetNonChildParent`); null on desktop or failure. */
  window: WindowSnapshot | null;
  /** The child control under the cursor inside `window` (AHK's point search); null when there is none. */
  control: WindowControl | null;
}

/** Mode aliases for the Send family (AHK `Send`/`SendInput`/`SendEvent`/`SendPlay`/`SendText`). */
export type KeyboardSendMode = "input" | "event" | "play" | "text" | "raw";

/** Options for the Send family: Action pipeline options plus the Send mode alias. */
export interface KeyboardSendOptions extends ActionOptions {
  /**
   * `text` compiles as `{Text}` (every char a Unicode packet), `raw` treats
   * `{}^+!#` as literal text, `input`/`event`/`play` all compile the full Send
   * grammar (this stage injects through SendInput, so event/play are
   * approximations). Default `input`.
   */
  mode?: KeyboardSendMode;
}

/**
 * Coordinate spaces for one mouse call (AHK `CoordMode Mouse/CoordMode
 * Pixel`), resolved per call instead of as a process-global mutable mode —
 * stdlib.md §2 makes settings parameters, and stdlib.md §5.3 blacklists
 * AHK-style implicit globals. Only the pixel/mouse spaces exist here:
 * ToolTip/Caret/Menu placement has no counterpart surface.
 */
export type MouseCoordSpace = "screen" | "window" | "client";

/** Coordinate-space options shared by the mouse surface (AHK `CoordMode`). */
export interface MouseCoordOptions {
  /**
   * Default `"screen"` — the only space the native payload understands
   * (int32 screen pixels, normalized to absolute at injection). `"window"`
   * offsets by the target window's outer origin and `"client"` by its
   * client-area origin (both already in screen coordinates on the snapshot);
   * both resolve through `rime:window` (`windows.active()` by default, the
   * first `windows.list(query)` match when `window` is given), so they need
   * the `windows.window.read` capability and reject when no window matches
   * or the `rime:window` module is not registered. The translated point must
   * stay inside int32 or the call throws.
   */
  coords?: MouseCoordSpace;
  /** Window for `"window"`/`"client"`; default the active window. Ignored when `coords` is `"screen"`. */
  window?: WindowsListOptions;
}

/** Options for `mouse.click` (AHK MouseClick's WhichButton/X/Y/Count/Speed). */
export interface MouseClickOptions extends MouseCoordOptions {
  /** Default 1 (left). */
  button?: MouseButton;
  /** Click point; both `x` and `y` or neither (partial throws TypeError). */
  x?: number;
  y?: number;
  /** Click repeats (default 1); a value below 1 does nothing — not even a move (AHK rule). */
  count?: number;
  /** 0..100 integer; validated then ignored at injection (see {@link MousePayload.speed}). */
  speed?: number;
}

/** Options for `mouse.drag` (AHK MouseClickDrag's WhichButton/X1/Y1/X2/Y2/Speed). */
export interface MouseDragOptions extends MouseCoordOptions {
  /** Default 1 (left). */
  button?: MouseButton;
  /** Optional drag start; both `x` and `y` or neither (partial throws TypeError; omitted = current cursor). */
  x?: number;
  y?: number;
  /** Required drag destination. */
  to: { x: number; y: number };
  /** 0..100 integer; validated then ignored at injection (see {@link MousePayload.speed}). */
  speed?: number;
}

/** Options for `mouse.move` (AHK MouseMove's Speed). */
export interface MouseMoveOptions extends MouseCoordOptions {
  /** 0..100 integer; validated then ignored at injection (see {@link MousePayload.speed}). */
  speed?: number;
}

/**
 * Key-state mode for `getKeyState` (AHK `GetKeyState`'s mode argument): the
 * first character decides — `L` logical (GetAsyncKeyState), `P` physical
 * (the hook snapshot; the free-function fallback is logical), `T` toggle
 * (CapsLock/NumLock LED). Case-insensitive; any other spelling throws.
 */
export type KeyStateMode = string;

/** The lock keys `keyboard.setLockState` accepts (AHK ships one built-in per key). */
export type LockKeyName = "capslock" | "numlock" | "scrolllock";

/**
 * `keyboard.setLockState` words: `on`/`off` set the toggle now, `alwaysOn`/
 * `alwaysOff` additionally arm the hook force (foreign presses cannot move
 * the toggle), `""`/omitted only clears the force without tapping.
 */
export type LockStateWord = "on" | "off" | "alwaysOn" | "alwaysOff" | "";

/** Options for `keyWait` (AHK KeyWait's wait state plus the house wait options). */
export interface KeyWaitOptions extends ActionOptions {
  /** Wait for a press instead of the AHK default: release. */
  down?: boolean;
  /** `physical` (default, the hook snapshot) or `logical` (GetAsyncKeyState). */
  mode?: "physical" | "logical";
}

/** Options for `blockInput` (a cancellation binding releases the block). */
export interface BlockInputOptions {
  /** When the bound cancellation fires, the block releases itself. */
  cancellationId?: number;
}

/** Options for `keyHistory` (AHK's KeyHistory ring-size argument). */
export interface KeyHistoryOptions {
  /** Resize the recording ring, 0..500 (default 40, AHK's `g_MaxHistoryKeys`). */
  maxEvents?: number;
}

/** One recorded key-history row (AHK KeyHistoryItem without the target column). */
export interface KeyHistoryRow {
  /** VK code; mouse-button rows use VK_LBUTTON/VK_RBUTTON/VK_MBUTTON. */
  vk: number;
  scan: number;
  down: boolean;
  injected: boolean;
  /** True only for input this process sent through `send()`/`input.mouse`. */
  selfInjected: boolean;
  /** Hook timestamp in ms since epoch. */
  timestamp: number;
  /** Delta to the previous recorded row, ms (0 for the first row). */
  elapsed: number;
}

/** Report from `keyHistory`; `events` is oldest-first. */
export interface KeyHistoryReport {
  capacity: number;
  count: number;
  events: KeyHistoryRow[];
}

/**
 * Bridge of the `rime:input` module. Handlers run on the JS thread.
 */
export interface InputBridge {
  /** Installs a hook subscription; returns a positive subscription id. */
  subscribe(handler: (event: InputEvent) => void): number;
  /** Closes a subscription. False for unknown or already-closed ids. */
  unsubscribe(subscriptionId: number): boolean;
  /**
   * Binds an exact-match key chord to an action template and returns a
   * positive binding id. Chords are `key` plus optional modifiers joined by
   * `+` — e.g. `ctrl+shift+k`, `f24`, `win+5`. Modifiers: ctrl|control, alt,
   * shift, super|win|logo; keys: letters, digits, f1..f24 and named nav keys
   * (space, tab, enter, escape, arrows, ...). The modifier mask must match
   * exactly, so `ctrl+k` does not fire while shift is also held.
   *
   * Throws TypeError for a malformed chord or template; throws Error naming
   * `windows.hook.global` when the hook capability is missing.
   */
  bind(chord: string, action: ChordActionTemplate): number;
  /** Closes a chord binding. False for unknown or already-closed ids. */
  unbind(bindingId: number): boolean;
  /**
   * Injects an ordered key batch through SendInput in one call, tagged as
   * this process's own input: subscribers observe the events with
   * `selfInjected: true`, and chord bindings never re-trigger on them, so a
   * script cannot feed its own chords. Foreign injected input still matches.
   * Resolves with `{ sent }` (the injected step count).
   *
   * A step's `vk` is 1..254, or a UTF-16 code unit 0..65535 when `unicode`
   * is set (the event then goes out with KEYEVENTF_UNICODE and `vk: 0` in
   * hook reports).
   *
   * Throws TypeError for malformed steps; throws Error naming
   * `windows.input.inject` when the capability is missing; rejects with
   * `invalid_state` when the input service is not running.
   */
  send(
    steps: readonly SendKeyStep[],
    options?: NativeActionOptions,
  ): Promise<{ sent: number }>;
  /**
   * Reads the live per-side modifier state (per-key `GetAsyncKeyState`, plus
   * the CapsLock LED). Synchronous: throws Error naming
   * `windows.input.inject` when the capability is missing — the state feed
   * sits behind the same gate as the injection that consumes it.
   */
  modifiers(): ModifiersSnapshot;
  /**
   * Runs the `input.mouse` action: ordered move/button steps injected through
   * SendInput in one batch. Resolves with `{ sent }` (the step count).
   *
   * Throws TypeError for malformed payloads (sync); throws Error naming
   * `windows.input.inject` when the capability is missing; rejects with
   * `invalid_state` when the input service is not running.
   */
  mouse(payload: MousePayload, options?: NativeActionOptions): Promise<{ sent: number }>;
  /**
   * Reads the cursor position and the window/control under it
   * (`MouseGetPos`). Rejects with `capability_denied` naming
   * `windows.input.read` when the read capability is missing; resolves with
   * null window/control when nothing is under the cursor (desktop).
   */
  mouseGetPos(options?: NativeActionOptions): Promise<MouseGetPosResult>;
  /**
   * Reads one key's state (`GetKeyState`): logical via GetAsyncKeyState,
   * physical via the hook snapshot, toggle via the LED bit. Synchronous:
   * TypeError for a bad key name/mode (validated before the gate); throws
   * Error naming `windows.input.read` when the capability is missing.
   */
  getKeyState(keyName: string, mode?: KeyStateMode): boolean;
  /**
   * Reads a key's scan code (`GetKeySC`): the name goes through the same
   * grammar as `getKeyState`, then `MapVirtualKeyW`; extended keys carry the
   * `0xE0` high byte (Right = 0xE04D instead of AHK's internal 0x100 flag).
   * Returns 0 for unparseable names and mouse buttons (AHK returns 0 too).
   * Synchronous: TypeError for a non-string name; no capability gate.
   */
  getKeySC(keyName: string): number;
  /**
   * Reads a key's virtual key code (`GetKeyVK`): the name goes through the
   * same grammar as `getKeyState` — chord tokens, modifier/mouse names,
   * `vkXX` hex, and the `scNNN` scan-code form — and resolves to its VK.
   * Returns 0 for names that do not parse or map to no VK (AHK returns 0
   * too); mouse buttons keep their VK values. Synchronous: TypeError for a
   * non-string name; no capability gate (it reads no input state).
   */
  getKeyVK(keyName: string): number;
  /**
   * Reads a key's canonical name (`GetKeyName`): any accepted spelling
   * (`F1`, `vk41`, `sc01e`, ...) resolves through the same grammar as
   * `getKeyState` and returns the lowercase input-grammar token for its VK —
   * `getKeyState(getKeyName(x))` accepts the result again. Returns `""` for
   * names that do not parse and for VKs no token names (AHK returns its
   * display-table name instead — `Escape`, `LControl`, or the unshifted
   * character for punctuation — and `vkNN` for unknown codes; we keep one
   * canonical spelling that round-trips). Synchronous: TypeError for a
   * non-string name; no capability gate.
   */
  getKeyName(keyName: string): string;
  /**
   * Waits for a key to reach a state (`KeyWait`): release by default,
   * `down: true` for a press, physical by default. Resolves `true` once
   * satisfied; rejects `{ code: "timeout" }` after `deadlineMs` (default
   * 5000 — AHK waits forever, the bounded wait is the house rule),
   * `"cancelled"` when the bound cancellation fires, or
   * `"capability_denied"` naming `windows.input.read`. Unknown keys, options
   * and modes throw synchronously.
   */
  keyWait(keyName: string, options?: KeyWaitOptions): Promise<true>;
  /**
   * Turns input blocking on/off (`BlockInput`): while on, the hook swallows
   * foreign input and lets this process's own injections through. Returns
   * whether input is blocked afterwards (`false` when the input service is
   * not running). Synchronous: TypeError for a bad mode/options; throws
   * Error naming `windows.input.inject` when the capability is missing.
   * Shutdown always clears the block — no path leaves the desktop blocked.
   */
  blockInput(mode: "on" | "off", options?: BlockInputOptions): boolean;

  /**
   * Arms or clears the persistent force behind `keyboard.setLockState`'s
   * `alwaysOn`/`alwaysOff` (`input.setLockForce`): `on`/`off` establish an
   * enforced direction — the low-level hook swallows foreign presses and
   * releases of that key so its toggle cannot move (this process's own
   * injected taps pass, which is how the state gets set) — and keep the
   * keyboard hook installed; `neutral` clears the force and is ungated,
   * mirroring `installKeybdHook`'s un-gated removal. Establishing requires
   * `windows.hook.global`. Synchronous: TypeError for a key other than
   * capslock/numlock/scrolllock or a bad force word; throws Error naming
   * `windows.hook.global` when establishing without it.
   */
  setLockForce(keyName: string, force: "on" | "off" | "neutral"): void;
  /**
   * Reads the hook's key-history ring (`KeyHistory`): `{ capacity, count,
   * events }` oldest-first; `options.maxEvents` resizes the ring after the
   * gate (a denied call never mutates it). Synchronous: TypeError for a
   * bad arity/maxEvents; throws Error naming `windows.input.read` when the
   * capability is missing. No GUI window and no target-column deviation.
   */
  keyHistory(options?: KeyHistoryOptions): KeyHistoryReport;

  /**
   * Reads the registered hotkeys (`ListHotkeys`): `{ suspended, hotkeys }`
   * with one row per registration in first-match order — name, enabled,
   * inputLevel, running (in-flight callbacks), suspendExempt, conditional.
   * AHK opens a GUI window with Type/Off?/Level/Running/Name columns; we
   * return the data instead (the `keyHistory` precedent) and drop the Type
   * column (every registration runs on the one hook dispatch). Synchronous;
   * no capability gate — it reads this script's own registrations, not
   * input history.
   */
  listHotkeys(): HotkeyListReport;

  /**
   * Registers a chord for the event stream (`Hotkey`). First match wins
   * under the current `hotIf*` criterion; the chord grammar is the same one
   * `bind()` accepts. Re-registering the same chord under the same criterion
   * replaces its action in place and returns its existing subscription.
   *
   * `action` is an observer function (`{ name }` payload), an action
   * template queued like `bind()`, or a control word — the control word
   * form requires the chord to be registered already (AHK's nonexistent
   * hotkey error). `options` is either a second control word that wins over
   * the action's own, or an object `{ on, suspendExempt, inputLevel }`.
   *
   * Requires `windows.hook.global`. Throws TypeError for a malformed chord,
   * action, control word or options object.
   */
  hotkey(name: string, action: HotkeyAction, options?: EventControlWord | RegistrationOptions): EventSubscription;
  /**
   * Registers a word that expands as it is typed (`Hotstring`). Specs are
   * `:options:trigger::replacement` (the omission form injects the
   * replacement through `input.send`), `:options:trigger::` with an observer
   * action (no erasure, no injection), and `:options:` which only updates
   * the defaults later registrations inherit. Option letters: `*` wildcard,
   * `?` inside word, `B` backspace, `C` case, `O` omit end char, `R`/`T`
   * accepted as no-ops (always raw); `K`, `P`, `S` are rejected.
   *
   * Also accepts the global settings forms: `EndChars` (get/set the end
   * characters), `MouseReset` (get/set whether mouse motion resets the
   * buffer), `Reset` (clear the buffer). The control word in the third
   * argument enables/disables an existing registration.
   *
   * Requires `windows.hook.global`, plus `windows.input.inject` when the
   * replacement form is used. Returns `null` for the settings and
   * options-only forms.
   */
  hotstring(spec: "EndChars"): string;
  hotstring(spec: "EndChars", value: string): string;
  hotstring(spec: "MouseReset"): boolean;
  hotstring(spec: "MouseReset", value: boolean): boolean;
  hotstring(spec: "Reset"): null;
  hotstring(
    spec: string,
    action: HotstringAction,
    onOff?: EventControlWord | RegistrationOptions,
  ): EventSubscription;
  hotstring(spec: string, onOff: EventControlWord | RegistrationOptions): EventSubscription;
  hotstring(spec: string): EventSubscription | string | boolean | null;
  /**
   * Sets the current HotIf criterion to a predicate and returns the
   * criterion that was active before the call; pass `null` to return to the
   * unconditional one. Re-using a function (or an identical window query)
   * reuses its criterion instead of allocating a new one.
   *
   * Throws TypeError for anything but a function or `null`.
   */
  hotIf(fn: HotIfCondition | null): HotIfDescriptor;
  /** HotIf criterion matching a window of the given title (regex by default, `title` wins over the rest). */
  hotIfWinActive(title?: string, className?: string, processName?: string): HotIfDescriptor;
  /** HotIf criterion matching a window of the given title. */
  hotIfWinExist(title?: string, className?: string, processName?: string): HotIfDescriptor;
  /** HotIf criterion matching an active window that does not match the query. */
  hotIfWinNotActive(title?: string, className?: string, processName?: string): HotIfDescriptor;
  /** HotIf criterion matching a window that does not exist. */
  hotIfWinNotExist(title?: string, className?: string, processName?: string): HotIfDescriptor;
  /**
   * Installs or removes the low-level keyboard hook on demand
   * (`InstallKeybdHook`); `force: true` removes it even while a stream
   * would otherwise keep it alive. Returns the effective installed state —
   * a deviation from AHK's void return so the forced case is observable.
   *
   * Requires `windows.hook.global` when installing. Throws TypeError for a
   * non-boolean argument.
   */
  installKeybdHook(install?: boolean, force?: boolean): boolean;
  /** As `installKeybdHook`, for the mouse hook (`InstallMouseHook`). */
  installMouseHook(install?: boolean, force?: boolean): boolean;
  /**
   * Registers a repeating callback (`SetTimer`). `period` is milliseconds;
   * a negative period runs the callback once; `0` deletes the timer and
   * returns `null`. Omitting it re-arms an existing timer with its previous
   * period (`250` when it is new). `priority` orders timers that fall due in
   * the same tick (higher first, then registration order).
   *
   * Callbacks are keyed by function identity: passing the same function
   * again updates its timer in place. Throws TypeError for a non-function
   * or a non-integer period/priority.
   */
  setTimer(fn: () => void, period?: number | null, priority?: number | null): EventSubscription | null;
  /**
   * Monitors a window message (`OnMessage`). The handler receives
   * `{ msg, wParam, lParam, hwnd }` on the JS thread after the pump observed
   * the message. `maxInstances` caps how many deliveries may be in flight
   * (default 1, extra messages are dropped rather than queued, AHK's rule);
   * `0` deletes the monitor with that exact `(msg, fn)` pair and returns
   * `null`. Re-registering the same pair updates its cap in place.
   *
   * Requires a running message pump. Throws TypeError for a message number
   * outside uint32 or a negative `maxInstances`.
   */
  onMessage(msgNumber: number, fn: (event: MessageEvent) => void, maxInstances?: number | null): EventSubscription | null;
  /**
   * Observes clipboard changes (`OnClipboardChange`): `{ type: 1 }` for a
   * change this process wrote, `{ type: 0 }` for a foreign one.
   *
   * Requires `windows.clipboard.read`, a clipboard service and a running
   * message pump. Each listener is called only for changes it was
   * registered for.
   */
  onClipboardChange(fn: (event: ClipboardChangeEvent) => void): EventSubscription;
  /**
   * Observes errors recorded by the host (`OnError`): `{ where, message }`.
   * An observer that throws is re-entered at most a few times before the
   * host stops re-queueing it, so a failing observer stays bounded.
   */
  onError(fn: (event: ErrorEvent) => void): EventSubscription;
  /**
   * Registers a shutdown handler (`OnExit`): `{ reason }`, delivered on the
   * JS thread while the runtime stops. Exit handlers are deliberately not
   * counted as busy subscriptions — a waiting handler never blocks unload.
   */
  onExit(fn: (event: ExitEvent) => void): EventSubscription;

  /**
   * Builds one runtime-owned `InputHook` (AHK's `InputHook()` constructor;
   * `options`/`endKeys`/`matchList` map to `__New`). Construction needs no
   * capability — `InputHook.Start()` gates on `windows.hook.global`.
   *
   * `options` are the AHK option letters `B C H I L M T V * E` (H/M/E are
   * accepted no-ops), `endKeys` is a comma/space or `{Brace}` key list,
   * `matchList` is comma separated with `,,` for a literal comma. Throws
   * TypeError (the AHK ValueError surface) for invalid input; the hook is
   * discarded when construction fails.
   */
  createInputHook(options?: string, endKeys?: string, matchList?: string): InputHook;
  /**
   * `Suspend` (`input.suspend`): pauses or resumes hotkey and hotstring
   * matching; timers, `onMessage` and `InputHook` capture keep running.
   * No argument (or `"toggle"`) flips the flag. Returns the resulting state.
   * Registrations with `suspendExempt` keep firing. Synchronous; throws
   * TypeError for a non-boolean / non-control-word argument.
   */
  suspend(on?: boolean | "on" | "off" | "toggle"): boolean;
  /**
   * Reads or replaces the central dispatch policy (`#MaxThreads`,
   * `#MaxThreadsPerHotkey`, `#InputLevel`, `#HotIfTimeout`, overflow).
   * Every provided field is validated first — an unknown field is a
   * TypeError and nothing is applied. Returns the active policy.
   */
  policy(snapshot?: Partial<DispatchPolicy>): DispatchPolicy;
}
