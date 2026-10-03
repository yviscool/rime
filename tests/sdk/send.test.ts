import { beforeEach, expect, mock, test } from "bun:test";
import type { ModifiersSnapshot, MousePayload } from "../../sdk/src/input";
import { compileSend, MOD_LCONTROL, type SendKeyStep } from "../../sdk/src/send";

// The facade reads `rime:input` through a static import (keyboard.modifiers
// must throw synchronously), so the mock has to exist before input.ts loads:
// register it here, then pull the facade in with a dynamic import below.
const allFalse = (): ModifiersSnapshot => ({
  lcontrol: false,
  rcontrol: false,
  lshift: false,
  rshift: false,
  lalt: false,
  ralt: false,
  lwin: false,
  rwin: false,
  capsLock: false,
});

let modifierSnapshot: ModifiersSnapshot = allFalse();
let modifiersShouldThrow = false;

const sendCalls: Array<{ steps: SendKeyStep[]; options?: unknown }> = [];
const mouseCalls: Array<{ payload: MousePayload; options?: unknown }> = [];
const posCalls: unknown[] = [];

type SendImpl = (steps: SendKeyStep[], options?: unknown) => Promise<{ sent: number }>;
type MouseImpl = (payload: MousePayload, options?: unknown) => Promise<{ sent: number }>;
type PosImpl = (options?: unknown) => Promise<unknown>;

const recordSend: SendImpl = async (steps, options) => {
  sendCalls.push({ steps, options });
  return { sent: steps.length };
};
let sendImpl: SendImpl = recordSend;
let mouseImpl: MouseImpl = async (payload, options) => {
  mouseCalls.push({ payload, options });
  return { sent: payload.steps.length };
};
let posImpl: PosImpl = async (options) => {
  posCalls.push(options);
  return { x: 4, y: 5, window: null, control: null };
};

mock.module("rime:input", () => ({
  input: {
    subscribe: () => 1,
    unsubscribe: () => true,
    bind: () => 1,
    unbind: () => true,
    send: (steps: SendKeyStep[], options?: unknown) => sendImpl(steps, options),
    modifiers: (): ModifiersSnapshot => {
      if (modifiersShouldThrow) {
        throw new Error("required capability was not granted: windows.input.inject");
      }
      return modifierSnapshot;
    },
    mouse: (payload: MousePayload, options?: unknown) => mouseImpl(payload, options),
    mouseGetPos: (options?: unknown) => posImpl(options),
  },
}));

const { keyboard, mouse } = await import("../../sdk/src/input");

beforeEach(() => {
  modifierSnapshot = allFalse();
  modifiersShouldThrow = false;
  sendImpl = recordSend;
  mouseImpl = async (payload, options) => {
    mouseCalls.push({ payload, options });
    return { sent: payload.steps.length };
  };
  posImpl = async (options) => {
    posCalls.push(options);
    return { x: 4, y: 5, window: null, control: null };
  };
  sendCalls.length = 0;
  mouseCalls.length = 0;
  posCalls.length = 0;
});

const ctx = (state = 0, persistent = 0) => ({ state, persistent });

// ---- compileSend: grammar units ----

test("compileSend: plain ascii becomes vk down/up pairs", () => {
  const result = compileSend("a", ctx());
  expect(result.steps).toEqual([
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
  ]);
  expect(result.persistent).toBe(0);
  expect(result.endState).toBe(0);
});

test("compileSend: prefixes press and release the matching modifier", () => {
  expect(compileSend("^a", ctx()).steps).toEqual([
    { vk: 0xa2, down: true },
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
    { vk: 0xa2, down: false },
  ]);
  expect(compileSend("+A", ctx()).steps).toEqual([
    { vk: 0xa0, down: true },
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
    { vk: 0xa0, down: false },
  ]);
  expect(compileSend("!a", ctx()).steps).toEqual([
    { vk: 0xa4, down: true },
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
    { vk: 0xa4, down: false },
  ]);
  expect(compileSend("#a", ctx()).steps).toEqual([
    { vk: 0x5b, down: true },
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
    { vk: 0x5b, down: false },
  ]);
});

test("compileSend: without {Blind} a held modifier is released around the key and restored", () => {
  const result = compileSend("a", ctx(MOD_LCONTROL));
  expect(result.steps).toEqual([
    { vk: 0xa2, down: false },
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
    { vk: 0xa2, down: true },
  ]);
  expect(result.endState).toBe(MOD_LCONTROL);
  expect(result.persistent).toBe(0);
});

