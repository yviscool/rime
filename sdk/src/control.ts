import type { ActionOptions, NativeActionOptions } from "./action";
import { runAction } from "./action";

/** Stable control id from `resolve` (same id space as Window.controls). */
export type ControlId = number;

/** How `resolve` names a control. Exactly one form per call. */
export type ControlSpec =
  | { classNN: string }
  | { text: string }
  | { hwnd: number }
  | { point: { x: number; y: number; space?: "window-client" | "screen" } }
  | { auto: string };

/** One resolved control: stable id plus the matched identity. */
export interface ControlSnapshot {
  id: ControlId;
  className: string;
  classNN: string;
}

/** Screen-space rect from `rect()`. */
export interface ControlRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** Click shape. `pos` defaults to the control center in control-client
 * coordinates; `activate: false` is the default (AHK NA mode). */
export interface ControlClickOptions {
  button?: "left" | "right" | "middle" | "x1" | "x2";
  count?: number;
  phase?: "downUp" | "down" | "up";
  activate?: boolean;
  pos?: { x: number; y: number };
  settleMs?: number;
}

/** Strategy for text paths. `auto` tries the message first (AHK semantics). */
export type ControlTextStrategy = "message" | "uia" | "auto";

/** Bridge of the `rime:control` module (Win32 control verbs). */
export interface ControlBridge {
  /** Resolves a spec to a control. Rejects `target_gone`/`invalid_contract`. */
  resolve(
    windowId: number,
    spec: ControlSpec,
    options?: NativeActionOptions,
  ): Promise<ControlSnapshot>;
  /** Posts click message(s); resolves `{ clicks }`. */
  click(
    id: ControlId,
    opts?: ControlClickOptions,
    options?: NativeActionOptions,
  ): Promise<{ clicks: number }>;
  /** AttachThreadInput + SetFocus. Never reports focus failure. */
  focus(id: ControlId, options?: NativeActionOptions): Promise<{ focused: true }>;
  /** WM_SETTEXT round trip. */
  setText(id: ControlId, text: string, options?: NativeActionOptions): Promise<{ text: string }>;
  /** WM_GETTEXTLENGTH + WM_GETTEXT round trip. */
  getText(id: ControlId, options?: NativeActionOptions): Promise<{ text: string }>;
  /** RAW_TEXT keystrokes (WM_CHAR, no parsing). */
  sendText(id: ControlId, text: string, options?: NativeActionOptions): Promise<{ text: string }>;
  /** Single Win32 reads. Synchronous (no Action, no trace). */
  isVisible(id: ControlId): boolean;
  /** Single Win32 reads. Synchronous (no Action, no trace). */
  isEnabled(id: ControlId): boolean;
  /** Screen-space rect. Synchronous (no Action, no trace). */
  rect(id: ControlId): ControlRect;
  /** Liveness probe. True while the id resolves; ids are never recycled. */
  dispose(id: ControlId): boolean;
  /** Adds an item; resolves the 1-based index. */
  listAdd(id: ControlId, text: string, options?: NativeActionOptions): Promise<{ index: number }>;
  /** Deletes the 1-based index. */
  listDelete(id: ControlId, index: number, options?: NativeActionOptions): Promise<{ deleted: true }>;
  /** Selects by index (0 clears) or text; notifies the parent by default. */
  listChoose(
    id: ControlId,
    sel: { index: number } | { text: string },
    notifyParent?: boolean,
    options?: NativeActionOptions,
  ): Promise<{ chosen: true }>;
  /** Exact-match find; 0 means no match (a result, not an error). */
  listFind(id: ControlId, text: string, options?: NativeActionOptions): Promise<{ index: number }>;
  /** Current selection, 1-based, 0 when nothing is selected. */
  listIndex(id: ControlId, options?: NativeActionOptions): Promise<{ index: number }>;
  /** Text of an item (omitted index reads the current one). */
  listChoice(
    id: ControlId,
    index?: number,
    options?: NativeActionOptions,
  ): Promise<{ text: string }>;
  /** All items up to `limit` (default 100, max 10000). */
  listItems(
    id: ControlId,
    limit?: number,
    options?: NativeActionOptions,
  ): Promise<{ items: string[] }>;
  /** Selects a tab page (1-based). */
  tabSelect(id: ControlId, index: number, options?: NativeActionOptions): Promise<{ selected: true }>;
  /** Edit line count. */
  editCount(id: ControlId, options?: NativeActionOptions): Promise<{ lines: number }>;
  /** Caret position, 1-based. */
  editCaret(id: ControlId, options?: NativeActionOptions): Promise<{ line: number; col: number }>;
  /** Line text, 1-based. */
  editLine(id: ControlId, line: number, options?: NativeActionOptions): Promise<{ text: string }>;
  /** Currently selected text (empty when nothing is selected). */
  editSelected(id: ControlId, options?: NativeActionOptions): Promise<{ text: string }>;
  /** EM_REPLACESEL paste. */
  editPaste(id: ControlId, text: string, options?: NativeActionOptions): Promise<{ text: string }>;
  /** Checkbox set; -1 toggles. */
  setChecked(
    id: ControlId,
    checked: boolean | -1 | 0 | 1,
    ensureActive?: boolean,
    options?: NativeActionOptions,
  ): Promise<{ checked: boolean }>;
  /** Checkbox state. */
  isChecked(id: ControlId, options?: NativeActionOptions): Promise<{ checked: boolean }>;
  /** Show without activating (SW_SHOWNOACTIVATE). */
  show(id: ControlId, options?: NativeActionOptions): Promise<{ visible: true }>;
  /** Hide. */
  hide(id: ControlId, options?: NativeActionOptions): Promise<{ visible: false }>;
  /** Move/resize in top-level-client coordinates; omitted fields keep values. */
  move(
    id: ControlId,
    rect: { x?: number; y?: number; w?: number; h?: number },
    options?: NativeActionOptions,
  ): Promise<{ moved: true }>;
  /** Enable/disable (verified, reports failure). */
  setEnabled(id: ControlId, enabled: boolean, options?: NativeActionOptions): Promise<{ enabled: boolean }>;
  /** Current tab page, 1-based. */
  tabIndex(id: ControlId, options?: NativeActionOptions): Promise<{ index: number }>;
  /** Shows a ComboBox drop-down. */
  showDropdown(id: ControlId, options?: NativeActionOptions): Promise<{ dropped: true }>;
  /** Hides a ComboBox drop-down. */
  hideDropdown(id: ControlId, options?: NativeActionOptions): Promise<{ dropped: false }>;
  /** Style bits with AHK +-=^ prefix op. */
  setStyle(
    id: ControlId,
    spec: { op: "+" | "-" | "^" | "="; bits: number },
    options?: NativeActionOptions,
  ): Promise<{ styled: true }>;
  /** Ex-style bits with AHK +-=^ prefix op. */
  setExStyle(
    id: ControlId,
    spec: { op: "+" | "-" | "^" | "="; bits: number },
    options?: NativeActionOptions,
  ): Promise<{ styled: true }>;
  /** Parsed keystrokes (documented Send subset). Resolves the step count. */
  send(id: ControlId, keys: string, options?: NativeActionOptions): Promise<{ steps: number }>;
  /** ClassNN string (agrees with Window.controls by construction). Sync. */
  classNN(id: ControlId): string;
  /** Raw GWL_STYLE bits. Sync. */
  getStyle(id: ControlId): number;
  /** Raw GWL_EXSTYLE bits. Sync. */
  getExStyle(id: ControlId): number;
  /** Focused child of a window, or 0. Sync. */
  focusedChild(windowId: number): number;
}

