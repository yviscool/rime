import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

/** AHK's MsgBox button sets (Group #1 options, script2.cpp:1008-1016). */
export type MsgBoxButtons =
  | 0 // OK
  | 1 // OKCancel
  | 2 // AbortRetryIgnore
  | 3 // YesNoCancel
  | 4 // YesNo
  | 5 // RetryCancel
  | 6; // CancelTryAgainContinue

/** AHK's MsgBox icon options (Iconx / Icon? / Icon! / Iconi, script2.cpp:968-978). */
export type MsgBoxIcon = 0 | 0x10 | 0x20 | 0x30 | 0x40;

/** AHK's TrayTip dwInfoFlags (script2.cpp:31-77). */
export type TrayTipIcon = 0 | 0x10 | 0x20 | 0x40;

/** Options of {@link ui.msgBox}: the shared action options plus AHK's Options pieces. */
export interface UiMsgBoxOptions extends ActionOptions {
  /** Dialog title (AHK's Title argument; empty when omitted). */
  title?: string;
  /** Button set 0..6 (see {@link MsgBoxButtons}); out-of-range values reject natively. */
  buttons?: MsgBoxButtons;
  /** Icon flag 0x10/0x20/0x30/0x40, or 0 for none. */
  icon?: MsgBoxIcon;
  /** 1-based default button (AHK's Default1..Default4). */
  defaultIndex?: number;
  /**
   * AHK's T option in seconds: the dialog closes itself with `"Timeout"`
   * after this long. Omitted means no timeout - a message box waits for the
   * user exactly as AHK does. Negatives clamp to 0.1s, huge values saturate
   * (window.cpp:1063-1066), and `deadlineMs` only bounds the wait for the UI
   * pump, never an open dialog.
   */
  timeout?: number;
}

/** Outcome of {@link ui.inputBox} (InputBox.cpp:148-173). */
export interface InputBoxOutcome {
  /** The edit contents at close - present even on Cancel/Timeout. */
  value: string;
  /** `"OK"`, `"Cancel"` or `"Timeout"`. */
  result: "OK" | "Cancel" | "Timeout";
}

/** Options of {@link ui.inputBox}: the shared action options plus AHK's InputBox pieces. */
export interface UiInputBoxOptions extends ActionOptions {
  /** Dialog title (AHK's Title argument). */
  title?: string;
  /** Pre-filled edit contents (AHK's Value argument). */
  value?: string;
  /** Mask the edit with AHK's `*` character (InputBox.cpp:47-48). */
  password?: boolean;
  /** Client width in pixels; omitted uses the measured prompt size. */
  width?: number;
  /** Client height in pixels; omitted uses the measured prompt size. */
  height?: number;
  /** Screen x; omitted (with y) centers the dialog on the cursor's monitor. */
  x?: number;
  /** Screen y; omitted (with x) centers the dialog on the cursor's monitor. */
  y?: number;
  /** AHK's T option in seconds; same clamp and bounds as {@link UiMsgBoxOptions.timeout}. */
  timeout?: number;
}

/** Options of {@link ui.toolTip}: the shared action options plus AHK's WhichTip/coords. */
export interface UiToolTipOptions extends ActionOptions {
  /** Screen x; omitted defaults to cursor + 16 (script2.cpp:1115-1121). */
  x?: number;
  /** Screen y; omitted defaults to cursor + 16. */
  y?: number;
  /** Tooltip slot 1..20 (AHK's WhichTip); out-of-range values reject natively. */
  index?: number;
}

/** Options of {@link ui.traySetIcon}: the shared action options plus AHK's icon pieces. */
export interface UiTraySetIconOptions extends ActionOptions {
  /** 1-based icon number inside a multi-icon resource; 0 means 1 (script.cpp:956-957). */
  iconNumber?: number;
  /**
   * Records AHK's frozen flag. Presence is the "given" signal, so
   * `freeze: false` is a deliberate unfreeze rather than an omission.
   */
  freeze?: boolean;
}

/** Options of {@link ui.trayTip}: the shared action options plus AHK's balloon pieces. */
export interface UiTrayTipOptions extends ActionOptions {
  /** Balloon title; with empty text this shows a title-only balloon (script2.cpp:124-126). */
  title?: string;
  /** dwInfoFlags: 0x10 error, 0x20 warning, 0x40 info (default none). */
  icon?: TrayTipIcon;
  /** Maps to NIIF_NOSOUND: show the balloon silently. */
  mute?: boolean;
}

/**
 * Bridge of the `rime:ui` module. Every call goes straight to the
 * `GuiService`, so none of them builds an Action or lands in the trace;
 * `ui.create` is read natively inside each worker body. Option objects carry
 * the feature keys and the native action keys in one shape, which is what
 * the native parsers read.
 */
export interface UiBridge {
  msgBox(text: string, options?: UiMsgBoxOptions & NativeActionOptions): Promise<string>;
  inputBox(
    prompt?: string,
    options?: UiInputBoxOptions & NativeActionOptions,
  ): Promise<InputBoxOutcome>;
  toolTip(text: string, options?: UiToolTipOptions & NativeActionOptions): Promise<null>;
  traySetIcon(file: string, options?: UiTraySetIconOptions & NativeActionOptions): Promise<null>;
  trayTip(text: string, options?: UiTrayTipOptions & NativeActionOptions): Promise<null>;
}

async function uiBridge(): Promise<UiBridge> {
  const module = await import("rime:ui");
  return module.ui;
}

