// Realism: L3 — real SDK facades (Window/Process/clipboard via runAction) +
// mocked `rime:*` bridges (in-memory window/process/clipboard store).
// Proves the normal-dev flow from the capability report:
// list → move("left"), process list/launch, clipboard write/read roundtrip.
import { expect, mock, test } from "bun:test";
import { Window } from "../../sdk/src/window";
import { Process } from "../../sdk/src/process";
import { clipboard } from "../../sdk/src/clipboard";

mock.module("rime:runtime", () => ({
  runtime: {
    ping: () => "pong",
    delay: async <T>(_ms: number, value: T) => value,
    cancellation: () => 1,
    cancel: () => true,
    releaseCancellation: () => true,
    subscribe: () => 1,
    unsubscribe: () => true,
    inspect: () => "{}",
  },
}));

const baseSnapshot = {
  title: "Untitled - Notepad",
  className: "Notepad",
  processName: "notepad.exe",
  processPath: "C:\\Windows\\notepad.exe",
  rect: { left: 0, top: 0, right: 800, bottom: 600 },
  clientRect: { left: 0, top: 0, right: 800, bottom: 600 },
  visible: true,
  minimized: false,
  state: "normal",
  processId: 42,
  style: 0,
  exStyle: 0,
  enabled: true,
  alwaysOnTop: false,
  minMax: 0,
  transparent: -1,
  transColor: "",
  region: "",
};

const moveCalls: Array<{ target: unknown; position: unknown }> = [];

mock.module("rime:window", () => ({
  windows: {
    list: async () => [{ ...baseSnapshot, id: 7 }],
    move: async (target: unknown, position: unknown) => {
      moveCalls.push({ target, position });
      return { ...baseSnapshot, id: 7 };
    },
  },
  groups: {},
  settings: { window: {} },
}));

mock.module("rime:process", () => ({
  process: {
    list: async () => [{ pid: 42, parentPid: 1, name: "notepad.exe", exePath: "C:\\Windows\\notepad.exe" }],
    launch: async (req: { command: string }) => ({ pid: req.command === "notepad.exe" ? 43 : 44 }),
  },
}));

let clipStore = "";
mock.module("rime:clipboard", () => ({
  clipboard: {
    read: async () => ({ text: clipStore }),
    write: async (text: string) => {
      clipStore = text;
      return { text: clipStore };
    },
  },
}));

test("(await Window.active())?.move(\"left\") shape: list then move by stable id", async () => {
  const wins = await Window.list({ title: "Notepad" });
  expect(wins).toHaveLength(1);
  expect(wins[0].id as number).toBe(7);
  const after = await wins[0].move("left");
  expect(after.id as number).toBe(7);
  expect(moveCalls).toEqual([{ target: 7, position: "left" }]);
  // Snapshot refreshes from the result; the id stays the stable one.
  expect(wins[0].snapshot.id as number).toBe(7);
});

test("process list then launch resolves a pid", async () => {
  const list = await Process.list();
  expect(list.map((p) => p.name)).toContain("notepad.exe");
  const launched = await Process.launch({ command: "notepad.exe" });
  expect(launched.pid as number).toBe(43);
});

test("clipboard write/read roundtrips text", async () => {
  await clipboard.write("hello rime");
  await expect(clipboard.read()).resolves.toEqual({ text: "hello rime" });
});