async function controlBridge(): Promise<ControlBridge> {
  // Lazy on purpose: a static `export ... from "rime:control"` would make
  // every importer of this module (e.g. window.ts, pulled by unit tests)
  // resolve the native specifier at import time. The bridge object itself
  // stays reachable through the dynamic import below.
  const module = (await import("rime:control")) as unknown as {
    control: ControlBridge;
  };
  // Every async path flows through here, so the sync-getter cache is primed
  // as a side effect: any awaited verb makes later sync getters work.
  primeControlBridge(module.control);
  return module.control;
}

/**
 * A resolved control handle. Async verbs run through the Action pipeline
 * (capability `windows.automation.control`, cancellable); sync queries
 * read straight through (capability `windows.window.read`).
 */
export class Control {
  constructor(
    readonly windowId: number,
    readonly snapshot: ControlSnapshot,
    private readonly strategy: ControlTextStrategy = "auto",
  ) {}

  /** Resolve a spec against a window. */
  static async resolve(
    windowId: number,
    spec: ControlSpec,
    options?: ActionOptions & { strategy?: ControlTextStrategy },
  ): Promise<Control> {
    const { strategy, ...rest } = options ?? {};
    const snapshot = await runAction(rest, (native) =>
      controlBridge().then((bridge) => bridge.resolve(windowId, spec, native)),
    );
    return new Control(windowId, snapshot, strategy ?? "auto");
  }

