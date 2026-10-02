import { expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type {
  TitleMatchMode,
  TitleMatchModeSpeed,
  WindowHandle,
  WindowId,
  WindowsBridge,
  WindowsListOptions,
} from "../../sdk/src/window";
import type { ProcessId } from "../../sdk/src/process";
import { settings, Window } from "../../sdk/src/window";

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
  target: WindowId | "active" | number;
  position?: string;
  options?: WindowsListOptions;
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
