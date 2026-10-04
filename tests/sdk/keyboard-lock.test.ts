import { beforeEach, expect, mock, test } from "bun:test";
import type { LockKeyName, LockStateWord } from "../../sdk/src/input";
import type { SendKeyStep } from "../../sdk/src/send";

// keyboard.setLockState orchestrates on top of the native bridge (force
// first, LED read, tap steps), so the facade is imported against a scripted
// rime:input mock: register it here, pull the facade in dynamically below
// (same pattern as send.test.ts, where the synchronous capability throws
// require the mock to exist before input.ts loads).

const lockKeys: Record<string, number> = { capslock: 0x14, numlock: 0x90, scrolllock: 0x91 };

let toggle: Record<string, boolean> = {};
let held = false;
let forceCalls: Array<[string, string]> = [];
let stateCalls: Array<[string, string]> = [];
let sentSteps: SendKeyStep[][] = [];
let forceThrows: Error | null = null;
let stateThrows: Error | null = null;
let sendThrows: Error | null = null;

mock.module("rime:input", () => ({
  input: {
    setLockForce: (keyName: string, force: "on" | "off" | "neutral") => {
      if (forceThrows) throw forceThrows;
      forceCalls.push([keyName, force]);
    },
    getKeyState: (keyName: string, mode?: string) => {
      if (stateThrows) throw stateThrows;
      stateCalls.push([keyName, mode ?? "l"]);
      if ((mode ?? "l")[0] === "t" || (mode ?? "l")[0] === "T") return toggle[keyName] === true;
      return held;
    },
    getKeyVK: (keyName: string) => lockKeys[keyName] ?? 0,
    send: (steps: SendKeyStep[]): Promise<{ sent: number }> => {
      if (sendThrows) throw sendThrows; // the native bridge's capability gate throws synchronously
      sentSteps.push(steps.map((step) => ({ ...step })));
      return Promise.resolve({ sent: steps.length });
    },
  },
}));

const { keyboard } = await import("../../sdk/src/input");

beforeEach(() => {
  toggle = { capslock: false, numlock: false, scrolllock: false };
  held = false;
  forceCalls = [];
  stateCalls = [];
  sentSteps = [];
  forceThrows = null;
  stateThrows = null;
  sendThrows = null;
});

test("setLockState rejects malformed arguments synchronously", () => {
  expect(() => (keyboard.setLockState as (k: unknown, s?: unknown) => unknown)(42, "on")).toThrow(
    TypeError,
  );
  expect(() => (keyboard.setLockState as (k: unknown, s?: unknown) => unknown)("a", "on")).toThrow(
    TypeError,
  );
  expect(() => (keyboard.setLockState as (k: unknown, s?: unknown) => unknown)("f1", "on")).toThrow(
    TypeError,
  );
  expect(() => keyboard.setLockState("capslock", "sometimes" as never)).toThrow(TypeError);
  expect(() => (keyboard.setLockState as (k: unknown, s?: unknown) => unknown)("capslock", 5)).toThrow(
    TypeError,
  );
  expect(forceCalls).toHaveLength(0);
  expect(sentSteps).toHaveLength(0);
});

test("omitted state only clears the force and never taps", async () => {
  const result = await keyboard.setLockState("capslock");
  expect(result).toEqual({ changed: false });
  expect(forceCalls).toEqual([["capslock", "neutral"]]);
  expect(stateCalls).toHaveLength(0);
  expect(sentSteps).toHaveLength(0);
  const blank = await keyboard.setLockState("numlock", "");
  expect(blank).toEqual({ changed: false });
  expect(forceCalls[1]).toEqual(["numlock", "neutral"]);
});

test("on clears the force before the LED read and skips a settled tap", async () => {
  toggle.capslock = true;
  const result = await keyboard.setLockState("capslock", "on");
  expect(result).toEqual({ changed: false });
  expect(forceCalls[0]).toEqual(["capslock", "neutral"]);
  expect(stateCalls).toEqual([["capslock", "t"]]);
  expect(sentSteps).toHaveLength(0);
});

test("on taps down/up when the toggle differs, force cleared first", async () => {
  const result = await keyboard.setLockState("CapsLock" as LockKeyName, "ON" as LockStateWord);
  expect(result).toEqual({ changed: true });
  expect(forceCalls).toEqual([["capslock", "neutral"]]);
  expect(stateCalls[0]).toEqual(["capslock", "t"]);
  expect(stateCalls[1]).toEqual(["capslock", "l"]);
  expect(sentSteps).toEqual([[{ vk: 0x14, down: true }, { vk: 0x14, down: false }]]);
});

test("a held lock key is released before the tap", async () => {
  toggle.scrolllock = true;
  held = true;
  const result = await keyboard.setLockState("scrolllock", "off");
  expect(result).toEqual({ changed: true });
  expect(sentSteps).toEqual([
    [
      { vk: 0x91, down: false },
      { vk: 0x91, down: true },
      { vk: 0x91, down: false },
    ],
  ]);
});

test("alwaysOn arms the force before the LED read", async () => {
  const result = await keyboard.setLockState("numlock", "alwaysOn");
  expect(result).toEqual({ changed: true });
  expect(forceCalls).toEqual([["numlock", "on"]]);
  expect(stateCalls[0]).toEqual(["numlock", "t"]);
  expect(sentSteps).toEqual([[{ vk: 0x90, down: true }, { vk: 0x90, down: false }]]);
});

test("alwaysOff maps to the off force and stays quiet when settled", async () => {
  toggle.scrolllock = false;
  const result = await keyboard.setLockState("scrolllock", "AlwaysOff" as LockStateWord);
  expect(result).toEqual({ changed: false });
  expect(forceCalls).toEqual([["scrolllock", "off"]]);
  expect(sentSteps).toHaveLength(0);
});

test("capability failures surface synchronously from the call", () => {
  forceThrows = new Error("required capability was not granted: windows.hook.global");
  expect(() => keyboard.setLockState("capslock", "alwaysOn")).toThrow(/windows\.hook\.global/);
  expect(stateCalls).toHaveLength(0);

  forceThrows = null;
  stateThrows = new Error("required capability was not granted: windows.input.read");
  expect(() => keyboard.setLockState("capslock", "on")).toThrow(/windows\.input\.read/);
  expect(sentSteps).toHaveLength(0);

  stateThrows = null;
  sendThrows = new Error("required capability was not granted: windows.input.inject");
  expect(() => keyboard.setLockState("capslock", "on")).toThrow(/windows\.input\.inject/);
  expect(sentSteps).toHaveLength(0);
});
