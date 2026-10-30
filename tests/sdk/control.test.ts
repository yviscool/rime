import { beforeEach, expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type { ControlId } from "../../sdk/src/control";
import { Control } from "../../sdk/src/control";

// Realism: L3 - the real Control class runs against an instrumented
// rime:control bridge speaking native 1-based wire numbering; the SDK is
// the environment, never the unit under test. Every forwarded index is a
// literal, so a facade that leaks 1-based numbering or the 0-sentinel to
// callers cannot pass by agreeing with itself.

const calls: Array<{ method: string; index?: number; strategy?: string }> = [];

const bridge = {
  async click(_id: ControlId, opts?: any, options?: any) {
    calls.push({ method: "click", strategy: opts?.strategy ?? options?.strategy });
    return { clicks: 1 };
  },
  async focus(_id: ControlId, options?: any) {
    calls.push({ method: "focus", strategy: options?.strategy });
    return { focused: true as const };
  },
  async setText(_id: ControlId, text: string, options?: any) {
    calls.push({ method: "setText", strategy: options?.strategy });
    return { text };
  },
  async getText(_id: ControlId, options?: any) {
    calls.push({ method: "getText", strategy: options?.strategy });
    return { text: "hello" };
  },
  async sendText(_id: ControlId, text: string, options?: any) {
    calls.push({ method: "sendText", strategy: options?.strategy });
    return { text };
  },
  async isChecked(_id: ControlId, options?: any) {
    calls.push({ method: "isChecked", strategy: options?.strategy });
    return { checked: true };
  },
  async listAdd() {
    calls.push({ method: "listAdd" });
    return { index: 3 };
  },
  async listDelete(_id: ControlId, index: number) {
    calls.push({ method: "listDelete", index });
    return { deleted: true as const };
  },
  async listChoose(_id: ControlId, sel: { index: number } | { text: string }) {
    calls.push({ method: "listChoose", index: "index" in sel ? sel.index : -1 });
    return { chosen: true as const };
  },
  async listFind() {
    calls.push({ method: "listFind" });
    return { index: 0 };
  },
  async listIndex() {
    calls.push({ method: "listIndex" });
    return { index: 0 };
  },
  async tabSelect(_id: ControlId, index: number) {
    calls.push({ method: "tabSelect", index });
    return { selected: true as const };
  },
  async tabIndex() {
    calls.push({ method: "tabIndex" });
    return { index: 2 };
  },
  async editCaret() {
    calls.push({ method: "editCaret" });
    return { line: 4, col: 7 };
  },
  async editLine(_id: ControlId, line: number) {
    calls.push({ method: "editLine", index: line });
    return { text: "row" };
  },
  async statusbarText(_id: ControlId, part?: number) {
    calls.push({ method: "statusbarText", index: part });
    return { text: "ready" };
  },
  async setChecked(_id: ControlId, checked: boolean | -1 | 0 | 1, _ensure?: boolean, options?: any) {
    calls.push({
      method: "setChecked",
      index: checked === true ? 1 : checked === false ? 0 : checked,
      strategy: options?.strategy,
    });
    return { checked: checked !== 0 };
  },
};

mock.module("rime:control", () => ({ control: bridge }));

beforeEach(() => {
  calls.length = 0;
});

function handle(): Control {
  return new Control(11, { id: 11 as ControlId, className: "ListBox", classNN: "ListBox1" });
}

test("listAdd resolves 0-based; listDelete/listChoose translate up", async () => {
  const control = handle();
  await expect(control.listAdd("x")).resolves.toStrictEqual({ index: 2 });
  await control.listDelete(2);
  await control.listChoose({ index: 0 });
  await control.listChoose({ index: null });
  await control.listChoose({ text: "x" });
  expect(calls).toStrictEqual([
    { method: "listAdd" },
    { method: "listDelete", index: 3 },
    { method: "listChoose", index: 1 },
    { method: "listChoose", index: 0 },
    { method: "listChoose", index: -1 },
  ]);
});

test("no-match and no-selection surface as null, never 0", async () => {
  const control = handle();
  await expect(control.listFind("zzz")).resolves.toStrictEqual({ index: null });
  await expect(control.listIndex()).resolves.toStrictEqual({ index: null });
});

test("tab/edit/statusbar translate both directions", async () => {
  const control = handle();
  await control.tabSelect(1);
  await expect(control.tabIndex()).resolves.toStrictEqual({ index: 1 });
  await expect(control.editCaret()).resolves.toStrictEqual({ line: 3, col: 6 });
  await control.editLine(0);
  await control.statusbarText();
  expect(calls).toStrictEqual([
    { method: "tabSelect", index: 2 },
    { method: "tabIndex" },
    { method: "editCaret" },
    { method: "editLine", index: 1 },
    { method: "statusbarText", index: 1 },
  ]);
});

test("setChecked takes boolean or toggle, never numbers", async () => {
  const control = handle();
  await control.setChecked(true);
  await control.setChecked(false);
  await control.setChecked("toggle");
  expect(calls).toStrictEqual([
    { method: "setChecked", index: 1, strategy: "auto" },
    { method: "setChecked", index: 0, strategy: "auto" },
    { method: "setChecked", index: -1, strategy: "auto" },
  ]);
});

test("negative and fractional indexes throw before the bridge runs", async () => {
  const control = handle();
  expect(() => control.listDelete(-1)).toThrow(TypeError);
  expect(() => control.listDelete(1.5)).toThrow(TypeError);
  expect(() => control.tabSelect(-2)).toThrow(TypeError);
  expect(calls).toStrictEqual([]);
});

test("control methods forward strategy to bridge", async () => {
  const control = new Control(11, { id: 11 as ControlId, className: "Button", classNN: "Button1" }, "uia");
  await control.click({ button: "left" });
  await control.click(undefined, { strategy: "visual" });
  await control.focus();
  await control.setText("val", { strategy: "message" });
  await control.getText();
  await control.sendText("abc");
  await control.setChecked(true, false, { strategy: "uia" });
  await control.isChecked();

  expect(calls).toStrictEqual([
    { method: "click", strategy: "uia" },
    { method: "click", strategy: "visual" },
    { method: "focus", strategy: "uia" },
    { method: "setText", strategy: "message" },
    { method: "getText", strategy: "uia" },
    { method: "sendText", strategy: "uia" },
    { method: "setChecked", index: 1, strategy: "uia" },
    { method: "isChecked", strategy: "uia" },
  ]);
});
