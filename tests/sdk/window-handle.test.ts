import { beforeEach, expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type { WindowId, WindowSnapshot } from "../../sdk/src/window";
import { Window } from "../../sdk/src/window";
import type { ProcessId } from "../../sdk/src/process";

// Realism: L3 - the real Window class runs against an instrumented
// rime:window bridge; the native module is the environment, never the unit
// under test. Ids, snapshots and warning-free delegation are literals
// written by hand, so a handle that caches a HWND, drops the id, or fails
// to adopt the post-verb snapshot cannot pass by agreeing with itself.

const snapshot = (id: number, title: string): WindowSnapshot => ({
  id: id as WindowId,
  title,
  className: "Static",
  processName: "rime_tests.exe",
  processPath: "C:\\build\\tests\\rime_tests.exe",
  rect: { left: 0, top: 0, right: 800, bottom: 600 },
  clientRect: { left: 8, top: 31, right: 800, bottom: 560 },
  visible: true,
  minimized: false,
  state: "normal",
  processId: 42 as ProcessId,
  style: 13565952,
  exStyle: 65536,
  enabled: true,
  alwaysOnTop: false,
  minMax: 0,
  transparent: -1,
  transColor: "",
  region: "",
});

let listed: WindowSnapshot[] = [];
let infoResult: WindowSnapshot = snapshot(7, "Slice Window");
let infoRejection: unknown = null;
let seen: Array<{ method: string; target: WindowId; options?: NativeActionOptions }> = [];

const bridge = {
  async list() {
    return listed;
  },
  async info(target: WindowId, options?: NativeActionOptions) {
    seen.push({ method: "info", target, options });
    if (infoRejection) throw infoRejection;
    return infoResult;
  },
  async focus(target: WindowId, options?: NativeActionOptions) {
    seen.push({ method: "focus", target, options });
    return snapshot(target as number, "Focused");
  },
  async move(target: WindowId) {
    seen.push({ method: "move", target });
    return snapshot(target as number, "Moved");
  },
  async controls() {
    return [];
  },
  async text() {
    return "";
  },
};

mock.module("rime:window", () => ({ windows: bridge }));

beforeEach(() => {
  listed = [snapshot(7, "Slice Window"), snapshot(9, "Other")];
  infoResult = snapshot(7, "Slice Window");
  infoRejection = null;
  seen = [];
});

test("find/list/active wrap snapshots without extra round trips", async () => {
  const found = await Window.find({ title: "Slice" });
  expect(found?.id).toBe(7 as WindowId);
  expect(found?.snapshot.title).toBe("Slice Window");

  const all = await Window.list();
  expect(all.map((w) => w.id)).toStrictEqual([7 as WindowId, 9 as WindowId]);

  const active = await Window.active();
  expect(active?.id).toBe(7 as WindowId);

  listed = [];
  await expect(Window.find()).resolves.toBeNull();
  await expect(Window.active()).resolves.toBeNull();
});

test("verbs re-resolve the stable id and adopt the new snapshot", async () => {
  const win = Window.fromSnapshot(snapshot(7, "Before"));
  const after = await win.focus();
  expect(after.title).toBe("Focused");
  expect(win.snapshot.title).toBe("Focused");
  expect(win.id).toBe(7 as WindowId);
  expect(seen).toStrictEqual([{ method: "focus", target: 7 as WindowId, options: undefined }]);

  await win.move({ x: 10 });
  expect(win.snapshot.title).toBe("Moved");
  expect(seen.map((s) => s.method)).toStrictEqual(["focus", "move"]);
});

test("refresh re-reads and isAlive maps only target_gone to false", async () => {
  const win = Window.fromSnapshot(snapshot(7, "Before"));
  infoResult = snapshot(7, "Renamed");
  await expect(win.refresh()).resolves.toMatchObject({ title: "Renamed" });
  expect(win.snapshot.title).toBe("Renamed");
  await expect(win.isAlive()).resolves.toBe(true);

  infoRejection = Object.assign(new Error("window no longer exists"), { code: "target_gone" });
  await expect(win.isAlive()).resolves.toBe(false);

  infoRejection = Object.assign(new Error("denied"), { code: "capability_denied" });
  await expect(win.isAlive()).rejects.toThrow("denied");
});
