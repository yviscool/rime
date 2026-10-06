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

