/** Error thrown by {@link runAction} when the bridge rejects with a `code`. */
export class ActionError extends Error {
  code: string;
  constructor(code: string, message?: string, options?: ErrorOptions) {
    super(message ?? code, options);
    this.name = "ActionError";
    this.code = code;
  }
}

/** Options every Action accepts from the SDK (mirrors native ActionOptions). */
export interface NativeActionOptions {
  /** Relative budget in milliseconds; applied when the Action is built (default 5000). */
  deadlineMs?: number;
  /** Runtime cancellation id (from `runtime.cancellation()`) bound to the Action. */
  cancellationId?: number;
  /** Groups the Action under a parent in the trace. */
  parentActionId?: number;
  /** Deduplicates retries of the same logical Action. */
  idempotencyKey?: string;
}

/**
 * Structural shape of an AbortSignal. QuickJS does not ship the DOM type, so
 * any object with `aborted` plus (optional) abort listeners works; real
 * AbortSignals satisfy it structurally.
 */
export interface CancellationSignal {
  readonly aborted: boolean;
  readonly reason?: unknown;
  addEventListener?(type: "abort", listener: () => void): void;
  removeEventListener?(type: "abort", listener: () => void): void;
}

/** SDK-level action options: native fields plus AbortSignal-style cancellation. */
export interface ActionOptions extends NativeActionOptions {
  signal?: CancellationSignal;
}

export interface BoundActionOptions {
  /** Native options to forward (signal already converted to a cancellation id). */
  native?: NativeActionOptions;
  /** True when the signal was already aborted; the caller must not run. */
  aborted: boolean;
  /** Abort reason to throw. */
  reason?: unknown;
  /** Drops the abort listener and releases the cancellation id. Always call it. */
  release(): void;
}

async function runtimeBridge() {
  const module = await import("rime:runtime");
  return module.runtime;
}

/**
 * Resolves SDK options into native options. An aborted signal short-circuits
 * (`aborted: true`); a live signal allocates a runtime cancellation id, flips
 * it on abort, and is released in `release()`.
 *
 * When both `signal` and an explicit `cancellationId` are present, the signal
 * wins: the explicit id is ignored and the signal-bound id is forwarded.
 */
export async function bindActionOptions(options?: ActionOptions): Promise<BoundActionOptions> {
  if (!options) return { aborted: false, release() {} };
  const sig = options.signal;
  if (options.deadlineMs !== undefined) {
    if (Number.isNaN(options.deadlineMs) || options.deadlineMs < 0) {
      throw new TypeError(`deadlineMs must be a non-negative finite number, got ${options.deadlineMs}`);
    }
    // NOTE: Infinity is also rejected by Number.isNaN? No — but native
    // optional_u64 rejects all non-finite values; keep the minimal
    // negative/NaN guard here and let the native layer reject the rest.
    if (!Number.isFinite(options.deadlineMs)) {
      throw new TypeError(`deadlineMs must be a non-negative finite number, got ${options.deadlineMs}`);
    }
  }
  const native: NativeActionOptions = {};
  if (options.deadlineMs !== undefined) native.deadlineMs = options.deadlineMs;
  if (options.parentActionId !== undefined) native.parentActionId = options.parentActionId;
  if (options.idempotencyKey !== undefined) native.idempotencyKey = options.idempotencyKey;
  if (sig) {
    if (sig.aborted) {
      return { aborted: true, reason: sig.reason, release() {} };
    }
    const runtime = await runtimeBridge();
    const id = runtime.cancellation();
    const onAbort = () => {
      runtime.cancel(id);
    };
    sig.addEventListener?.("abort", onAbort);
    native.cancellationId = id;
    return {
      native,
      aborted: false,
      release() {
        sig.removeEventListener?.("abort", onAbort);
        runtime.releaseCancellation(id);
      },
    };
  }
  if (options.cancellationId !== undefined) native.cancellationId = options.cancellationId;
  return { native, aborted: false, release() {} };
}

/**
 * Runs one bridge call with the options pipeline: resolves options, rejects
 * early when the signal is already aborted, and always releases bindings.
 * A rejection carrying a string `code` is mapped to {@link ActionError};
 * rejections without a code are rethrown unchanged.
 */
export async function runAction<T>(
  options: ActionOptions | undefined,
  call: (native?: NativeActionOptions) => Promise<T>,
): Promise<T> {
  const binding = await bindActionOptions(options);
  if (binding.aborted) throw binding.reason ?? new Error("action aborted");
  try {
    return await call(binding.native);
  } catch (error) {
    if (error instanceof ActionError) throw error;
    if (
      error !== null &&
      typeof error === "object" &&
      "code" in error &&
      typeof (error as { code: unknown }).code === "string"
    ) {
      const code = (error as { code: string }).code;
      const maybeMessage = (error as unknown as { message?: unknown }).message;
      const message = typeof maybeMessage === "string" ? maybeMessage : code;
      throw new ActionError(code, message, { cause: error });
    }
    throw error;
  } finally {
    binding.release();
  }
}
