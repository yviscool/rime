export type RuntimeLayer = "core" | "action" | "automation" | "ui" | "host";

export const runtimeVersion = "0.1.0";

/** Read-only environment snapshot returned by {@link RuntimeBridge.context}. */
export interface RuntimeContext {
  /** Contract version of this snapshot shape. */
  schemaVersion: 1;
  /** Native modules registered on this host. */
  modules: string[];
  /** In-flight work the runtime currently owns. */
  tasks: { async: number; queued: number; timers: number; callbacks: number };
  /** Live event-subscription registry entries. */
  subscriptions: number;
  /** Live cancellation ids not yet released. */
  cancellations: number;
}

export interface RuntimeReloadState {
  /** Reloads the embedder already performed for this script file (0 before the first one). */
  count: number;
  /** True while the current turn is unwinding for a reload. */
  pending: boolean;
}

export interface RuntimeBridge {
  ping(): string;
  /** Resolves `value` after `milliseconds`, rejecting early when cancelled. */
  delay<T = unknown>(milliseconds: number, value: T, cancellationId?: number): Promise<T>;
  /** Creates a cancellation id usable by `delay` and `cancel`. */
  cancellation(): number;
  /** Cancels every promise bound to the id. Returns false when unknown. */
  cancel(cancellationId: number): boolean;
  /** Releases a cancellation id. Returns false when unknown. */
  releaseCancellation(cancellationId: number): boolean;
  /** Registers a runtime-owned callback; receives the host event payload when delivered. Outstanding subscriptions block unload. */
  subscribe(callback: (event: unknown) => void): number;
  /** Removes a subscription. Returns false when unknown. */
  unsubscribe(subscriptionId: number): boolean;
  /** Reads live host state (modules, functions, subscriptions, tasks, errors). */
  inspect(): string;
  /** Reads a fresh read-only environment snapshot (modules, tasks, ownership). */
  context(): RuntimeContext;
  /** Passes text to the debugger (AHK OutputDebug). Returns nothing. */
  debug(text: string): void;
  /** Reads the process working directory (AHK A_WorkingDir). */
  cwd(): string;
  /** Sets the process working directory (AHK SetWorkingDir). Throws on Win32 failure, leaving the directory unchanged. */
  setCwd(path: string): void;
  /**
   * Ends the script process with `code` (AHK Exit / ExitApp - one API; there
   * are no script threads to leave, so both map here). Synchronous: the call
   * always throws to unwind the current turn, queued async work is abandoned,
   * `input.onExit` handlers run with `{reason: "exit", code}`, and the host
   * returns `code` from its entry point. First call wins; later calls are
   * unreachable. `code` must be an integer (default 0); portable exit codes
   * are 0..255. Ungated host lifecycle, like `ping`.
   */
  exit(code?: number): never;
  /**
   * Asks the embedder to re-read this script file from disk and run it again
   * in a fresh runtime (AHK `Reload`). Synchronous: the call always throws to
   * unwind the current turn, `input.onExit` handlers run with
   * `{reason: "reload"}`, the host abandons the current instance and starts
   * the next one with `reloadState().count` incremented. A pending
   * `runtime.exit` outranks a reload, so calling this after `exit` throws the
   * refusal error instead. Ungated host lifecycle, like `ping`.
   */
  reload(): never;
  /**
   * Reads the reload diagnostics: how many times the embedder already
   * reloaded this script file, and whether the current turn is unwinding for
   * a reload right now. The count survives the teardown a reload does, so the
   * next instance observes the incremented value. Ungated host lifecycle,
   * like `ping`.
   */
  reloadState(): RuntimeReloadState;
  /**
   * Reads the effective residency flag (AHK `Persistent`): true while the
   * force flag is set or while declarative work (input hooks, hotkeys,
   * hotstrings, `setTimer` timers, live subscription objects) exists.
   * Ungated host lifecycle, like `ping`.
   */
  persistent(): boolean;
  /**
   * Sets the force-residency flag and returns the effective flag afterwards.
   * With `true` the host keeps pumping after the script body settles until
   * `runtime.exit`, an explicit `persistent(false)`, or removal of all
   * declarative work. Declarative work persists regardless of this flag.
   */
  persistent(value: boolean): boolean;
}

export { runtime } from "rime:runtime";

export * from "./action";
export * from "./inspect";
export * from "./window";
export * from "./input";
export * from "./process";
export * from "./clipboard";
export * from "./automation";
export * from "./storage";
export * from "./contracts";
export * from "./runtime-language";
