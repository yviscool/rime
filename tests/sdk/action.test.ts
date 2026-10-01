import { expect, mock, test } from "bun:test";
import { ActionError, bindActionOptions, runAction } from "../../sdk/src/action";

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
