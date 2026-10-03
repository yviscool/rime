import { runAction, type ActionOptions, type NativeActionOptions } from "./action";
import {
  compileSend,
  MOD_LCONTROL,
  MOD_LALT,
  MOD_LSHIFT,
  MOD_LWIN,
  MOD_RCONTROL,
  MOD_RALT,
  MOD_RSHIFT,
  MOD_RWIN,
  type SendKeyStep,
  type SendMode,
} from "./send";
import type { WindowControl, WindowSnapshot } from "./window";
import { input } from "rime:input";

export type { SendKeyStep } from "./send";

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

/** Options for `mouse.click` (AHK MouseClick's WhichButton/X/Y/Count/Speed). */
export interface MouseClickOptions {
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
export interface MouseDragOptions {
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
export interface MouseMoveOptions {
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
 * Action template bound to a chord. The runtime validates it at bind time
 * and rebuilds it into a fresh Action (new id, fresh deadline, source
 * `chord/rime:input`) on every matching key-down, then queues it through the
 * shared dispatcher pipeline.
 */
export interface ChordActionTemplate {
  type: string;
  capability: string;
  target: { kind: string; id: string };
  /** JSON object; default `{}`. Stringified once at bind time. */
  payload?: Record<string, unknown>;
}

/** Bridge of the `rime:input` module. Handlers run on the JS thread. */
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
   * `windows.input.inject` when the hook capability is missing.
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
   * Reads the hook's key-history ring (`KeyHistory`): `{ capacity, count,
   * events }` oldest-first; `options.maxEvents` resizes the ring after the
   * gate (a denied call never mutates it). Synchronous: TypeError for a
   * bad arity/maxEvents; throws Error naming `windows.input.read` when the
   * capability is missing. No GUI window and no target-column deviation.
   */
  keyHistory(options?: KeyHistoryOptions): KeyHistoryReport;
}

/**
 * Cross-call persistent modifier mask (AHK `sModifiersLR_persistent`): bits a
 * Send string held down with `{Ctrl down}`-style items stay down across
 * calls until released with `{Ctrl up}`.
 */
let persistentModifiers = 0;

function modifierMaskOf(snapshot: ModifiersSnapshot): number {
  let mask = 0;
  if (snapshot.lcontrol) mask |= MOD_LCONTROL;
  if (snapshot.rcontrol) mask |= MOD_RCONTROL;
  if (snapshot.lshift) mask |= MOD_LSHIFT;
  if (snapshot.rshift) mask |= MOD_RSHIFT;
  if (snapshot.lalt) mask |= MOD_LALT;
  if (snapshot.ralt) mask |= MOD_RALT;
  if (snapshot.lwin) mask |= MOD_LWIN;
  if (snapshot.rwin) mask |= MOD_RWIN;
  return mask;
}

function requireFiniteNumber(value: unknown, field: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new TypeError(`${field} must be a finite number, got ${String(value)}`);
  }
  return value;
}

/** Coordinates are int32 screen pixels: exact integers, same bound the native payload enforces. */
function requireCoordinate(value: unknown, field: string): number {
  const number = requireFiniteNumber(value, field);
  if (!Number.isInteger(number) || number < -2147483648 || number > 2147483647) {
    throw new TypeError(`${field} must be an int32 integer coordinate, got ${String(value)}`);
  }
  return number;
}

function validateSpeed(speed: number | undefined, field: string): number | undefined {
  if (speed === undefined) return undefined;
  const value = requireFiniteNumber(speed, field);
  if (!Number.isInteger(value) || value < 0 || value > 100) {
    throw new TypeError(`${field} must be an integer 0..100, got ${speed}`);
  }
  return value;
}

function validateButton(button: number | undefined, field: string): MouseButton {
  if (button === undefined) return 1;
  if (button !== 1 && button !== 2 && button !== 3) {
    throw new TypeError(`${field} button must be 1 (left), 2 (right) or 3 (middle), got ${String(button)}`);
  }
  return button;
}

function validateCount(count: number | undefined, field: string): number {
  if (count === undefined) return 1;
  return Math.trunc(requireFiniteNumber(count, field));
}

/** Validates an optional point pair: both coordinates or neither, each an int32 pixel. */
function validateOptionalPoint(x: unknown, y: unknown, field: string): void {
  if ((x === undefined) !== (y === undefined)) {
    throw new TypeError(`${field}: x and y must be given together (a partial point is rejected; AHK silently returns)`);
  }
  if (x !== undefined) requireCoordinate(x, `${field}.x`);
  if (y !== undefined) requireCoordinate(y, `${field}.y`);
}

function runSend(keys: string, options?: KeyboardSendOptions): Promise<{ sent: number }> {
  const { mode = "input", ...actionOptions } = options ?? {};
  if (mode !== "input" && mode !== "event" && mode !== "play" && mode !== "text" && mode !== "raw") {
    throw new TypeError(
      `keyboard.send: mode must be "input"|"event"|"play"|"text"|"raw", got ${String(mode)}`,
    );
  }
  const compileMode: SendMode = mode === "text" ? "text" : mode === "raw" ? "raw" : "send";
  // Live state first: modifiers() is the inject-gated sync read the compiler
  // starts from, so a missing capability throws synchronously here.
  const state = modifierMaskOf(input.modifiers());
  const compiled = compileSend(keys, { state, persistent: persistentModifiers }, compileMode);
  return runAction(actionOptions, async (native) => {
    if (compiled.steps.length === 0) {
      persistentModifiers = compiled.persistent;
      return { sent: 0 };
    }
    const result = await input.send(compiled.steps, native);
    persistentModifiers = compiled.persistent;
    return result;
  });
}

function runMouse(payload: MousePayload, options?: ActionOptions): Promise<{ sent: number }> {
  return runAction(options, (native) => input.mouse(payload, native));
}

/**
 * Send-string family: compiles the AHK Send grammar (`sdk/src/send.ts`)
 * against the live modifier state and dispatches the resulting batch through
 * `input.send`. Grammar and mode errors are synchronous TypeErrors; a missing
 * `windows.input.inject` capability surfaces synchronously from the modifier
 * read; Action failures reject (ActionError with `timeout` / `cancelled` /
 * `capability_denied` / `invalid_state`).
 *
 * `SendEvent`/`SendPlay` are SendInput approximations in this stage (same
 * compiled grammar, single SendInput batch), matching the injected-behavior
 * contract while the delivery mode differs from AHK.
 */
export const keyboard = {
  /** AHK `Send`/`SendInput`: full Send grammar, default mode `input`. */
  send(keys: string, options?: KeyboardSendOptions): Promise<{ sent: number }> {
    return runSend(keys, options);
  },
  /** AHK `SendInput`: identical compilation to `send` (this stage always injects via SendInput). */
  sendInput(keys: string, options?: Omit<KeyboardSendOptions, "mode">): Promise<{ sent: number }> {
    return runSend(keys, { ...options, mode: "input" });
  },
  /** AHK `SendEvent`: same grammar and batch as `send` (event-mode delivery is approximated). */
  sendEvent(keys: string, options?: Omit<KeyboardSendOptions, "mode">): Promise<{ sent: number }> {
    return runSend(keys, { ...options, mode: "event" });
  },
  /** AHK `SendPlay`: same grammar and batch as `send` (play-mode delivery is approximated). */
  sendPlay(keys: string, options?: Omit<KeyboardSendOptions, "mode">): Promise<{ sent: number }> {
    return runSend(keys, { ...options, mode: "play" });
  },
  /** AHK `SendText`: every non-control char becomes a KEYEVENTF_UNICODE packet (layout-independent). */
  sendText(keys: string, options?: Omit<KeyboardSendOptions, "mode">): Promise<{ sent: number }> {
    return runSend(keys, { ...options, mode: "text" });
  },
  /**
   * Live per-side modifier snapshot. Synchronous; throws Error naming
   * `windows.input.inject` when the capability is missing (the state feed
   * sits behind the same gate as the injection it serves).
   */
  modifiers(): ModifiersSnapshot {
    return input.modifiers();
  },
};

/**
 * Mouse family: `move`/`click`/`drag` compile to ordered `input.mouse` steps
 * (one SendInput batch, self-injected like the keyboard path), `getPos` is
 * the `windows.input.read` cursor/window read.
 *
 * Deviations from AHK, all deliberate and pinned by tests: a partial point
 * (only one of x/y) throws TypeError instead of silently returning; X1/X2
 * and wheel buttons are refused; the title-bar click-down workaround
 * (`keyboard_mouse.cpp:2193-2291`) is not replicated; `speed` is validated
 * but ignored at injection (AHK SendInput does the same).
 */
export const mouse = {
  /** AHK `MouseMove X, Y, Speed`: absolute move in one step. */
  move(x: number, y: number, options?: MouseMoveOptions): Promise<{ sent: number }> {
    requireCoordinate(x, "mouse.move x");
    requireCoordinate(y, "mouse.move y");
    const speed = validateSpeed(options?.speed, "mouse.move speed");
    const payload: MousePayload = { steps: [{ action: "move", x, y }] };
    if (speed !== undefined) payload.speed = speed;
    return runMouse(payload);
  },
  /**
   * AHK `MouseClick WhichButton, X, Y, Count, Speed`: optional move first,
   * then `count` down/up pairs. `count < 1` does nothing, not even a move.
   */
  click(options?: MouseClickOptions): Promise<{ sent: number }> {
    const button = validateButton(options?.button, "mouse.click");
    const speed = validateSpeed(options?.speed, "mouse.click speed");
    const count = validateCount(options?.count, "mouse.click count");
    validateOptionalPoint(options?.x, options?.y, "mouse.click");
    if (count < 1) return Promise.resolve({ sent: 0 });
    const steps: MouseStep[] = [];
    if (options?.x !== undefined && options?.y !== undefined) {
      steps.push({ action: "move", x: options.x, y: options.y });
    }
    for (let index = 0; index < count; ++index) {
      steps.push({ action: "down", button }, { action: "up", button });
    }
    const payload: MousePayload = { steps };
    if (speed !== undefined) payload.speed = speed;
    return runMouse(payload);
  },
  /**
   * AHK `MouseClickDrag WhichButton, X1, Y1, X2, Y2, Speed`: optional move to
   * the start, button down, move to `to`, button up — always as separate
   * events inside one batch (AHK `keyboard_mouse.cpp:2082-2106`).
   */
  drag(options: MouseDragOptions): Promise<{ sent: number }> {
    if (options === null || typeof options !== "object") {
      throw new TypeError("mouse.drag(options): options must be an object with to { x, y }");
    }
    const button = validateButton(options.button, "mouse.drag");
    const speed = validateSpeed(options.speed, "mouse.drag speed");
    validateOptionalPoint(options.x, options.y, "mouse.drag");
    if (options.to === null || typeof options.to !== "object") {
      throw new TypeError("mouse.drag: to { x, y } is required");
    }
    const toX = requireCoordinate(options.to.x, "mouse.drag to.x");
    const toY = requireCoordinate(options.to.y, "mouse.drag to.y");
    const steps: MouseStep[] = [];
    if (options.x !== undefined && options.y !== undefined) {
      steps.push({ action: "move", x: options.x, y: options.y });
    }
    steps.push({ action: "down", button });
    steps.push({ action: "move", x: toX, y: toY });
    steps.push({ action: "up", button });
    const payload: MousePayload = { steps };
    if (speed !== undefined) payload.speed = speed;
    return runMouse(payload);
  },
  /**
   * AHK `MouseGetPos`: cursor position plus the window and child control
   * under it. Rejects with `capability_denied` naming `windows.input.read`.
   * AHK's OutputVarX/Y/Win/Control and flags 0x01/0x02 (simple mode) are not
   * exposed; the full snapshot and control id are returned instead.
   */
  getPos(options?: ActionOptions): Promise<MouseGetPosResult> {
    return runAction(options, (native) => input.mouseGetPos(native));
  },
};

export { input };
