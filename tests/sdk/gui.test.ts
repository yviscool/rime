import { beforeEach, expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type { GuiControlNative, GuiNative } from "../../sdk/src/gui";

// Realism: L3 - the real Gui/GuiControl facades run against an instrumented
// rime:ui bridge; the native module is the environment, never the unit
// under test. Method names, option shapes and sync/async splits are
// literals, so a facade that misroutes Add kinds, drops the trailing
// action options, or awaits synchronous identity reads cannot pass by
// agreeing with itself.

type AddCall = [string, string | undefined, unknown, NativeActionOptions | undefined];

let addCalls: AddCall[] = [];
let showCalls: Array<string | undefined> = [];
let destroyCalls = 0;

const madeControl: GuiControlNative = {
  Name: "ok",
  Type: "Button",
  ClassNN: "Button1",
  Text: async () => "OK",
  Value: async () => undefined,
  Enabled: async () => true,
  Visible: async () => true,
  Focus: async () => undefined,
  Move: async () => undefined,
  SetCue: async () => undefined,
};

const madeGui: GuiNative = {
  Add: async (type: string, options?: string, content?: unknown) => {
    addCalls.push([type, options, content, undefined]);
    return madeControl;
  },
  Destroy: async () => {
    destroyCalls++;
    return undefined;
  },
  Show: async (options?: string) => {
    showCalls.push(options);
    return undefined;
  },
  Hide: async () => undefined,
  Move: async () => undefined,
  Submit: async () => ({ ok: "pressed" }),
  OnEvent: async () => undefined,
  GetPos: async () => ({ x: 10, y: 20, width: 300, height: 200 }),
};

let createArgs: Array<[string | undefined, string | undefined]> = [];

mock.module("rime:ui", () => ({
  ui: {},
  createGui: async (options?: string, title?: string) => {
    createArgs.push([options, title]);
    return madeGui;
  },
}));

const { Gui } = await import("../../sdk/src/gui");

beforeEach(() => {
  addCalls = [];
  showCalls = [];
  destroyCalls = 0;
  createArgs = [];
});

test("create resolves a Gui; add routes kind/options/content", async () => {
  const gui = await Gui.create("+Resize", "Demo");
  expect(createArgs).toStrictEqual([["+Resize", "Demo"]]);
  const control = await gui.addButton("x10 y20", "OK");
  expect(control.name).toBe("ok");
  expect(control.type).toBe("Button");
  expect(control.classNN).toBe("Button1");
  expect(addCalls).toStrictEqual([["Button", "x10 y20", "OK", undefined]]);
});

test("generic add passes the kind through untouched", async () => {
  const gui = await Gui.create();
  await gui.add("Edit", "w200", "seed");
  expect(addCalls).toStrictEqual([["Edit", "w200", "seed", undefined]]);
});

test("show/submit/destroy forward; text reads through the pump", async () => {
  const gui = await Gui.create();
  await gui.show("Center");
  expect(showCalls).toStrictEqual(["Center"]);
  await expect(gui.submit()).resolves.toStrictEqual({ ok: "pressed" });
  const control = await gui.addText(undefined, "hello");
  await expect(control.text()).resolves.toBe("OK");
  await gui.destroy();
  expect(destroyCalls).toBe(1);
  await expect(gui.getPos()).resolves.toStrictEqual({ x: 10, y: 20, width: 300, height: 200 });
});
