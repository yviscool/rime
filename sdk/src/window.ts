import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

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
  id: number;
  title: string;
  /** Win32 class name, e.g. "Static" or "CabinetWClass". */
  className: string;
  /** Process image basename, e.g. "notepad.exe". */
  processName: string;
  rect: WindowRect;
  visible: boolean;
  minimized: boolean;
  state: WindowState;
  processId: number;
}

/** WinTitle-style selector fields shared by reads (window-v1 query). */
export interface WindowQueryFields {
  title?: string;
  matchMode?: "exact" | "contains";
  ahkClass?: string;
  ahkExe?: string;
  ahkId?: number | string;
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
  info(windowId: number, options?: NativeActionOptions): Promise<WindowHandle>;
  /** `target` is a window id or the string "active". */
  move(
    target: number | "active",
    position: WindowPlacement,
    options?: NativeActionOptions,
  ): Promise<WindowHandle>;
  focus(target: number | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  close(target: number | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  hide(target: number | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  show(target: number | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  minimize(target: number | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  maximize(target: number | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  restore(target: number | "active", options?: NativeActionOptions): Promise<WindowHandle>;
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
  pick: (bridge: WindowsBridge) => (target: number | "active", options?: NativeActionOptions) => Promise<WindowHandle>,
): (target: number, options?: ActionOptions) => Promise<WindowHandle> {
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
  move(windowId: number, position: WindowPlacement, options?: ActionOptions): Promise<WindowHandle> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.move(windowId, position, native)),
    );
  },
  focus: mutation((windows) => windows.focus),
  close: mutation((windows) => windows.close),
  hide: mutation((windows) => windows.hide),
  show: mutation((windows) => windows.show),
  minimize: mutation((windows) => windows.minimize),
  maximize: mutation((windows) => windows.maximize),
  restore: mutation((windows) => windows.restore),
  list(query?: WindowQueryFields & ActionOptions): Promise<WindowHandle[]> {
    // `signal` never reaches the wire; it is converted to a cancellation id.
    const { signal: _signal, ...fields } = query ?? {};
    return runAction(query, (native) =>
      windowBridge().then((windows) => windows.list({ ...fields, ...native })),
    );
  },
  info(windowId: number, options?: ActionOptions): Promise<WindowHandle> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.info(windowId, native)),
    );
  },
};
