import { beforeEach, expect, mock, test } from "bun:test";
import { ActionError, type NativeActionOptions } from "../../sdk/src/action";

// Realism: L3 - the real sdk/src/ui.ts facade runs against an instrumented
// rime:ui bridge; the native module is the environment, never the unit under
// test. Every expectation is a literal written by hand, so a facade that
// leaks a feature key into the shared action options, drops `freeze: false`
// (which the native side reads as an explicit unfreeze) or swallows a bridge
// rejection cannot pass by agreeing with itself.

type CodedRejection = { code: string; message: string };

const codedError = ({ code, message }: CodedRejection) =>
  Object.assign(new Error(message), { code });

type MsgCall = [string, Record<string, unknown> | undefined];
type InputCall = [string | undefined, Record<string, unknown> | undefined];
type TipCall = [string, Record<string, unknown> | undefined];
type IconCall = [string, Record<string, unknown> | undefined];
type BalloonCall = [string, Record<string, unknown> | undefined];

let msgCalls: MsgCall[] = [];
let inputCalls: InputCall[] = [];
let tipCalls: TipCall[] = [];
let iconCalls: IconCall[] = [];
let balloonCalls: BalloonCall[] = [];
let rejection: CodedRejection | null = null;
let rawRejection: unknown = null;
let msgResult: string = "Timeout";
let inputResult = { value: "seed", result: "Timeout" as "OK" | "Cancel" | "Timeout" };

function reset(): void {
  msgCalls = [];
  inputCalls = [];
  tipCalls = [];
  iconCalls = [];
  balloonCalls = [];
  rejection = null;
  rawRejection = null;
  msgResult = "Timeout";
  inputResult = { value: "seed", result: "Timeout" };
}

const bridge = {
  async msgBox(text: string, options?: NativeActionOptions & Record<string, unknown>) {
    msgCalls.push([text, options]);
    if (rawRejection) throw rawRejection;
    if (rejection) throw codedError(rejection);
    return msgResult;
  },
  async inputBox(prompt?: string, options?: NativeActionOptions & Record<string, unknown>) {
    inputCalls.push([prompt, options]);
    if (rejection) throw codedError(rejection);
    return inputResult;
  },
  async toolTip(text: string, options?: NativeActionOptions & Record<string, unknown>) {
    tipCalls.push([text, options]);
    if (rejection) throw codedError(rejection);
    return null;
  },
  async traySetIcon(file: string, options?: NativeActionOptions & Record<string, unknown>) {
    iconCalls.push([file, options]);
    if (rejection) throw codedError(rejection);
    return null;
  },
  async trayTip(text: string, options?: NativeActionOptions & Record<string, unknown>) {
    balloonCalls.push([text, options]);
    if (rejection) throw codedError(rejection);
    return null;
  },
};

mock.module("rime:ui", () => ({ ui: bridge }));

const { ui } = await import("../../sdk/src/ui");

beforeEach(reset);

test("msgBox passes only the text when the caller gave no options", async () => {
  await expect(ui.msgBox("hello")).resolves.toBe("Timeout");
  // toStrictEqual: the absent options must stay an absent key (undefined is
  // dropped, not written), so the native side always sees one shape.
  expect(msgCalls).toStrictEqual([["hello", {}]]);
});

test("msgBox splits the dialog keys from the shared action options", async () => {
  const options = { title: "T", buttons: "yes-no" as const, icon: "warning" as const, timeout: 0.5, deadlineMs: 250 };
  await expect(ui.msgBox("yes/no", options)).resolves.toBe("Timeout");
  expect(msgCalls).toStrictEqual([
    ["yes/no", { title: "T", buttons: 4, icon: 0x30, timeout: 0.5, deadlineMs: 250 }],
  ]);
  // The caller's options object is not mutated by the split.
  expect(options).toStrictEqual({
    title: "T",
    buttons: "yes-no",
    icon: "warning",
    timeout: 0.5,
    deadlineMs: 250,
  });
});

