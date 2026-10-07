import { ActionError, runAction, type ActionOptions, type NativeActionOptions } from "./action";
import type { ProcessId } from "./process";
import { Control, focusedControl, type ControlClickOptions, type ControlSpec } from "./control";

export type Brand<K, T> = K & { readonly __brand: T };

/** Stable window id issued by the UI-lane registry (never a raw HWND). */
export type WindowId = Brand<number, "WindowId">;

export type WindowPlacement = "left" | "right" | "top" | "bottom" | "full";

/**
 * The coordinate form of `window.move` (AHK WinMove X/Y/Width/Height): a
 * partial move/resize where omitted fields keep the current value. `x`/`y`
 * are screen coordinates of the top-left corner (negative is valid on a
 * multi-monitor desktop); `w`/`h` are the outer frame size in pixels and
 * must be at least 1. Mutually exclusive with {@link WindowPlacement}.
 */
export interface WindowMoveRect {
  x?: number;
  y?: number;
  w?: number;
  h?: number;
}
/** Z-order placement for `zorder` (AHK WinMoveTop/WinMoveBottom). */
export type WindowZorder = "top" | "bottom";

export type WindowState = "normal" | "minimized" | "maximized" | "hidden";

export interface WindowRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

/** Snapshot returned by every window read and mutation (window-v1 snapshot). */
export interface WindowSnapshot {
  id: WindowId;
  title: string;
  /** Win32 class name, e.g. "Static" or "CabinetWClass". */
  className: string;
  /** Process image basename, e.g. "notepad.exe". */
  processName: string;
  /** Full image path (WinGetProcessPath); "" when the path cannot be read. */
  processPath: string;
  rect: WindowRect;
  /** Client area in screen coordinates (WinGetClientPos). */
  clientRect: WindowRect;
  visible: boolean;
  minimized: boolean;
  state: WindowState;
  processId: ProcessId;
  /** GWL_STYLE bits (WinGetStyle). */
  style: number;
  /** GWL_EXSTYLE bits (WinGetExStyle). */
  exStyle: number;
  /** IsWindowEnabled (WinGetEnabled). */
  enabled: boolean;
  /** WS_EX_TOPMOST (WinGetAlwaysOnTop). */
  alwaysOnTop: boolean;
  /** WinGetMinMax: -1 minimized | 0 normal | 1 maximized. */
  minMax: -1 | 0 | 1;
  /** Layered alpha 0..255, or -1 when the window has no LWA_ALPHA (WinGetTransparent). */
  transparent: number;
  /** Layered color key as "0xRRGGBB", or "" when absent (WinGetTransColor). */
  transColor: string;
  /** Region bounding box as "left,top,right,bottom" in window coordinates,
   * or "" when the window has no region (extension beyond AHK's getters;
   * non-rectangular regions report their bounding box). */
  region: string;
}

/** A child control from `windows.controls` (WinGetControls/WinGetControlsHwnd). */
export interface WindowControl {
  /** Stable id in the same space as `WindowSnapshot.id` — never a raw HWND. */
  id: WindowId;
  /** Win32 class name, e.g. "Edit". */
  className: string;
  /** AHK ClassNN, e.g. "Edit1". */
  classNN: string;
}

/** AHK TitleMatchMode vocabulary: "1" startswith, "2" contains, "3" exact, "RegEx". */
export type TitleMatchMode = "1" | "2" | "3" | "RegEx";

/** SetTitleMatchMode's speed knob (default "Fast"); orthogonal to the mode. */
export type TitleMatchModeSpeed = "Fast" | "Slow";

/** WinTitle-style selector fields shared by reads (window-v1 query). */
export interface WindowQuery {
  /**
   * Window title pattern. Case-sensitive in every match mode (AHK rule) —
   * `regex` mode is case-sensitive too unless the pattern carries AHK's
   * `i)` option prefix.
   */
  title?: string;
  /** Title match mode override; absent resolves against `settings.window.titleMatchMode`. */
  matchMode?: "startswith" | "contains" | "exact" | "regex";
  /** Case-insensitive Win32 class-name match (`regex` mode applies `title`'s pattern instead). */
  ahkClass?: string;
  /** Case-insensitive match against the process image basename only. */
  ahkExe?: string;
  ahkId?: WindowId | string;
  /** When true, hidden windows are included — absent resolves against `settings.window.detectHiddenWindows`; untitled (title-less) windows stay filtered. */
  includeHidden?: boolean;
  /** Selects the foreground window (same as `title: "A"`). */
  active?: boolean;
}

