import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

/**
 * Gui object family (M6 batch 2 surface, batch 3 extends it). Ownership:
 * the native `GuiService` on the UI thread owns every HWND; this layer
 * holds opaque native objects plus stable ids, never raw handles. Sync
 * members read JS-side state (Name/Type/ClassNN); everything touching a
 * HWND is a Promise through the UI pump. Indexes are 0-based throughout;
 * control lookup is by `vName`, enumeration is insertion-ordered.
 */

/** Bounds snapshot returned by `getPos`/`getClientPos`. */
export interface GuiBounds {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** Native `Gui` object shape (subset used by this facade). */
export interface GuiNative {
  Add(
    type: string,
    options?: string,
    content?: unknown,
    action?: NativeActionOptions,
  ): Promise<GuiControlNative>;
  Destroy(action?: NativeActionOptions): Promise<unknown>;
  Show(options?: string, action?: NativeActionOptions): Promise<unknown>;
  Hide(action?: NativeActionOptions): Promise<unknown>;
  Move(x: number, y: number, w?: number, h?: number, action?: NativeActionOptions): Promise<unknown>;
  Submit(action?: NativeActionOptions): Promise<Record<string, unknown>>;
  OnEvent(
    name: string,
    fn: (target: unknown, ...args: unknown[]) => void,
    addRemove?: number,
    action?: NativeActionOptions,
  ): Promise<unknown>;
  GetPos(action?: NativeActionOptions): Promise<GuiBounds>;
}

/** Native `GuiControl` object shape (subset used by this facade). */
export interface GuiControlNative {
  readonly Name: string;
  readonly Type: string;
  readonly ClassNN: string;
  Text(action?: NativeActionOptions): Promise<string>;
  Value(action?: NativeActionOptions): Promise<unknown>;
  Enabled(action?: NativeActionOptions): Promise<boolean>;
  Visible(action?: NativeActionOptions): Promise<boolean>;
  Focus(action?: NativeActionOptions): Promise<unknown>;
  Move(x: number, y: number, w?: number, h?: number, action?: NativeActionOptions): Promise<unknown>;
  SetCue(text: string, action?: NativeActionOptions): Promise<unknown>;
  Add(a: string | string[] | number, b?: number | boolean | string, c?: boolean, d?: boolean, e?: boolean, action?: NativeActionOptions): Promise<unknown>;
  Choose(value: number | string, action?: NativeActionOptions): Promise<unknown>;
  Delete(index?: number, action?: NativeActionOptions): Promise<unknown>;
  Insert(index: number, columns: string[], selected?: boolean, focused?: boolean, checked?: boolean, action?: NativeActionOptions): Promise<unknown>;
  Modify(index: number, columns?: string[] | string, b?: boolean | string, c?: boolean, d?: boolean, e?: boolean, action?: NativeActionOptions): Promise<unknown>;
  GetCount(mode?: string, action?: NativeActionOptions): Promise<unknown>;
  GetText(row: number, column?: number, action?: NativeActionOptions): Promise<unknown>;
  GetNext(start?: number, filter?: string, action?: NativeActionOptions): Promise<unknown>;
  InsertCol(index?: number, title?: string, width?: number, align?: string, action?: NativeActionOptions): Promise<unknown>;
  ModifyCol(index: number, title?: string, width?: number, align?: string, action?: NativeActionOptions): Promise<unknown>;
  DeleteCol(index: number, action?: NativeActionOptions): Promise<unknown>;
  Get(nodeId: number, attribute: string, action?: NativeActionOptions): Promise<unknown>;
  GetChild(nodeId: number, action?: NativeActionOptions): Promise<unknown>;
  GetParent(nodeId: number, action?: NativeActionOptions): Promise<unknown>;
  GetPrev(nodeId: number, action?: NativeActionOptions): Promise<unknown>;
  GetSelection(action?: NativeActionOptions): Promise<unknown>;
  SetParts(widths: number[], action?: NativeActionOptions): Promise<unknown>;
  SetText(text: string, part?: number, borders?: boolean, action?: NativeActionOptions): Promise<unknown>;
  SetIcon(file: string, iconIndex?: number, part?: number, action?: NativeActionOptions): Promise<unknown>;
  UseTab(index: number | string, exactMatch?: boolean, action?: NativeActionOptions): Promise<unknown>;
  SetFormat(picture: string, action?: NativeActionOptions): Promise<unknown>;
  SetHotkey(modifiers: string[], key: string, action?: NativeActionOptions): Promise<unknown>;
}

async function createGuiBridge(): Promise<{
  createGui(options?: string, title?: string, eventObj?: object): Promise<GuiNative>;
}> {
  const module = await import("rime:ui");
  return module as unknown as {
    createGui(options?: string, title?: string, eventObj?: object): Promise<GuiNative>;
  };
}

/** Structured control options, compiled to the wire option string at the boundary. */
export interface GuiAddOptions {
  x?: number;
  y?: number;
  w?: number;
  h?: number;
  hidden?: boolean;
  disabled?: boolean;
  /** Submit key name (wire `vName`; the `v` sigil never appears here). */
  name?: string;
}

/** Validates structured options and compiles the wire string. */
function compileAddOptions(options: GuiAddOptions | string | undefined, fn: string): string | undefined {
  if (options === undefined) return undefined;
  if (typeof options === "string") return options;
  if (typeof options !== "object") {
    throw new TypeError(`${fn}: options must be an object or string, got ${String(options)}`);
  }
  const parts: string[] = [];
  const coord = (key: "x" | "y" | "w" | "h"): void => {
    const value = options[key];
    if (value === undefined) return;
    if (!Number.isInteger(value)) {
      throw new TypeError(`${fn}: options.${key} must be an integer, got ${String(value)}`);
    }
    parts.push(`${key}${value}`);
  };
  coord("x");
  coord("y");
  coord("w");
  coord("h");
  if (options.hidden !== undefined) parts.push(options.hidden ? "+Hidden" : "-Hidden");
  if (options.disabled !== undefined) parts.push(options.disabled ? "+Disabled" : "-Disabled");
  if (options.name !== undefined) {
    if (typeof options.name !== "string" || options.name === "") {
      throw new TypeError(`${fn}: options.name must be a non-empty string`);
    }
    parts.push(`v${options.name}`);
  }
  return parts.length === 0 ? undefined : parts.join(" ");
}

/** A resolved Gui window handle. */
export class Gui {
  private constructor(private readonly native: GuiNative) {}