/**
 * Keeps the keys whose value is not `undefined`, so an option nobody passed
 * stays an absent key rather than an explicit `undefined` - the same rule
 * `sound.play` follows for `wait` - and the native side always sees one
 * shape.
 */
function definedOnly<T extends object>(value: T): Partial<T> {
  const out: Partial<T> = {};
  for (const key of Object.keys(value) as (keyof T)[]) {
    if (value[key] !== undefined) out[key] = value[key];
  }
  return out;
}

/**
 * GUI dialogs, tooltips and tray verbs of the M6 batch-1 family (AHK
 * `MsgBox`, `InputBox`, `ToolTip`, `TraySetIcon`, `TrayTip`). All five are
 * async Promise calls - AHK's blocking "wait for the user" is `await` here -
 * gated on the `ui.create` capability, and none of them builds an Action
 * (the native trace stays empty; the capability read is the audit surface).
 * Every HWND stays on the UI Thread: an open modal is closed by its own
 * `timeout` (the T option), while `deadlineMs`/`signal` only bound the wait
 * for the pump and the pre-window phase.
 */
export const ui = {
  /**
   * AHK `MsgBox(text, options?)`: shows a modal message box and resolves to
   * the pressed button's word (`"OK"`, `"Yes"`, `"No"`, `"Cancel"`,
   * `"Abort"`, `"Retry"`, `"Ignore"`, `"TryAgain"`, `"Continue"`) or
   * `"Timeout"`. The X/ESC rules are the OS's own: OK-only dialogs close on
   * X, a Cancel-bearing dialog closes on X/ESC with Cancel, and the rest
   * swallow both (docs MsgBox).
   *
   * Capability `ui.create`; no Action Trace (a direct service call).
   * @throws ActionError with `timeout` (queued past `deadlineMs`),
   *   `cancelled`, `capability_denied`, `invalid_contract` (button set
   *   outside 0..6), `invalid_state` (no UI pump attached).
   */
  msgBox(text: string, options?: UiMsgBoxOptions): Promise<string> {
    const { title, buttons, icon, defaultIndex, timeout, ...action } = options ?? {};
    return runAction(action, (native) =>
      uiBridge().then((bridge) =>
        bridge.msgBox(text, {
          ...definedOnly({ title, buttons, icon, defaultIndex, timeout }),
          ...native,
        }),
      ),
    );
  },
  /**
   * AHK `InputBox(prompt?, options?)`: shows a modal input dialog and
   * resolves to `{value, result}`. `value` carries the edit contents even
   * when the dialog was cancelled or timed out - AHK hands the text back on
   * every path (InputBox.cpp:148-173) - and `result` is `"OK"`, `"Cancel"`
   * or `"Timeout"`.
   *
   * Capability `ui.create`; no Action Trace.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  inputBox(prompt?: string, options?: UiInputBoxOptions): Promise<InputBoxOutcome> {
    const { title, value, password, width, height, x, y, timeout, ...action } = options ?? {};
    return runAction(action, (native) =>
      uiBridge().then((bridge) =>
        bridge.inputBox(prompt, {
          ...definedOnly({ title, value, password, width, height, x, y, timeout }),
          ...native,
        }),
      ),
    );
  },
  /**
   * AHK `ToolTip(text, options?)`: shows (or with empty text, destroys) a
   * tooltip near the cursor or at `x`/`y`, clamped inside the monitor work
   * area like AHK clamps it. `index` selects one of AHK's 20 tooltip slots.
   * Resolves to nothing - AHK's HWND return is a raw handle this runtime
   * never exposes (gui-menu.md §0.4).
   *
   * Capability `ui.create`; no Action Trace.
   * @throws ActionError with `invalid_contract` (index outside 1..20) /
   *   `timeout` / `cancelled` / `capability_denied`.
   */
  toolTip(text: string, options?: UiToolTipOptions): Promise<void> {
    const { x, y, index, ...action } = options ?? {};
    return runAction(action, (native) =>
      uiBridge().then((bridge) =>
        bridge.toolTip(text, { ...definedOnly({ x, y, index }), ...native }),
      ),
    ).then(() => undefined);
  },
  /**
   * AHK `TraySetIcon(file, options?)`: sets the process tray icon, adding
   * the icon first if none exists. `""` or `"*"` restores the standard
   * icon; `iconNumber` 0 means 1. Resolves to nothing.
   *
   * Capability `ui.create`; no Action Trace.
   * @throws ActionError with `execution_failed` (the file cannot be loaded,
   *   the tray icon stays as it was) / `timeout` / `cancelled` /
   *   `capability_denied`.
   */
  traySetIcon(file: string, options?: UiTraySetIconOptions): Promise<void> {
    const { iconNumber, freeze, ...action } = options ?? {};
    return runAction(action, (native) =>
      uiBridge().then((bridge) =>
        bridge.traySetIcon(file, { ...definedOnly({ iconNumber, freeze }), ...native }),
      ),
    ).then(() => undefined);
  },
  /**
   * AHK `TrayTip(text, options?)`: shows a balloon on the tray icon. Empty
   * `text` with a `title` shows a title-only balloon; both empty removes the
   * notification. Like AHK this does not fail once the tray icon exists -
   * only the pump/capability path can reject. Resolves to nothing.
   *
   * Capability `ui.create`; no Action Trace.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  trayTip(text: string, options?: UiTrayTipOptions): Promise<void> {
    const { title, icon, mute, ...action } = options ?? {};
    return runAction(action, (native) =>
      uiBridge().then((bridge) =>
        bridge.trayTip(text, { ...definedOnly({ title, icon, mute }), ...native }),
      ),
    ).then(() => undefined);
  },
};