test("msgBox rejects unknown buttons/icon/defaultIndex before the bridge runs", async () => {
  expect(() => ui.msgBox("x", { buttons: "nope" as never })).toThrow(TypeError);
  expect(() => ui.msgBox("x", { icon: "nope" as never })).toThrow(TypeError);
  expect(() => ui.msgBox("x", { defaultIndex: -1 })).toThrow(TypeError);
  expect(() => ui.msgBox("x", { defaultIndex: 1.5 })).toThrow(TypeError);
  expect(msgCalls).toEqual([]);
});

test("an explicitly undefined feature stays an absent key", async () => {
  await ui.msgBox("x", { title: undefined, buttons: "ok" });
  // toStrictEqual: `title: undefined` must not reach the bridge as a key.
  expect(msgCalls[0][1]).toStrictEqual({ buttons: 0 });
});

test("msgBox translates 0-based defaultIndex to the wire", async () => {
  await ui.msgBox("x", { defaultIndex: 0 });
  expect(msgCalls).toStrictEqual([["x", { defaultIndex: 1 }]]);
});

test("inputBox forwards the prompt, the feature keys and the outcome", async () => {
  const result = await ui.inputBox("type here", { password: true, value: "seed", width: 320 });
  expect(result).toStrictEqual({ value: "seed", result: "Timeout" });
  expect(inputCalls).toStrictEqual([
    ["type here", { password: true, value: "seed", width: 320 }],
  ]);
});

test("inputBox with no prompt still reaches the bridge as an absent argument", async () => {
  await ui.inputBox(undefined, { timeout: 1 });
  expect(inputCalls).toStrictEqual([[undefined, { timeout: 1 }]]);
});

test("toolTip translates the 0-based slot and rejects out-of-range", async () => {
  await expect(ui.toolTip("tip", { x: 10, y: 20, index: 2, cancellationId: 7 })).resolves.toBe(
    undefined,
  );
  // Slot 2 on the surface reaches the wire as slot 3.
  expect(tipCalls).toStrictEqual([["tip", { x: 10, y: 20, index: 3, cancellationId: 7 }]]);
  expect(() => ui.toolTip("tip", { index: 20 })).toThrow(TypeError);
  expect(() => ui.toolTip("tip", { index: -1 })).toThrow(TypeError);
});

test("traySetIcon translates the 0-based icon number", async () => {
  // freeze:false is a deliberate unfreeze: the native side keys off the
  // property's presence, so a facade that dropped false would silently
  // turn it into "freeze was never given".
  await ui.traySetIcon("C:\\tmp\\a.ico", { iconNumber: 1, freeze: false });
  expect(iconCalls).toStrictEqual([["C:\\tmp\\a.ico", { iconNumber: 2, freeze: false }]]);
  expect(() => ui.traySetIcon("C:\\tmp\\a.ico", { iconNumber: -1 })).toThrow(TypeError);
});

test("traySetIcon with no options forwards an empty options object", async () => {
  await ui.traySetIcon("");
  expect(iconCalls).toStrictEqual([["", {}]]);
});

test("trayTip forwards title/icon/mute and resolves undefined", async () => {
  await expect(
    ui.trayTip("body", { title: "T", icon: "info", mute: true, deadlineMs: 50 }),
  ).resolves.toBe(undefined);
  expect(balloonCalls).toStrictEqual([["body", { title: "T", icon: 0x40, mute: true, deadlineMs: 50 }]]);
  expect(() => ui.trayTip("body", { icon: "nope" as never })).toThrow(TypeError);
});

test("a coded bridge rejection becomes an ActionError with that code", async () => {
  rejection = {
    code: "capability_denied",
    message: "required capability was not granted: ui.create",
  };
  try {
    await ui.msgBox("x");
    throw new Error("msgBox must reject");
  } catch (error) {
    expect(error).toBeInstanceOf(ActionError);
    expect((error as ActionError).code).toBe("capability_denied");
    expect((error as ActionError).message).toContain("ui.create");
  }
});

test("a rejection without a code is rethrown unchanged", async () => {
  const raw = new Error("native exploded");
  rawRejection = raw;
  await expect(ui.msgBox("x")).rejects.toBe(raw);
});

test("a negative deadlineMs is rejected before the bridge runs", async () => {
  await expect(ui.msgBox("x", { deadlineMs: -1 })).rejects.toThrow(TypeError);
  expect(msgCalls).toEqual([]);
});