  /** Creates a window (HWND materializes on the first async member call). */
  static create(options?: string, title?: string, eventObj?: object): Promise<Gui> {
    return createGuiBridge()
      .then((bridge) => bridge.createGui(options, title, eventObj))
      .then((native) => new Gui(native));
  }

  private addKind(
    kind: string,
    options?: GuiAddOptions | string,
    content?: unknown,
    action?: ActionOptions,
  ): Promise<GuiControl> {
    const wire = compileAddOptions(options, "Gui.add");
    return runAction(action, (native) =>
      this.native
        .Add(kind, wire, content, native)
        .then((control) => new GuiControl(control)),
    );
  }

  /** Generic constructor; `addButton` and friends are sugar over this. */
  add(
    type: string,
    options?: GuiAddOptions | string,
    content?: unknown,
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind(type, options, content, action);
  }

  addButton(options?: GuiAddOptions | string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Button", options, content, action);
  }
  addCheckBox(options?: GuiAddOptions | string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("CheckBox", options, content, action);
  }
  addEdit(options?: GuiAddOptions | string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Edit", options, content, action);
  }
  addGroupBox(options?: GuiAddOptions | string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("GroupBox", options, content, action);
  }
  addPicture(options?: GuiAddOptions | string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Picture", options, content, action);
  }
  addProgress(options?: GuiAddOptions | string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Progress", options, content, action);
  }
  addRadio(options?: GuiAddOptions | string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Radio", options, content, action);
  }
  addText(options?: GuiAddOptions | string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Text", options, content, action);
  }
  addListBox(
    options?: GuiAddOptions | string,
    content?: string | string[],
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("ListBox", options, content, action);
  }
  addComboBox(
    options?: GuiAddOptions | string,
    content?: string | string[],
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("ComboBox", options, content, action);
  }
  addDropDownList(
    options?: GuiAddOptions | string,
    content?: string | string[],
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("DropDownList", options, content, action);
  }
  /** Ledger alias for `addDropDownList` (same control kind, two registry rows). */
  addDDL(
    options?: GuiAddOptions | string,
    content?: string | string[],
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("DropDownList", options, content, action);
  }
  addDateTime(
    options?: GuiAddOptions | string,
    content?: string,
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("DateTime", options, content, action);
  }
  addMonthCal(
    options?: GuiAddOptions | string,
    content?: string,
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("MonthCal", options, content, action);
  }
  addSlider(
    options?: GuiAddOptions | string,
    content?: number,
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("Slider", options, content, action);
  }
  addUpDown(
    options?: GuiAddOptions | string,
    content?: number,
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("UpDown", options, content, action);
  }
  addTab(
    options?: GuiAddOptions | string,
    titles?: string[],
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("Tab", options, titles, action);
  }
  /** Ledger alias for `addTab` (same control kind, two registry rows). */
  addTab2(
    options?: GuiAddOptions | string,
    titles?: string[],
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("Tab", options, titles, action);
  }
  /** Ledger alias for Picture (same control kind, two registry rows). */
  addPic(options?: GuiAddOptions | string, path?: string, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Picture", options, path, action);
  }
  addLink(
    options?: GuiAddOptions | string,
    markup?: string,
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("Link", options, markup, action);
  }
  /** No construction content: the initial hotkey is set with SetHotkey (3b). */
  addHotkey(options?: GuiAddOptions | string, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Hotkey", options, undefined, action);
  }
  addListView(
    options?: GuiAddOptions | string,
    headers?: string[],
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("ListView", options, headers, action);
  }
  addTreeView(
    options?: GuiAddOptions | string,
    roots?: string[],
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("TreeView", options, roots, action);
  }
  addStatusBar(
    options?: GuiAddOptions | string,
    parts?: string[],
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return this.addKind("StatusBar", options, parts, action);
  }

