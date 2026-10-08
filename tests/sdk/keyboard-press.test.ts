import { beforeEach, expect, mock, test } from "bun:test";
import type { SendKeyStep } from "../../sdk/src/send";

// keyboard.press builds structured chords into SendInput batches without
// touching the Send string grammar, so the facade is imported against a
// scripted rime:input mock (same pattern as keyboard-lock.test.ts).

let sentSteps: SendKeyStep[][] = [];

const idleModifiers = {
  lcontrol: false,
  rcontrol: false,
  lshift: false,
  rshift: false,
  lalt: false,
  ralt: false,
  lwin: false,
  rwin: false,
};

mock.module("rime:input", () => ({
  input: {
    modifiers: () => ({ ...idleModifiers }),
    send: (steps: SendKeyStep[]): Promise<{ sent: number }> => {
      sentSteps.push(steps.map((step) => ({ ...step })));
      return Promise.resolve({ sent: steps.length });
    },
  },
}));

const { keyboard } = await import("../../sdk/src/input");

beforeEach(() => {
  sentSteps = [];
});

const VK_C = 0x43;
const VK_CONTROL = 0x11;
const VK_SHIFT = 0x10;
const VK_F2 = 0x71;
const VK_RETURN = 0x0d;

test("a chord taps down+up with modifiers held around it", async () => {
  await expect(keyboard.press({ key: "c", ctrl: true })).resolves.toStrictEqual({ sent: 4 });
  expect(sentSteps).toStrictEqual([
    [
      { vk: VK_CONTROL, down: true },
      { vk: VK_C, down: true },
      { vk: VK_C, down: false },
      { vk: VK_CONTROL, down: false },
    ],
  ]);
});

test("named keys resolve case-insensitively; single strings tap bare", async () => {
  await keyboard.press(["F2", "Enter"]);
  expect(sentSteps).toStrictEqual([
    [
      { vk: VK_F2, down: true },
      { vk: VK_F2, down: false },
      { vk: VK_RETURN, down: true },
      { vk: VK_RETURN, down: false },
    ],
  ]);
});

test("uppercase chars carry their own shift; explicit shift merges", async () => {
  await keyboard.press("C");
  expect(sentSteps).toStrictEqual([
    [
      { vk: VK_SHIFT, down: true },
      { vk: VK_C, down: true },
      { vk: VK_C, down: false },
      { vk: VK_SHIFT, down: false },
    ],
  ]);
});

test("unknown keys reject before the bridge runs; empty input sends nothing", () => {
  expect(() => keyboard.press("NotAKey")).toThrow(TypeError);
  expect(() => keyboard.press("")).toThrow(TypeError);
  expect(sentSteps).toStrictEqual([]);
});

test("an empty chord list resolves zero without touching the device", async () => {
  await expect(keyboard.press([])).resolves.toStrictEqual({ sent: 0 });
  expect(sentSteps).toStrictEqual([]);
});
