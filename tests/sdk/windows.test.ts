import { expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type { WindowHandle, WindowId, WindowsBridge, WindowsListOptions } from "../../sdk/src/window";
import type { ProcessId } from "../../sdk/src/process";
import { Window } from "../../sdk/src/window";

const runtimeIds = { next: 1000, cancelled: [] as number[], released: [] as number[] };

mock.module("rime:runtime", () => ({
  runtime: {
    ping: () => "pong",
    delay: async <T>(_ms: number, value: T) => value,
    cancellation: () => runtimeIds.next++,
    cancel: (id: number) => {
      runtimeIds.cancelled.push(id);
      return true;
    },
    releaseCancellation: (id: number) => {
      runtimeIds.released.push(id);
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

const movedHandle: WindowHandle = {
  id: 7 as WindowId,
  title: "Slice Window",
  className: "Static",
  processName: "rime_tests.exe",
  rect: { left: 0, top: 0, right: 800, bottom: 600 },
  visible: true,
  minimized: false,
  state: "normal",
  processId: 42 as ProcessId,
};

const calls: Array<{
  method: string;
  target: WindowId | "active" | number;
  position?: string;
  options?: WindowsListOptions;
}> = [];
let rejectNext = false;

const passThrough = async (method: string, target: WindowId | "active", options?: NativeActionOptions) => {
  calls.push({ method, target, options });
  if (rejectNext) {
    rejectNext = false;
    throw new Error("window no longer exists");
  }
  return movedHandle;
};

mock.module("rime:window", () => ({
  windows: {
    list: async (options?: WindowsListOptions) => {
      calls.push({ method: "list", target: 0, options });
      return [movedHandle];
    },
    active: async (options?: NativeActionOptions) => {
      calls.push({ method: "active", target: "active", options });
      return movedHandle;
    },
    info: async (windowId: WindowId, options?: NativeActionOptions) => {
      calls.push({ method: "info", target: windowId, options });
      return { ...movedHandle, id: windowId };
    },
    move: (target: WindowId | "active", position: string, options?: NativeActionOptions) => {
      calls.push({ method: "move", target, position, options });
      if (rejectNext) {
        rejectNext = false;
        throw new Error("window no longer exists");
      }
      return Promise.resolve(movedHandle);
    },
    focus: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("focus", target, options),
    close: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("close", target, options),
    hide: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("hide", target, options),
    show: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("show", target, options),
    minimize: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("minimize", target, options),
    maximize: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("maximize", target, options),
    restore: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("restore", target, options),
  } satisfies WindowsBridge,
}));

test("Window.active().move routes through the window bridge", async () => {
  calls.length = 0;
  const handle = await Window.active().move("left");
  expect(calls).toEqual([{ method: "move", target: "active", position: "left", options: undefined }]);
  expect(handle).toEqual(movedHandle);
});

test("Window.move passes the window id through", async () => {
  calls.length = 0;
  const handle = await Window.move(7 as WindowId, "right");
  expect(calls).toEqual([{ method: "move", target: 7, position: "right", options: undefined }]);
  expect(handle.id as number).toBe(7);
});

test("bridge rejections propagate to the caller", async () => {
  calls.length = 0;
  rejectNext = true;
  await expect(Window.move(9 as WindowId, "left")).rejects.toThrow("window no longer exists");
  expect(calls).toEqual([{ method: "move", target: 9, position: "left", options: undefined }]);
});

test("Window.list and Window.info forward to the bridge", async () => {
  const list = await Window.list();
  expect(list).toEqual([movedHandle]);
  const info = await Window.info(3 as WindowId);
  expect(info.id as number).toBe(3);
});

test("state mutations route target and native options", async () => {
  calls.length = 0;
  await Window.active().close({ deadlineMs: 250 });
  expect(calls).toEqual([
    { method: "close", target: "active", options: { deadlineMs: 250 } },
  ]);
  calls.length = 0;
  await Window.hide(11 as WindowId, { deadlineMs: 100, parentActionId: 3 });
  expect(calls).toEqual([
    { method: "hide", target: 11, options: { deadlineMs: 100, parentActionId: 3 } },
  ]);
});

test("list queries reach the wire without the signal field", async () => {
  calls.length = 0;
  await Window.list({ title: "Notepad", matchMode: "exact", deadlineMs: 75 });
  expect(calls[0]?.options).toEqual({ title: "Notepad", matchMode: "exact", deadlineMs: 75 });
});

test("signal binds a cancellation id on the wire and releases it", async () => {
  runtimeIds.cancelled.length = 0;
  runtimeIds.released.length = 0;
  calls.length = 0;
  const signal = new FakeSignal();
  const handle = await Window.move(7 as WindowId, "left", { signal });
  expect(handle).toEqual(movedHandle);
  const wiredId = calls[0]?.options?.cancellationId ?? 0;
  expect(wiredId).toBeGreaterThan(0);
  expect(runtimeIds.released).toContain(wiredId);
});

test("active/info forward native options", async () => {
  calls.length = 0;
  await Window.list({ active: true, deadlineMs: 50 });
  expect(calls[0]).toEqual({
    method: "list",
    target: 0,
    options: { active: true, deadlineMs: 50 },
  });
  calls.length = 0;
  const info = await Window.info(5 as WindowId, { deadlineMs: 60, parentActionId: 2 });
  expect(info.id as number).toBe(5);
  expect(calls).toEqual([
    { method: "info", target: 5, options: { deadlineMs: 60, parentActionId: 2 } },
  ]);
});