/**
 * Global window settings (SetTitleMatchMode / DetectHiddenWindows /
 * DetectHiddenText). Synchronous reads and writes — never an Action.
 * Each setter returns the previous value (AHK's return-previous contract).
 */
export interface WindowSettings {
  titleMatchMode: TitleMatchMode;
  titleMatchModeSpeed: TitleMatchModeSpeed;
  detectHiddenWindows: boolean;
  detectHiddenText: boolean;
}

/** One-field settings patch; unset fields keep their current value. */
export interface WindowSettingsPatch {
  titleMatchMode?: TitleMatchMode;
  titleMatchModeSpeed?: TitleMatchModeSpeed;
  detectHiddenWindows?: boolean;
  detectHiddenText?: boolean;
}

/** Synchronous settings surface of `rime:window` (`settings.window`). */
export interface WindowSettingsBridge extends WindowSettings {
  /** Returns the previous mode ("Fast"/"Slow" returns the previous speed knob). */
  setTitleMatchMode(mode: TitleMatchMode): TitleMatchMode;
  setTitleMatchMode(mode: TitleMatchModeSpeed): TitleMatchModeSpeed;
  setDetectHiddenWindows(value: boolean): boolean;
  setDetectHiddenText(value: boolean): boolean;
}

/** Options accepted by `windows.list` on the wire. */
export type WindowsListOptions = WindowQuery & NativeActionOptions;

/**
 * WinWait family condition (`windows.wait` / AHK WinWait, WinWaitActive,
 * WinWaitClose, WinWaitNotActive): a matching window appears, becomes the
 * foreground, no longer exists, or stops being foreground. `notActive` is
 * the logical negation of `active`, so a query that matches nothing is
 * satisfied immediately.
 */
export type WindowWaitUntil = "exists" | "active" | "closed" | "notActive";

/** Wire options for `windows.wait`: one query plus the `until` condition. */
export type WindowsWaitOptions = WindowsListOptions & { until?: WindowWaitUntil };

/** Close modes for `groups.close` (AHK GroupClose's second argument). */
export type GroupCloseMode = "reverse" | "all";

/** Options for the group focus cycle (`groups.activate` / `groups.deactivate`). */
export interface GroupFocusOptions extends NativeActionOptions {
  /** Starts the cycle at the most recent (top) member instead of the oldest. */
  reverse?: boolean;
}

/** Result of `groups.close`: the number of windows closed plus the window the cycle landed on. */
export interface GroupCloseResult {
  /** How many members this call actually closed (0 when the foreground was not a member). */
  closed: number;
  /** The successor the cycle activated, or null when it had nowhere to land (mode "all", or an empty group). */
  activated: WindowSnapshot | null;
}

