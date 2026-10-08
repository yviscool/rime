import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

/** Dialog button sets, named (wire: numeric sets 0..6). */
export type MsgBoxButtons =
  | "ok"
  | "ok-cancel"
  | "abort-retry-ignore"
  | "yes-no-cancel"
  | "yes-no"
  | "retry-cancel"
  | "cancel-try-continue";

const MSG_BOX_BUTTONS: Record<MsgBoxButtons, number> = {
  ok: 0,
  "ok-cancel": 1,
  "abort-retry-ignore": 2,
  "yes-no-cancel": 3,
  "yes-no": 4,
  "retry-cancel": 5,
  "cancel-try-continue": 6,
};

/** Dialog icons, named (wire: numeric icon flags). */
export type MsgBoxIcon = "none" | "error" | "question" | "warning" | "info";

const MSG_BOX_ICONS: Record<MsgBoxIcon, number> = {
  none: 0,
  error: 0x10,
  question: 0x20,
  warning: 0x30,
  info: 0x40,
};

/** Balloon icons, named (wire: NIIF dwInfoFlags). */
export type TrayTipIcon = "none" | "error" | "warning" | "info";

const TRAY_TIP_ICONS: Record<TrayTipIcon, number> = {
  none: 0,
  error: 0x10,
  warning: 0x20,
  info: 0x40,
};

