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
  Add: async () => 0,
  Choose: async () => 0,
  Delete: async () => 0,
  Insert: async () => 0,
  Modify: async () => 0,
  GetCount: async () => 0,
  GetText: async () => "",
  GetNext: async () => null,
  InsertCol: async () => 0,
  ModifyCol: async () => 0,
  DeleteCol: async () => 0,
  Get: async () => false,
  GetChild: async () => null,
  GetParent: async () => null,
  GetPrev: async () => null,
  GetSelection: async () => null,
  SetParts: async () => 0,
  SetText: async () => 0,
  SetIcon: async () => 0,
  UseTab: async () => 0,
  SetFormat: async () => undefined,
  SetHotkey: async () => undefined,
  SetImageList: async () => undefined,
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
  MenuBar: Promise.resolve(null),
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
const { GuiControl } = await import("../../sdk/src/gui");

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

test("3b control methods forward positionals and map misses to null", async () => {
  const calls: Array<[string, unknown[]]> = [];
  const recording: GuiControlNative = {
    ...madeControl,
    Add: async (...args: unknown[]) => {
      calls.push(["Add", args.slice(0, -1)]);
      return 2;
    },
    Choose: async (...args: unknown[]) => {
      calls.push(["Choose", args.slice(0, -1)]);
      return 1;
    },
    Delete: async (...args: unknown[]) => {
      calls.push(["Delete", args.slice(0, -1)]);
      return 0;
    },
    GetNext: async (...args: unknown[]) => {
      calls.push(["GetNext", args.slice(0, -1)]);
      return null;
    },
    GetChild: async (...args: unknown[]) => {
      calls.push(["GetChild", args.slice(0, -1)]);
      return 7;
    },
    Get: async (...args: unknown[]) => {
      calls.push(["Get", args.slice(0, -1)]);
      return true;
    },
    SetParts: async (...args: unknown[]) => {
      calls.push(["SetParts", args.slice(0, -1)]);
      return 2;
    },
    UseTab: async (...args: unknown[]) => {
      calls.push(["UseTab", args.slice(0, -1)]);
      return 0;
    },
    SetHotkey: async (...args: unknown[]) => {
      calls.push(["SetHotkey", args.slice(0, -1)]);
      return undefined;
    },
  };
  const control = new GuiControl(recording);
  await expect(control.add(["a", "b"], true)).resolves.toBe(2);
  await expect(control.choose("d")).resolves.toBe(1);
  await expect(control.deleteItem()).resolves.toBe(0);
  await expect(control.getNext()).resolves.toBeNull();
  await expect(control.getChild(3)).resolves.toBe(7);
  await expect(control.get(3, "expanded")).resolves.toBe(true);
  await expect(control.setParts([100, -1])).resolves.toBe(2);
  await expect(control.useTab("T1")).resolves.toBe(0);
  await expect(control.setHotkey(["ctrl"], "A")).resolves.toBeUndefined();
  expect(calls).toStrictEqual([
    ["Add", [["a", "b"], true, undefined, undefined, undefined, undefined]],
    ["Choose", ["d"]],
    ["Delete", [undefined]],
    ["GetNext", [undefined, undefined]],
    ["GetChild", [3]],
    ["Get", [3, "expanded"]],
    ["SetParts", [[100, -1]]],
    ["UseTab", ["T1", undefined]],
    ["SetHotkey", [["ctrl"], "A"]],
  ]);
});

test("menu wrappers forward positionals and map null misses", async () => {
  const calls: Array<[string, unknown[]]> = [];
  const { Menu } = await import("../../sdk/src/gui");
  const recording = {
    Add: async (...args: unknown[]) => {
      calls.push(["Add", args.slice(0, -1)]);
      return 0;
    },
    Insert: async (...args: unknown[]) => {
      calls.push(["Insert", args.slice(0, -1)]);
      return 1;
    },
    Delete: async (...args: unknown[]) => {
      calls.push(["Delete", args.slice(0, -1)]);
      return "gone";
    },
    Rename: async (...args: unknown[]) => {
      calls.push(["Rename", args.slice(0, -1)]);
      return "old";
    },
    Check: async () => undefined,
    Uncheck: async () => undefined,
    Enable: async () => undefined,
    Disable: async () => undefined,
    ToggleCheck: async () => true,
    ToggleEnable: async () => false,
    SetColor: async (...args: unknown[]) => {
      calls.push(["SetColor", args.slice(0, -1)]);
      return undefined;
    },
    SetIcon: async () => undefined,
    Show: async () => null,
    Destroy: async () => undefined,
    ClickCount: Promise.resolve(2),
    Default: Promise.resolve("alpha"),
  };
  const menu = Menu.fromNative(recording as never);
  await expect(menu.add("alpha", () => {})).resolves.toBe(0);
  await expect(menu.insert("beta", "mid")).resolves.toBe(1);
  await expect(menu.delete("old")).resolves.toBe("gone");
  await expect(menu.rename("mid", "middle")).resolves.toBe("old");
  await expect(menu.toggleCheck("a")).resolves.toBe(true);
  await expect(menu.toggleEnable("a")).resolves.toBe(false);
  await expect(menu.setColor("#FF0000")).resolves.toBeUndefined();
  await expect(menu.show()).resolves.toBeNull();
  await expect(menu.getClickCount()).resolves.toBe(2);
  await expect(menu.getDefault()).resolves.toBe("alpha");
  expect(calls).toStrictEqual([
    ["Add", ["alpha", expect.anything()]],
    ["Insert", ["beta", "mid", undefined]],
    ["Delete", ["old"]],
    ["Rename", ["mid", "middle"]],
    ["SetColor", ["#FF0000", undefined]],
  ]);
});

test("imagelist wrappers forward file/index and list attach", async () => {
  const calls: Array<[string, unknown[]]> = [];
  const { ImageList } = await import("../../sdk/src/gui");
  const recording = {
    Add: async (...args: unknown[]) => {
      calls.push(["Add", args.slice(0, -1)]);
      return 0;
    },
    Destroy: async () => undefined,
  };
  const list = ImageList.fromNative(recording as never);
  await expect(list.add("x.ico", 3)).resolves.toBe(0);
  await expect(list.destroy()).resolves.toBeUndefined();
  expect(calls).toStrictEqual([["Add", ["x.ico", 3]]]);
});
