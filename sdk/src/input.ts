import type { NativeActionOptions } from "./action";

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

/** One `send()` step: a key transition. `vk` is 1..254. */
export interface SendKeyStep {
  vk: number;
  down: boolean;
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
   * Throws TypeError for malformed steps; throws Error naming
   * `windows.input.inject` when the capability is missing; rejects with
   * `invalid_state` when the input service is not running.
   */
  send(
    steps: readonly SendKeyStep[],
    options?: NativeActionOptions,
  ): Promise<{ sent: number }>;
}

export { input } from "rime:input";