  get id(): ControlId {
    return this.snapshot.id;
  }

  /** Posts click message(s); resolves `{ clicks }`. */
  click(opts?: ControlClickOptions, options?: ActionOptions): Promise<{ clicks: number }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.click(id, opts, native)),
    );
  }

  /** AttachThreadInput + SetFocus. */
  focus(options?: ActionOptions): Promise<{ focused: true }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.focus(id, native)),
    );
  }

  /** WM_SETTEXT round trip. */
  setText(text: string, options?: ActionOptions): Promise<{ text: string }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.setText(id, text, native)),
    );
  }

  /** WM_GETTEXTLENGTH + WM_GETTEXT round trip. */
  getText(options?: ActionOptions): Promise<{ text: string }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.getText(id, native)),
    );
  }

  /** RAW_TEXT keystrokes (WM_CHAR, no parsing). */
  sendText(text: string, options?: ActionOptions): Promise<{ text: string }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.sendText(id, text, native)),
    );
  }

  /** Adds an item; resolves the 1-based index. */
  listAdd(text: string, options?: ActionOptions): Promise<{ index: number }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.listAdd(id, text, native)),
    );
  }

  /** Deletes the 1-based index. */
  listDelete(index: number, options?: ActionOptions): Promise<{ deleted: true }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.listDelete(id, index, native)),
    );
  }

  /** Selects by index (0 clears) or text; notifies the parent by default. */
  listChoose(
    sel: { index: number } | { text: string },
    notifyParent?: boolean,
    options?: ActionOptions,
  ): Promise<{ chosen: true }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.listChoose(id, sel, notifyParent, native)),
    );
  }

  /** Exact-match find; 0 means no match (a result, not an error). */
  listFind(text: string, options?: ActionOptions): Promise<{ index: number }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.listFind(id, text, native)),
    );
  }

  /** Current selection, 1-based, 0 when nothing is selected. */
  listIndex(options?: ActionOptions): Promise<{ index: number }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.listIndex(id, native)),
    );
  }

  /** Text of an item (omitted index reads the current one). */
  listChoice(index?: number, options?: ActionOptions): Promise<{ text: string }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.listChoice(id, index, native)),
    );
  }

  /** All items up to `limit` (default 100, max 10000). */
  listItems(limit?: number, options?: ActionOptions): Promise<{ items: string[] }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.listItems(id, limit, native)),
    );
  }

  /** Selects a tab page (1-based). */
  tabSelect(index: number, options?: ActionOptions): Promise<{ selected: true }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.tabSelect(id, index, native)),
    );
  }

  /** Edit line count. */
  editCount(options?: ActionOptions): Promise<{ lines: number }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.editCount(id, native)),
    );
  }

  /** Caret position, 1-based. */
  editCaret(options?: ActionOptions): Promise<{ line: number; col: number }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.editCaret(id, native)),
    );
  }

  /** Line text, 1-based. */
  editLine(line: number, options?: ActionOptions): Promise<{ text: string }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.editLine(id, line, native)),
    );
  }

  /** Currently selected text (empty when nothing is selected). */
  editSelected(options?: ActionOptions): Promise<{ text: string }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.editSelected(id, native)),
    );
  }

  /** EM_REPLACESEL paste. */
  editPaste(text: string, options?: ActionOptions): Promise<{ text: string }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.editPaste(id, text, native)),
    );
  }

  /** Checkbox set; -1 toggles. */
  setChecked(
    checked: boolean | -1 | 0 | 1,
    ensureActive?: boolean,
    options?: ActionOptions,
  ): Promise<{ checked: boolean }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.setChecked(id, checked, ensureActive, native)),
    );
  }

  /** Checkbox state. */
  isChecked(options?: ActionOptions): Promise<{ checked: boolean }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.isChecked(id, native)),
    );
  }

  /** Show without activating (SW_SHOWNOACTIVATE). */
  show(options?: ActionOptions): Promise<{ visible: true }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.show(id, native)),
    );
  }

  /** Hide. */
  hide(options?: ActionOptions): Promise<{ visible: false }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.hide(id, native)),
    );
  }

  /** Move/resize in top-level-client coordinates; omitted fields keep values. */
  move(
    rect: { x?: number; y?: number; w?: number; h?: number },
    options?: ActionOptions,
  ): Promise<{ moved: true }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.move(id, rect, native)),
    );
  }

  /** Enable/disable (verified, reports failure). */
  setEnabled(enabled: boolean, options?: ActionOptions): Promise<{ enabled: boolean }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.setEnabled(id, enabled, native)),
    );
  }

  /** Current tab page, 1-based. */
  tabIndex(options?: ActionOptions): Promise<{ index: number }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.tabIndex(id, native)),
    );
  }

  /** Shows a ComboBox drop-down. */
  showDropdown(options?: ActionOptions): Promise<{ dropped: true }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.showDropdown(id, native)),
    );
  }

  /** Hides a ComboBox drop-down. */
  hideDropdown(options?: ActionOptions): Promise<{ dropped: false }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.hideDropdown(id, native)),
    );
  }

  /** Style bits with AHK +-=^ prefix op. */
  setStyle(
    spec: { op: "+" | "-" | "^" | "="; bits: number },
    options?: ActionOptions,
  ): Promise<{ styled: true }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.setStyle(id, spec, native)),
    );
  }

  /** Ex-style bits with AHK +-=^ prefix op. */
  setExStyle(
    spec: { op: "+" | "-" | "^" | "="; bits: number },
    options?: ActionOptions,
  ): Promise<{ styled: true }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.setExStyle(id, spec, native)),
    );
  }

  /** Parsed keystrokes (documented Send subset). Resolves the step count. */
  send(keys: string, options?: ActionOptions): Promise<{ steps: number }> {
    const id = this.id;
    return runAction(options, (native) =>
      controlBridge().then((bridge) => bridge.send(id, keys, native)),
    );
  }

  /** ClassNN string (agrees with Window.controls by construction). Sync. */
  get nn(): string {
    return controlBridgeSync().classNN(this.id);
  }

  /** Raw GWL_STYLE bits. Sync. */
  get styleBits(): number {
    return controlBridgeSync().getStyle(this.id);
  }

  /** Raw GWL_EXSTYLE bits. Sync. */
  get exStyleBits(): number {
    return controlBridgeSync().getExStyle(this.id);
  }

  /** IsWindowVisible. Synchronous. */
  get visible(): boolean {
    return controlBridgeSync().isVisible(this.id);
  }

  /** IsWindowEnabled. Synchronous. Sync getters cannot await the bridge,
   * so control handles cache it on first use (see controlBridgeSync). */
  get enabled(): boolean {
    return controlBridgeSync().isEnabled(this.id);
  }

  /** Screen-space rect. Synchronous. */
  get bounds(): ControlRect {
    return controlBridgeSync().rect(this.id);
  }

  /** Liveness probe. Synchronous. */
  dispose(): boolean {
    return controlBridgeSync().dispose(this.id);
  }
}

// Sync getters need the bridge without awaiting. The native module object
// is cached on the first resolve(); every sync getter therefore requires a
// prior await (resolve or any async verb, which primes through the same path
// below). A getter before any await throws instead of returning stale data.
let cachedBridge: ControlBridge | null = null;
function controlBridgeSync(): ControlBridge {
  if (cachedBridge) return cachedBridge;
  throw new Error("control bridge not ready: await Control.resolve() first");
}

function primeControlBridge(bridge: ControlBridge): void {
  cachedBridge = bridge;
}

/** Focused child of a window, or 0. Synchronous shared helper for Window. */
export function focusedControl(windowId: number): number {
  return controlBridgeSync().focusedChild(windowId);
}

