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
  Add(type: string, options?: string, content?: unknown, action?: NativeActionOptions): Promise<GuiControlNative>;
  Destroy(action?: NativeActionOptions): Promise<unknown>;
  Show(options?: string, action?: NativeActionOptions): Promise<unknown>;
  Hide(action?: NativeActionOptions): Promise<unknown>;
  Move(x: number, y: number, w?: number, h?: number, action?: NativeActionOptions): Promise<unknown>;
  Submit(action?: NativeActionOptions): Promise<Record<string, unknown>>;
  OnEvent(name: string, fn: (target: unknown, ...args: unknown[]) => void, action?: NativeActionOptions): Promise<unknown>;
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
}

async function createGuiBridge(): Promise<{
  createGui(options?: string, title?: string, eventObj?: object): Promise<GuiNative>;
}> {
  const module = await import("rime:ui");
  return module as unknown as {
    createGui(options?: string, title?: string, eventObj?: object): Promise<GuiNative>;
  };
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
    options?: string,
    content?: unknown,
    action?: ActionOptions,
  ): Promise<GuiControl> {
    return runAction(action, (native) =>
      this.native
        .Add(kind, options, content, native)
        .then((control) => new GuiControl(control)),
    );
  }

  /** Generic constructor; `AddButton` and friends are sugar over this. */
  add(type: string, options?: string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind(type, options, content, action);
  }

  addButton(options?: string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Button", options, content, action);
  }
  addCheckBox(options?: string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("CheckBox", options, content, action);
  }
  addEdit(options?: string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Edit", options, content, action);
  }
  addGroupBox(options?: string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("GroupBox", options, content, action);
  }
  addPicture(options?: string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Picture", options, content, action);
  }
  addProgress(options?: string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Progress", options, content, action);
  }
  addRadio(options?: string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Radio", options, content, action);
  }
  addText(options?: string, content?: unknown, action?: ActionOptions): Promise<GuiControl> {
    return this.addKind("Text", options, content, action);
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
      this.native.OnEvent(name, fn, native).then(() => undefined),
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
}
