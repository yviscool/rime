import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

/** Screen-space rectangle in virtual-desktop pixels (right/bottom exclusive, as Win32 reports). */
export interface ScreenRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

/** One display in `EnumDisplayMonitors` order. */
export interface ScreenMonitor {
  /** 0-based position in `EnumDisplayMonitors` order. */
  index: number;
  /** True for the system's primary monitor. */
  primary: boolean;
  /** Device name, e.g. `\\.\DISPLAY1`. */
  name: string;
  /** The whole monitor, including anything covered by the taskbar. */
  bounds: ScreenRect;
  /** The monitor minus the taskbar and other auto-hiding app bars. */
  work: ScreenRect;
  /** Monitor DPI and scale factor (96 DPI = 1.0 scale). */
  dpi?: {
    x: number;
    y: number;
    scale: number;
  };
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

/** Options for visual semantic OCR text searching. */
export interface FindTextOptions extends ActionOptions {
  /** True for exact string match; false for substring/contains (default false). */
  exact?: boolean;
  /** True for case-sensitive match (default false). */
  caseSensitive?: boolean;
  /** Boundary rectangle to constrain search; omitted searches the whole screen. */
  area?: ScreenRect;
  /** Milliseconds to wait/poll for text to appear (default 0). */
  timeoutMs?: number;
}

/** Result of a visual semantic text search. */
export interface TextMatch {
  /** The matched text string. */
  text: string;
  /** Bounding box in virtual-desktop screen coordinates. */
  rect: ScreenRect;
  /** Center coordinate of the bounding box. */
  center: { x: number; y: number };
  /** Clicks the center coordinate using mouse input. */
  click(): Promise<{ clicks: number }>;
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
  imageSearch(
    area: ScreenRect,
    imagePath: string,
    options?: NativeActionOptions,
  ): Promise<ScreenPixelSearchResult>;
  caret(options?: NativeActionOptions): Promise<ScreenCaretResult>;
  sysGet(index: number, options?: NativeActionOptions): Promise<{ metric: number }>;
  sysGetIPAddresses(options?: NativeActionOptions): Promise<{ addresses: string[] }>;
  findText?(
    text: string,
    options?: NativeActionOptions & {
      exact?: boolean;
      caseSensitive?: boolean;
      area?: ScreenRect;
    },
  ): Promise<{ text: string; rect: ScreenRect } | null>;
  ocr?(
    area?: ScreenRect,
    options?: NativeActionOptions,
  ): Promise<Array<{ text: string; rect: ScreenRect }>>;
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

function matchesText(
  candidate: string,
  target: string,
  exact = false,
  caseSensitive = false,
): boolean {
  const c = caseSensitive ? candidate : candidate.toLowerCase();
  const t = caseSensitive ? target : target.toLowerCase();
  return exact ? c === t : c.includes(t);
}

function rectIntersects(rect: ScreenRect, area: ScreenRect): boolean {
  return !(
    rect.right <= area.left ||
    rect.left >= area.right ||
    rect.bottom <= area.top ||
    rect.top >= area.bottom
  );
}

function rectWithinArea(rect: ScreenRect, area: ScreenRect): boolean {
  const cx = (rect.left + rect.right) / 2;
  const cy = (rect.top + rect.bottom) / 2;
  return cx >= area.left && cx <= area.right && cy >= area.top && cy <= area.bottom;
}

function createTextMatch(text: string, rect: ScreenRect): TextMatch {
  const center = {
    x: Math.round((rect.left + rect.right) / 2),
    y: Math.round((rect.top + rect.bottom) / 2),
  };
  return {
    text,
    rect,
    center,
    async click(): Promise<{ clicks: number }> {
      const { mouse } = await import("./input/mouse");
      await mouse.click({ x: center.x, y: center.y });
      return { clicks: 1 };
    },
  };
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
   * An omitted (or null) `index` means the primary monitor.
   *
   * @param index 0-based monitor number; null/omitted selects the primary.
   * @throws ActionError with `invalid_contract` (no such monitor),
   *   `capability_denied`, `timeout` or `cancelled`.
   */
  monitor(index?: number | null, options?: ActionOptions): Promise<ScreenMonitor> {
    if (index !== undefined && index !== null && (!Number.isInteger(index) || index < 0)) {
      throw new TypeError(`screen.monitor: index must be a 0-based integer >= 0, got ${String(index)}`);
    }
    return runAction(options, (bridgeOptions) =>
      screenBridge().then((bridge) =>
        (index === undefined || index === null
          ? bridge.monitor(bridgeOptions ?? {})
          : bridge.monitor(index + 1, bridgeOptions ?? {})
        ).then((monitor) => ({ ...monitor, index: monitor.index - 1 })),
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
  /**
   * Visual semantic text finder: searches for `text` using OCR / visual bridge,
   * with fallback to semantic UI automation discovery.
   *
   * @param text String to locate on screen.
   * @param options Search criteria: exact match, case sensitivity, search area, timeout.
   * @returns TextMatch with bounding rect, center coordinate and .click() action, or null.
   */
  async findText(text: string, options?: FindTextOptions): Promise<TextMatch | null> {
    if (typeof text !== "string") {
      throw new TypeError(`screen.findText: text must be a string, got ${typeof text}`);
    }
    if (text.length === 0) return null;

    const exact = options?.exact ?? false;
    const caseSensitive = options?.caseSensitive ?? false;
    const area = options?.area;
    const timeoutMs = options?.timeoutMs ?? 0;

    const singlePass = async (): Promise<TextMatch | null> => {
      // 1. Try screen bridge findText or ocr
      try {
        const bridge = await screenBridge();
        if (typeof (bridge as any).findText === "function") {
          const hit = await (bridge as any).findText(text, options);
          if (hit && hit.rect) {
            return createTextMatch(hit.text ?? text, hit.rect);
          }
        }

        if (typeof (bridge as any).ocr === "function") {
          const items: Array<{ text: string; rect: ScreenRect }> = await (bridge as any).ocr(
            area,
            options,
          );
          if (Array.isArray(items)) {
            for (const item of items) {
              if (!item || typeof item.text !== "string" || !item.rect) continue;
              if (matchesText(item.text, text, exact, caseSensitive)) {
                if (
                  area &&
                  !rectIntersects(item.rect, area) &&
                  !rectWithinArea(item.rect, area)
                ) {
                  continue;
                }
                return createTextMatch(item.text, item.rect);
              }
            }
          }
        }
      } catch {
        // Screen bridge call failed or not registered; proceed to automation fallback
      }

      // 2. Semantic fallback to automation UIA elements
      try {
        const { automation } = await import("./automation");
        const { elements } = await automation.find({ allowDesktopRoot: true });
        for (const el of elements) {
          if (!el.name) continue;
          if (matchesText(el.name, text, exact, caseSensitive)) {
            const rect: ScreenRect = {
              left: el.x,
              top: el.y,
              right: el.x + el.width,
              bottom: el.y + el.height,
            };
            if (area && !rectIntersects(rect, area) && !rectWithinArea(rect, area)) {
              continue;
            }
            return createTextMatch(el.name, rect);
          }
        }
      } catch {
        // Automation unavailable or unmocked, proceed to miss
      }

      return null;
    };

    if (timeoutMs <= 0) {
      return singlePass();
    }

    const start = Date.now();
    while (true) {
      const match = await singlePass();
      if (match) return match;
      if (Date.now() - start >= timeoutMs) break;
      await new Promise((resolve) => setTimeout(resolve, Math.min(50, timeoutMs)));
    }
    return null;
  },
};
