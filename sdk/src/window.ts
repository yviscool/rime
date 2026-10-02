import { runAction, type ActionOptions, type NativeActionOptions } from "./action";
import type { ProcessId } from "./process";

export type Brand<K, T> = K & { readonly __brand: T };

/** Stable window id issued by the UI-lane registry (never a raw HWND). */
export type WindowId = Brand<number, "WindowId">;

export type WindowPlacement = "left" | "right" | "top" | "bottom" | "full";

export type WindowState = "normal" | "minimized" | "maximized" | "hidden";

export interface WindowRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

/** Snapshot returned by every window read and mutation (window-v1 snapshot). */
export interface WindowHandle {
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
}

/** A child control from `windows.controls` (WinGetControls/WinGetControlsHwnd). */
export interface WindowControl {
  /** Stable id in the same space as `WindowHandle.id` — never a raw HWND. */
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
export interface WindowQueryFields {
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
export type WindowsListOptions = WindowQueryFields & NativeActionOptions;

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
  activated: WindowHandle | null;
}

/** Bridge of the `rime:window` module. Every mutation is an Action. */
export interface WindowsBridge {
  list(options?: WindowsListOptions): Promise<WindowHandle[]>;
  /** Resolves null when no window is foreground. */
  active(options?: NativeActionOptions): Promise<WindowHandle | null>;
  /** WinExist: true when at least one window matches the query. */
  exists(options?: WindowsListOptions): Promise<boolean>;
  /** WinActive: true when the foreground window matches the query. */
  isActive(options?: WindowsListOptions): Promise<boolean>;
  /**
   * WinWait family: resolves when `until` (default "exists") is satisfied —
   * the target snapshot for exists/active, null for closed/notActive.
   * Rejects with `timeout` once deadlineMs (default 5000) elapses.
   */
  wait(options?: WindowsWaitOptions): Promise<WindowHandle | null>;
  info(windowId: WindowId, options?: NativeActionOptions): Promise<WindowHandle>;
  /** WinGetControls/WinGetControlsHwnd: child controls in z-order (hidden included). */
  controls(windowId: WindowId, options?: NativeActionOptions): Promise<WindowControl[]>;
  /** WinGetText: concatenated control text, "\r\n" after each non-empty entry. */
  text(windowId: WindowId, options?: NativeActionOptions): Promise<string>;
  /** `target` is a window id or the string "active". */
  move(
    target: WindowId | "active",
    position: WindowPlacement,
    options?: NativeActionOptions,
  ): Promise<WindowHandle>;
  focus(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  close(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  hide(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  show(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  minimize(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  maximize(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
  restore(target: WindowId | "active", options?: NativeActionOptions): Promise<WindowHandle>;
}

/** Bridge of the named-window-group surface (`rime:window`'s `groups` export). */
export interface WindowsGroupsBridge {
  /**
   * GroupAdd: appends a query spec to the group unless an exact duplicate
   * is already registered; a missing group is created.
   */
  add(
    name: string,
    query: WindowQueryFields,
    options?: NativeActionOptions,
  ): Promise<{ count: number }>;
  /** GroupActivate: cycles focus through the members; null when there is nothing to activate. */
  activate(name: string, options?: GroupFocusOptions): Promise<WindowHandle | null>;
  /** GroupDeactivate: activates an eligible non-member; null when there is none. */
  deactivate(name: string, options?: GroupFocusOptions): Promise<WindowHandle | null>;
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
  add(name: string, query?: WindowQueryFields & ActionOptions): Promise<number>;
  /**
   * GroupActivate: cycles focus through the group's members (oldest first;
   * `reverse` starts at the most recent). A missing group is created and
   * resolves null; an existing empty group also resolves null.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  activate(name: string, options?: GroupFocusOptions & ActionOptions): Promise<WindowHandle | null>;
  /**
   * GroupDeactivate: activates an eligible non-member (AHK's "deactivate to
   * the next window"). The group must exist.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  deactivate(
    name: string,
    options?: GroupFocusOptions & ActionOptions,
  ): Promise<WindowHandle | null>;
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

export interface ActiveWindowRequest {
  /** Moves the foreground window through the `window.move` action pipeline. */
  move(position: WindowPlacement, options?: ActionOptions): Promise<WindowHandle>;
  focus(options?: ActionOptions): Promise<WindowHandle>;
  close(options?: ActionOptions): Promise<WindowHandle>;
  hide(options?: ActionOptions): Promise<WindowHandle>;
  show(options?: ActionOptions): Promise<WindowHandle>;
  minimize(options?: ActionOptions): Promise<WindowHandle>;
  maximize(options?: ActionOptions): Promise<WindowHandle>;
  restore(options?: ActionOptions): Promise<WindowHandle>;
}

function mutation(
  pick: (bridge: WindowsBridge) => (target: WindowId | "active", options?: NativeActionOptions) => Promise<WindowHandle>,
): (target: WindowId, options?: ActionOptions) => Promise<WindowHandle> {
  return (target, options) =>
    runAction(options, (native) => windowBridge().then((windows) => pick(windows)(target, native)));
}

export const Window = {
  /** Lazy handle on the foreground window; resolves when the operation runs. */
  active(): ActiveWindowRequest {
    return {
      move: (position, options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.move("active", position, native)),
        ),
      focus: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.focus("active", native)),
        ),
      close: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.close("active", native)),
        ),
      hide: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.hide("active", native)),
        ),
      show: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.show("active", native)),
        ),
      minimize: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.minimize("active", native)),
        ),
      maximize: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.maximize("active", native)),
        ),
      restore: (options) =>
        runAction(options, (native) =>
          windowBridge().then((windows) => windows.restore("active", native)),
        ),
    };
  },
  /**
   * Moves a window through the `window.move` action pipeline.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`.
   */
  move(windowId: WindowId, position: WindowPlacement, options?: ActionOptions): Promise<WindowHandle> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.move(windowId, position, native)),
    );
  },
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  focus: mutation((windows) => windows.focus),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  close: mutation((windows) => windows.close),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  hide: mutation((windows) => windows.hide),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  show: mutation((windows) => windows.show),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  minimize: mutation((windows) => windows.minimize),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  maximize: mutation((windows) => windows.maximize),
  /** @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`. */
  restore: mutation((windows) => windows.restore),
  /**
   * Lists windows matching the query (window-v1 query; `schemaVersion` stays
   * in the vocabulary and never goes on the wire).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  list(query?: WindowQueryFields & ActionOptions): Promise<WindowHandle[]> {
    // `signal` never reaches the wire; it is converted to a cancellation id.
    const { signal: _signal, ...fields } = query ?? {};
    return runAction(query, (native) =>
      windowBridge().then((windows) => windows.list({ ...fields, ...native })),
    );
  },
  /**
   * Reads one window snapshot by id.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`.
   */
  info(windowId: WindowId, options?: ActionOptions): Promise<WindowHandle> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.info(windowId, native)),
    );
  },
  /**
   * WinGetControls/WinGetControlsHwnd: child controls with stable ids and
   * AHK ClassNN names, in z-order (hidden controls included).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`.
   */
  controls(windowId: WindowId, options?: ActionOptions): Promise<WindowControl[]> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.controls(windowId, native)),
    );
  },
  /**
   * WinGetText: concatenated control text ("\r\n"-separated); hidden controls
   * are skipped while DetectHiddenText is off.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` / `target_gone`.
   */
  text(windowId: WindowId, options?: ActionOptions): Promise<string> {
    return runAction(options, (native) =>
      windowBridge().then((windows) => windows.text(windowId, native)),
    );
  },
  /**
   * WinExist: true when at least one window matches the query.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  exists(query?: WindowQueryFields & ActionOptions): Promise<boolean> {
    const { signal: _signal, ...fields } = query ?? {};
    return runAction(query, (native) =>
      windowBridge().then((windows) => windows.exists({ ...fields, ...native })),
    );
  },
  /**
   * WinActive: true when the foreground window matches the query.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  isActive(query?: WindowQueryFields & ActionOptions): Promise<boolean> {
    const { signal: _signal, ...fields } = query ?? {};
    return runAction(query, (native) =>
      windowBridge().then((windows) => windows.isActive({ ...fields, ...native })),
    );
  },
  /**
   * WinWait/WinWaitActive/WinWaitClose/WinWaitNotActive: polls until `until`
   * is satisfied without blocking a thread. Resolves with the target
   * snapshot (exists/active) or null (closed/notActive).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  wait(
    query?: WindowQueryFields & { until?: WindowWaitUntil } & ActionOptions,
  ): Promise<WindowHandle | null> {
    const { signal: _signal, ...fields } = query ?? {};
    return runAction(query, (native) =>
      windowBridge().then((windows) => windows.wait({ ...fields, ...native })),
    );
  },
};
