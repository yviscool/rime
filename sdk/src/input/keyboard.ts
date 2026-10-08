import { runAction } from "../action";
import {
  CHAR_KEYS,
  NAMED_KEYS,
  VK_CONTROL,
  VK_LWIN,
  VK_MENU,
  VK_SHIFT,
  compileSend,
  type SendKeyStep,
  type SendMode,
} from "../send";
import type { KeyboardSendOptions, LockKeyName, LockStateWord, ModifiersSnapshot } from "./types";
import { modifierMaskOf } from "./helpers";
import { input } from "rime:input";

/**
 * Cross-call persistent modifier mask (AHK `sModifiersLR_persistent`): bits a
 * Send string held down with `{Ctrl down}`-style items stay down across
 * calls until released with `{Ctrl up}`.
 */
let persistentModifiers = 0;

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

/**
 * One structured key chord: a key name plus explicit modifiers. Key names
 * are the `NAMED_KEYS` vocabulary (`"enter"`, `"f2"`, `"esc"`, …,
 * case-insensitive) or a single character (`"c"`, `"?"`; anything outside
 * the US table injects as Unicode). This is the primary input surface;
 * the Send string DSL (`send()`) is a legacy migration frontend over the
 * same batch path.
 */
export interface KeyChord {
  key: string;
  ctrl?: boolean;
  alt?: boolean;
  shift?: boolean;
  win?: boolean;
}

function keyToVk(key: string, fn: string): { vk: number; shift: boolean; unicode: boolean } {
  if (typeof key !== "string" || key.length === 0) {
    throw new TypeError(`${fn}: key must be a non-empty string, got ${String(key)}`);
  }
  const named = NAMED_KEYS[key.toLowerCase()];
  if (named !== undefined) return { vk: named, shift: false, unicode: false };
  if (key.length === 1) {
    const entry = CHAR_KEYS[key];
    if (entry !== undefined) return { vk: entry[0], shift: entry[1], unicode: false };
    return { vk: key.charCodeAt(0), shift: false, unicode: true };
  }
  throw new TypeError(`${fn}: unknown key ${JSON.stringify(key)}`);
}

function tapSteps(
  vk: number,
  unicode: boolean,
  autoShift: boolean,
  mods: { ctrl?: boolean; alt?: boolean; shift?: boolean; win?: boolean },
): SendKeyStep[] {
  const held: number[] = [];
  if (mods.ctrl) held.push(VK_CONTROL);
  if (mods.alt) held.push(VK_MENU);
  if (mods.shift || autoShift) held.push(VK_SHIFT);
  if (mods.win) held.push(VK_LWIN);
  const steps: SendKeyStep[] = held.map((code) => ({ vk: code, down: true }));
  const key = unicode ? { vk, down: true, unicode: true } : { vk, down: true };
  const release = unicode ? { vk, down: false, unicode: true } : { vk, down: false };
  steps.push(key, release);
  for (let i = held.length - 1; i >= 0; i--) steps.push({ vk: held[i] as number, down: false });
  return steps;
}

function runSteps(steps: SendKeyStep[], options?: Omit<KeyboardSendOptions, "mode">): Promise<{ sent: number }> {
  return runAction(options, async (native) => {
    if (steps.length === 0) return { sent: 0 };
    return input.send(steps, native);
  });
}
/**
 * Key injection family. `press()` (structured chords) is the primary
 * surface; the `send*()` string forms compile the legacy Send DSL
 * (`sdk/src/send`) against the live modifier state and dispatch through the
 * same batch path. Grammar and mode errors are synchronous TypeErrors;
 * Action failures reject (ActionError with `timeout` / `cancelled` /
 * `capability_denied` / `invalid_state`).
 */
