import { runAction, type ActionOptions, type NativeActionOptions } from "./action";
import type { ProcessId } from "./process";

export type Brand<K, T> = K & { readonly __brand: T };

/** Stable window id issued by the UI-lane registry (never a raw HWND). */
export type WindowId = Brand<number, "WindowId">;

export type WindowPlacement = "left" | "right" | "top" | "bottom" | "full";

export type WindowState = "normal" | "minimized" | "maximized" | "hidden";

export interface WindowRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

/** Snapshot returned by every window read and mutation (window-v1 snapshot). */
export interface WindowHandle {
  id: WindowId;
  title: string;
  /** Win32 class name, e.g. "Static" or "CabinetWClass". */
  className: string;
  /** Process image basename, e.g. "notepad.exe". */
  processName: string;
  rect: WindowRect;
  visible: boolean;
  minimized: boolean;
  state: WindowState;
  processId: ProcessId;
}

/** WinTitle-style selector fields shared by reads (window-v1 query). */
export interface WindowQueryFields {
  /** Case-insensitive substring (`contains`) or exact (`exact`) title match. */
  title?: string;
  matchMode?: "exact" | "contains";
  /** Case-insensitive Win32 class-name match. */
  ahkClass?: string;
  /** Case-insensitive match against the process image basename only. */
  ahkExe?: string;
  ahkId?: WindowId | string;
  /** When true, hidden windows are included — still-unheaded (title-less) windows stay filtered. */
  includeHidden?: boolean;
  /** Selects the foreground window (same as `title: "A"`). */
  active?: boolean;
}

/** Options accepted by `windows.list` on the wire. */
export type WindowsListOptions = WindowQueryFields & NativeActionOptions;

/** Bridge of the `rime:window` module. Every mutation is an Action. */
export interface WindowsBridge {
  list(options?: WindowsListOptions): Promise<WindowHandle[]>;
  /** Resolves null when no window is foreground. */
  active(options?: NativeActionOptions): Promise<WindowHandle | null>;
  info(windowId: WindowId, options?: NativeActionOptions): Promise<WindowHandle>;
  /** `target` is a window id or the string "active". */
  move(
    target: WindowId | "active",
    position: WindowPlacement,
    options?: NativeActionOptions,
  ): Promise<WindowHandle>;
  focus(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  close(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  hide(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  show(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  minimize(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  maximize(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  restore(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
}

async function windowBridge(): Promise<WindowsBridge> {
  const module = await import("rime:window");
  return module.windows;
}

export interface ActiveWindowRequest {
  /** Moves the foreground window through the `window.move` action pipeline. */
  move(position: WindowPlacement, options?: ActionOptions): Promise<WindowHandle>;
  focus(options?: ActionOptions): Promise<WindowHandle>;
  close(options?: ActionOptions): Promise<WindowHandle>;
  hide(options?: ActionOptions): Promise<WindowHandle>;
  show(options?: ActionOptions): Promise<WindowHandle>;
  minimize(options?: ActionOptions): Promise<WindowHandle>;
  maximize(options?: ActionOptions): Promise<WindowHandle>;
  restore(options?: ActionOptions): Promise<WindowHandle>;
}

function mutation(
  pick: (bridge: WindowsBridge) => (target: WindowId | "active", options?: NativeActionOptions) => Promise<WindowHandle>,
): (target: WindowId, options?: ActionOptions) => Promise<WindowHandle> {
  return (target, options) =>
    runAction(options, (native) => windowBridge().then((windows) => pick(windows)(target, native)));
}

export const Window = {
  /** Lazy handle on the foreground window; resolves when the operation runs. */
  active(): ActiveWindowRequest {
    return {
      move: (position, options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.move("active", position, native)),
        ),
      focus: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.focus("active", native)),
        ),
      close: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.close("active", native)),
        ),
      hide: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.hide("active", native)),
        ),
      show: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.show("active", native)),
        ),
      minimize: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.minimize("active", native)),
        ),
      maximize: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.maximize("active", native)),
        ),
      restore: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.restore("active", native)),
        ),
    };
  },
  /**
   * Moves a window through the `window.move` action pipeline.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`.
   */
  move(windowId: WindowId, position: WindowPlacement, options?: ActionOptions): Promise<WindowHandle> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.move(windowId, position, native)),
    );
  },
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  focus: mutation((windows) => windows.focus),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  close: mutation((windows) => windows.close),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  hide: mutation((windows) => windows.hide),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  show: mutation((windows) => windows.show),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  minimize: mutation((windows) => windows.minimize),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  maximize: mutation((windows) => windows.maximize),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  restore: mutation((windows) => windows.restore),
  /**
   * Lists windows matching the query (window-v1 query; `schemaVersion` stays
   * in the vocabulary and never goes on the wire).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  list(query?: WindowQueryFields & ActionOptions): Promise<WindowHandle[]> {
    // `signal` never reaches the wire; it is converted to a cancellation id.
    const { signal: _signal, ...fields } = query ?? {};
    return runAction(query, (native) =>
      windowBridge().then((windows) => windows.list({ ...fields, ...native })),
    );
  },
  /**
   * Reads one window snapshot by id.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`.
   */
  info(windowId: WindowId, options?: ActionOptions): Promise<WindowHandle> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.info(windowId, native)),
    );
  },
};
