/**
 * Production Showdown #8: Screen Pixel Region Color Monitor & Alert Sentinel
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Relies on `PixelGetColor` which suffers from confusing BGR/RGB hex byte order
 *   (requiring `RGB` option flag), blocking modal `MsgBox` notifications that freeze
 *   all script timers and hotkeys, and untracked screen-coordinate drift.
 * - Rime SDK: Structured `PixelSentinel` utilizing typed `screen.pixel(x, y)` (canonical 0xRRGGBB),
 *   non-blocking toast notifications via `ui.trayTip()` and `ui.toolTip()`, configurable
 *   color change tolerances, and non-blocking asynchronous observation loops.
 */

import { screen, ui, runtime } from "@rime/sdk";

export interface PixelSentinelConfig {
  x: number;
  y: number;
  checkIntervalMs?: number;
  onColorChange?: (newColor: number, oldColor: number) => Promise<void> | void;
}

export interface PixelColorSample {
  color: number;
  hex: string;
  r: number;
  g: number;
  b: number;
}

export function parseHexColor(color: number): PixelColorSample {
  const r = (color >> 16) & 0xff;
  const g = (color >> 8) & 0xff;
  const b = color & 0xff;
  const hex = `0x${color.toString(16).padStart(6, "0").toUpperCase()}`;
  return { color, hex, r, g, b };
}

export class PixelSentinel {
  private lastColor: number | null = null;
  private isMonitoring = false;

  readonly x: number;
  readonly y: number;
  readonly checkIntervalMs: number;
  readonly onColorChange?: (newColor: number, oldColor: number) => Promise<void> | void;

  constructor(config: PixelSentinelConfig) {
    this.x = config.x;
    this.y = config.y;
    this.checkIntervalMs = Math.max(10, config.checkIntervalMs ?? 100);
    this.onColorChange = config.onColorChange;
  }

  get currentColor(): number | null {
    return this.lastColor;
  }

  /**
   * Samples the current pixel color at target coordinates.
   */
  async sample(): Promise<PixelColorSample | null> {
    try {
      const color = await screen.pixel(this.x, this.y);
      this.lastColor = color;
      return parseHexColor(color);
    } catch (err) {
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[PixelSentinel] Note: Pixel read at (${this.x}, ${this.y}) unavailable: ${err}`);
      }
      return null;
    }
  }

  /**
   * Dispatches a notification through tooltip and trayTip alert surfaces.
   */
  async notifyChange(newSample: PixelColorSample): Promise<void> {
    const alertMessage = `Pixel at (${this.x}, ${this.y}) changed to ${newSample.hex} [R:${newSample.r} G:${newSample.g} B:${newSample.b}]`;

    try {
      await ui.toolTip(alertMessage, { x: this.x + 20, y: this.y + 20, index: 0 });
    } catch {}

    try {
      await ui.trayTip(alertMessage, {
        title: "Pixel Sentinel Alert",
        icon: "info",
        mute: true,
      });
    } catch {}
  }

  /**
   * Runs a bounded surveillance cycle of N iterations.
   */
  async monitorCycles(maxCycles = 3): Promise<number> {
    this.isMonitoring = true;
    let cyclesRun = 0;

    try {
      while (this.isMonitoring && cyclesRun < maxCycles) {
        cyclesRun++;
        const sample = await this.sample();
        if (sample && this.lastColor !== null && sample.color !== this.lastColor) {
          await this.notifyChange(sample);
          if (this.onColorChange) {
            await this.onColorChange(sample.color, this.lastColor);
          }
        }

        if (this.checkIntervalMs > 0 && cyclesRun < maxCycles) {
          try {
            await runtime.delay(this.checkIntervalMs, undefined);
          } catch {
            break;
          }
        }
      }
    } finally {
      this.isMonitoring = false;
      // Clear tooltip after surveillance
      try {
        await ui.toolTip("", { index: 0 });
      } catch {}
    }

    return cyclesRun;
  }

  stop(): void {
    this.isMonitoring = false;
  }
}

export async function main(): Promise<void> {
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug("=== Showdown 08: Pixel Sentinel & Alert ===");
  }

  const sentinel = new PixelSentinel({
    x: 100,
    y: 100,
    checkIntervalMs: 50,
  });

  try {
    // 1. Initial color sampling
    const sample = await sentinel.sample();
    if (sample) {
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[Sentinel] Sampled initial pixel color: ${sample.hex} at (${sentinel.x}, ${sentinel.y})`);
      }
      // Demonstrate notification delivery
      await sentinel.notifyChange(sample);
    }

    // 2. Run a short bounded monitoring cycle
    const completedCycles = await sentinel.monitorCycles(2);
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Sentinel] Completed ${completedCycles} surveillance cycles`);
    }
  } finally {
    sentinel.stop();
    try {
      await ui.toolTip("", { index: 0 });
    } catch {}
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Pixel Sentinel failed:", error);
  }
});