export const keyboard = {
  /**
   * Presses structured key chords: each item taps down+up with its
   * modifiers held around it (`{ key: "c", ctrl: true }`). Modifiers from
   * outside stay as they are; every modifier this call holds is released
   * before it resolves. Unknown key names and empty input are synchronous
   * TypeErrors; an empty list resolves `{ sent: 0 }` without touching the
   * device.
   */
  press(
    keys: string | KeyChord | Array<string | KeyChord>,
    options?: Omit<KeyboardSendOptions, "mode">,
  ): Promise<{ sent: number }> {
    const items = Array.isArray(keys) ? keys : [keys];
    const steps: SendKeyStep[] = [];
    for (const item of items) {
      const chord: KeyChord = typeof item === "string" ? { key: item } : item;
      if (chord === null || typeof chord !== "object") {
        throw new TypeError(`keyboard.press: chord must be a key name or object, got ${String(item)}`);
      }
      const resolved = keyToVk(chord.key, "keyboard.press");
      steps.push(...tapSteps(resolved.vk, resolved.unicode, resolved.shift, chord));
    }
    // Live capability read first, like runSend: a missing
    // `windows.input.inject` throws synchronously here.
    modifierMaskOf(input.modifiers());
    return runSteps(steps, options);
  },
  /** Legacy Send string (`^c`, `{Enter}`): a migration frontend over `press()` semantics. Prefer `press()`. */
  send(keys: string, options?: KeyboardSendOptions): Promise<{ sent: number }> {
    return runSend(keys, options);
  },
  /** Legacy `SendInput` string form (this stage always injects via SendInput). Prefer `press()`. */
  sendInput(keys: string, options?: Omit<KeyboardSendOptions, "mode">): Promise<{ sent: number }> {
    return runSend(keys, { ...options, mode: "input" });
  },
  /** Legacy `SendEvent` string form (event-mode delivery is approximated). Prefer `press()`. */
  sendEvent(keys: string, options?: Omit<KeyboardSendOptions, "mode">): Promise<{ sent: number }> {
    return runSend(keys, { ...options, mode: "event" });
  },
  /** Legacy `SendPlay` string form (play-mode delivery is approximated). Prefer `press()`. */
  sendPlay(keys: string, options?: Omit<KeyboardSendOptions, "mode">): Promise<{ sent: number }> {
    return runSend(keys, { ...options, mode: "play" });
  },
  /** Legacy `SendText` string form (layout-independent Unicode packets). Prefer `press()`. */
  sendText(keys: string, options?: Omit<KeyboardSendOptions, "mode">): Promise<{ sent: number }> {
    return runSend(keys, { ...options, mode: "text" });
  },
  /**
   * Sets a lock key's toggle state (AHK `SetCapsLockState`/`SetNumLockState`/
   * `SetScrollLockState`): `on`/`off` clear any Always* force first and
   * inject a self-tagged key tap when the LED differs (released first while
   * the key is held, `keyboard_mouse.cpp:2976-2990`); `alwaysOn`/`alwaysOff`
   * arm the hook force BEFORE the tap (AHK's order, `script2.cpp:1784-1794`)
   * so foreign presses can no longer move the toggle; `""` or an omitted
   * state only clears the force and never taps. Resolves `{ changed }` —
   * `true` when a tap was injected; the toggle settles asynchronously, so
   * read it back with `getKeyState(key, "t")`.
   *
   * Deliberate deviations from AHK: no Shift tap fallback for the "CapsLock
   * turns off only with Shift" OS setting (`keyboard_mouse.cpp:2997-3007`)
   * and no post-tap message-pump sleep (`keyboard_mouse.cpp:2984`) — callers
   * poll the settled state instead.
   *
   * Argument errors are synchronous TypeErrors, and the capability checks
   * (`windows.input.read` for the LED read, `windows.input.inject` for the
   * tap, `windows.hook.global` when arming Always*) throw synchronously
   * before anything is armed or injected.
   */
  setLockState(keyName: LockKeyName, state?: LockStateWord): Promise<{ changed: boolean }> {
    if (typeof keyName !== "string") {
      throw new TypeError(
        `keyboard.setLockState(keyName): keyName must be a string, got ${String(keyName)}`,
      );
    }
    const key = keyName.toLowerCase();
    if (key !== "capslock" && key !== "numlock" && key !== "scrolllock") {
      throw new TypeError(
        `keyboard.setLockState(keyName): keyName must be capslock, numlock or scrolllock, got ${keyName}`,
      );
    }
    if (state !== undefined && state !== null && typeof state !== "string") {
      throw new TypeError(
        `keyboard.setLockState(state): state must be a string, got ${String(state)}`,
      );
    }
    const word = (state ?? "").toLowerCase();
    if (word !== "" && word !== "on" && word !== "off" && word !== "alwayson" && word !== "alwaysoff") {
      throw new TypeError(
        `keyboard.setLockState(state): state must be "on"|"off"|"alwaysOn"|"alwaysOff", got ${String(state)}`,
      );
    }
    // Force first, always: On/Off override a prior Always* (AHK's rule) and
    // Always* must be armed before the tap it may inject. "neutral" (or the
    // omitted/"" form) is an ungated release.
    if (word === "alwayson") input.setLockForce(key, "on");
    else if (word === "alwaysoff") input.setLockForce(key, "off");
    else input.setLockForce(key, "neutral");
    const want =
      word === "on" || word === "alwayson" ? true : word === "off" || word === "alwaysoff" ? false : null;
    if (want === null) return Promise.resolve({ changed: false });
    if (input.getKeyState(key, "t") === want) return Promise.resolve({ changed: false });
    const vk = input.getKeyVK(key);
    const steps: SendKeyStep[] = [];
    // A tap cannot flip the LED while the key is held down unless it is
    // released first (AHK's KEYUP-then-DOWNANDUP, keyboard_mouse.cpp:2976).
    if (input.getKeyState(key, "l")) steps.push({ vk, down: false });
    steps.push({ vk, down: true }, { vk, down: false });
    return input.send(steps).then(() => ({ changed: true }));
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
