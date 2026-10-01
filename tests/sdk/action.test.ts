import { expect, mock, test } from "bun:test";
import { bindActionOptions, runAction } from "../../sdk/src/action";

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
