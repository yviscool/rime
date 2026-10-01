import { expect, mock, test } from "bun:test";
import type { WindowHandle, WindowsBridge } from "../../sdk/src/window";
import { Window } from "../../sdk/src/window";

const movedHandle: WindowHandle = {
  id: 7,
  title: "Slice Window",
  rect: { left: 0, top: 0, right: 800, bottom: 600 },
  visible: true,
  minimized: false,
  processId: 42,
};

const calls: Array<{ target: number | "active"; position: string }> = [];
let rejectNext = false;

mock.module("rime:window", () => ({
  windows: {
    list: async () => [movedHandle],
    active: async () => movedHandle,
    info: async (windowId: number) => ({ ...movedHandle, id: windowId }),
    move: async (target: number | "active", position: string) => {
      calls.push({ target, position });
      if (rejectNext) {
        rejectNext = false;
        throw new Error("window no longer exists");
      }
      return movedHandle;
    },
  } satisfies WindowsBridge,
}));

test("Window.active().move routes through the window bridge", async () => {
  calls.length = 0;
  const handle = await Window.active().move("left");
  expect(calls).toEqual([{ target: "active", position: "left" }]);
  expect(handle).toEqual(movedHandle);
});

test("Window.move passes the window id through", async () => {
  calls.length = 0;
  const handle = await Window.move(7, "right");
  expect(calls).toEqual([{ target: 7, position: "right" }]);
  expect(handle.id).toBe(7);
});

test("bridge rejections propagate to the caller", async () => {
  calls.length = 0;
  rejectNext = true;
  await expect(Window.move(9, "left")).rejects.toThrow("window no longer exists");
  expect(calls).toEqual([{ target: 9, position: "left" }]);
});

test("Window.list and Window.info forward to the bridge", async () => {
  const list = await Window.list();
  expect(list).toEqual([movedHandle]);
  const info = await Window.info(3);
  expect(info.id).toBe(3);
});
