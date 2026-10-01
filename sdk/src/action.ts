import type { ActionV1Identity } from "./contracts";

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

// ---- plan / inspect / execute (pure action-intent pipeline) ----

/** Minimal action intent a script can state without host knowledge. */
export interface ActionSpec {
  type: string;
  capability: string;
  target: ActionV1Identity;
  payload?: Record<string, unknown>;
}

/** Build-time controls for {@link planAction}. Pure: no host calls, injected clock. */
export interface PlanOptions {
  /** Identity recorded in the IR (default `{ kind: "plan", id: "rime:sdk" }`). */
  source?: ActionV1Identity;
  /** Relative budget previewed in the IR deadline, milliseconds (default 5000). */
  deadlineMs?: number;
  /** Clock reading for the deadline (default `Date.now()`); inject for determinism. */
  nowMs?: number;
}

/**
 * Contract-shaped action intent: action-v1 minus the runtime-assigned id.
 * Immutable once planned, so it is safe to log, diff, audit or replay.
 */
export interface PlannedAction {
  schemaVersion: 1;
  source: ActionV1Identity;
  type: string;
  capability: string;
  target: ActionV1Identity;
  /** Always `[]`: the kernel refuses any declared precondition as unsupported. */
  preconditions: [];
  deadlineUnixMs: number;
  payload: Record<string, unknown>;
}

/** Read-only preview of a plan: the frozen IR plus the rules that will govern execution. */
export interface ActionInspection {
  action: PlannedAction;
  /** Rules enforced at execution, stated up front instead of failing later. */
  notes: readonly string[];
}

/**
 * A planned, not yet executed action (future-runtime §9 shape). `cancel()`
 * discards the plan before `execute()`; bind an `ActionOptions.signal` for
 * mid-run cancellation.
 */
export interface ActionPlan {
  /** Frozen contract-shaped intent. */
  readonly action: PlannedAction;
  /** True once `cancel()` discarded the plan. */
  readonly cancelled: boolean;
  /** Previews the frozen IR and the kernel rules that apply to it. */
  inspect(): Promise<ActionInspection>;
  /** Discards the plan: any later `execute()` rejects with code `cancelled`. Idempotent. */
  cancel(): void;
  /**
   * Runs the options/error pipeline against the module bridge the caller
   * provides (the SDK has no generic dispatcher: each action type executes
   * through the module that owns its executor). Rejects with code `cancelled`
   * when the plan was discarded.
   */
  execute<T>(
    options: ActionOptions | undefined,
    call: (native?: NativeActionOptions) => Promise<T>,
  ): Promise<T>;
}

const MAX_SAFE_INTEGER = 9007199254740991; // 2^53 - 1, action-v1's id/deadline bound.

function requireNonEmptyString(value: unknown, field: string): string {
  if (typeof value !== "string" || value.length === 0) {
    throw new TypeError(`${field} must be a non-empty string`);
  }
  return value;
}

function requireFiniteNumber(value: unknown, field: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new TypeError(`${field} must be a finite number, got ${String(value)}`);
  }
  return value;
}

function deepFreeze(value: unknown): void {
  if (value === null || typeof value !== "object" || Object.isFrozen(value)) return;
  Object.freeze(value);
  for (const entry of Object.values(value)) deepFreeze(entry);
}

/**
 * Validates `spec` and builds the contract-shaped action intent. Throws a
 * field-named TypeError on malformed input (the same rules the native kernel
 * and action-v1 schema enforce); everything is pure — the clock is injectable
 * and no host state is read.
 */
export function planAction(spec: ActionSpec, options?: PlanOptions): ActionPlan {
  if (spec === null || typeof spec !== "object") {
    throw new TypeError("spec must be an object");
  }
  const type = requireNonEmptyString(spec.type, "spec.type");
  const capability = requireNonEmptyString(spec.capability, "spec.capability");
  if (spec.target === null || typeof spec.target !== "object") {
    throw new TypeError("spec.target must be an object with kind and id");
  }
  const target: ActionV1Identity = {
    kind: requireNonEmptyString(spec.target.kind, "spec.target.kind"),
    id: requireNonEmptyString(spec.target.id, "spec.target.id"),
  };
  let payload: Record<string, unknown> = {};
  if (spec.payload !== undefined) {
    if (spec.payload === null || typeof spec.payload !== "object" || Array.isArray(spec.payload)) {
      throw new TypeError("spec.payload must be a plain object");
    }
    // Round-trip through JSON so the plan owns plain data only: circular or
    // exotic values fail here, at plan time, not at execution.
    payload = JSON.parse(JSON.stringify(spec.payload)) as Record<string, unknown>;
  }
  const source: ActionV1Identity = options?.source
    ? {
        kind: requireNonEmptyString(options.source.kind, "options.source.kind"),
        id: requireNonEmptyString(options.source.id, "options.source.id"),
      }
    : { kind: "plan", id: "rime:sdk" };
  const deadlineMs = options?.deadlineMs !== undefined ? options.deadlineMs : 5000;
  if (deadlineMs < 0 || !Number.isFinite(deadlineMs)) {
    throw new TypeError(`options.deadlineMs must be a non-negative finite number, got ${deadlineMs}`);
  }
  const nowMs = options?.nowMs !== undefined ? options.nowMs : Date.now();
  if (nowMs < 0 || !Number.isFinite(nowMs)) {
    throw new TypeError(`options.nowMs must be a non-negative finite number, got ${nowMs}`);
  }
  // Saturate like the native builder: an extreme budget pins the deadline at
  // the end of the exactly-representable range instead of overflowing past it,
  // and a degenerate zero clock still yields a deadline the schema accepts.
  const base = Math.floor(nowMs);
  const budget = Math.floor(deadlineMs);
  const deadlineUnixMs = Math.max(
    1,
    budget > MAX_SAFE_INTEGER - base ? MAX_SAFE_INTEGER : base + budget,
  );

  const action: PlannedAction = {
    schemaVersion: 1,
    source,
    type,
    capability,
    target,
    preconditions: [],
    deadlineUnixMs,
    payload,
  };
  deepFreeze(action);

  const notes: readonly string[] = [
    "preconditions are never evaluated: the kernel refuses any non-empty list, so the plan keeps []",
    "execution builds a fresh action (runtime-assigned id, fresh deadline, owning-module source) " +
      "through the module bridge; this IR previews intent",
    `capability '${capability}' must be granted or the kernel refuses with 'capability_denied'`,
  ];

  let cancelled = false;
  return {
    action,
    get cancelled() {
      return cancelled;
    },
    async inspect() {
      return { action, notes };
    },
    cancel() {
      cancelled = true;
    },
    async execute<T>(
      options: ActionOptions | undefined,
      call: (native?: NativeActionOptions) => Promise<T>,
    ): Promise<T> {
      if (cancelled) throw new ActionError("cancelled", "action plan was cancelled");
      return runAction(options, call);
    },
  };
}

/**
 * Plans `spec` and executes it in one step: validation first (a malformed
 * spec never reaches the bridge), then the full options/error pipeline.
 */
export async function executeAction<T>(
  spec: ActionSpec,
  options: ActionOptions | undefined,
  call: (native?: NativeActionOptions) => Promise<T>,
): Promise<T> {
  return planAction(spec).execute(options, call);
}