/** Options of {@link ui.msgBox}: dialog pieces plus the shared action options. */
export interface UiMsgBoxOptions extends ActionOptions {
  /** Dialog title (empty when omitted). */
  title?: string;
  /** Button set; unknown names reject synchronously. */
  buttons?: MsgBoxButtons;
  /** Icon; unknown names reject synchronously. */
  icon?: MsgBoxIcon;
  /** 0-based default button. */
  defaultIndex?: number;
  /**
   * Auto-close in seconds: the dialog closes itself with `"Timeout"`
   * after this long. Omitted means no timeout - a message box waits for the
   * user. Negatives clamp to 0.1s, huge values saturate, and `deadlineMs`
   * only bounds the wait for the UI pump, never an open dialog.
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

/** Options of {@link ui.inputBox}: dialog pieces plus the shared action options. */
export interface UiInputBoxOptions extends ActionOptions {
  /** Dialog title. */
  title?: string;
  /** Pre-filled edit contents. */
  value?: string;
  /** Mask the edit (password field). */
  password?: boolean;
  /** Client width in pixels; omitted uses the measured prompt size. */
  width?: number;
  /** Client height in pixels; omitted uses the measured prompt size. */
  height?: number;
  /** Screen x; omitted (with y) centers the dialog on the cursor's monitor. */
  x?: number;
  /** Screen y; omitted (with x) centers the dialog on the cursor's monitor. */
  y?: number;
  /** Auto-close in seconds; same clamp and bounds as {@link UiMsgBoxOptions.timeout}. */
  timeout?: number;
}

/** Options of {@link ui.toolTip}: tooltip slot plus coords. */
export interface UiToolTipOptions extends ActionOptions {
  /** Screen x; omitted defaults to cursor + 16. */
  x?: number;
  /** Screen y; omitted defaults to cursor + 16. */
  y?: number;
  /** Tooltip slot 0..19 (20 slots); out-of-range values reject synchronously. */
  index?: number;
}

/** Options of {@link ui.traySetIcon}: icon selection plus freeze flag. */
export interface UiTraySetIconOptions extends ActionOptions {
  /** 0-based icon number inside a multi-icon resource. */
  iconNumber?: number;
  /**
   * Records the frozen flag. Presence is the "given" signal, so
   * `freeze: false` is a deliberate unfreeze rather than an omission.
   */
  freeze?: boolean;
}

/** Options of {@link ui.trayTip}: balloon title, icon and mute flag. */
export interface UiTrayTipOptions extends ActionOptions {
  /** Balloon title; with empty text this shows a title-only balloon. */
  title?: string;
  /** Balloon icon; unknown names reject synchronously. */
  icon?: TrayTipIcon;
  /** Maps to NIIF_NOSOUND: show the balloon silently. */
  mute?: boolean;
}

/**
 * Bridge of the `rime:ui` module. Every call goes straight to the
 * `GuiService`, so none of them builds an Action or lands in the trace;
 * `ui.create` is read natively inside each worker body. The bridge speaks
 * wire numbering (1-based slots, numeric button/icon sets); the `ui`
 * facade below translates the modern 0-based/named forms, so application
 * code never sees the wire shapes.
 */
export interface UiBridge {
  msgBox(
    text: string,
    options?: {
      title?: string;
      buttons?: number;
      icon?: number;
      defaultIndex?: number;
      timeout?: number;
    } & NativeActionOptions,
  ): Promise<string>;
  inputBox(
    prompt?: string,
    options?: NativeActionOptions & {
      title?: string;
      value?: string;
      password?: boolean;
      width?: number;
      height?: number;
      x?: number;
      y?: number;
      timeout?: number;
    },
  ): Promise<InputBoxOutcome>;
  toolTip(
    text: string,
    options?: { x?: number; y?: number; index?: number } & NativeActionOptions,
  ): Promise<null>;
  traySetIcon(
    file: string,
    options?: { iconNumber?: number; freeze?: boolean } & NativeActionOptions,
  ): Promise<null>;
  trayTip(
    text: string,
    options?: { title?: string; icon?: number; mute?: boolean } & NativeActionOptions,
  ): Promise<null>;
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
 * GUI dialogs, tooltips and tray verbs. All five are async Promise calls -
 * a blocking "wait for the user" is `await` here - gated on the
 * `ui.create` capability, and none of them builds an Action
 * (the native trace stays empty; the capability read is the audit surface).
 * Every HWND stays on the UI Thread: an open modal is closed by its own
 * `timeout`, while `deadlineMs`/`signal` only bound the wait
 * for the pump and the pre-window phase.
 */
export const ui = {
  /**
   * Shows a modal message box and resolves to the pressed button's word
   * (`"OK"`, `"Yes"`, `"No"`, `"Cancel"`, `"Abort"`, `"Retry"`, `"Ignore"`,
   * `"TryAgain"`, `"Continue"`) or `"Timeout"`. The X/ESC rules are the OS's
   * own: OK-only dialogs close on X, a Cancel-bearing dialog closes on
   * X/ESC with Cancel, and the rest swallow both.
   *
   * Capability `ui.create`; no Action Trace (a direct service call).
   * @throws ActionError with `timeout` (queued past `deadlineMs`),
   *   `cancelled`, `capability_denied`, `invalid_contract` (button set
   *   outside 0..6), `invalid_state` (no UI pump attached).
   */
  msgBox(text: string, options?: UiMsgBoxOptions): Promise<string> {
    const { title, buttons, icon, defaultIndex, timeout, ...action } = options ?? {};
    if (buttons !== undefined && !(buttons in MSG_BOX_BUTTONS)) {
      throw new TypeError(`ui.msgBox: unknown buttons set ${JSON.stringify(buttons)}`);
    }
    if (icon !== undefined && !(icon in MSG_BOX_ICONS)) {
      throw new TypeError(`ui.msgBox: unknown icon ${JSON.stringify(icon)}`);
    }
    if (defaultIndex !== undefined && (!Number.isInteger(defaultIndex) || defaultIndex < 0)) {
      throw new TypeError(`ui.msgBox: defaultIndex must be a 0-based integer >= 0, got ${String(defaultIndex)}`);
    }
    return runAction(action, (native) =>
      uiBridge().then((bridge) =>
        bridge.msgBox(text, {
          ...definedOnly({
            title,
            buttons: buttons === undefined ? undefined : MSG_BOX_BUTTONS[buttons],
            icon: icon === undefined ? undefined : MSG_BOX_ICONS[icon],
            defaultIndex: defaultIndex === undefined ? undefined : defaultIndex + 1,
            timeout,
          }),
          ...native,
        }),
      ),
    );
  },
  /**
   * Shows a modal input dialog and resolves to `{value, result}`. `value`
   * carries the edit contents even when the dialog was cancelled or timed
   * out, and `result` is `"OK"`, `"Cancel"` or `"Timeout"`.
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
   * Shows (or with empty text, destroys) a tooltip near the cursor or at
   * `x`/`y`, clamped inside the monitor work area. `index` selects one of
   * 20 tooltip slots (0-based). Resolves to nothing - the HWND is a raw
   * handle this runtime never exposes.
   *
   * Capability `ui.create`; no Action Trace.
   * @throws ActionError with `invalid_contract` (index outside 0..19) /
   *   `timeout` / `cancelled` / `capability_denied`.
   */
  toolTip(text: string, options?: UiToolTipOptions): Promise<void> {
    const { x, y, index, ...action } = options ?? {};
    if (index !== undefined && (!Number.isInteger(index) || index < 0 || index > 19)) {
      throw new TypeError(`ui.toolTip: index must be a 0-based slot 0..19, got ${String(index)}`);
    }
    return runAction(action, (native) =>
      uiBridge().then((bridge) =>
        bridge.toolTip(text, {
          ...definedOnly({ x, y, index: index === undefined ? undefined : index + 1 }),
          ...native,
        }),
      ),
    ).then(() => undefined);
  },
  /**
   * Sets the process tray icon, adding the icon first if none exists.
   * `""` or `"*"` restores the standard icon. Resolves to nothing.
   *
   * Capability `ui.create`; no Action Trace.
   * @throws ActionError with `execution_failed` (the file cannot be loaded,
   *   the tray icon stays as it was) / `timeout` / `cancelled` /
   *   `capability_denied`.
   */
  traySetIcon(file: string, options?: UiTraySetIconOptions): Promise<void> {
    const { iconNumber, freeze, ...action } = options ?? {};
    if (iconNumber !== undefined && (!Number.isInteger(iconNumber) || iconNumber < 0)) {
      throw new TypeError(`ui.traySetIcon: iconNumber must be a 0-based integer >= 0, got ${String(iconNumber)}`);
    }
    return runAction(action, (native) =>
      uiBridge().then((bridge) =>
        bridge.traySetIcon(file, {
          ...definedOnly({ iconNumber: iconNumber === undefined ? undefined : iconNumber + 1, freeze }),
          ...native,
        }),
      ),
    ).then(() => undefined);
  },
  /**
   * Shows a balloon on the tray icon. Empty `text` with a `title` shows a
   * title-only balloon; both empty removes the notification. Resolves to
   * nothing.
   *
   * Capability `ui.create`; no Action Trace.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  trayTip(text: string, options?: UiTrayTipOptions): Promise<void> {
    const { title, icon, mute, ...action } = options ?? {};
    if (icon !== undefined && !(icon in TRAY_TIP_ICONS)) {
      throw new TypeError(`ui.trayTip: unknown icon ${JSON.stringify(icon)}`);
    }
    return runAction(action, (native) =>
      uiBridge().then((bridge) =>
        bridge.trayTip(text, {
          ...definedOnly({ title, icon: icon === undefined ? undefined : TRAY_TIP_ICONS[icon], mute }),
          ...native,
        }),
      ),
    ).then(() => undefined);
  },
};
