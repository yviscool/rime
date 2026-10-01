import { expect, mock, test } from "bun:test";
import type { WindowHandle, WindowsBridge } from "../../sdk/src/window";
import { Window } from "../../sdk/src/window";

const movedHandle: WindowHandle = {
  id: 7,
  title: "Slice Window",
  className: "Static",
  processName: "rime_tests.exe",
  rect: { left: 0, top: 0, right: 800, bottom: 600 },
  visible: true,
  minimized: false,
  state: "normal",
  processId: 42,
};

const calls: Array<{
  method: string;
  target: number | "active";
  position?: string;
  options?: Record<string, unknown>;
}> = [];
let rejectNext = false;

const passThrough = async (method: string, target: number | "active", options?: Record<string, unknown>) => {
  calls.push({ method, target, options });
  if (rejectNext) {
    rejectNext = false;
    throw new Error("window no longer exists");
  }
  return movedHandle;
};

mock.module("rime:window", () => ({
  windows: {
    list: async (options?: Record<string, unknown>) => {
      calls.push({ method: "list", target: 0, options });
      return [movedHandle];
    },
    active: async () => movedHandle,
    info: async (windowId: number) => ({ ...movedHandle, id: windowId }),
    move: (target: number | "active", position: string, options?: Record<string, unknown>) => {
      calls.push({ method: "move", target, position, options });
      if (rejectNext) {
        rejectNext = false;
        throw new Error("window no longer exists");
      }
      return Promise.resolve(movedHandle);
    },
    focus: (target: number | "active", options?: Record<string, unknown>) =>
      passThrough("focus", target, options),
    close: (target: number | "active", options?: Record<string, unknown>) =>
      passThrough("close", target, options),
    hide: (target: number | "active", options?: Record<string, unknown>) =>
      passThrough("hide", target, options),
    show: (target: number | "active", options?: Record<string, unknown>) =>
      passThrough("show", target, options),
    minimize: (target: number | "active", options?: Record<string, unknown>) =>
      passThrough("minimize", target, options),
    maximize: (target: number | "active", options?: Record<string, unknown>) =>
      passThrough("maximize", target, options),
    restore: (target: number | "active", options?: Record<string, unknown>) =>
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
  const handle = await Window.move(7, "right");
  expect(calls).toEqual([{ method: "move", target: 7, position: "right", options: undefined }]);
  expect(handle.id).toBe(7);
});

test("bridge rejections propagate to the caller", async () => {
  calls.length = 0;
  rejectNext = true;
  await expect(Window.move(9, "left")).rejects.toThrow("window no longer exists");
  expect(calls).toEqual([{ method: "move", target: 9, position: "left", options: undefined }]);
});

test("Window.list and Window.info forward to the bridge", async () => {
  const list = await Window.list();
  expect(list).toEqual([movedHandle]);
  const info = await Window.info(3);
  expect(info.id).toBe(3);
});

test("state mutations route target and native options", async () => {
  calls.length = 0;
  await Window.active().close({ deadlineMs: 250 });
  expect(calls).toEqual([
    { method: "close", target: "active", options: { deadlineMs: 250 } },
  ]);
  calls.length = 0;
  await Window.hide(11, { deadlineMs: 100, parentActionId: 3 });
  expect(calls).toEqual([
    { method: "hide", target: 11, options: { deadlineMs: 100, parentActionId: 3 } },
  ]);
});

test("list queries reach the wire without the signal field", async () => {
  calls.length = 0;
  await Window.list({ title: "Notepad", matchMode: "exact", deadlineMs: 75 });
  expect(calls[0]?.options).toEqual({ title: "Notepad", matchMode: "exact", deadlineMs: 75 });
});