test("compileSend: {Blind} keeps the held modifier down", () => {
  const result = compileSend("{Blind}a", ctx(MOD_LCONTROL));
  expect(result.steps).toEqual([
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
  ]);
  expect(result.endState).toBe(MOD_LCONTROL);
});

test("compileSend: {Text} emits unicode packets for every character", () => {
  const result = compileSend("{Text}\u4f60\u597d", ctx());
  expect(result.steps).toEqual([
    { vk: 0x4f60, down: true, unicode: true },
    { vk: 0x4f60, down: false, unicode: true },
    { vk: 0x597d, down: true, unicode: true },
    { vk: 0x597d, down: false, unicode: true },
  ]);
});

test("compileSend: text mode sends ascii as unicode too", () => {
  const result = compileSend("a", ctx(), "text");
  expect(result.steps).toEqual([
    { vk: 0x61, down: true, unicode: true },
    { vk: 0x61, down: false, unicode: true },
  ]);
});

test("compileSend: characters outside the US table inject as unicode", () => {
  const result = compileSend("\u00e9", ctx());
  expect(result.steps).toEqual([
    { vk: 0xe9, down: true, unicode: true },
    { vk: 0xe9, down: false, unicode: true },
  ]);
});

test("compileSend: {Raw} treats specials as literal text", () => {
  const result = compileSend("{Raw}^", ctx());
  expect(result.steps).toEqual([
    { vk: 0xa0, down: true },
    { vk: 0x36, down: true },
    { vk: 0x36, down: false },
    { vk: 0xa0, down: false },
  ]);
});

test("compileSend: raw mode compiles specials literally", () => {
  const result = compileSend("^a", ctx(), "raw");
  expect(result.steps).toEqual([
    { vk: 0xa0, down: true },
    { vk: 0x36, down: true },
    { vk: 0x36, down: false },
    { vk: 0xa0, down: false },
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
  ]);
});

test("compileSend: {Ctrl down} persists across calls until {Ctrl up}", () => {
  const down = compileSend("{Ctrl down}", ctx());
  expect(down.steps).toEqual([{ vk: 0x11, down: true }, { vk: 0xa2, down: true }]);
  expect(down.persistent).toBe(MOD_LCONTROL);

  const held = compileSend("a", ctx(MOD_LCONTROL, down.persistent));
  expect(held.steps).toEqual([
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
  ]);
  expect(held.persistent).toBe(MOD_LCONTROL);

  const up = compileSend("{Ctrl up}", ctx(MOD_LCONTROL, down.persistent));
  expect(up.steps).toEqual([{ vk: 0x11, down: false }]);
  expect(up.persistent).toBe(0);
});

test("compileSend: {Enter 3} repeats the tap", () => {
  const result = compileSend("{Enter 3}", ctx());
  expect(result.steps).toHaveLength(6);
  for (const step of result.steps) expect(step.vk).toBe(0x0d);
});

test("compileSend: CRLF collapses to one Enter", () => {
  expect(compileSend("\r\n", ctx()).steps).toEqual([
    { vk: 0x0d, down: true },
    { vk: 0x0d, down: false },
  ]);
});

test("compileSend: {}} sends a literal closing brace", () => {
  const result = compileSend("{}}", ctx());
  expect(result.steps).toEqual([
    { vk: 0xa0, down: true },
    { vk: 0xdd, down: true },
    { vk: 0xdd, down: false },
    { vk: 0xa0, down: false },
  ]);
});

test("compileSend: unsupported items throw TypeError", () => {
  expect(() => compileSend("{Click}", ctx())).toThrow(TypeError);
  expect(() => compileSend("{ASC 65}", ctx())).toThrow(TypeError);
  expect(() => compileSend("{U+4F60}", ctx())).toThrow(TypeError);
  expect(() => compileSend("{LButton}", ctx())).toThrow(TypeError);
  expect(() => compileSend("a{Blind}", ctx())).toThrow(TypeError);
  expect(() => compileSend("{unknownkey}", ctx())).toThrow(TypeError);
});

test("compileSend: brace errors throw TypeError", () => {
  expect(() => compileSend("{a", ctx())).toThrow(TypeError);
  expect(() => compileSend("{}", ctx())).toThrow(TypeError);
  expect(() => compileSend(42 as unknown as string, ctx())).toThrow(TypeError);
});

