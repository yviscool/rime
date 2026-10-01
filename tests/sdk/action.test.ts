import { expect, mock, test } from "bun:test";
import {
  ActionError,
  bindActionOptions,
  executeAction,
  planAction,
  runAction,
} from "../../sdk/src/action";

const ids = { next: 1, cancelled: [] as number[], released: [] as number[] };

mock.module("rime:runtime", () => ({
  runtime: {
    ping: () => "pong",
    delay: async <T>(_ms: number, value: T) => value,
    cancellation: () => ids.next++,
    cancel: (id: number) => {
      ids.cancelled.push(id);
      return true;
    },
    releaseCancellation: (id: number) => {
      ids.released.push(id);
      return true;
    },
    subscribe: () => 1,
    unsubscribe: () => true,
    inspect: () => "{}",
  },
}));

class FakeSignal {
  aborted = false;
  reason?: unknown;
  private listeners: Array<() => void> = [];

  addEventListener(_type: "abort", listener: () => void): void {
    this.listeners.push(listener);
  }

  removeEventListener(_type: "abort", listener: () => void): void {
    this.listeners = this.listeners.filter((entry) => entry !== listener);
  }

  abort(reason?: unknown): void {
    this.aborted = true;
    this.reason = reason;
    for (const listener of [...this.listeners]) listener();
  }
}

test("runAction forwards native options without a signal", async () => {
  let seen: unknown;
  const result = await runAction({ deadlineMs: 250 }, async (native) => {
    seen = native;
    return "ok";
  });
  expect(result).toBe("ok");
  expect(seen).toEqual({ deadlineMs: 250 });
});

test("a live signal binds a cancellation id and releases it afterwards", async () => {
  ids.cancelled.length = 0;
  ids.released.length = 0;
  const bindSignal = new FakeSignal();
  const bound = await bindActionOptions({ signal: bindSignal });
  expect(bound.aborted).toBe(false);
  const boundId = bound.native?.cancellationId ?? 0;
  expect(boundId).toBeGreaterThan(0);
  bindSignal.abort();
  expect(ids.cancelled).toContain(boundId);
  bound.release();
  expect(ids.released).toContain(boundId);

  const runSignal = new FakeSignal();
  let seen: { cancellationId?: number } | undefined;
  await runAction({ signal: runSignal }, async (native) => {
    seen = native;
    return "ok";
  });
  const runId = seen?.cancellationId ?? 0;
  expect(runId).toBeGreaterThan(0);
  expect(ids.released).toContain(runId);
});

test("an already aborted signal rejects before the bridge runs", async () => {
  const signal = new FakeSignal();
  const reason = new Error("user aborted");
  signal.abort(reason);
  let ran = false;
  await expect(
    runAction({ signal }, async () => {
      ran = true;
      return "ok";
    }),
  ).rejects.toThrow("user aborted");
  expect(ran).toBe(false);
});

test("without options the call receives no native options", async () => {
  let seen: unknown = "sentinel";
  await runAction(undefined, async (native) => {
    seen = native;
    return "ok";
  });
  expect(seen).toBeUndefined();
});

test("idempotencyKey and parentActionId pass through to native options", async () => {
  let seen: unknown;
  await runAction({ idempotencyKey: "k-1", parentActionId: 9, deadlineMs: 100 }, async (native) => {
    seen = native;
    return "ok";
  });
  expect(seen).toEqual({ deadlineMs: 100, parentActionId: 9, idempotencyKey: "k-1" });
});

test("a signal without listener methods still binds and releases", async () => {
  ids.cancelled.length = 0;
  ids.released.length = 0;
  const bare = { aborted: false } as { aborted: boolean; reason?: unknown };
  let seen: { cancellationId?: number } | undefined;
  await runAction({ signal: bare }, async (native) => {
    seen = native;
    return "ok";
  });
  const boundId = seen?.cancellationId ?? 0;
  expect(boundId).toBeGreaterThan(0);
  expect(ids.released).toContain(boundId);
  expect(ids.cancelled).toEqual([]);
});

test("aborting mid-flight cancels the runtime id and still releases", async () => {
  ids.cancelled.length = 0;
  ids.released.length = 0;
  const signal = new FakeSignal();
  let seenId = 0;
  await expect(
    runAction({ signal }, async (native) => {
      seenId = native?.cancellationId ?? 0;
      signal.abort(new Error("mid-flight abort"));
      throw Object.assign(new Error("cancelled by runtime"), { code: "cancelled" });
    }),
  ).rejects.toBeInstanceOf(ActionError);
  expect(seenId).toBeGreaterThan(0);
  expect(ids.cancelled).toContain(seenId);
  expect(ids.released).toContain(seenId);
});

test("runAction maps coded rejections to ActionError and passes through the rest", async () => {
  const coded = await runAction(undefined, async () => {
    throw Object.assign(new Error("deadline"), { code: "timeout" });
  }).catch((error: unknown) => error);
  expect(coded).toBeInstanceOf(ActionError);
  expect((coded as ActionError).code).toBe("timeout");

  const plain = new Error("plain boom");
  await expect(
    runAction(undefined, async () => {
      throw plain;
    }),
  ).rejects.toBe(plain);
});

