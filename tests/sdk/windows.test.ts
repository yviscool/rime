import { expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type {
  GroupFocusOptions,
  TitleMatchMode,
  TitleMatchModeSpeed,
  WindowHandle,
  WindowId,
  WindowQueryFields,
  WindowsBridge,
  WindowsGroupsBridge,
  WindowsListOptions,
  WindowsWaitOptions,
  WindowZorder,
} from "../../sdk/src/window";
import type { ProcessId } from "../../sdk/src/process";
import { groups, settings, Window } from "../../sdk/src/window";

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
};

const calls: Array<{
  method: string;
  target: WindowId | "active" | number | string;
  position?: string;
  placement?: string;
  title?: string;
  value?: boolean | -1 | 0 | 1;
  // WindowsWaitOptions is a superset of WindowsListOptions (until is
  // optional); group calls add their own fields (reverse/mode) on top.
  options?: WindowsWaitOptions & { reverse?: boolean; mode?: string };
}> = [];
let rejectNext = false;

const fakeSettings = {
  titleMatchMode: "2" as TitleMatchMode,
  titleMatchModeSpeed: "Fast" as TitleMatchModeSpeed,
  detectHiddenWindows: false,
  detectHiddenText: false,
};

const passThrough = async (method: string, target: WindowId | "active", options?: NativeActionOptions) => {
  calls.push({ method, target, options });
  if (rejectNext) {
    rejectNext = false;
    throw new Error("window no longer exists");
  }
  return movedHandle;
};

mock.module("rime:window", () => ({
  settings: {
    window: {
      get titleMatchMode() {
        return fakeSettings.titleMatchMode;
      },
      set titleMatchMode(value: TitleMatchMode) {
        fakeSettings.titleMatchMode = value;
      },
      get titleMatchModeSpeed() {
        return fakeSettings.titleMatchModeSpeed;
      },
      set titleMatchModeSpeed(value: TitleMatchModeSpeed) {
        fakeSettings.titleMatchModeSpeed = value;
      },
      get detectHiddenWindows() {
        return fakeSettings.detectHiddenWindows;
      },
      set detectHiddenWindows(value: boolean) {
        fakeSettings.detectHiddenWindows = value;
      },
      get detectHiddenText() {
        return fakeSettings.detectHiddenText;
      },
      set detectHiddenText(value: boolean) {
        fakeSettings.detectHiddenText = value;
      },
      setTitleMatchMode: (mode: TitleMatchMode | TitleMatchModeSpeed) => {
        if (mode === "Fast" || mode === "Slow") {
          const previous = fakeSettings.titleMatchModeSpeed;
          fakeSettings.titleMatchModeSpeed = mode;
          return previous;
        }
        const previous = fakeSettings.titleMatchMode;
        fakeSettings.titleMatchMode = mode;
        return previous;
      },
      setDetectHiddenWindows: (value: boolean) => {
        const previous = fakeSettings.detectHiddenWindows;
        fakeSettings.detectHiddenWindows = value;
        return previous;
      },
      setDetectHiddenText: (value: boolean) => {
        const previous = fakeSettings.detectHiddenText;
        fakeSettings.detectHiddenText = value;
        return previous;
      },
    },
  },
  windows: {
    list: async (options?: WindowsListOptions) => {
      calls.push({ method: "list", target: 0, options });
      return [movedHandle];
    },
    active: async (options?: NativeActionOptions) => {
      calls.push({ method: "active", target: "active", options });
      return movedHandle;
    },
    exists: async (options?: WindowsListOptions) => {
      calls.push({ method: "exists", target: 0, options });
      return true;
    },
    isActive: async (options?: WindowsListOptions) => {
      calls.push({ method: "isActive", target: 0, options });
      return false;
    },
    wait: async (options?: WindowsWaitOptions) => {
      calls.push({ method: "wait", target: 0, options });
      return options?.until === "closed" || options?.until === "notActive" ? null : movedHandle;
    },
    info: async (windowId: WindowId, options?: NativeActionOptions) => {
      calls.push({ method: "info", target: windowId, options });
      return { ...movedHandle, id: windowId };
    },
    controls: async (windowId: WindowId, options?: NativeActionOptions) => {
      calls.push({ method: "controls", target: windowId, options });
      return [{ id: 9 as WindowId, className: "Edit", classNN: "Edit1" }];
    },
    text: async (windowId: WindowId, options?: NativeActionOptions) => {
      calls.push({ method: "text", target: windowId, options });
      return "Rime Slice Control\r\n";
    },
    move: (target: WindowId | "active", position: string, options?: NativeActionOptions) => {
      calls.push({ method: "move", target, position, options });
      if (rejectNext) {
        rejectNext = false;
        throw new Error("window no longer exists");
      }
      return Promise.resolve(movedHandle);
    },
    zorder: (target: WindowId | "active", placement: WindowZorder, options?: NativeActionOptions) => {
      calls.push({ method: "zorder", target, placement, options });
      return Promise.resolve(movedHandle);
    },
    focus: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("focus", target, options),
    kill: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("kill", target, options),
    redraw: (target: WindowId | "active", options?: NativeActionOptions) =>
      passThrough("redraw", target, options),
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
    minimizeAll: async (options?: NativeActionOptions) => {
      calls.push({ method: "minimizeAll", target: 0, options });
    },
    minimizeAllUndo: async (options?: NativeActionOptions) => {
      calls.push({ method: "minimizeAllUndo", target: 0, options });
    },
    setTitle: (target: WindowId | "active", title: string, options?: NativeActionOptions) => {
      calls.push({ method: "setTitle", target, title, options });
      return Promise.resolve(movedHandle);
    },
    setEnabled: (
      target: WindowId | "active",
      value: boolean | -1 | 0 | 1,
      options?: NativeActionOptions,
    ) => {
      calls.push({ method: "setEnabled", target, value, options });
      return Promise.resolve(movedHandle);
    },
    setAlwaysOnTop: (
      target: WindowId | "active",
      value?: boolean | -1 | 0 | 1,
      options?: NativeActionOptions,
    ) => {
      calls.push({ method: "setAlwaysOnTop", target, value, options });
      return Promise.resolve(movedHandle);
    },
  } satisfies WindowsBridge,
  groups: {
    add: async (name: string, query: WindowQueryFields, options?: NativeActionOptions) => {
      calls.push({ method: "group.add", target: name, options: { ...query, ...options } });
      return { count: 3 };
    },
    activate: async (name: string, options?: GroupFocusOptions) => {
      calls.push({ method: "group.activate", target: name, options });
      return name === "empty" ? null : movedHandle;
    },
    deactivate: async (name: string, options?: GroupFocusOptions) => {
      calls.push({ method: "group.deactivate", target: name, options });
      return movedHandle;
    },
    close: async (name: string, mode?: string, options?: NativeActionOptions) => {
      calls.push({ method: "group.close", target: name, options: { mode, ...options } });
      return { closed: 2, activated: null };
    },
  } satisfies WindowsGroupsBridge,
}));