// ---- keyboard facade glue ----

test("keyboard.send compiles through the Send grammar and forwards steps", async () => {
  const result = await keyboard.send("^a");
  expect(result).toEqual({ sent: 4 });
  expect(sendCalls).toHaveLength(1);
  expect(sendCalls[0]!.steps).toEqual([
    { vk: 0xa2, down: true },
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
    { vk: 0xa2, down: false },
  ]);
  expect(sendCalls[0]!.options).toEqual({});
});

test("keyboard.send forwards native options", async () => {
  await keyboard.send("a", { deadlineMs: 250 });
  expect(sendCalls[0]!.options).toEqual({ deadlineMs: 250 });
});

test("keyboard.send rejects an invalid mode synchronously", () => {
  expect(() => keyboard.send("a", { mode: "bogus" as never })).toThrow(TypeError);
  expect(sendCalls).toHaveLength(0);
});

test("keyboard.send throws synchronously when the capability read fails", () => {
  modifiersShouldThrow = true;
  expect(() => keyboard.send("a")).toThrow(/windows\.input\.inject/);
  expect(sendCalls).toHaveLength(0);
});

test("keyboard.modifiers passes the snapshot through and throws without the capability", () => {
  modifierSnapshot = { ...allFalse(), lcontrol: true, capsLock: true };
  expect(keyboard.modifiers()).toEqual(modifierSnapshot);
  modifiersShouldThrow = true;
  expect(() => keyboard.modifiers()).toThrow(/windows\.input\.inject/);
});

test("keyboard.send resolves { sent: 0 } for an empty string without touching the bridge", async () => {
  const result = await keyboard.send("");
  expect(result).toEqual({ sent: 0 });
  expect(sendCalls).toHaveLength(0);
});

test("keyboard.send does not store the persistent set when the bridge rejects", async () => {
  sendImpl = async () => {
    throw Object.assign(new Error("boom"), { code: "invalid_state" });
  };
  const failure = (await keyboard.send("{Ctrl down}").catch((error: unknown) => error)) as {
    name?: string;
    code?: string;
  };
  expect(failure.name).toBe("ActionError");
  expect(failure.code).toBe("invalid_state");
  sendImpl = recordSend;
  await keyboard.send("a");
  expect(sendCalls.at(-1)!.steps).toEqual([
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
  ]);
});

test("keyboard.send carries {Ctrl down} across calls until released", async () => {
  await keyboard.send("{Ctrl down}");
  expect(sendCalls[0]!.steps).toEqual([{ vk: 0x11, down: true }, { vk: 0xa2, down: true }]);
  modifierSnapshot = { ...allFalse(), lcontrol: true };
  await keyboard.send("a");
  expect(sendCalls[1]!.steps).toEqual([
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
  ]);
  await keyboard.send("{Ctrl up}");
  expect(sendCalls[2]!.steps).toEqual([{ vk: 0x11, down: false }]);
  modifierSnapshot = allFalse();
  await keyboard.send("a");
  expect(sendCalls[3]!.steps).toEqual([
    { vk: 0x41, down: true },
    { vk: 0x41, down: false },
  ]);
});

test("keyboard.sendText emits unicode packets", async () => {
  await keyboard.sendText("hi");
  expect(sendCalls[0]!.steps).toEqual([
    { vk: 0x68, down: true, unicode: true },
    { vk: 0x68, down: false, unicode: true },
    { vk: 0x69, down: true, unicode: true },
    { vk: 0x69, down: false, unicode: true },
  ]);
});

test("sendInput/sendEvent/sendPlay compile the full grammar like send", async () => {
  await keyboard.send("^a");
  await keyboard.sendInput("^a");
  await keyboard.sendEvent("^a");
  await keyboard.sendPlay("^a");
  expect(sendCalls).toHaveLength(4);
  const expected = sendCalls[0]!.steps;
  expect(expected).toHaveLength(4);
  for (const call of sendCalls) expect(call.steps).toEqual(expected);
});

// ---- mouse facade glue ----