// ---- plan / inspect / execute ----

const spec = {
  type: "window.move",
  capability: "windows.window.write",
  target: { kind: "window", id: "42" },
  payload: { edge: "left" },
};

test("planAction builds a contract-shaped intent with an injected clock", () => {
  const plan = planAction(spec, { nowMs: 1_000_000, deadlineMs: 2_500 });
  expect(plan.cancelled).toBe(false);
  expect(plan.action).toEqual({
    schemaVersion: 1,
    source: { kind: "plan", id: "rime:sdk" },
    type: "window.move",
    capability: "windows.window.write",
    target: { kind: "window", id: "42" },
    preconditions: [],
    deadlineUnixMs: 1_002_500,
    payload: { edge: "left" },
  });
});

test("planAction defaults deadline and source, and freezes the owned IR", async () => {
  const plan = planAction(spec, { nowMs: 500 });
  expect(plan.action.deadlineUnixMs).toBe(5_500);
  expect(plan.action.source).toEqual({ kind: "plan", id: "rime:sdk" });
  expect(Object.isFrozen(plan.action)).toBe(true);
  expect(Object.isFrozen(plan.action.payload)).toBe(true);
  // The plan owns a copy: mutating the caller's payload cannot rewrite the IR.
  spec.payload.edge = "right";
  expect((plan.action.payload as { edge: string }).edge).toBe("left");
  spec.payload.edge = "left";
  const inspection = await plan.inspect();
  expect(inspection.action).toBe(plan.action);
  const notes = inspection.notes.join(" ");
  expect(notes).toContain("preconditions");
  expect(notes).toContain("capability_denied");
});

test("planAction saturates the deadline at the exactly-representable range", () => {
  const plan = planAction(spec, { nowMs: 9007199254740991, deadlineMs: 10_000 });
  expect(plan.action.deadlineUnixMs).toBe(9007199254740991);
  const zero = planAction(spec, { nowMs: 0, deadlineMs: 0 });
  expect(zero.action.deadlineUnixMs).toBe(1);
});

test("planAction rejects malformed specs with field-named TypeErrors", () => {
  expect(() => planAction(null as never)).toThrow("spec must be an object");
  expect(() => planAction({ ...spec, type: "" })).toThrow("spec.type");
  expect(() => planAction({ ...spec, capability: "" })).toThrow("spec.capability");
  expect(() => planAction({ ...spec, target: undefined as never })).toThrow("spec.target");
  expect(() => planAction({ ...spec, target: { kind: "window", id: "" } })).toThrow(
    "spec.target.id",
  );
  expect(() => planAction({ ...spec, payload: [] as never })).toThrow("spec.payload");
  const circular: Record<string, unknown> = {};
  circular.self = circular;
  expect(() => planAction({ ...spec, payload: circular })).toThrow(TypeError);
  expect(() => planAction(spec, { deadlineMs: -1 })).toThrow("options.deadlineMs");
  expect(() => planAction(spec, { deadlineMs: Number.NaN })).toThrow("options.deadlineMs");
  expect(() => planAction(spec, { nowMs: Number.POSITIVE_INFINITY })).toThrow("options.nowMs");
  expect(() => planAction(spec, { source: { kind: "", id: "x" } })).toThrow("options.source.kind");
});

test("cancel discards the plan: execute rejects with the cancelled code", async () => {
  const plan = planAction(spec);
  plan.cancel();
  expect(plan.cancelled).toBe(true);
  plan.cancel(); // idempotent
  let ran = false;
  const error = await plan
    .execute(undefined, async () => {
      ran = true;
      return "ok";
    })
    .catch((caught: unknown) => caught);
  expect(error).toBeInstanceOf(ActionError);
  expect((error as ActionError).code).toBe("cancelled");
  expect(ran).toBe(false);
  // The preview survives the discard.
  const inspection = await plan.inspect();
  expect(inspection.action.type).toBe("window.move");
});

test("plan execute runs the options pipeline through the provided bridge", async () => {
  ids.cancelled.length = 0;
  ids.released.length = 0;
  const plan = planAction(spec);
  const signal = new FakeSignal();
  let seen: { deadlineMs?: number; cancellationId?: number } | undefined;
  const result = await plan.execute({ signal, deadlineMs: 120 }, async (native) => {
    seen = native;
    return "ok";
  });
  expect(result).toBe("ok");
  expect(seen?.deadlineMs).toBe(120);
  const boundId = seen?.cancellationId ?? 0;
  expect(boundId).toBeGreaterThan(0);
  expect(ids.released).toContain(boundId);
});

test("executeAction validates the spec before the bridge ever runs", async () => {
  let ran = false;
  await expect(
    executeAction({ ...spec, type: "" }, undefined, async () => {
      ran = true;
      return "ok";
    }),
  ).rejects.toThrow("spec.type");
  expect(ran).toBe(false);
  await expect(executeAction(spec, undefined, async () => "ok")).resolves.toBe("ok");
});
