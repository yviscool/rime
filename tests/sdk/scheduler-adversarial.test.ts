// Realism: L3 — real SDK pipeline (runAction/bindActionOptions) + mocked
// `rime:runtime` environment (cancellation/subscription ids only). The queue,
// unload-gate and merge helpers under test are local policy functions mirroring
// AGENTS.md §线程与生命周期; they do not claim native scheduler behavior.
import { expect, mock, test } from "bun:test";
import { ActionError, planAction, runAction } from "../../sdk/src/action";

const env = { next: 100, cancelled: [] as number[], released: [] as number[] };

mock.module("rime:runtime", () => ({
  runtime: {
    ping: () => "pong",
    delay: async <T>(_ms: number, value: T) => value,
    cancellation: () => env.next++,
    cancel: (id: number) => {
      env.cancelled.push(id);
      return true;
    },
    releaseCancellation: (id: number) => {
      env.released.push(id);
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
  addEventListener(_t: "abort", fn: () => void): void {
    this.listeners.push(fn);
  }
  removeEventListener(_t: "abort", fn: () => void): void {
    this.listeners = this.listeners.filter((l) => l !== fn);
  }
  abort(reason?: unknown): void {
    this.aborted = true;
    this.reason = reason;
    for (const l of [...this.listeners]) l();
  }
}

// Policy mirror 1: nested pump must not create a second scheduler — nested
// runAction calls execute strictly inside the outer call (FIFO, no reentry).
test("nested runAction executes inside the outer call, in order", async () => {
  const order: string[] = [];
  await runAction(undefined, async () => {
    order.push("outer-start");
    await runAction(undefined, async () => {
      order.push("inner");
      return "in";
    });
    order.push("outer-end");
    return "out";
  });
  expect(order).toEqual(["outer-start", "inner", "outer-end"]);
});

// Policy mirror 2: an already-aborted signal never reaches the bridge.
test("already-aborted signal rejects before the bridge runs", async () => {
  const signal = new FakeSignal();
  signal.abort(new Error("user stopped it"));
  let ran = false;
  await expect(
    runAction({ signal }, async () => {
      ran = true;
      return "x";
    }),
  ).rejects.toThrow("user stopped it");
  expect(ran).toBe(false);
});

// Policy mirror 3: mid-flight abort cancels the runtime id and still releases.
test("mid-flight abort cancels the runtime id and releases it", async () => {
  env.cancelled.length = 0;
  env.released.length = 0;
  const signal = new FakeSignal();
  let seenId = 0;
  await expect(
    runAction({ signal }, async (native) => {
      seenId = native?.cancellationId ?? 0;
      signal.abort(new Error("stop now"));
      throw Object.assign(new Error("cancelled by runtime"), { code: "cancelled" });
    }),
  ).rejects.toBeInstanceOf(ActionError);
  expect(seenId).toBeGreaterThan(0);
  expect(env.cancelled).toContain(seenId);
  expect(env.released).toContain(seenId);
});

// Policy mirror 4 (L2 pure): unload gate. Mirrors AGENTS.md — unload must fail
// with per-item reasons while host-registered resources remain.
interface UnloadState {
  hookSubscriptions: number;
  undeliveredHostEvents: number;
  pendingPromises: number;
  armedTimers: number;
  jsCallbacks: number;
}
function unloadGate(state: UnloadState): { ok: true } | { ok: false; reasons: string[] } {
  const reasons: string[] = [];
  if (state.hookSubscriptions > 0) reasons.push(`hookSubscriptions:${state.hookSubscriptions}`);
  if (state.undeliveredHostEvents > 0)
    reasons.push(`undeliveredHostEvents:${state.undeliveredHostEvents}`);
  if (state.pendingPromises > 0) reasons.push(`pendingPromises:${state.pendingPromises}`);
  if (state.armedTimers > 0) reasons.push(`armedTimers:${state.armedTimers}`);
  if (state.jsCallbacks > 0) reasons.push(`jsCallbacks:${state.jsCallbacks}`);
  return reasons.length === 0 ? { ok: true } : { ok: false, reasons };
}

test("unload gate fails per-item while resources remain, passes when drained", () => {
  const busy = unloadGate({
    hookSubscriptions: 1,
    undeliveredHostEvents: 2,
    pendingPromises: 0,
    armedTimers: 1,
    jsCallbacks: 0,
  });
  expect(busy.ok).toBe(false);
  if (!busy.ok) {
    expect(busy.reasons).toEqual([
      "hookSubscriptions:1",
      "undeliveredHostEvents:2",
      "armedTimers:1",
    ]);
  }
  expect(
    unloadGate({
      hookSubscriptions: 0,
      undeliveredHostEvents: 0,
      pendingPromises: 0,
      armedTimers: 0,
      jsCallbacks: 0,
    }),
  ).toEqual({ ok: true });
});

// Policy mirror 5 (L2 pure): queue-full strategy is explicit — merge repeats,
// drop lowest priority, or fail with a code. No silent loss.
type FullPolicy = "merge" | "drop" | "fail";
function onQueueFull(policy: FullPolicy, pending: string[], incoming: string): string[] | { error: string } {
  if (policy === "merge") {
    // Coalesce repeated mouse-move style repeats: keep one latest marker.
    if (incoming === "mouse.move" && pending.includes("mouse.move")) return [...pending];
    return [...pending, incoming];
  }
  if (policy === "drop") return pending;
  return { error: "queue_full" };
}

test("queue-full policy merges repeats, drops, or fails explicitly", () => {
  expect(onQueueFull("merge", ["mouse.move"], "mouse.move")).toEqual(["mouse.move"]);
  expect(onQueueFull("merge", ["key.press"], "mouse.move")).toEqual(["key.press", "mouse.move"]);
  expect(onQueueFull("drop", ["a"], "b")).toEqual(["a"]);
  expect(onQueueFull("fail", ["a"], "b")).toEqual({ error: "queue_full" });
});

test("cancelled plan never runs the bridge", async () => {
  const plan = planAction({
    type: "window.move",
    capability: "windows.window.write",
    target: { kind: "window", id: "7" },
    payload: { edge: "left" },
  });
  plan.cancel();
  let ran = false;
  const err = await plan
    .execute(undefined, async () => {
      ran = true;
      return "x";
    })
    .catch((e: unknown) => e);
  expect(err).toBeInstanceOf(ActionError);
  expect((err as ActionError).code).toBe("cancelled");
  expect(ran).toBe(false);
});
