export type WindowPlacement = "left" | "right" | "top" | "bottom" | "full";

export interface WindowRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

export interface WindowHandle {
  id: number;
  title: string;
  rect: WindowRect;
  visible: boolean;
  minimized: boolean;
  processId: number;
}

/** Bridge of the `rime:window` module. Every mutation is an Action. */
export interface WindowsBridge {
  list(): Promise<WindowHandle[]>;
  /** Resolves null when no window is foreground. */
  active(): Promise<WindowHandle | null>;
  info(windowId: number): Promise<WindowHandle>;
  /** `target` is a window id or the string "active". */
  move(target: number | "active", position: WindowPlacement): Promise<WindowHandle>;
}

async function windowBridge(): Promise<WindowsBridge> {
  const module = await import("rime:window");
  return module.windows;
}

export interface ActiveWindowRequest {
  /** Moves the foreground window through the `window.move` action pipeline. */
  move(position: WindowPlacement): Promise<WindowHandle>;
}

export const Window = {
  /** Lazy handle on the foreground window; resolves when the operation runs. */
  active(): ActiveWindowRequest {
    return {
      move: (position: WindowPlacement) =>
        windowBridge().then((windows) => windows.move("active", position)),
    };
  },
  move(windowId: number, position: WindowPlacement): Promise<WindowHandle> {
    return windowBridge().then((windows) => windows.move(windowId, position));
  },
  list(): Promise<WindowHandle[]> {
    return windowBridge().then((windows) => windows.list());
  },
  info(windowId: number): Promise<WindowHandle> {
    return windowBridge().then((windows) => windows.info(windowId));
  },
};
