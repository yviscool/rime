/**
 * Production Showdown #10: Process Watchdog & Auto-Restart Supervisor
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Process watchdogs use `Process, Exist` loops with static `Sleep 1000`.
 *   Suffers from infinite restart loops when a binary crashes immediately (fork-bombing the OS),
 *   untracked parent/child process trees, and lack of restart budgets or telemetry.
 * - Rime SDK: Production `DaemonSupervisor` utilizing `Process.list()`, `Process.launch()`,
 *   and `Process.terminate()`. Implements restart budgets, exponential/linear backoff,
 *   PID tracking, structured logging, and clean shutdown hooks.
 */

import { Process, runtime } from "@rime/sdk";
import type { ProcessId, ProcessInfo } from "@rime/sdk";

export interface SupervisorConfig {
  command: string;
  processName: string;
  args?: string;
  checkIntervalMs?: number;
  maxRestarts?: number;
  backoffMs?: number;
}

export interface SupervisorStatus {
  targetProcessName: string;
  activePid: ProcessId | null;
  restarts: number;
  maxRestarts: number;
  isAlive: boolean;
}

export class DaemonSupervisor {
  private currentPid: ProcessId | null = null;
  private restarts = 0;
  private isRunning = false;

  readonly command: string;
  readonly processName: string;
  readonly args?: string;
  readonly checkIntervalMs: number;
  readonly maxRestarts: number;
  readonly backoffMs: number;

  constructor(config: SupervisorConfig) {
    this.command = config.command;
    this.processName = config.processName.toLowerCase();
    this.args = config.args;
    this.checkIntervalMs = Math.max(10, config.checkIntervalMs ?? 100);
    this.maxRestarts = config.maxRestarts ?? 5;
    this.backoffMs = Math.max(0, config.backoffMs ?? 50);
  }

  get restartCount(): number {
    return this.restarts;
  }

  get pid(): ProcessId | null {
    return this.currentPid;
  }

  /**
   * Locates any currently running instance of the target process name.
   */
  async findRunningProcess(): Promise<ProcessInfo | null> {
    const list = await Process.list();
    return list.find((p) => p.name.toLowerCase() === this.processName) ?? null;
  }

  /**
   * Performs a single watchdog surveillance check. If target process is missing,
   * restarts it within the configured restart budget.
   */
  async checkAndHeal(): Promise<SupervisorStatus> {
    const existing = await this.findRunningProcess();

    if (existing) {
      this.currentPid = existing.pid;
      return {
        targetProcessName: this.processName,
        activePid: existing.pid,
        restarts: this.restarts,
        maxRestarts: this.maxRestarts,
        isAlive: true,
      };
    }

    // Process is not running: check budget before restarting
    if (this.restarts >= this.maxRestarts) {
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[Supervisor] Max restart budget (${this.maxRestarts}) reached for ${this.processName}`);
      }
      return {
        targetProcessName: this.processName,
        activePid: null,
        restarts: this.restarts,
        maxRestarts: this.maxRestarts,
        isAlive: false,
      };
    }

    // Apply backoff delay if configured
    if (this.backoffMs > 0) {
      await runtime.delay(this.backoffMs, undefined);
    }

    try {
      const launched = await Process.launch({
        command: this.command,
        args: this.args,
      });

      this.currentPid = launched.pid;
      this.restarts++;

      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(
          `[Supervisor] Restarted "${this.command}" (PID ${launched.pid}) [Restart ${this.restarts}/${this.maxRestarts}]`,
        );
      }

      return {
        targetProcessName: this.processName,
        activePid: launched.pid,
        restarts: this.restarts,
        maxRestarts: this.maxRestarts,
        isAlive: true,
      };
    } catch (err) {
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[Supervisor] Failed to launch "${this.command}": ${err}`);
      }
      return {
        targetProcessName: this.processName,
        activePid: null,
        restarts: this.restarts,
        maxRestarts: this.maxRestarts,
        isAlive: false,
      };
    }
  }

  /**
   * Runs a bounded number of check iterations.
   */
  async runCycles(cycles = 2): Promise<SupervisorStatus[]> {
    this.isRunning = true;
    const history: SupervisorStatus[] = [];

    try {
      for (let i = 0; i < cycles && this.isRunning; i++) {
        const status = await this.checkAndHeal();
        history.push(status);
        if (i < cycles - 1 && this.checkIntervalMs > 0) {
          try {
            await runtime.delay(this.checkIntervalMs, undefined);
          } catch {
            break;
          }
        }
      }
    } finally {
      this.isRunning = false;
    }

    return history;
  }

  /**
   * Terminates the spawned supervised process if active.
   */
  async terminateSupervised(): Promise<boolean> {
    if (this.currentPid === null) return false;
    try {
      await Process.terminate(this.currentPid);
      this.currentPid = null;
      return true;
    } catch {
      return false;
    }
  }

  stop(): void {
    this.isRunning = false;
  }
}

export async function main(): Promise<void> {
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug("=== Showdown 10: Daemon Supervisor Watchdog ===");
  }

  // Supervise notepad as target
  const supervisor = new DaemonSupervisor({
    command: "notepad.exe",
    processName: "notepad.exe",
    checkIntervalMs: 50,
    maxRestarts: 3,
    backoffMs: 10,
  });

  try {
    // 1. Initial check (detect or launch)
    const initialStatus = await supervisor.checkAndHeal();
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(
        `[Supervisor] Initial check: alive=${initialStatus.isAlive}, PID=${initialStatus.activePid}, restarts=${initialStatus.restarts}`,
      );
    }

    // 2. Run bounded surveillance cycles
    const reports = await supervisor.runCycles(2);
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Supervisor] Completed ${reports.length} surveillance cycles successfully`);
    }

    // 3. Clean up any process spawned by this demonstration
    if (initialStatus.restarts > 0) {
      await supervisor.terminateSupervised();
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug("[Supervisor] Cleaned up spawned test process");
      }
    }
  } finally {
    supervisor.stop();
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Daemon Supervisor failed:", error);
  }
});
