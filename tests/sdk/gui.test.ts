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
let eventCalls: Array<[string, unknown, unknown]> = [];

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
  OnEvent: async (name: string, fn: unknown, addRemove?: number) => {
    eventCalls.push([name, fn, addRemove]);
    return undefined;
  },
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
  eventCalls = [];
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

test("structured options compile to the wire string; bad shapes throw first", async () => {
  const gui = await Gui.create();
  await gui.addEdit({ x: 10, y: 20, w: 200, name: "MyEdit" }, "seed");
  await gui.addCheckBox({ hidden: true, disabled: false });
  expect(addCalls).toStrictEqual([
    ["Edit", "x10 y20 w200 vMyEdit", "seed", undefined],
    ["CheckBox", "+Hidden -Disabled", undefined, undefined],
  ]);
  expect(() => gui.addText({ x: 1.5 })).toThrow(TypeError);
  expect(() => gui.addText({ name: "" })).toThrow(TypeError);
  expect(addCalls).toHaveLength(2);
});

test("offEvent removes by reference where onEvent hides the slot", async () => {
  const gui = await Gui.create();
  const fn = () => undefined;
  await gui.onEvent("Close", fn);
  await gui.offEvent("Close", fn);
  expect(eventCalls).toStrictEqual([
    ["Close", fn, undefined],
    ["Close", fn, 0],
  ]);
});

test("generic add passes the kind through untouched", async () => {
  const gui = await Gui.create();
  await gui.add("Edit", "w200", "seed");
  expect(addCalls).toStrictEqual([["Edit", "w200", "seed", undefined]]);
});

test("list sugars forward arrays; AddDDL aliases DropDownList", async () => {
  const gui = await Gui.create();
  await gui.addListBox({ x: 1 }, ["a", "b"]);
  await gui.addDDL(undefined, ["m"]);
  expect(addCalls).toStrictEqual([
    ["ListBox", "x1", ["a", "b"], undefined],
    ["DropDownList", undefined, ["m"], undefined],
  ]);
});

test("date/slider sugars forward content untouched", async () => {
  const gui = await Gui.create();
  await gui.addDateTime({ x: 1 }, "20261008");
  await gui.addSlider(undefined, 42);
  await gui.addUpDown(undefined, 7);
  await gui.addMonthCal(undefined, "20260228");
  expect(addCalls).toStrictEqual([
    ["DateTime", "x1", "20261008", undefined],
    ["Slider", undefined, 42, undefined],
    ["UpDown", undefined, 7, undefined],
    ["MonthCal", undefined, "20260228", undefined],
  ]);
});

test("rich sugars forward kinds, aliases resolve canonically", async () => {
  const gui = await Gui.create();
  await gui.addTab(undefined, ["T1", "T2"]);
  await gui.addTab2(undefined, ["U1"]);
  await gui.addListView(undefined, ["A"]);
  await gui.addTreeView(undefined, ["r"]);
  await gui.addStatusBar(undefined, ["s"]);
  await gui.addLink(undefined, "go");
  await gui.addHotkey(undefined);
  await gui.addPic(undefined, "C:\\x.bmp");
  expect(addCalls).toStrictEqual([
    ["Tab", undefined, ["T1", "T2"], undefined],
    ["Tab", undefined, ["U1"], undefined],
    ["ListView", undefined, ["A"], undefined],
    ["TreeView", undefined, ["r"], undefined],
    ["StatusBar", undefined, ["s"], undefined],
    ["Link", undefined, "go", undefined],
    ["Hotkey", undefined, undefined, undefined],
    ["Picture", undefined, "C:\\x.bmp", undefined],
  ]);
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
