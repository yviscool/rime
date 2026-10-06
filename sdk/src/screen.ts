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

/** Result of `screen.caret`: a desktop without a caret carries no coordinate. */
export type ScreenCaretResult =
  | { found: false }
  | { found: true; x: number; y: number };

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
  imageSearch(
    area: ScreenRect,
    imagePath: string,
    options?: NativeActionOptions,
  ): Promise<ScreenPixelSearchResult>;
  caret(options?: NativeActionOptions): Promise<ScreenCaretResult>;
  sysGet(index: number, options?: NativeActionOptions): Promise<{ metric: number }>;
  sysGetIPAddresses(options?: NativeActionOptions): Promise<{ addresses: string[] }>;
}

// runAction's bridge options carry only action concerns (deadline, parent,
// cancellation) and are rebuilt from scratch for every call, so a domain
// option like `variation` has to be merged back in at the call site - letting
// it ride along on ActionOptions would silently drop it.
function withVariation(
  bridgeOptions: NativeActionOptions | undefined,
  variation: number | undefined,
): (NativeActionOptions & { variation?: number }) | undefined {
  if (variation === undefined) return bridgeOptions;
  return { ...(bridgeOptions ?? {}), variation };
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
      screenBridge().then((bridge) =>
        bridge.pixelSearch(area, color, withVariation(bridgeOptions, options?.variation)),
      ),
    );
  },
  /**
   * First place in `area` where the image file fits (AHK `ImageSearch`),
   * scanning like {@link screen.pixelSearch}. A miss resolves
   * `{found:false}`. The file is decoded before the screen is read, so a bad
   * path fails without touching the desktop.
   *
   * @param area rectangle in virtual-desktop pixels; reversed corners are
   *   accepted and normalized.
   * @param imagePath UTF-8 path to an image Windows can decode (BMP, PNG,
   *   JPEG, GIF, TIFF, ...). Alpha is not part of the comparison: only the
   *   RGB channels are matched, exactly like {@link screen.pixelSearch}.
   * @throws ActionError with `invalid_contract` (area misses the screen,
   *   undecodable file, bad variation range), `capability_denied`,
   *   `cancelled`.
   */
  imageSearch(
    area: ScreenRect,
    imagePath: string,
    options?: ScreenPixelSearchOptions,
  ): Promise<ScreenPixelSearchResult> {
    return runAction(options, (bridgeOptions) =>
      screenBridge().then((bridge) =>
        bridge.imageSearch(area, imagePath, withVariation(bridgeOptions, options?.variation)),
      ),
    );
  },
  /**
   * Caret (text insertion point) position in screen pixels (AHK
   * `CaretGetPos`). Only the foreground window's caret can be read - a
   * caret belongs to the focused control - so a desktop without one
   * resolves `{found:false}`; that is a result, not an error.
   *
   * @throws ActionError with `capability_denied`, `cancelled`.
   */
  caret(options?: ActionOptions): Promise<ScreenCaretResult> {
    return runAction(options, (bridgeOptions) =>
      screenBridge().then((bridge) => bridge.caret(bridgeOptions)),
    );
  },
  /**
   * One system metric by index (AHK `SysGet(Index)`), straight from
   * `GetSystemMetrics`. Windows answers `0` for an index it does not know and
   * passes that through, so `0` may mean either "zero" or "no such metric" -
   * exactly the ambiguity AHK scripts live with, and a reason not to invent
   * an error code for it.
   *
   * @param index Win32 system metric index (`SM_CXSCREEN` and friends).
   * @throws ActionError with `invalid_contract` (index outside int32),
   *   `capability_denied`, `cancelled`.
   */
  sysGet(index: number, options?: ActionOptions): Promise<number> {
    return runAction(options, (bridgeOptions) =>
      screenBridge().then((bridge) =>
        bridge.sysGet(index, bridgeOptions).then((r) => r.metric),
      ),
    );
  },
  /**
   * This machine's IPv4 addresses as dotted quads (AHK `SysGetIPAddresses`),
   * loopback included, in adapter order. Reads the adapter list rather than
   * resolving the host name, so no process-wide Winsock initialisation
   * happens; an empty array is a result, not an error.
   *
   * @throws ActionError with `capability_denied`, `cancelled`.
   */
  sysGetIPAddresses(options?: ActionOptions): Promise<string[]> {
    return runAction(options, (bridgeOptions) =>
      screenBridge().then((bridge) =>
        bridge.sysGetIPAddresses(bridgeOptions).then((r) => r.addresses),
      ),
    );
  },
};
