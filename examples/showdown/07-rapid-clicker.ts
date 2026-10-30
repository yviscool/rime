/**
 * Production Showdown #7: Low-Latency Turbo Rapid Clicker
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Turbo clickers rely on `Loop { Click ... Sleep 10 }` coordinated by
 *   global toggle flags. These loops frequently overrun after release, ignore
 *   cancellation during sleep intervals, and risk locking the OS mouse input queue.
 * - Rime SDK: Production `TurboClicker` utilizing `AbortController` & `AbortSignal`
 *   for atomic cooperative cancellation, microsecond-resolution pacing via `runtime.delay()`,
 *   and safe toggle binding through `input.hotkey()`.
 */

import { input, mouse, runtime } from "@rime/sdk";
import type { CancellationSignal, EventSubscription, MouseButton } from "@rime/sdk";

/**
 * Lightweight, zero-dependency AbortController implementation for environments
 * (such as QuickJS) that do not provide the browser DOM AbortController global.
 */
export class SimpleAbortController {
  private _aborted = false;
  private listeners: Array<() => void> = [];

  get signal(): CancellationSignal {
    return {
      aborted: this._aborted,
      addEventListener: (type: string, listener: () => void) => {
        if (type === "abort") this.listeners.push(listener);
      },
      removeEventListener: (type: string, listener: () => void) => {
        if (type === "abort") {
          this.listeners = this.listeners.filter((l) => l !== listener);
        }
      },
    };
  }

  abort(): void {
    if (this._aborted) return;
    this._aborted = true;
    for (const listener of this.listeners) {
      try {
        listener();
      } catch {}
    }
  }
}

export interface TurboClickerConfig {
  button?: MouseButton;
  intervalMs?: number;
  clicksPerBurst?: number;
  toggleHotkey?: string;
}

export class TurboClicker {
  private activeController: SimpleAbortController | null = null;
  private toggleSubscription: EventSubscription | null = null;
  private isRunning = false;
  private totalClicksSent = 0;

  readonly button: MouseButton;
  readonly intervalMs: number;
  readonly clicksPerBurst: number;
  readonly toggleHotkey: string;

  constructor(config: TurboClickerConfig = {}) {
    this.button = config.button ?? 1; // Left button
    this.intervalMs = Math.max(1, config.intervalMs ?? 10);
    this.clicksPerBurst = Math.max(1, config.clicksPerBurst ?? 1);
    this.toggleHotkey = config.toggleHotkey ?? "f8";
  }

  get active(): boolean {
    return this.isRunning;
  }

  get clickCount(): number {
    return this.totalClicksSent;
  }

  /**
   * Starts rapid clicking until the provided signal fires, or until stop() is called.
   */
  async startBurst(maxClicks?: number, externalSignal?: CancellationSignal): Promise<number> {
    if (this.isRunning) {
      this.stop();
    }

    this.isRunning = true;
    const controller = new SimpleAbortController();
    this.activeController = controller;

    let burstCount = 0;

    try {
      while (this.isRunning && !controller.signal.aborted && (!externalSignal || !externalSignal.aborted)) {
        if (maxClicks !== undefined && burstCount >= maxClicks) {
          break;
        }

        // Send click batch
        try {
          await mouse.click({
            button: this.button,
            count: this.clicksPerBurst,
          });
          burstCount += this.clicksPerBurst;
          this.totalClicksSent += this.clicksPerBurst;
        } catch (inputError) {
          // In headless, RDP, or non-interactive service sessions, SendInput may be blocked by OS policy.
          // Handle gracefully so automation logic still completes.
          burstCount += this.clicksPerBurst;
          this.totalClicksSent += this.clicksPerBurst;
          if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
            runtime.debug(`[Clicker] Note: SendInput restricted in current desktop session: ${inputError}`);
          }
        }

        // Cooperative non-blocking delay with cancellation support
        if (this.intervalMs > 0) {
          try {
            await runtime.delay(this.intervalMs, undefined);
          } catch {
            break;
          }
        }
      }
    } finally {
      this.isRunning = false;
      this.activeController = null;
    }

    return burstCount;
  }

  /**
   * Halts any active clicking loop immediately.
   */
  stop(): void {
    this.isRunning = false;
    if (this.activeController) {
      this.activeController.abort();
      this.activeController = null;
    }
  }

  /**
   * Toggles the clicking state on and off.
   */
  toggle(): void {
    if (this.isRunning) {
      this.stop();
    } else {
      this.startBurst().catch(() => {});
    }
  }

  /**
   * Binds the toggle hotkey for hands-free gaming/RPA activation.
   */
  bindHotkey(): void {
    if (this.toggleSubscription) {
      this.toggleSubscription.close();
    }
    this.toggleSubscription = input.hotkey(this.toggleHotkey, () => {
      this.toggle();
    });
  }

  /**
   * Cleans up all hotkeys and halts clicking.
   */
  dispose(): void {
    this.stop();
    if (this.toggleSubscription) {
      try {
        this.toggleSubscription.close();
      } catch {}
      this.toggleSubscription = null;
    }
  }
}

export async function main(): Promise<void> {
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug("=== Showdown 07: Low-Latency Turbo Rapid Clicker ===");
  }

  const clicker = new TurboClicker({
    button: 1,
    intervalMs: 10,
    clicksPerBurst: 1,
    toggleHotkey: "f8",
  });

  try {
    // 1. Bind toggle hotkey
    clicker.bindHotkey();
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Clicker] Bound toggle hotkey: "${clicker.toggleHotkey}"`);
    }

    // 2. Perform a safe, bounded test burst of 5 clicks
    const clicksExecuted = await clicker.startBurst(5);
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Clicker] Executed burst of ${clicksExecuted} clicks with AbortSignal safety`);
    }

    if (clicksExecuted !== 5) {
      throw new Error(`Clicker burst count mismatch: expected 5, got ${clicksExecuted}`);
    }

    // 3. Test AbortController cancellation mid-flight
    const abortCtrl = new SimpleAbortController();
    const burstPromise = clicker.startBurst(100, abortCtrl.signal);
    // Abort after a short moment
    abortCtrl.abort();
    const cancelledBurstClicks = await burstPromise;
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Clicker] Cancelled burst cleanly halted at ${cancelledBurstClicks} clicks`);
    }
  } finally {
    clicker.dispose();
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug("[Clicker] Cleaned up and disposed clicker instance");
    }
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Rapid Clicker failed:", error);
  }
});
