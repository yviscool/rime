import { beforeEach, expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type { ControlId } from "../../sdk/src/control";
import {
  ButtonControl,
  ComboBoxControl,
  Control,
  EditControl,
  ListViewControl,
  StatusBarControl,
} from "../../sdk/src/control";

// Realism: L3 - the real Control class runs against an instrumented
// rime:control bridge speaking native 1-based wire numbering; the SDK is
// the environment, never the unit under test. Every forwarded index is a
// literal, so a facade that leaks 1-based numbering or the 0-sentinel to
// callers cannot pass by agreeing with itself.

const calls: Array<{
  method: string;
  index?: number;
  col?: number;
  limit?: number;
  text?: string;
  strategy?: string;
}> = [];

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
  async listChoice(_id: ControlId, index?: number) {
    calls.push({ method: "listChoice", index });
    return { text: index !== undefined ? `choice_${index}` : "active_choice" };
  },
  async listItems(_id: ControlId, limit?: number) {
    calls.push({ method: "listItems", limit });
    return { items: ["apple", "banana"] };
  },
  async tabSelect(_id: ControlId, index: number) {
    calls.push({ method: "tabSelect", index });
    return { selected: true as const };
  },
  async tabIndex() {
    calls.push({ method: "tabIndex" });
    return { index: 2 };
  },
  async editCount(_id: ControlId) {
    calls.push({ method: "editCount" });
    return { lines: 5 };
  },
  async editCaret() {
    calls.push({ method: "editCaret" });
    return { line: 4, col: 7 };
  },
  async editLine(_id: ControlId, line: number) {
    calls.push({ method: "editLine", index: line });
    return { text: "row" };
  },
  async editSelected(_id: ControlId) {
    calls.push({ method: "editSelected" });
    return { text: "sel" };
  },
  async editPaste(_id: ControlId, text: string) {
    calls.push({ method: "editPaste", text });
    return { text };
  },
  async showDropdown(_id: ControlId) {
    calls.push({ method: "showDropdown" });
    return { dropped: true as const };
  },
  async hideDropdown(_id: ControlId) {
    calls.push({ method: "hideDropdown" });
    return { dropped: false as const };
  },
  async listviewCount(_id: ControlId) {
    calls.push({ method: "listviewCount" });
    return { rows: 12 };
  },
  async listviewText(_id: ControlId, row: number, col: number) {
    calls.push({ method: "listviewText", index: row, col });
    return { text: `cell_${row}_${col}` };
  },
  async listviewItems(_id: ControlId, limit?: number) {
    calls.push({ method: "listviewItems", limit });
    return { items: [{ col1: "val1" }] };
  },
  async statusbarText(_id: ControlId, part?: number) {
    calls.push({ method: "statusbarText", index: part });
    return { text: "ready" };
  },
  async statusbarWait(_id: ControlId, text: string, part?: number) {
    calls.push({ method: "statusbarWait", text, index: part });
    return { waited: true as const };
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

test("control accessors and converters return properly typed facades", () => {
  const control = handle();

  const lv1 = control.asListView();
  const lv2 = control.listView;
  expect(lv1).toBeInstanceOf(ListViewControl);
  expect(lv2).toBeInstanceOf(ListViewControl);
  expect(lv1.control).toBe(control);
  expect(lv1.id).toBe(control.id);
  expect(lv1.windowId).toBe(control.windowId);
  expect(lv1.snapshot).toBe(control.snapshot);
  expect(lv1.strategy).toBe("auto");

  const sb1 = control.asStatusBar();
  const sb2 = control.statusBar;
  expect(sb1).toBeInstanceOf(StatusBarControl);
  expect(sb2).toBeInstanceOf(StatusBarControl);
  expect(sb1.control).toBe(control);
  expect(sb1.id).toBe(control.id);
  expect(sb1.strategy).toBe("auto");

  const edit1 = control.asEdit();
  const edit2 = control.edit;
  expect(edit1).toBeInstanceOf(EditControl);
  expect(edit2).toBeInstanceOf(EditControl);
  expect(edit1.control).toBe(control);
  expect(edit1.id).toBe(control.id);
  expect(edit1.strategy).toBe("auto");

  const cb1 = control.asComboBox();
  const cb2 = control.comboBox;
  expect(cb1).toBeInstanceOf(ComboBoxControl);
  expect(cb2).toBeInstanceOf(ComboBoxControl);
  expect(cb1.control).toBe(control);
  expect(cb1.id).toBe(control.id);
  expect(cb1.strategy).toBe("auto");

  const btn1 = control.asButton();
  const btn2 = control.button;
  expect(btn1).toBeInstanceOf(ButtonControl);
  expect(btn2).toBeInstanceOf(ButtonControl);
  expect(btn1.control).toBe(control);
  expect(btn1.id).toBe(control.id);
  expect(btn1.strategy).toBe("auto");

  // Strategy override via converter and withStrategy
  const btnUia = control.asButton("uia");
  expect(btnUia.strategy).toBe("uia");
  expect(btnUia.control.strategy).toBe("uia");

  const editMsg = control.edit.withStrategy("message");
  expect(editMsg.strategy).toBe("message");
  expect(editMsg.control.strategy).toBe("message");
});

test("ListViewControl provides count, 0-based cell text, and items", async () => {
  const control = handle();
  const lv = control.listView;

  await expect(lv.count()).resolves.toStrictEqual({ rows: 12 });
  await expect(lv.text(0, 0)).resolves.toStrictEqual({ text: "cell_1_1" });
  await expect(lv.text(2, 4)).resolves.toStrictEqual({ text: "cell_3_5" });
  await expect(lv.items()).resolves.toStrictEqual({ items: [{ col1: "val1" }] });
  await expect(lv.items(50)).resolves.toStrictEqual({ items: [{ col1: "val1" }] });

  expect(calls).toStrictEqual([
    { method: "listviewCount" },
    { method: "listviewText", index: 1, col: 1 },
    { method: "listviewText", index: 3, col: 5 },
    { method: "listviewItems", limit: undefined },
    { method: "listviewItems", limit: 50 },
  ]);

  // Index validation throws TypeError on invalid row or column
  expect(() => lv.text(-1, 0)).toThrow(TypeError);
  expect(() => lv.text(0, -1)).toThrow(TypeError);
  expect(() => lv.text(1.5, 0)).toThrow(TypeError);
  expect(() => lv.text(0, 2.5)).toThrow(TypeError);
});

test("StatusBarControl provides 0-based getText and wait", async () => {
  const control = handle();
  const sb = control.statusBar;

  await expect(sb.getText()).resolves.toStrictEqual({ text: "ready" });
  await expect(sb.getText(2)).resolves.toStrictEqual({ text: "ready" });
  await expect(sb.wait("done")).resolves.toStrictEqual({ waited: true });
  await expect(sb.wait("ok", 3)).resolves.toStrictEqual({ waited: true });

  expect(calls).toStrictEqual([
    { method: "statusbarText", index: 1 },
    { method: "statusbarText", index: 3 },
    { method: "statusbarWait", text: "done", index: 1 },
    { method: "statusbarWait", text: "ok", index: 4 },
  ]);

  // Index validation
  expect(() => sb.getText(-1)).toThrow(TypeError);
  expect(() => sb.getText(1.2)).toThrow(TypeError);
  expect(() => sb.wait("err", -1)).toThrow(TypeError);
  expect(() => sb.wait("err", 0.5)).toThrow(TypeError);
});

test("EditControl provides lineCount, caret, line, selected, paste, setText, getText and preserves strategy", async () => {
  const control = handle();
  const edit = control.edit;

  await expect(edit.lineCount()).resolves.toStrictEqual({ lines: 5 });
  await expect(edit.caret()).resolves.toStrictEqual({ line: 3, col: 6 });
  await expect(edit.line(0)).resolves.toStrictEqual({ text: "row" });
  await expect(edit.line(4)).resolves.toStrictEqual({ text: "row" });
  await expect(edit.selected()).resolves.toStrictEqual({ text: "sel" });
  await expect(edit.paste("inserted")).resolves.toStrictEqual({ text: "inserted" });
  await expect(edit.setText("updated", { strategy: "uia" })).resolves.toStrictEqual({ text: "updated" });
  await expect(edit.getText({ strategy: "message" })).resolves.toStrictEqual({ text: "hello" });

  expect(calls).toStrictEqual([
    { method: "editCount" },
    { method: "editCaret" },
    { method: "editLine", index: 1 },
    { method: "editLine", index: 5 },
    { method: "editSelected" },
    { method: "editPaste", text: "inserted" },
    { method: "setText", strategy: "uia" },
    { method: "getText", strategy: "message" },
  ]);

  // Strategy inheritance on EditControl
  calls.length = 0;
  const editUia = control.asEdit("uia");
  await editUia.setText("abc");
  await editUia.getText();
  expect(calls).toStrictEqual([
    { method: "setText", strategy: "uia" },
    { method: "getText", strategy: "uia" },
  ]);

  // Line index validation
  expect(() => edit.line(-1)).toThrow(TypeError);
  expect(() => edit.line(1.1)).toThrow(TypeError);
});

test("ComboBoxControl provides dropdown, add, delete, choose, selectedIndex, choice, and items", async () => {
  const control = handle();
  const cb = control.comboBox;

  await expect(cb.showDropdown()).resolves.toStrictEqual({ dropped: true });
  await expect(cb.hideDropdown()).resolves.toStrictEqual({ dropped: false });
  await expect(cb.add("item3")).resolves.toStrictEqual({ index: 2 });
  await expect(cb.delete(0)).resolves.toStrictEqual({ deleted: true });
  await expect(cb.delete(2)).resolves.toStrictEqual({ deleted: true });

  // Choose variants: object and ergonomics literals
  await expect(cb.choose({ index: 0 })).resolves.toStrictEqual({ chosen: true });
  await expect(cb.choose({ index: null })).resolves.toStrictEqual({ chosen: true });
  await expect(cb.choose({ text: "opt" })).resolves.toStrictEqual({ chosen: true });
  await expect(cb.choose(1)).resolves.toStrictEqual({ chosen: true });
  await expect(cb.choose(null)).resolves.toStrictEqual({ chosen: true });
  await expect(cb.choose("direct")).resolves.toStrictEqual({ chosen: true });

  await expect(cb.selectedIndex()).resolves.toStrictEqual({ index: null });
  await expect(cb.choice()).resolves.toStrictEqual({ text: "active_choice" });
  await expect(cb.choice(1)).resolves.toStrictEqual({ text: "choice_2" });
  await expect(cb.items()).resolves.toStrictEqual({ items: ["apple", "banana"] });
  await expect(cb.items(20)).resolves.toStrictEqual({ items: ["apple", "banana"] });

  expect(calls).toStrictEqual([
    { method: "showDropdown" },
    { method: "hideDropdown" },
    { method: "listAdd" },
    { method: "listDelete", index: 1 },
    { method: "listDelete", index: 3 },
    { method: "listChoose", index: 1 },
    { method: "listChoose", index: 0 },
    { method: "listChoose", index: -1 },
    { method: "listChoose", index: 2 },
    { method: "listChoose", index: 0 },
    { method: "listChoose", index: -1 },
    { method: "listIndex" },
    { method: "listChoice", index: undefined },
    { method: "listChoice", index: 2 },
    { method: "listItems", limit: undefined },
    { method: "listItems", limit: 20 },
  ]);

  // Index validation
  expect(() => cb.delete(-1)).toThrow(TypeError);
  expect(() => cb.delete(1.5)).toThrow(TypeError);
  expect(() => cb.choice(-1)).toThrow(TypeError);
  expect(() => cb.choice(2.3)).toThrow(TypeError);
  expect(() => cb.choose(-1)).toThrow(TypeError);
  expect(() => cb.choose({ index: -1 })).toThrow(TypeError);
});

test("ButtonControl provides click, setChecked, isChecked and preserves strategy", async () => {
  const control = handle();
  const btn = control.button;

  await expect(btn.click()).resolves.toStrictEqual({ clicks: 1 });
  await expect(btn.click({ button: "right" }, { strategy: "message" })).resolves.toStrictEqual({ clicks: 1 });
  await expect(btn.setChecked(true)).resolves.toStrictEqual({ checked: true });
  await expect(btn.setChecked(false)).resolves.toStrictEqual({ checked: false });
  await expect(btn.setChecked("toggle")).resolves.toStrictEqual({ checked: true });
  await expect(btn.setChecked(true, false, { strategy: "uia" })).resolves.toStrictEqual({ checked: true });
  await expect(btn.isChecked()).resolves.toStrictEqual({ checked: true });
  await expect(btn.isChecked({ strategy: "visual" })).resolves.toStrictEqual({ checked: true });

  expect(calls).toStrictEqual([
    { method: "click", strategy: "auto" },
    { method: "click", strategy: "message" },
    { method: "setChecked", index: 1, strategy: "auto" },
    { method: "setChecked", index: 0, strategy: "auto" },
    { method: "setChecked", index: -1, strategy: "auto" },
    { method: "setChecked", index: 1, strategy: "uia" },
    { method: "isChecked", strategy: "auto" },
    { method: "isChecked", strategy: "visual" },
  ]);

  // Strategy inheritance on ButtonControl
  calls.length = 0;
  const btnVisual = control.asButton("visual");
  await btnVisual.click();
  await btnVisual.setChecked(true);
  await btnVisual.isChecked();

  expect(calls).toStrictEqual([
    { method: "click", strategy: "visual" },
    { method: "setChecked", index: 1, strategy: "visual" },
    { method: "isChecked", strategy: "visual" },
  ]);
});
