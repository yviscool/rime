import { beforeEach, expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type { AutomationElement, FindQuery } from "../../sdk/src/automation";

// Realism: L3 - the real sdk/src/automation.ts facade runs against an
// instrumented rime:automation bridge; the native module is the environment,
// never the unit under test. Warning counts and forwarded query shapes are
// literals written by hand, so a facade that leaks `allowDesktopRoot` to the
// bridge or warns on scoped searches cannot pass by agreeing with itself.

type FindCall = [FindQuery, NativeActionOptions | undefined];

let findCalls: FindCall[] = [];
let warnCalls: string[] = [];
const realWarn = console.warn;

const element: AutomationElement = {
  id: 7,
  name: "OK",
  controlType: "button",
  automationId: "ok-btn",
  enabled: true,
  x: 10,
  y: 20,
  width: 80,
  height: 24,
};

const bridge = {
  async find(query: FindQuery, options?: NativeActionOptions) {
    findCalls.push([query, options]);
    return { elements: [element] };
  },
  async read(elementId: number) {
    return { ...element, id: elementId };
  },
  async invoke() {
    return { invoked: true as const };
  },
  release() {
    return true;
  },
};

mock.module("rime:automation", () => ({ automation: bridge }));

const { automation } = await import("../../sdk/src/automation");

beforeEach(() => {
  findCalls = [];
  warnCalls = [];
  console.warn = (...args: unknown[]) => {
    warnCalls.push(args.map(String).join(" "));
  };
});

test("scoped find forwards the query untouched and never warns", async () => {
  const result = await automation.find({ name: "OK", controlType: "button", fromId: 3 });
  expect(result).toStrictEqual({ elements: [element] });
  expect(findCalls).toStrictEqual([[{ name: "OK", controlType: "button", fromId: 3 }, undefined]]);
  expect(warnCalls).toStrictEqual([]);
  console.warn = realWarn;
});

test("desktop-root find warns exactly once and still runs", async () => {
  await automation.find({ name: "OK" });
  await automation.find({ name: "OK" });
  expect(findCalls.length).toBe(2);
  expect(warnCalls.length).toBe(1);
  expect(warnCalls[0]).toContain("without fromId");
  expect(warnCalls[0]).toContain("allowDesktopRoot");
  console.warn = realWarn;
});

test("explicit desktop opt-in runs silently but never leaks the key", async () => {
  await automation.find({ name: "OK", allowDesktopRoot: true });
  expect(findCalls).toStrictEqual([[{ name: "OK" }, undefined]]);
  expect(warnCalls).toStrictEqual([]);
  console.warn = realWarn;
});

test("read/invoke/release pass straight through to the bridge", async () => {
  await expect(automation.read(9)).resolves.toStrictEqual({ ...element, id: 9 });
  await expect(automation.invoke(9)).resolves.toStrictEqual({ invoked: true });
  expect(automation.release(9)).toBe(true);
  console.warn = realWarn;
});
