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
}

export { runtime } from "rime:runtime";

export * from "./action";
export * from "./inspect";
export * from "./window";
export * from "./input";
export * from "./process";
export * from "./clipboard";
export * from "./automation";
export * from "./contracts";
export * from "./runtime-language";
