import { input } from "rime:input";

/** Payload handed to `InputHook.OnChar`. */
export interface InputHookCharEvent {
  /** The collected character (a single BMP code unit). */
  char: string;
  vk: number;
  scan: number;
  down: boolean;
}

/** Payload handed to `InputHook.OnKeyDown` / `InputHook.OnKeyUp`. */
export interface InputHookKeyEvent {
  vk: number;
  scan: number;
  down: boolean;
}

/** EndReason values an `InputHook` reports (AHK `EndReason`). */
export type InputHookEndReason = "" | "Timeout" | "Match" | "EndKey" | "Max" | "Stopped";

/** Payload handed to `InputHook.OnEnd`. */
export interface InputHookEndEvent {
  reason: InputHookEndReason;
  /** The collected buffer at the moment the input ended. */
  input: string;
  /** Lowercase key name that ended the input (`EndKey`), empty otherwise. */
  endKey: string;
  /** Left-side modifier glyphs held at the end: `<^`, `<!`, `<+`, `<#`. */
  endMods: string;
  /** The MatchList phrase that matched, empty unless reason is `Match`. */
  match: string;
}

/**
 * One runtime-owned input capture (AHK `InputHook`). `new InputHook(...)`
 * delegates to the native `createInputHook` and returns that object — the
 * interface below supplies the member types, the native object supplies the
 * 23 enumerable members of `objects.json`'s InputHook table.
 *
 * Construction validates first (`options` / `endKeys` / `matchList`) and
 * needs no capability; only `Start()` gates on `windows.hook.global`.
 */
export class InputHook {
  constructor(options?: string, endKeys?: string, matchList?: string) {
    return input.createInputHook(options, endKeys, matchList) as unknown as InputHook;
  }
}

export interface InputHook {
  /** Key-level options (`+A-SEnv`-style): letters `+ - E I N S V Z`, `{All}`. */
  KeyOpt(keys: string, keyOptions: string): void;
  /** Starts (or restarts) capture; idempotent — an in-progress input just clears its buffer. */
  Start(): void;
  /** Ends an active capture with EndReason `"Stopped"`; no-op when idle. */
  Stop(): void;
  /**
   * Resolves with the EndReason once the input ends. Resolves immediately
   * with the last reason when idle; `maxTime` (seconds, negative clamps to
   * 0) also ends a running input with `"Timeout"` — capture keeps running
   * past a `Wait` deadline of its own.
   */
  Wait(maxTime?: number): Promise<InputHookEndReason>;
  /** Backspace undoes the previous character (default on). */
  BackspaceIsUndo: boolean;
  /** MatchList comparison honors case (default off). */
  CaseSensitive: boolean;
  /** Matches may appear anywhere in the buffer, not just at its start. */
  FindAnywhere: boolean;
  /** OnKeyDown / OnKeyUp fire for non-text keys too. */
  NotifyNonText: boolean;
  /** Mirrors non-text keys into the buffer (show them as `{U+...}` etc.). */
  VisibleNonText: boolean;
  /** Mirrors text keys into the buffer. */
  VisibleText: boolean;
  /** Key name that ended the input, lowercase chord tokens; "" otherwise. */
  readonly EndKey: string;
  /** Left-side modifier glyphs held at the end (`<^ <! <+ <#`, control/alt/shift/super). */
  readonly EndMods: string;
  /** `"Timeout" | "Match" | "EndKey" | "Max" | "Stopped" | ""`. */
  readonly EndReason: InputHookEndReason;
  /** Whether capture is running. */
  readonly InProgress: boolean;
  /** The collected buffer (snapshot). */
  readonly Input: string;
  /** The MatchList phrase that matched; "" unless reason is `"Match"`. */
  readonly Match: string;
  /** Send level that determines whether generated input is visible here (negative clamps to 0). */
  MinSendLevel: number;
  /** Capture timeout in seconds; re-arms only while capture is running. */
  Timeout: number;
  /** Fires for each collected character — never for the terminating one. */
  OnChar: ((event: InputHookCharEvent) => void) | undefined | null;
  /** Fires once when capture ends; never followed by further OnChar/OnKeyUp. */
  OnEnd: ((event: InputHookEndEvent) => void) | undefined | null;
  OnKeyDown: ((event: InputHookKeyEvent) => void) | undefined | null;
  OnKeyUp: ((event: InputHookKeyEvent) => void) | undefined | null;
}