test("mouse.move sends one absolute step and validates coordinates", async () => {
  const result = await mouse.move(10, 20);
  expect(result).toEqual({ sent: 1 });
  expect(mouseCalls[0]!.payload).toEqual({ steps: [{ action: "move", x: 10, y: 20 }] });
  expect(() => mouse.move(10.5, 20)).toThrow(TypeError);
  expect(() => mouse.move(10, 20.5)).toThrow(TypeError);
  expect(() => mouse.move(Number.NaN, 20)).toThrow(TypeError);
  expect(() => mouse.move(10, 20, { speed: 101 })).toThrow(TypeError);
  expect(() => mouse.move(10, 20, { speed: -1 })).toThrow(TypeError);
  expect(() => mouse.move(10, 20, { speed: 1.5 })).toThrow(TypeError);
  expect(mouseCalls).toHaveLength(1);
});

test("mouse speed is validated then carried; 0 and 50 share one move step", async () => {
  await mouse.move(1, 2, { speed: 0 });
  await mouse.move(1, 2, { speed: 50 });
  expect(mouseCalls[0]!.payload).toEqual({
    steps: [{ action: "move", x: 1, y: 2 }],
    speed: 0,
  });
  expect(mouseCalls[1]!.payload).toEqual({
    steps: [{ action: "move", x: 1, y: 2 }],
    speed: 50,
  });
  expect(mouseCalls[0]!.payload.steps).toHaveLength(1);
  expect(mouseCalls[1]!.payload.steps).toHaveLength(1);
});

test("mouse.click builds down/up pairs and honors count", async () => {
  await mouse.click();
  expect(mouseCalls[0]!.payload.steps).toEqual([
    { action: "down", button: 1 },
    { action: "up", button: 1 },
  ]);
  await mouse.click({ button: 2, count: 2 });
  expect(mouseCalls[1]!.payload.steps).toEqual([
    { action: "down", button: 2 },
    { action: "up", button: 2 },
    { action: "down", button: 2 },
    { action: "up", button: 2 },
  ]);
  await mouse.click({ x: 5, y: 6 });
  expect(mouseCalls[2]!.payload.steps).toEqual([
    { action: "move", x: 5, y: 6 },
    { action: "down", button: 1 },
    { action: "up", button: 1 },
  ]);
});

test("mouse.click with count below 1 does nothing", async () => {
  expect(await mouse.click({ count: 0 })).toEqual({ sent: 0 });
  expect(await mouse.click({ count: -5 })).toEqual({ sent: 0 });
  expect(mouseCalls).toHaveLength(0);
});

test("mouse.click rejects partial points and bad buttons", () => {
  expect(() => mouse.click({ x: 5 })).toThrow(TypeError);
  expect(() => mouse.click({ y: 6 })).toThrow(TypeError);
  expect(() => mouse.click({ button: 4 as never })).toThrow(TypeError);
  expect(mouseCalls).toHaveLength(0);
});

test("mouse.drag orders move/down/move/up", async () => {
  await mouse.drag({ to: { x: 9, y: 9 } });
  expect(mouseCalls[0]!.payload.steps).toEqual([
    { action: "down", button: 1 },
    { action: "move", x: 9, y: 9 },
    { action: "up", button: 1 },
  ]);
  await mouse.drag({ x: 1, y: 2, to: { x: 9, y: 9 }, button: 3 });
  expect(mouseCalls[1]!.payload.steps).toEqual([
    { action: "move", x: 1, y: 2 },
    { action: "down", button: 3 },
    { action: "move", x: 9, y: 9 },
    { action: "up", button: 3 },
  ]);
});

test("mouse.drag requires options with to", () => {
  expect(() => mouse.drag(undefined as never)).toThrow(TypeError);
  expect(() => mouse.drag({ to: undefined as never })).toThrow(TypeError);
  expect(() => mouse.drag({ to: { x: 1 } as never })).toThrow(TypeError);
  expect(mouseCalls).toHaveLength(0);
});

test("mouse.getPos resolves the read and forwards options", async () => {
  const result = await mouse.getPos({ deadlineMs: 100 });
  expect(result).toEqual({ x: 4, y: 5, window: null, control: null });
  expect(posCalls[0]).toEqual({ deadlineMs: 100 });
});

test("mouse.getPos maps the capability rejection to ActionError", async () => {
  posImpl = async () => {
    throw Object.assign(new Error("required capability was not granted: windows.input.read"), {
      code: "capability_denied",
    });
  };
  const failure = (await mouse.getPos().catch((error: unknown) => error)) as {
    name?: string;
    code?: string;
  };
  expect(failure.name).toBe("ActionError");
  expect(failure.code).toBe("capability_denied");
});
