import type { WindowSnapshot } from "../window";

/** One `listHotkeys()` row: a registered hotkey's state snapshot. */
export interface HotkeyListRow {
  /** The registered chord name, exactly as passed to `hotkey()`. */
  name: string;
  /** False while the registration is toggled off (the `"off"` control word). */
  enabled: boolean;
  /** Registration-time `#InputLevel`; events below it never match. */
  inputLevel: number;
  /** Callbacks currently in flight for this registration (AHK's Running column). */
  running: number;
  /** `#SuspendExempt`: keeps firing while `input.suspend()` is on. */
  suspendExempt: boolean;
  /** True when the registration sits under a `hotIf*` criterion. */
  conditional: boolean;
}

/** Report from `listHotkeys`: the suspend state plus rows in registration (first-match) order. */
export interface HotkeyListReport {
  suspended: boolean;
  hotkeys: HotkeyListRow[];
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

/** Control word accepted by the event registrations: `on`, `off`, `toggle`. */
export type EventControlWord = "on" | "off" | "toggle";

/**
 * Action bound to a `hotkey()`: an observer function, an action template
 * queued through the dispatcher like `bind()`, or a control word that
 * enables/disables an existing registration.
 */
export type HotkeyAction = ((event: HotkeyEvent) => void) | ChordActionTemplate | EventControlWord;

/**
 * Action bound to a `hotstring()`: an observer function (the trigger is not
 * erased and nothing is injected), an action template, a control word, or
 * omitted — the omission form injects the replacement text itself.
 */
export type HotstringAction = ((event: {}) => void) | ChordActionTemplate | EventControlWord;

/** Payload handed to a `hotkey()` observer. */
export interface HotkeyEvent {
  /** The registered chord name, exactly as passed to `hotkey()`. */
  name: string;
}

/**
 * Payload handed to an `onClipboardChange()` listener. `type: 1` marks the
 * change this process wrote, `type: 0` a foreign one (AHK's A_EventInfo).
 */
export interface ClipboardChangeEvent {
  type: 0 | 1;
}

/** Payload handed to an `onError()` observer: where it was recorded and its text. */
export interface ErrorEvent {
  /** e.g. `promise`, `job`, `invoke_callback`, `onExit`, `rime:input.setTimer`. */
  where: string;
  message: string;
}

/** Payload handed to an `onExit()` handler while the runtime shuts down. */
export interface ExitEvent {
  reason: string;
}

/** Payload handed to an `onMessage()` monitor for the message it registered. */
export interface MessageEvent {
  msg: number;
  wParam: number;
  lParam: number;
  hwnd: number;
}

/**
 * The criterion a `hotIf*` call returned: `kind` names the criterion that
 * was active before the call (`none` = no criterion, `gone` = the criterion
 * was already released), `id` identifies it for `hotIf(null)`-style restores.
 */
export interface HotIfDescriptor {
  kind: "none" | "function" | "winActive" | "winExist" | "winNotActive" | "winNotExist" | "gone";
  id: number;
}

/**
 * A `hotIf(fn)` predicate: evaluated on the JS thread with the window
 * snapshot already resolved by the watcher, never on the UI lane. Returning
 * a non-boolean or throwing fails closed (the registration does not fire)
 * and records an error through `onError`.
 */
export type HotIfCondition = (event: { active: WindowSnapshot | null; seq: number }) => boolean;

/** One event registration. `close()` is idempotent and reports whether it was still open. */
export interface EventSubscription {
  readonly id: number;
  readonly kind: "hotkey" | "hotstring" | "timer" | "message" | "clipboard" | "error" | "exit";
  close(): boolean;
}

/**
 * Registration options accepted by `hotkey()` and `hotstring()` besides the
 * plain `"on"|"off"|"toggle"` control word (M2-D): `on` toggles the
 * registration, `suspendExempt` keeps it firing while `input.suspend()` is
 * on (`#SuspendExempt`), `inputLevel` sets the registration's `#InputLevel`
 * (events below it never match; defaults to `input.policy().inputLevel`).
 */
export interface RegistrationOptions {
  on?: boolean;
  suspendExempt?: boolean;
  inputLevel?: number;
}

/** The central dispatch policy (`input.policy()`), M2-D directive knobs. */
export interface DispatchPolicy {
  /** `#MaxThreads`: deliveries in flight across the scheduler; 0 = unlimited. */
  maxConcurrency: number;
  /** `#MaxThreadsPerHotkey`: in-flight deliveries per registration; 0 = unlimited. */
  maxConcurrencyPerHotkey: number;
  /** `#InputLevel`: level new registrations start at. */
  inputLevel: number;
  /** `#HotIfTimeout`: budget in ms for one HotIf evaluation; 0 = none. */
  hotIfTimeout: number;
  /** Queue overflow under load (`#MaxThreadsBuffer` behavior). */
  overflow: "reject" | "dropOldest" | "coalesce";
}