  destroy(options?: ActionOptions): Promise<void> {
    return runAction(options, (native) =>
      this.native.Destroy(native).then(() => undefined),
    );
  }
  show(options?: string, action?: ActionOptions): Promise<void> {
    return runAction(action, (native) =>
      this.native.Show(options, native).then(() => undefined),
    );
  }
  hide(options?: ActionOptions): Promise<void> {
    return runAction(options, (native) =>
      this.native.Hide(native).then(() => undefined),
    );
  }
  submit(options?: ActionOptions): Promise<Record<string, unknown>> {
    return runAction(options, (native) => this.native.Submit(native));
  }
  onEvent(
    name: string,
    fn: (target: unknown, ...args: unknown[]) => void,
    options?: ActionOptions,
  ): Promise<void> {
    return runAction(options, (native) =>
      this.native.OnEvent(name, fn, undefined, native).then(() => undefined),
    );
  }
  /** Removes one registration installed by `onEvent` (wire addRemove 0). */
  offEvent(
    name: string,
    fn: (target: unknown, ...args: unknown[]) => void,
    options?: ActionOptions,
  ): Promise<void> {
    return runAction(options, (native) =>
      this.native.OnEvent(name, fn, 0, native).then(() => undefined),
    );
  }
  getPos(options?: ActionOptions): Promise<GuiBounds> {
    return runAction(options, (native) => this.native.GetPos(native));
  }
}

/** A resolved Gui control handle. */
export class GuiControl {
  constructor(private readonly native: GuiControlNative) {}

  /** Script-side identity (synchronous, no pump). */
  get name(): string {
    return this.native.Name;
  }
  /** Control kind name, e.g. `"Button"` (synchronous). */
  get type(): string {
    return this.native.Type;
  }
  /** ClassNN, e.g. `"Edit1"` (synchronous, computed from kind + order). */
  get classNN(): string {
    return this.native.ClassNN;
  }

  /** Pump read of the control text. */
  text(options?: ActionOptions): Promise<string> {
    return runAction(options, (native) => this.native.Text(native));
  }
  /** Pump read of the control value (Edit/CheckBox/Radio/Progress). */
  value(options?: ActionOptions): Promise<unknown> {
    return runAction(options, (native) => this.native.Value(native));
  }
  focus(options?: ActionOptions): Promise<void> {
    return runAction(options, (native) =>
      this.native.Focus(native).then(() => undefined),
    );
  }
  /** Edit cue banner text (`EM_SETCUEBANNER`; needs the v6 manifest host). */
  setCue(text: string, options?: ActionOptions): Promise<void> {
    return runAction(options, (native) =>
      this.native.SetCue(text, native).then(() => undefined),
    );
  }

  // ---- batch 3b: per-kind control methods --------------------------------
  // Positions are 0-based and misses resolve null (gui-menu.md §4.x). Add /
  // Delete / Modify / GetCount / GetText / GetNext are shared native names
  // dispatched by kind: list kinds take option values, ListView takes rows,
  // TreeView takes stable node ids issued by add.

