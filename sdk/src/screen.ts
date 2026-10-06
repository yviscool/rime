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

/** Result of a pixel search: a miss carries no coordinate on purpose. */
export type ScreenPixelSearchResult =
  | { found: false }
  | { found: true; x: number; y: number };

/** Options of `screen.pixelSearch`. */
export interface ScreenPixelSearchOptions extends ActionOptions {
  /** Per-channel slack 0..255 (default 0 = exact match). */
  variation?: number;
}

/** Bridge of the `rime:screen` module. Every call is a read. */
export interface ScreenBridge {
  monitorCount(options?: NativeActionOptions): Promise<{ count: number }>;
  monitor(options?: NativeActionOptions): Promise<ScreenMonitor>;
  monitor(index: number, options?: NativeActionOptions): Promise<ScreenMonitor>;
  pixel(x: number, y: number, options?: NativeActionOptions): Promise<{ color: number }>;
  pixelSearch(
    area: ScreenRect,
    color: number,
    options?: NativeActionOptions,
  ): Promise<ScreenPixelSearchResult>;
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
  /**
   * One screen pixel as `0xRRGGBB` (AHK `PixelGetColor`, with AHK's `Mode`
   * argument dropped - the runtime has one color format). Coordinates are
   * virtual-desktop pixels and may be negative on multi-monitor layouts.
   * @throws ActionError with `invalid_contract` (outside the virtual screen),
   *   `capability_denied`, `cancelled`.
   */
  pixel(x: number, y: number, options?: ActionOptions): Promise<number> {
    return runAction(options, (bridgeOptions) =>
      screenBridge().then((bridge) => bridge.pixel(x, y, bridgeOptions).then((r) => r.color)),
    );
  },
  /**
   * First pixel matching `color` inside `area` (AHK `PixelSearch`), scanning
   * the top row first and left to right. A miss resolves `{found:false}` -
   * it is a result, not an error.
   *
   * @param area rectangle in virtual-desktop pixels; reversed corners are
   *   accepted and normalized.
   * @param color `0xRRGGBB`.
   * @throws ActionError with `invalid_contract` (area misses the screen,
   *   bad variation/color range), `capability_denied`, `cancelled`.
   */
  pixelSearch(
    area: ScreenRect,
    color: number,
    options?: ScreenPixelSearchOptions,
  ): Promise<ScreenPixelSearchResult> {
    return runAction(options, (bridgeOptions) =>
      screenBridge().then((bridge) => bridge.pixelSearch(area, color, bridgeOptions)),
    );
  },
};