test("settings exposes the synchronous bridge with return-previous setters", async () => {
  const bridge = await settings();
  expect(bridge.titleMatchMode).toBe("2");
  expect(bridge.titleMatchModeSpeed).toBe("Fast");
  expect(bridge.detectHiddenWindows).toBe(false);
  expect(bridge.setTitleMatchMode("3")).toBe("2");
  expect(bridge.titleMatchMode).toBe("3");
  expect(bridge.setTitleMatchMode("RegEx")).toBe("3");
  expect(bridge.setTitleMatchMode("Slow")).toBe("Fast");
  expect(bridge.titleMatchMode).toBe("RegEx");
  expect(bridge.setDetectHiddenWindows(true)).toBe(false);
  expect(bridge.detectHiddenWindows).toBe(true);
  expect(bridge.setDetectHiddenText(true)).toBe(false);
  expect(bridge.detectHiddenText).toBe(true);
});

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

test("Window.zorder passes target and placement through", async () => {
  calls.length = 0;
  const handle = await Window.zorder(7 as WindowId, "bottom");
  expect(calls).toEqual([{ method: "zorder", target: 7, placement: "bottom", options: undefined }]);
  expect(handle).toEqual(movedHandle);
  calls.length = 0;
  await Window.active().zorder("top");
  expect(calls).toEqual([{ method: "zorder", target: "active", placement: "top", options: undefined }]);
});

test("kill and redraw route through the window bridge", async () => {
  calls.length = 0;
  await Window.kill(7 as WindowId);
  expect(calls).toEqual([{ method: "kill", target: 7, options: undefined }]);
  calls.length = 0;
  await Window.active().redraw();
  expect(calls).toEqual([{ method: "redraw", target: "active", options: undefined }]);
});

test("minimizeAll and minimizeAllUndo route through the window bridge", async () => {
  calls.length = 0;
  await Window.minimizeAll();
  expect(calls).toEqual([{ method: "minimizeAll", target: 0, options: undefined }]);
  calls.length = 0;
  await Window.minimizeAllUndo({ deadlineMs: 100 });
  expect(calls).toEqual([{ method: "minimizeAllUndo", target: 0, options: { deadlineMs: 100 } }]);
});