  /** List kinds: append value(s), returns the new count. */
  add(value: string | string[], options?: ActionOptions): Promise<number>;
  /** ListView: append a row, returns its 0-based index. */
  add(columns: string[], selected?: boolean, focused?: boolean, checked?: boolean, options?: ActionOptions): Promise<number>;
  /** TreeView: append a node under parentId (omitted = root), returns its id. */
  add(text: string, parentId?: number, selected?: boolean, expanded?: boolean, bold?: boolean, options?: ActionOptions): Promise<number>;
  add(a: string | string[], ...rest: unknown[]): Promise<number> {
    const [b, c, d, e, options] = rest;
    return runAction(options as ActionOptions | undefined, (native) =>
      this.native.Add(a, b as number | boolean | string | undefined, c as boolean | undefined,
        d as boolean | undefined, e as boolean | undefined, native).then(Number),
    );
  }
  /** List kinds only: select by 0-based index or exact text. */
  choose(value: number | string, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.Choose(value, native).then(Number),
    );
  }
  /** List/ListView: remove index (omitted clears). TreeView: remove node (omitted clears). */
  deleteItem(index?: number, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.Delete(index, native).then(Number),
    );
  }
  /** ListView only: insert a row at index. */
  insert(index: number, columns: string[], selected?: boolean, focused?: boolean, checked?: boolean, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.Insert(index, columns, selected, focused, checked, native).then(Number),
    );
  }
  /** ListView: rewrite row cells and/or flags. TreeView: rename a node and/or flags. */
  modify(index: number, ...rest: unknown[]): Promise<number> {
    const [second, b, c, d, e, options] = rest;
    return runAction(options as ActionOptions | undefined, (native) =>
      this.native.Modify(index, second as string[] | string | undefined,
        b as boolean | string | undefined, c as boolean | undefined, d as boolean | undefined,
        e as boolean | undefined, native).then(Number),
    );
  }
  /** ListView: row count (mode all/selected/columns). TreeView: node count. */
  getCount(mode?: string, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.GetCount(mode, native).then(Number),
    );
  }
  /** ListView: cell text. TreeView: pass a node id as row. */
  getText(row: number, column?: number, options?: ActionOptions): Promise<string> {
    return runAction(options, (native) =>
      this.native.GetText(row, column, native).then(String),
    );
  }
  /** ListView: next matching row (null when none). TreeView: pass a node id. */
  getNext(start?: number, filter?: string, options?: ActionOptions): Promise<number | null> {
    return runAction(options, (native) =>
      this.native.GetNext(start, filter, native).then((v) => (v === null ? null : Number(v))),
    );
  }
  /** ListView only: insert a column (omitted index appends). */
  insertCol(index?: number, title?: string, width?: number, align?: string, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.InsertCol(index, title, width, align, native).then(Number),
    );
  }
  /** ListView only: retitle/resize/realign a column. */
  modifyCol(index: number, title?: string, width?: number, align?: string, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.ModifyCol(index, title, width, align, native).then(Number),
    );
  }
  /** ListView only: remove a column, returns the remaining count. */
  deleteCol(index: number, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.DeleteCol(index, native).then(Number),
    );
  }
  /** TreeView only: expanded/checked/bold flag of a node. */
  get(nodeId: number, attribute: string, options?: ActionOptions): Promise<boolean> {
    return runAction(options, (native) =>
      this.native.Get(nodeId, attribute, native).then((v) => v === true),
    );
  }
  /** TreeView only: first child / parent / previous sibling (null when none). */
  getChild(nodeId: number, options?: ActionOptions): Promise<number | null> {
    return runAction(options, (native) =>
      this.native.GetChild(nodeId, native).then((v) => (v === null ? null : Number(v))),
    );
  }
  getParent(nodeId: number, options?: ActionOptions): Promise<number | null> {
    return runAction(options, (native) =>
      this.native.GetParent(nodeId, native).then((v) => (v === null ? null : Number(v))),
    );
  }
  getPrev(nodeId: number, options?: ActionOptions): Promise<number | null> {
    return runAction(options, (native) =>
      this.native.GetPrev(nodeId, native).then((v) => (v === null ? null : Number(v))),
    );
  }
  /** TreeView only: selected node id (null when none). */
  getSelection(options?: ActionOptions): Promise<number | null> {
    return runAction(options, (native) =>
      this.native.GetSelection(native).then((v) => (v === null ? null : Number(v))),
    );
  }
  /** StatusBar only: set part widths (positive pixels, last may be -1 for fill). */
  setParts(widths: number[], options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.SetParts(widths, native).then(Number),
    );
  }
  /** StatusBar only: set part text. */
  setText(text: string, part?: number, borders?: boolean, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.SetText(text, part, borders, native).then(Number),
    );
  }
  /** StatusBar only: set a part icon from a .ico file or a dll/exe index. */
  setIcon(file: string, iconIndex?: number, part?: number, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.SetIcon(file, iconIndex, part, native).then(Number),
    );
  }
  /** Tab only: select by 0-based index or label. */
  useTab(index: number | string, exactMatch?: boolean, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      this.native.UseTab(index, exactMatch, native).then(Number),
    );
  }
  /** DateTime only: set the Win32 format picture (e.g. "yyyy-MM-dd"). */
  setFormat(picture: string, options?: ActionOptions): Promise<void> {
    return runAction(options, (native) =>
      this.native.SetFormat(picture, native).then(() => undefined),
    );
  }
  /** Hotkey only: set the chord from modifier names and a key name. */
  setHotkey(modifiers: string[], key: string, options?: ActionOptions): Promise<void> {
    return runAction(options, (native) =>
      this.native.SetHotkey(modifiers, key, native).then(() => undefined),
    );
  }
}