/** Bridge of the `rime:window` module. Every mutation is an Action. */
export interface WindowsBridge {
  list(options?: WindowsListOptions): Promise<WindowSnapshot[]>;
  /** Resolves null when no window is foreground. */
  active(options?: NativeActionOptions): Promise<WindowSnapshot | null>;
  /** WinExist: true when at least one window matches the query. */
  exists(options?: WindowsListOptions): Promise<boolean>;
  /** WinActive: true when the foreground window matches the query. */
  isActive(options?: WindowsListOptions): Promise<boolean>;
  /**
   * WinWait family: resolves when `until` (default "exists") is satisfied —
   * the target snapshot for exists/active, null for closed/notActive.
   * Rejects with `timeout` once deadlineMs (default 5000) elapses.
   */
  wait(options?: WindowsWaitOptions): Promise<WindowSnapshot | null>;
  info(windowId: WindowId, options?: NativeActionOptions): Promise<WindowSnapshot>;
  /** WinGetControls/WinGetControlsHwnd: child controls in z-order (hidden included). */
  controls(windowId: WindowId, options?: NativeActionOptions): Promise<WindowControl[]>;
  /** WinGetText: concatenated control text, "\r\n" after each non-empty entry. */
  text(windowId: WindowId, options?: NativeActionOptions): Promise<string>;
  /** `target` is a window id or the string "active". */
  move(
    target: WindowId | "active",
    position: WindowPlacement | WindowMoveRect,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
  /** Reorders to the top/bottom of the z-order without activating (WinMoveTop/WinMoveBottom). */
  zorder(
    target: WindowId | "active",
    placement: WindowZorder,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
  focus(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowSnapshot>;
  /**
   * Force close (AHK WinKill): WM_CLOSE first, TerminateProcess fallback
   * when the target is hung; resolves with the pre-close snapshot.
   */
  kill(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowSnapshot>;
  /** Invalidate the window (AHK WinRedraw); resolves with the unchanged snapshot. */
  redraw(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowSnapshot>;
  close(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowSnapshot>;
  hide(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowSnapshot>;
  show(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowSnapshot>;
  minimize(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowSnapshot>;
  maximize(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowSnapshot>;
  restore(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowSnapshot>;
  /**
   * WinMinimizeAll: minimizes every window on the desktop by posting the
   * shell tray command; resolves when the command is posted (the shell
   * applies the change asynchronously - observe it through `info`).
   */
  minimizeAll(options?: NativeActionOptions): Promise<void>;
  /** WinMinimizeAllUndo: posts the shell's undo tray command (fire-and-forget like `minimizeAll`). */
  minimizeAllUndo(options?: NativeActionOptions): Promise<void>;
  /**
   * WinSetTitle: sets the window title (the empty string clears it);
   * resolves with the new snapshot.
   */
  setTitle(
    target: WindowId | "active",
    title: string,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
  /**
   * WinSetEnabled: 1 enables, 0 disables, -1 toggles the current state;
   * resolves with the new snapshot.
   */
  setEnabled(
    target: WindowId | "active",
    value: boolean | -1 | 0 | 1,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
  /**
   * WinSetAlwaysOnTop: 1 topmost, 0 clears topmost, -1 toggles; an absent
   * value means topmost (AHK's default). Resolves with the new snapshot.
   */
  setAlwaysOnTop(
    target: WindowId | "active",
    value?: boolean | -1 | 0 | 1,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
  /**
   * WinSetStyle: applies the AHK change string ('+N' adds, '-N' removes,
   * '^N' toggles, a bare N replaces); resolves with the new snapshot.
   */
  setStyle(
    target: WindowId | "active",
    value: string,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
  /** WinSetExStyle: same change-string grammar against the extended style. */
  setExStyle(
    target: WindowId | "active",
    value: string,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
  /**
   * WinSetTransparent: 0..255 sets the layered alpha, -1 turns transparency
   * off (drops WS_EX_LAYERED); resolves with the new snapshot.
   */
  setTransparent(
    target: WindowId | "active",
    value: number,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
  /**
   * WinSetTransColor: ''/'off' clears the color key, 'RRGGBB'/'0xRRGGBB'
   * sets it (hex only), an optional ' <0-255>' suffix adds alpha alongside.
   */
  setTransColor(
    target: WindowId | "active",
    value: string,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
  /**
   * WinSetRegion: the AHK options string ('<x>-<y>' pairs plus E, R, W/Wind
   * and H letter options); '' restores the normal region.
   */
  setRegion(
    target: WindowId | "active",
    value: string,
    options?: NativeActionOptions,
  ): Promise<WindowSnapshot>;
}

/** Bridge of the named-window-group surface (`rime:window`'s `groups` export). */
export interface WindowsGroupsBridge {
  /**
   * GroupAdd: appends a query spec to the group unless an exact duplicate
   * is already registered; a missing group is created.
   */
  add(
    name: string,
    query: WindowQuery,
    options?: NativeActionOptions,
  ): Promise<{ count: number }>;
  /** GroupActivate: cycles focus through the members; null when there is nothing to activate. */
  activate(name: string, options?: GroupFocusOptions): Promise<WindowSnapshot | null>;
  /** GroupDeactivate: activates an eligible non-member; null when there is none. */
  deactivate(name: string, options?: GroupFocusOptions): Promise<WindowSnapshot | null>;
  /** GroupClose: `mode` defaults to "" (close the foreground member, then activate the next). */
  close(
    name: string,
    mode?: GroupCloseMode | "",
    options?: NativeActionOptions,
  ): Promise<GroupCloseResult>;
}

async function windowBridge(): Promise<WindowsBridge> {
  const module = await import("rime:window");
  return module.windows;
}

/**
 * Loads the synchronous settings surface (`settings.window`). Reads and
 * writes after the first await never go through the Action pipeline.
 * @throws TypeError on invalid mode/pattern (sync), never rejects on values.
 */
export async function settings(): Promise<WindowSettingsBridge> {
  const module = await import("rime:window");
  return module.settings.window;
}

/** Action-mapped surface of the named-window-group mutations. */
export interface WindowGroups {
  /**
   * GroupAdd: registers a deduplicated query spec (a missing group is
   * created) and resolves with the resulting spec count.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   * @throws TypeError (sync) when the name is empty or the query is invalid.
   */
  add(name: string, query?: WindowQuery & ActionOptions): Promise<number>;
  /**
   * GroupActivate: cycles focus through the group's members (oldest first;
   * `reverse` starts at the most recent). A missing group is created and
   * resolves null; an existing empty group also resolves null.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  activate(name: string, options?: GroupFocusOptions & ActionOptions): Promise<WindowSnapshot | null>;
  /**
   * GroupDeactivate: activates an eligible non-member (AHK's "deactivate to
   * the next window"). The group must exist.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  deactivate(
    name: string,
    options?: GroupFocusOptions & ActionOptions,
  ): Promise<WindowSnapshot | null>;
  /**
   * GroupClose: "" closes the foreground member (when it is one) and then
   * activates the next; "reverse" walks from the most recent member; "all"
   * closes every member and activates nothing. The group must exist.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  close(name: string, mode?: GroupCloseMode, options?: ActionOptions): Promise<GroupCloseResult>;
}

/**
 * Loads the named-window-group surface (`groups` on `rime:window`). Every
 * method is an Action (capability `windows.window.write`); group state
 * itself lives on the UI lane inside the window service.
 */
export async function groups(): Promise<WindowGroups> {
  const module = await import("rime:window");
  const bridge = module.groups;
  return {
    add: (name, query) => {
      const { signal: _signal, ...fields } = query ?? {};
      return runAction(query, (native) => bridge.add(name, fields, native)).then(
        (result) => result.count,
      );
    },
    activate: (name, options) => {
      const { signal: _signal, ...fields } = options ?? {};
      return runAction(options, (native) => bridge.activate(name, { ...fields, ...native }));
    },
    deactivate: (name, options) => {
      const { signal: _signal, ...fields } = options ?? {};
      return runAction(options, (native) => bridge.deactivate(name, { ...fields, ...native }));
    },
    close: (name, mode, options) =>
      runAction(options, (native) => bridge.close(name, mode, native)),
  };
}

/**
 * A resolved window (P1-4 object model, modelled on `Control`).
 *
 * Ownership: the native `WindowService` on the UI lane owns the HWND; this
 * object holds only the stable `WindowId` plus the last immutable
 * `WindowSnapshot`. Every verb re-resolves the id through the service
 * (capability `windows.window.write`, cancellable, traced) and refreshes
 * the cached snapshot from the result — no raw handle is ever cached, and
 * a dead window surfaces as the standard `target_gone` ActionError from the
 * service, never as a new client-side error kind. This class is the only
 * window surface: resolve via `Window.list` / `find` / `active`, then act
 * on the handle (`win.focus()`, `win.controls()`).
 */
export class Window {
  private current: WindowSnapshot;

  constructor(snapshot: WindowSnapshot) {
    this.current = snapshot;
  }

  /** Resolves every window matching the query (window-v1 query). */
  static async list(query?: WindowQuery & ActionOptions): Promise<Window[]> {
    // `signal` never reaches the wire; it is converted to a cancellation id.
    const { signal: _signal, ...fields } = query ?? {};
    const snapshots = await runAction(query, (native) =>
      windowBridge().then((windows) => windows.list({ ...fields, ...native })),
    );
    return snapshots.map((snapshot) => new Window(snapshot));
  }

  /** Resolves the first match, or null when nothing matches. */
  static async find(query?: WindowQuery & ActionOptions): Promise<Window | null> {
    const [first] = await Window.list(query);
    return first ?? null;
  }

  /** Resolves the foreground window, or null when none holds it. */
  static async active(options?: ActionOptions): Promise<Window | null> {
    const [first] = await Window.list({ ...options, active: true });
    return first ?? null;
  }

  /** Wraps an already-resolved snapshot without a round trip. */
  static fromSnapshot(snapshot: WindowSnapshot): Window {
    return new Window(snapshot);
  }

  /**
   * WinExist: true when at least one window matches the query.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  static exists(query?: WindowQuery & ActionOptions): Promise<boolean> {
    const { signal: _signal, ...fields } = query ?? {};
    return runAction(query, (native) =>
      windowBridge().then((windows) => windows.exists({ ...fields, ...native })),
    );
  }

  /**
   * WinActive: true when the foreground window matches the query.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  static isActive(query?: WindowQuery & ActionOptions): Promise<boolean> {
    const { signal: _signal, ...fields } = query ?? {};
    return runAction(query, (native) =>
      windowBridge().then((windows) => windows.isActive({ ...fields, ...native })),
    );
  }

  /**
   * WinWait/WinWaitActive/WinWaitClose/WinWaitNotActive: polls until `until`
   * is satisfied without blocking a thread. Resolves with the target
   * snapshot (exists/active) or null (closed/notActive).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  static wait(
    query?: WindowQuery & { until?: WindowWaitUntil } & ActionOptions,
  ): Promise<WindowSnapshot | null> {
    const { signal: _signal, ...fields } = query ?? {};
    return runAction(query, (native) =>
      windowBridge().then((windows) => windows.wait({ ...fields, ...native })),
    );
  }

  /**
   * WinMinimizeAll: minimizes every window on the desktop (fire-and-forget
   * shell tray command; observe the effect through `refresh`).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  static minimizeAll(options?: ActionOptions): Promise<void> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.minimizeAll(native)),
    );
  }

  /**
   * WinMinimizeAllUndo: undoes `minimizeAll` (same fire-and-forget contract).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  static minimizeAllUndo(options?: ActionOptions): Promise<void> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.minimizeAllUndo(native)),
    );
  }

  get id(): WindowId {
    return this.current.id;
  }

  /** Last observed snapshot; refreshed by every verb and `refresh()`. */
  get snapshot(): WindowSnapshot {
    return this.current;
  }

  /**
   * Re-reads the window; resolves with the new snapshot.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`.
   */
  async refresh(options?: ActionOptions): Promise<WindowSnapshot> {
    this.current = await runAction(options, (native) =>
      windowBridge().then((windows) => windows.info(this.id, native)),
    );
    return this.current;
  }

  /** True when the window still resolves; false only on `target_gone`. */
  async isAlive(): Promise<boolean> {
    try {
      await this.refresh();
      return true;
    } catch (error) {
      if (error instanceof ActionError && error.code === "target_gone") return false;
      throw error;
    }
  }

  private adopt(snapshot: WindowSnapshot): WindowSnapshot {
    this.current = snapshot;
    return snapshot;
  }

  private mutate(
    pick: (bridge: WindowsBridge, native: NativeActionOptions | undefined) => Promise<WindowSnapshot>,
    options?: ActionOptions,
  ): Promise<WindowSnapshot> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => pick(windows, native)),
    ).then((snapshot) => this.adopt(snapshot));
  }

  /** Restores when minimized and requests foreground activation. */
  focus(options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.focus(this.id, native), options);
  }
  /** Moves/resizes: named placement or coordinate rect (omitted fields keep). */
  move(
    position: WindowPlacement | WindowMoveRect,
    options?: ActionOptions,
  ): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.move(this.id, position, native), options);
  }
  /** Reorders to the top/bottom without activating (WinMoveTop/Bottom). */
  zorder(placement: WindowZorder, options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.zorder(this.id, placement, native), options);
  }
  /** Graceful close; resolves with the pre-close snapshot. */
  close(options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.close(this.id, native), options);
  }
  hide(options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.hide(this.id, native), options);
  }
  show(options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.show(this.id, native), options);
  }
  minimize(options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.minimize(this.id, native), options);
  }
  maximize(options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.maximize(this.id, native), options);
  }
  restore(options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.restore(this.id, native), options);
  }
  /** Force close (WinKill); resolves with the pre-close snapshot. */
  kill(options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.kill(this.id, native), options);
  }
  /** Invalidates the window (WinRedraw). */
  redraw(options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.redraw(this.id, native), options);
  }
  /** WinSetTitle: the empty string clears the title. */
  setTitle(title: string, options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.setTitle(this.id, title, native), options);
  }
  /** WinSetEnabled: 1 enables, 0 disables, -1 toggles. */
  setEnabled(value: boolean | -1 | 0 | 1, options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.setEnabled(this.id, value, native), options);
  }
  /** WinSetAlwaysOnTop: absent value means topmost. */
  setAlwaysOnTop(value?: boolean | -1 | 0 | 1, options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate(
      (windows, native) => windows.setAlwaysOnTop(this.id, value, native),
      options,
    );
  }
  /** WinSetStyle: '+N' adds, '-N' removes, '^N' toggles, bare N replaces. */
  setStyle(value: string, options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.setStyle(this.id, value, native), options);
  }
  /** WinSetExStyle: same change-string grammar against the extended style. */
  setExStyle(value: string, options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.setExStyle(this.id, value, native), options);
  }
  /** WinSetTransparent: 0..255 alpha, -1 turns transparency off. */
  setTransparent(value: number, options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate(
      (windows, native) => windows.setTransparent(this.id, value, native),
      options,
    );
  }
  /** WinSetTransColor: ''/'off' clears, hex sets, optional ' <0-255>' alpha. */
  setTransColor(value: string, options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate(
      (windows, native) => windows.setTransColor(this.id, value, native),
      options,
    );
  }
  /** WinSetRegion: '' restores the normal region. */
  setRegion(value = "", options?: ActionOptions): Promise<WindowSnapshot> {
    return this.mutate((windows, native) => windows.setRegion(this.id, value, native), options);
  }
  /** WinGetControls/WinGetControlsHwnd: child controls in z-order. */
  controls(options?: ActionOptions): Promise<WindowControl[]> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.controls(this.id, native)),
    );
  }
  /** WinGetText: concatenated control text. */
  text(options?: ActionOptions): Promise<string> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.text(this.id, native)),
    );
  }
  /** Resolves a control spec against this window (@rime/control Phase 1). */
  control(spec: ControlSpec, options?: ActionOptions): Promise<Control> {
    return Control.resolve(this.id, spec, options);
  }
  /** One-shot control click: resolve + click in a single call. */
  clickControl(
    spec: ControlSpec,
    opts?: ControlClickOptions,
    options?: ActionOptions,
  ): Promise<{ clicks: number }> {
    return Control.resolve(this.id, spec, options).then((control) =>
      control.click(opts, options),
    );
  }
  /** One-shot control setText: resolve + WM_SETTEXT in a single call. */
  setControlText(spec: ControlSpec, text: string, options?: ActionOptions): Promise<{ text: string }> {
    return Control.resolve(this.id, spec, options).then((control) =>
      control.setText(text, options),
    );
  }
  /** One-shot control getText: resolve + WM_GETTEXT in a single call. */
  getControlText(spec: ControlSpec, options?: ActionOptions): Promise<{ text: string }> {
    return Control.resolve(this.id, spec, options).then((control) =>
      control.getText(options),
    );
  }
  /**
   * Focused child control of this window, or 0 when nothing inside holds
   * focus (AHK ControlGetFocus rule). Synchronous.
   */
  focusedControl(): number {
    return focusedControl(this.id);
  }
}
