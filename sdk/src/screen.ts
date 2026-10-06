import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

/** Screen-space rectangle in virtual-desktop pixels (right/bottom exclusive, as Win32 reports). */
export interface ScreenRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

/** One display, as AHK's Monitor* family reports it. */
export interface ScreenMonitor {
  /** 1-based position in `EnumDisplayMonitors` order. */
  index: number;
  /** True for the system's primary monitor. */
  primary: boolean;
  /** Device name, e.g. `\\.\DISPLAY1`. */
  name: string;
  /** The whole monitor, including anything covered by the taskbar. */
  bounds: ScreenRect;
  /** The monitor minus the taskbar and other auto-hiding app bars. */
  work: ScreenRect;
}

/** Bridge of the `rime:screen` module. Every call is a read. */
export interface ScreenBridge {
  monitorCount(options?: NativeActionOptions): Promise<{ count: number }>;
  monitor(options?: NativeActionOptions): Promise<ScreenMonitor>;
  monitor(index: number, options?: NativeActionOptions): Promise<ScreenMonitor>;
}

async function screenBridge(): Promise<ScreenBridge> {
  const module = await import("rime:screen");
  return module.screen;
}

export const screen = {
  /**
   * Number of connected monitors, counted the way AHK counts them
   * (`EnumDisplayMonitors`, not `SM_CMONITORS`). Capability `screen.capture`;
   * a read, so it produces no Action Trace.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  monitorCount(options?: ActionOptions): Promise<number> {
    return runAction(options, (bridgeOptions) =>
      screenBridge().then((bridge) => bridge.monitorCount(bridgeOptions).then((r) => r.count)),
    );
  },
  /**
   * One monitor: bounds, work area, device name and whether it is primary.
   * An omitted `index` means the primary monitor, which is how AHK's
   * `MonitorGet` / `MonitorGetWorkArea` / `MonitorGetName` /
   * `MonitorGetPrimary` all behave.
   *
   * @param index 1-based monitor number; 0 or omitted selects the primary.
   * @throws ActionError with `invalid_contract` (no such monitor),
   *   `capability_denied`, `timeout` or `cancelled`.
   */
  monitor(index?: number, options?: ActionOptions): Promise<ScreenMonitor> {
    return runAction(options, (bridgeOptions) =>
      screenBridge().then((bridge) =>
        index === undefined
          ? bridge.monitor(bridgeOptions)
          : bridge.monitor(index, bridgeOptions),
      ),
    );
  },
};
