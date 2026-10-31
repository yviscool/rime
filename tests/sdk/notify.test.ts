import { beforeEach, expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type { NotifyOptions, NotifyResult } from "../../sdk/src/ui";

// Realism: L3 - tests sdk/src/ui.ts modern notification functionality
// against an instrumented rime:ui bridge. The bridge is the environment,
// and the facade's validation, defaults, toast handling, and resilient fallbacks
// are verified against hand-crafted expectations.

type CodedRejection = { code: string; message: string };

const codedError = ({ code, message }: CodedRejection) =>
  Object.assign(new Error(message), { code });

type NotifyCall = [NotifyOptions & NativeActionOptions];
type TrayTipCall = [string, Record<string, unknown> | undefined];
type ToolTipCall = [string, Record<string, unknown> | undefined];

let notifyCalls: NotifyCall[] = [];
let trayTipCalls: TrayTipCall[] = [];
let toolTipCalls: ToolTipCall[] = [];

let notifyHandler:
  | ((options: NotifyOptions & NativeActionOptions) => Promise<NotifyResult>)
  | undefined = undefined;

let trayTipRejection: CodedRejection | null = null;
let toolTipRejection: CodedRejection | null = null;

function reset(): void {
  notifyCalls = [];
  trayTipCalls = [];
  toolTipCalls = [];
  notifyHandler = undefined;
  trayTipRejection = null;
  toolTipRejection = null;
}

const bridge = {
  async msgBox() {
    return "OK";
  },
  async inputBox() {
    return { value: "", result: "OK" as const };
  },
  async toolTip(text: string, options?: NativeActionOptions & Record<string, unknown>) {
    toolTipCalls.push([text, options]);
    if (toolTipRejection) throw codedError(toolTipRejection);
    return null;
  },
  async traySetIcon() {
    return null;
  },
  async trayTip(text: string, options?: NativeActionOptions & Record<string, unknown>) {
    trayTipCalls.push([text, options]);
    if (trayTipRejection) throw codedError(trayTipRejection);
    return null;
  },
  notify: async (options: NotifyOptions & NativeActionOptions) => {
    notifyCalls.push([options]);
    if (notifyHandler) {
      return notifyHandler(options);
    }
    return { dismissed: true };
  },
};

mock.module("rime:ui", () => ({ ui: bridge }));

const { ui } = await import("../../sdk/src/ui");

beforeEach(reset);

test("ui.notify validates input options and throws TypeError for invalid arguments", async () => {
  // @ts-expect-error test invalid invocation
  expect(() => ui.notify(null)).toThrow(TypeError);
  // @ts-expect-error test missing title
  expect(() => ui.notify({ body: "test" })).toThrow(TypeError);
  // @ts-expect-error test missing body
  expect(() => ui.notify({ title: "test" })).toThrow(TypeError);
  // @ts-expect-error test non-string title
  expect(() => ui.notify({ title: 123, body: "test" })).toThrow(TypeError);
  // @ts-expect-error test non-string body
  expect(() => ui.notify({ title: "test", body: {} })).toThrow(TypeError);

  expect(() =>
    ui.notify({ title: "test", body: "msg", timeoutMs: -100 }),
  ).toThrow(TypeError);

  expect(() =>
    // @ts-expect-error test invalid actions
    ui.notify({ title: "test", body: "msg", actions: "invalid" }),
  ).toThrow(TypeError);

  expect(() =>
    // @ts-expect-error test invalid action items
    ui.notify({ title: "test", body: "msg", actions: [{ id: "1" }] }),
  ).toThrow(TypeError);
});

test("ui.notify forwards default values (icon: info, actions: [], timeoutMs: 5000) when omitted", async () => {
  notifyHandler = async () => ({ dismissed: true });

  const result = await ui.notify({ title: "Build", body: "Succeeded" });

  expect(result).toEqual({ action: undefined, dismissed: true });
  expect(notifyCalls.length).toBe(1);
  expect(notifyCalls[0][0]).toEqual({
    title: "Build",
    body: "Succeeded",
    icon: "info",
    actions: [],
    timeoutMs: 5000,
  });
});

test("ui.notify passes explicit options faithfully", async () => {
  notifyHandler = async (opts) => ({
    action: opts.actions?.[0]?.id,
    dismissed: false,
  });

  const actions = [
    { id: "view_log", title: "View Log" },
    { id: "dismiss", title: "Dismiss" },
  ];

  const result = await ui.notify({
    title: "Deploy Alert",
    body: "Deployment finished with 1 warning",
    icon: "warning",
    actions,
    timeoutMs: 3000,
  });

  expect(result).toEqual({ action: "view_log", dismissed: false });
  expect(notifyCalls.length).toBe(1);
  expect(notifyCalls[0][0]).toEqual({
    title: "Deploy Alert",
    body: "Deployment finished with 1 warning",
    icon: "warning",
    actions,
    timeoutMs: 3000,
  });
});

test("ui.notify resolves user interaction or dismissal from system toast", async () => {
  notifyHandler = async () => ({ action: "retry", dismissed: false });
  const actionRes = await ui.notify({ title: "Test", body: "Failed" });
  expect(actionRes).toEqual({ action: "retry", dismissed: false });

  notifyHandler = async () => ({ dismissed: true });
  const dismissRes = await ui.notify({ title: "Test", body: "Passed" });
  expect(dismissRes).toEqual({ action: undefined, dismissed: true });
});

test("ui.notify falls back to trayTip when system toast is unavailable", async () => {
  // Simulate bridge without notify capability
  const originalNotify = bridge.notify;
  // @ts-expect-error deleting property to simulate absence
  delete bridge.notify;

  try {
    const result = await ui.notify({
      title: "Notice",
      body: "Background job completed",
      icon: "info",
    });

    expect(result).toEqual({ dismissed: true });
    expect(trayTipCalls.length).toBe(1);
    expect(trayTipCalls[0][0]).toBe("Background job completed");
    expect(trayTipCalls[0][1]).toEqual({
      title: "Notice",
      icon: 0x40, // MSG_BOX_ICONS / TRAY_TIP_ICONS for info
    });
    expect(toolTipCalls.length).toBe(0);
  } finally {
    bridge.notify = originalNotify;
  }
});

test("ui.notify falls back to toolTip when trayTip throws", async () => {
  const originalNotify = bridge.notify;
  // @ts-expect-error deleting property to simulate absence
  delete bridge.notify;
  trayTipRejection = { code: "execution_failed", message: "no tray available" };

  try {
    const result = await ui.notify({
      title: "Tray Fallback",
      body: "Tool tip delivery",
    });

    expect(result).toEqual({ dismissed: true });
    expect(trayTipCalls.length).toBe(1);
    expect(toolTipCalls.length).toBe(1);
    expect(toolTipCalls[0][0]).toBe("Tray Fallback\nTool tip delivery");
  } finally {
    bridge.notify = originalNotify;
  }
});

test("ui.notify resiliently returns dismissed: true even if both trayTip and toolTip fail", async () => {
  const originalNotify = bridge.notify;
  // @ts-expect-error deleting property to simulate absence
  delete bridge.notify;
  trayTipRejection = { code: "execution_failed", message: "tray error" };
  toolTipRejection = { code: "execution_failed", message: "tooltip error" };

  try {
    const result = await ui.notify({
      title: "Total Failure",
      body: "Should not crash caller",
    });

    expect(result).toEqual({ dismissed: true });
    expect(trayTipCalls.length).toBe(1);
    expect(toolTipCalls.length).toBe(1);
  } finally {
    bridge.notify = originalNotify;
  }
});

test("ui.notify resiliently falls back to trayTip if bridge.notify throws an error", async () => {
  notifyHandler = async () => {
    throw new Error("System toast service crash");
  };

  const result = await ui.notify({
    title: "System Error",
    body: "Recovered via tray",
  });

  expect(result).toEqual({ dismissed: true });
  expect(trayTipCalls.length).toBe(1);
  expect(trayTipCalls[0][0]).toBe("Recovered via tray");
});
