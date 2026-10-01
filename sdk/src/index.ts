export type RuntimeLayer = "core" | "action" | "automation" | "ui" | "host";

export const runtimeVersion = "0.1.0";

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
  /** Registers a runtime-owned callback. Outstanding subscriptions block unload. */
  subscribe(callback: () => void): number;
  /** Removes a subscription. Returns false when unknown. */
  unsubscribe(subscriptionId: number): boolean;
  /** Reads live host state (modules, functions, subscriptions, tasks, errors). */
  inspect(): string;
}

export { runtime } from "rime:runtime";

export * from "./action";
export * from "./window";
export * from "./input";
export * from "./process";
export * from "./clipboard";
