import { runAction } from "../action";
import { compileSend, type SendKeyStep, type SendMode } from "../send";
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
 * Send-string family: compiles the AHK Send grammar (`sdk/src/send`)
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
