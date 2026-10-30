/**
 * Production Showdown #4: Global Hotkey App Switcher & Quick Launcher
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Scripts use `#n::` blocks with `IfWinExist`, `WinActivate`, and `Run`.
 *   Race conditions occur when an app is starting up: repeated hotkey presses
 *   spawn multiple duplicate instances. Error handling requires checking `ErrorLevel`.
 * - Rime SDK: Clean, atomic async switching logic in `QuickLauncher`. Uses
 *   `input.hotkey()` for chord bindings, `Window.list()` and `win.focus()` to bring
 *   existing windows to foreground, and `Process.launch()` with debounce protection.
 */

import { input, Process, Window, runtime } from "@rime/sdk";
import type { EventSubscription, WindowSnapshot } from "@rime/sdk";

export interface LaunchTarget {
  name: string;
  hotkeyChord: string;
  command: string;
  processName: string;
  args?: string;
}

export class QuickLauncher {
  private subscriptions: EventSubscription[] = [];
  private launchLock = new Set<string>();

  constructor(readonly targets: LaunchTarget[] = []) {}

  /**
   * Focuses the first running window of the target app, or launches it if none exists.
   */
  async activateOrLaunch(target: LaunchTarget): Promise<"focused" | "launched" | "in_flight"> {
    if (this.launchLock.has(target.name)) {
      return "in_flight";
    }

    try {
      this.launchLock.add(target.name);

      // 1. Check for existing window
      const windows = await Window.list({ ahkExe: target.processName });
      const candidate = windows.find((w) => w.snapshot.visible && !w.snapshot.minimized) ?? windows[0];

      if (candidate) {
        await candidate.focus();
        if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
          runtime.debug(`[QuickLauncher] Focused existing window: [${candidate.snapshot.processName}] "${candidate.snapshot.title}"`);
        }
        return "focused";
      }

      // 2. No active window: check running process before spawning
      const processes = await Process.list();
      const existingProc = processes.find(
        (p) => p.name.toLowerCase() === target.processName.toLowerCase(),
      );

      if (!existingProc) {
        // Launch a new process instance
        const launched = await Process.launch({
          command: target.command,
          args: target.args,
        });

        if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
          runtime.debug(`[QuickLauncher] Launched "${target.command}" with PID ${launched.pid}`);
        }
        return "launched";
      }

      return "focused";
    } finally {
      this.launchLock.delete(target.name);
    }
  }

  /**
   * Binds global hotkeys for all registered targets.
   */
  start(): void {
    for (const target of this.targets) {
      try {
        const sub = input.hotkey(target.hotkeyChord, () => {
          this.activateOrLaunch(target).catch((err) => {
            if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
              runtime.debug(`[QuickLauncher] Error handling hotkey for ${target.name}: ${err}`);
            }
          });
        });
        this.subscriptions.push(sub);
      } catch (err) {
        if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
          runtime.debug(`[QuickLauncher] Failed to register hotkey ${target.hotkeyChord}: ${err}`);
        }
      }
    }
  }

  /**
   * Releases all hotkey subscriptions.
   */
  dispose(): void {
    for (const sub of this.subscriptions) {
      try {
        sub.close();
      } catch {}
    }
    this.subscriptions = [];
  }
}

export const DEFAULT_TARGETS: LaunchTarget[] = [
  {
    name: "Notepad",
    hotkeyChord: "win+n",
    command: "notepad.exe",
    processName: "notepad.exe",
  },
  {
    name: "Terminal",
    hotkeyChord: "win+t",
    command: "cmd.exe",
    processName: "cmd.exe",
  },
];

export async function main(): Promise<void> {
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug("=== Showdown 04: Global Hotkey Quick Launcher ===");
  }

  const launcher = new QuickLauncher(DEFAULT_TARGETS);

  try {
    // Register hotkeys
    launcher.start();

    // Demonstrate programmatic switch or launch for target
    const outcome = await launcher.activateOrLaunch(DEFAULT_TARGETS[0]);
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[QuickLauncher] Action for ${DEFAULT_TARGETS[0].name} completed: ${outcome}`);
    }
  } finally {
    launcher.dispose();
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug("[QuickLauncher] Released all hotkey bindings successfully");
    }
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Quick Launcher failed:", error);
  }
});