test("setTitle, setEnabled and setAlwaysOnTop route through the window bridge", async () => {
  calls.length = 0;
  await Window.setTitle(7 as WindowId, "Renamed");
  expect(calls).toEqual([{ method: "setTitle", target: 7, title: "Renamed", options: undefined }]);
  calls.length = 0;
  await Window.setEnabled(7 as WindowId, -1, { deadlineMs: 100 });
  expect(calls).toEqual([
    { method: "setEnabled", target: 7, value: -1, options: { deadlineMs: 100 } },
  ]);
  calls.length = 0;
  await Window.setAlwaysOnTop(7 as WindowId);
  expect(calls).toEqual([
    { method: "setAlwaysOnTop", target: 7, value: undefined, options: undefined },
  ]);
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

test("Window.controls and Window.text forward reads to the bridge", async () => {
  calls.length = 0;
  const controls = await Window.controls(7 as WindowId, { deadlineMs: 30 });
  expect(controls[0]?.classNN).toBe("Edit1");
  const text = await Window.text(7 as WindowId);
  expect(text).toBe("Rime Slice Control\r\n");
  expect(calls).toEqual([
    { method: "controls", target: 7, options: { deadlineMs: 30 } },
    { method: "text", target: 7, options: undefined },
  ]);
});

test("Window.exists and Window.isActive forward probes with the signal stripped", async () => {
  calls.length = 0;
  const signal = new FakeSignal();
  expect(await Window.exists({ ahkExe: "notepad.exe", signal, deadlineMs: 40 })).toBe(true);
  expect(await Window.isActive({ title: "Notepad" })).toBe(false);
  const first = calls[0];
  expect(first?.method).toBe("exists");
  expect(first?.options?.ahkExe).toBe("notepad.exe");
  expect("signal" in (first?.options ?? {})).toBe(false);
  expect(first?.options?.deadlineMs).toBe(40);
  expect(calls[1]).toEqual({
    method: "isActive",
    target: 0,
    options: { title: "Notepad" },
  });
});

test("Window.wait forwards the until condition with the signal stripped", async () => {
  calls.length = 0;
  const signal = new FakeSignal();
  expect(
    await Window.wait({ title: "Slice Window", until: "exists", signal, deadlineMs: 25 }),
  ).toEqual(movedHandle);
  expect(await Window.wait({ title: "Slice Window", until: "closed" })).toBeNull();
  expect(calls[0]?.method).toBe("wait");
  expect(calls[0]?.options?.until).toBe("exists");
  expect("signal" in (calls[0]?.options ?? {})).toBe(false);
  expect(calls[0]?.options?.deadlineMs).toBe(25);
  expect(calls[1]?.options?.until).toBe("closed");
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

test("groups routes add/activate/deactivate/close through the group bridge", async () => {
  calls.length = 0;
  const facade = await groups();
  expect(await facade.add("demo", { title: "Notepad", matchMode: "exact", deadlineMs: 50 })).toBe(3);
  expect(await facade.activate("demo", { reverse: true })).toEqual(movedHandle);
  expect(await facade.activate("empty")).toBeNull();
  expect(await facade.deactivate("demo")).toEqual(movedHandle);
  expect(await facade.close("demo", "all", { deadlineMs: 60 })).toEqual({
    closed: 2,
    activated: null,
  });
  expect(calls[0]).toEqual({
    method: "group.add",
    target: "demo",
    options: { title: "Notepad", matchMode: "exact", deadlineMs: 50 },
  });
  expect(calls[1]).toEqual({
    method: "group.activate",
    target: "demo",
    options: { reverse: true },
  });
  expect(calls[2]).toEqual({ method: "group.activate", target: "empty", options: {} });
  expect(calls[3]).toEqual({ method: "group.deactivate", target: "demo", options: {} });
  expect(calls[4]).toEqual({
    method: "group.close",
    target: "demo",
    options: { mode: "all", deadlineMs: 60 },
  });
});

test("groups strips the signal and binds a cancellation id", async () => {
  runtimeIds.cancelled.length = 0;
  runtimeIds.released.length = 0;
  calls.length = 0;
  const signal = new FakeSignal();
  const facade = await groups();
  expect(await facade.add("demo", { title: "Notepad", signal })).toBe(3);
  const wire = calls[0]?.options as { cancellationId?: number; signal?: unknown } | undefined;
  expect("signal" in (wire ?? {})).toBe(false);
  expect(wire?.cancellationId).toBeGreaterThan(0);
  expect(runtimeIds.released).toContain(wire?.cancellationId ?? 0);
});
