// Realism: L4 — Production Showdown integration test verifying all 10 showdown scripts
// exist, parse, export runnable main/lifecycle functions, and execute with zero unhandled rejections.

import { existsSync, readdirSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, mock, test } from "bun:test";

// Mock native rime:* modules for isolated SDK testing
const mockWindowSnapshot = {
  id: 101,
  title: "Showdown Target Window",
  className: "ShowdownClass",
  processName: "notepad.exe",
  processPath: "C:\\Windows\\notepad.exe",
  rect: { left: 100, top: 100, right: 900, bottom: 700 },
  clientRect: { left: 100, top: 100, right: 900, bottom: 700 },
  visible: true,
  minimized: false,
  state: "normal" as const,
  processId: 2048,
  style: 0x14cf0000,
  exStyle: 0x00000100,
  enabled: true,
  alwaysOnTop: false,
  minMax: 0 as const,
  transparent: -1,
  transColor: "",
  region: "",
};

mock.module("rime:runtime", () => ({
  runtime: {
    ping: () => "pong",
    delay: async <T>(_ms: number, value: T) => value,
    cancellation: () => 1,
    cancel: () => true,
    releaseCancellation: () => true,
    subscribe: () => 1,
    unsubscribe: () => true,
    inspect: () => JSON.stringify({ pendingTimers: 0, pendingCompletions: 0 }),
    context: () => ({ schemaVersion: 1, modules: [], tasks: { async: 0, queued: 0, timers: 0, callbacks: 0 }, subscriptions: 0, cancellations: 0 }),
    debug: () => {},
    cwd: () => "C:\\Users\\Administrator\\Desktop\\rime",
    setCwd: () => {},
    exit: () => { throw new Error("exit"); },
    reload: () => { throw new Error("reload"); },
    reloadState: () => ({ count: 0, pending: false }),
    persistent: () => false,
  },
}));

mock.module("rime:window", () => ({
  windows: {
    list: async () => [{ ...mockWindowSnapshot }],
    active: async () => ({ ...mockWindowSnapshot }),
    exists: async () => true,
    isActive: async () => true,
    wait: async () => ({ ...mockWindowSnapshot }),
    info: async () => ({ ...mockWindowSnapshot }),
    controls: async () => [
      { id: 201, className: "Edit", classNN: "Edit1" },
      { id: 202, className: "Button", classNN: "Button1" },
    ],
    text: async () => "Sample Window Text",
    move: async () => ({ ...mockWindowSnapshot }),
    focus: async () => ({ ...mockWindowSnapshot }),
    zorder: async () => ({ ...mockWindowSnapshot }),
    kill: async () => ({ ...mockWindowSnapshot }),
    redraw: async () => ({ ...mockWindowSnapshot }),
    close: async () => ({ ...mockWindowSnapshot }),
    hide: async () => ({ ...mockWindowSnapshot }),
    show: async () => ({ ...mockWindowSnapshot }),
    minimize: async () => ({ ...mockWindowSnapshot }),
    maximize: async () => ({ ...mockWindowSnapshot }),
    restore: async () => ({ ...mockWindowSnapshot }),
    minimizeAll: async () => {},
    minimizeAllUndo: async () => {},
    setTitle: async () => ({ ...mockWindowSnapshot }),
    setEnabled: async () => ({ ...mockWindowSnapshot }),
    setAlwaysOnTop: async () => ({ ...mockWindowSnapshot }),
    setStyle: async () => ({ ...mockWindowSnapshot }),
    setExStyle: async () => ({ ...mockWindowSnapshot }),
    setTransparent: async () => ({ ...mockWindowSnapshot }),
    setTransColor: async () => ({ ...mockWindowSnapshot }),
    setRegion: async () => ({ ...mockWindowSnapshot }),
  },
  groups: {},
  settings: { window: {} },
}));

mock.module("rime:process", () => ({
  process: {
    list: async () => [
      { pid: 2048, parentPid: 1000, name: "notepad.exe", exePath: "C:\\Windows\\notepad.exe" },
      { pid: 3000, parentPid: 1000, name: "cmd.exe", exePath: "C:\\Windows\\System32\\cmd.exe" },
    ],
    info: async () => ({ pid: 2048, parentPid: 1000, name: "notepad.exe", exePath: "C:\\Windows\\notepad.exe" }),
    launch: async (req: { command: string }) => ({ pid: 4096 }),
    edit: async (path: string) => ({ path }),
    terminate: async () => ({ pid: 2048 }),
    wait: async () => ({ pid: 2048 }),
    waitClose: async () => ({ pid: 2048, exitCode: 0 }),
    runWait: async () => ({ pid: 2048, exitCode: 0 }),
    setPriority: async () => ({ pid: 2048 }),
    runAs: async () => ({ pid: 2048 }),
    shutdown: async () => ({ mode: "shutdown" }),
  },
}));

let mockClipText = "Initial Clipboard";
mock.module("rime:clipboard", () => ({
  clipboard: {
    read: async () => ({ text: mockClipText }),
    write: async (text: string) => {
      mockClipText = text;
      return { text };
    },
    wait: async () => ({ ready: true }),
    saveAll: async () => ({ bytes: [1, 2, 3, 4] }),
    restoreAll: async () => ({ formats: 1 }),
  },
}));

mock.module("rime:screen", () => ({
  screen: {
    monitorCount: async () => ({ count: 1 }),
    monitor: async () => ({
      index: 1,
      primary: true,
      name: "\\\\.\\DISPLAY1",
      bounds: { left: 0, top: 0, right: 1920, bottom: 1080 },
      work: { left: 0, top: 0, right: 1920, bottom: 1040 },
    }),
    pixel: async () => ({ color: 0x00ff00 }),
    pixelSearch: async () => ({ found: true, x: 100, y: 100 }),
    imageSearch: async () => ({ found: false }),
    caret: async () => ({ found: false }),
    sysGet: async () => ({ metric: 1920 }),
    sysGetIPAddresses: async () => ({ addresses: ["127.0.0.1"] }),
  },
}));

let mockVol = 75.0;
let mockMuted = false;
mock.module("rime:sound", () => ({
  sound: {
    beep: async () => null,
    play: async () => null,
    getVolume: async () => mockVol,
    setVolume: async (val: number | string) => {
      if (typeof val === "number") mockVol = val;
      return null;
    },
    getMute: async () => mockMuted,
    setMute: async (m: boolean) => {
      mockMuted = m;
      return null;
    },
    getName: async () => "High Definition Audio Device",
  },
}));

mock.module("rime:input", () => ({
  input: {
    subscribe: () => 1,
    unsubscribe: () => true,
    bind: () => 1,
    unbind: () => true,
    send: async (steps: unknown[]) => ({ sent: steps.length }),
    modifiers: () => ({
      lcontrol: false,
      rcontrol: false,
      lshift: false,
      rshift: false,
      lalt: false,
      ralt: false,
      lwin: false,
      rwin: false,
      capsLock: false,
    }),
    mouse: async (p: { steps: unknown[] }) => ({ sent: p.steps.length }),
    mouseGetPos: async () => ({ x: 200, y: 200, window: null, control: null }),
    getKeyState: () => false,
    getKeySC: () => 0x1e,
    getKeyVK: () => 0x41,
    getKeyName: () => "a",
    keyWait: async () => true,
    blockInput: () => true,
    sendLevel: () => 0,
    setLockForce: () => {},
    keyHistory: () => ({ capacity: 40, count: 0, events: [] }),
    listHotkeys: () => ({ suspended: false, hotkeys: [] }),
    hotkey: () => ({ close: () => {} }),
    hotstring: () => ({ close: () => {} }),
    hotIf: () => ({ id: 0, kind: "unconditional" }),
    hotIfWinActive: () => ({ id: 0, kind: "active" }),
    hotIfWinExist: () => ({ id: 0, kind: "exist" }),
    hotIfWinNotActive: () => ({ id: 0, kind: "notActive" }),
    hotIfWinNotExist: () => ({ id: 0, kind: "notExist" }),
    installKeybdHook: () => true,
    installMouseHook: () => true,
    setTimer: () => ({ close: () => {} }),
    onMessage: () => ({ close: () => {} }),
    onClipboardChange: () => ({ close: () => {} }),
    onError: () => ({ close: () => {} }),
    onExit: () => ({ close: () => {} }),
    createInputHook: () => ({} as never),
    suspend: () => false,
  },
}));

mock.module("rime:control", () => ({
  control: {
    resolve: async () => ({ id: 501, className: "Edit", classNN: "Edit1" }),
    click: async () => ({ clicks: 1 }),
    focus: async () => ({ focused: true }),
    setText: async (_id: number, text: string) => ({ text }),
    getText: async () => ({ text: "Sample Field Text" }),
    sendText: async (_id: number, text: string) => ({ text }),
    isVisible: () => true,
    isEnabled: () => true,
    rect: () => ({ x: 10, y: 10, width: 100, height: 25 }),
    dispose: () => true,
    listAdd: async () => ({ index: 1 }),
    listDelete: async () => ({ deleted: true }),
    listChoose: async () => ({ chosen: true }),
    listIndex: async () => ({ index: 0 }),
    listChoice: async () => ({ text: "Item" }),
    listItems: async () => ({ items: ["Item1"] }),
    tabSelect: async () => ({ selected: true }),
    editCount: async () => ({ lines: 1 }),
    editCaret: async () => ({ line: 1, col: 1 }),
    editLine: async () => ({ text: "Line" }),
    editSelected: async () => ({ text: "" }),
    editPaste: async () => ({ text: "Pasted" }),
    setChecked: async () => ({ checked: true }),
    isChecked: async () => ({ checked: false }),
    show: async () => ({ visible: true }),
    hide: async () => ({ visible: false }),
    move: async () => ({ moved: true }),
    setEnabled: async () => ({ enabled: true }),
    tabIndex: async () => ({ index: 0 }),
    showDropdown: async () => ({ dropped: true }),
    hideDropdown: async () => ({ dropped: false }),
    setStyle: async () => ({ styled: true }),
    setExStyle: async () => ({ styled: true }),
    send: async () => ({ steps: 1 }),
    classNN: () => "Edit1",
    getStyle: () => 0,
    getExStyle: () => 0,
    listviewCount: async () => ({ rows: 0 }),
    listviewText: async () => ({ text: "" }),
    listviewItems: async () => ({ items: [] }),
    statusbarText: async () => ({ text: "" }),
    statusbarWait: async () => ({ waited: true }),
  },
}));

mock.module("rime:ui", () => ({
  ui: {
    msgBox: async () => "OK",
    inputBox: async () => ({ value: "input", result: "OK" }),
    toolTip: async () => null,
    traySetIcon: async () => null,
    trayTip: async () => null,
    createMenu: async () => ({
      Add: async () => 1,
      Insert: async () => 1,
      Delete: async () => "Item",
      Rename: async () => "Old",
      Check: async () => undefined,
      Uncheck: async () => undefined,
      Enable: async () => undefined,
      Disable: async () => undefined,
      ToggleCheck: async () => true,
      ToggleEnable: async () => true,
      SetColor: async () => undefined,
      SetIcon: async () => undefined,
      Show: async () => "Item",
      Destroy: async () => undefined,
    }),
  },
  createGui: async () => ({} as never),
}));

mock.module("rime:automation", () => ({
  automation: {
    find: async () => ({ elements: [] }),
    read: async () => ({ value: null }),
    invoke: async () => ({ value: null }),
    release: async () => ({ released: 0 }),
  },
}));

mock.module("rime:storage", () => ({
  storage: {
    stat: async () => ({ size: 0, isDir: false, mtimeMs: 0 }),
    readFile: async () => "",
    writeFile: async () => {},
    readdir: async () => [],
    watch: () => ({ close: () => {} }),
  },
}));

mock.module("rime:registry", () => ({
  registry: {
    read: async () => ({ value: "" }),
    write: async () => ({ written: true }),
    delete: async () => ({ deleted: true }),
  },
}));

describe("Real-World Showdown Suite (10 Production Applications)", () => {
  const showdownDir = resolve(import.meta.dir, "../../examples/showdown");

  test("all 10 showdown scripts exist on disk with expected naming", () => {
    const expectedFiles = [
      "01-window-tiler.ts",
      "02-clipboard-history.ts",
      "03-text-expander.ts",
      "04-quick-launcher.ts",
      "05-batch-form-filler.ts",
      "06-audio-switcher.ts",
      "07-rapid-clicker.ts",
      "08-pixel-sentinel.ts",
      "09-tray-controller.ts",
      "10-daemon-supervisor.ts",
    ];

    expect(existsSync(showdownDir)).toBe(true);
    const files = readdirSync(showdownDir);

    for (const expected of expectedFiles) {
      expect(files).toContain(expected);
      const filePath = resolve(showdownDir, expected);
      expect(existsSync(filePath)).toBe(true);
    }
  });

  test("01-window-tiler.ts exports runnable main and accurate grid layout math", async () => {
    const mod = await import("../../examples/showdown/01-window-tiler");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.calculateGridLayout).toBe("function");
    expect(typeof mod.filterTileableWindows).toBe("function");
    expect(typeof mod.tileWindows).toBe("function");

    const workArea = { left: 0, top: 0, right: 1920, bottom: 1040 };

    // 1 window -> 1x1
    const g1 = mod.calculateGridLayout(1, workArea);
    expect(g1.cols).toBe(1);
    expect(g1.rows).toBe(1);
    expect(g1.cellWidth).toBe(1920);
    expect(g1.cellHeight).toBe(1040);

    // 2 windows -> 2x1
    const g2 = mod.calculateGridLayout(2, workArea);
    expect(g2.cols).toBe(2);
    expect(g2.rows).toBe(1);
    expect(g2.cellWidth).toBe(960);
    expect(g2.cellHeight).toBe(1040);

    // 4 windows -> 2x2
    const g4 = mod.calculateGridLayout(4, workArea);
    expect(g4.cols).toBe(2);
    expect(g4.rows).toBe(2);
    expect(g4.cellWidth).toBe(960);
    expect(g4.cellHeight).toBe(520);

    // Main executes cleanly
    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("02-clipboard-history.ts exports ring buffer manager with bounded history", async () => {
    const mod = await import("../../examples/showdown/02-clipboard-history");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.ClipboardRingBuffer).toBe("function");

    const buffer = new mod.ClipboardRingBuffer(3);
    expect(buffer.size).toBe(0);

    // Record items
    buffer.record("A");
    buffer.record("B");
    buffer.record("C");
    expect(buffer.size).toBe(3);

    // Deduplication
    const dup = buffer.record("C");
    expect(dup).toBeNull();
    expect(buffer.size).toBe(3);

    // Exceed capacity: drops oldest ("A")
    buffer.record("D");
    expect(buffer.size).toBe(3);
    const texts = buffer.entries.map((e) => e.text);
    expect(texts).toEqual(["D", "C", "B"]);

    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("03-text-expander.ts exports snippet expansion engine with registration lifecycle", async () => {
    const mod = await import("../../examples/showdown/03-text-expander");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.SnippetEngine).toBe("function");
    expect(Array.isArray(mod.SHOWDOWN_SNIPPETS)).toBe(true);

    const engine = new mod.SnippetEngine();
    const ok = engine.register({
      trigger: "test",
      replacement: "Testing snippet",
    });
    expect(ok).toBe(true);
    expect(engine.size).toBe(1);

    const removed = engine.unregister("test");
    expect(removed).toBe(true);
    expect(engine.size).toBe(0);

    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("04-quick-launcher.ts exports QuickLauncher with switch/launch capabilities", async () => {
    const mod = await import("../../examples/showdown/04-quick-launcher");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.QuickLauncher).toBe("function");
    expect(Array.isArray(mod.DEFAULT_TARGETS)).toBe(true);

    const launcher = new mod.QuickLauncher(mod.DEFAULT_TARGETS);
    launcher.start();

    const result = await launcher.activateOrLaunch(mod.DEFAULT_TARGETS[0]);
    expect(["focused", "launched", "in_flight"]).toContain(result);

    launcher.dispose();
    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("05-batch-form-filler.ts exports rapid RPA form filler", async () => {
    const mod = await import("../../examples/showdown/05-batch-form-filler");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.fillForm).toBe("function");

    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("06-audio-switcher.ts exports volume, mute, and sound automation helpers", async () => {
    const mod = await import("../../examples/showdown/06-audio-switcher");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.volume).toBe("function");
    expect(typeof mod.mute).toBe("function");
    expect(typeof mod.getAudioSnapshot).toBe("function");

    const snap = await mod.getAudioSnapshot();
    expect(typeof snap.volume).toBe("number");
    expect(typeof snap.muted).toBe("boolean");
    expect(typeof snap.deviceName).toBe("string");

    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("07-rapid-clicker.ts exports low-latency TurboClicker with cooperative cancellation", async () => {
    const mod = await import("../../examples/showdown/07-rapid-clicker");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.TurboClicker).toBe("function");
    expect(typeof mod.SimpleAbortController).toBe("function");

    const controller = new mod.SimpleAbortController();
    expect(controller.signal.aborted).toBe(false);
    controller.abort();
    expect(controller.signal.aborted).toBe(true);

    const clicker = new mod.TurboClicker({ intervalMs: 1 });
    const clicks = await clicker.startBurst(3);
    expect(clicks).toBe(3);

    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("08-pixel-sentinel.ts exports pixel monitor and hex color parser", async () => {
    const mod = await import("../../examples/showdown/08-pixel-sentinel");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.PixelSentinel).toBe("function");
    expect(typeof mod.parseHexColor).toBe("function");

    const sample = mod.parseHexColor(0x123456);
    expect(sample.color).toBe(0x123456);
    expect(sample.hex).toBe("0x123456");
    expect(sample.r).toBe(0x12);
    expect(sample.g).toBe(0x34);
    expect(sample.b).toBe(0x56);

    const sentinel = new mod.PixelSentinel({ x: 50, y: 50 });
    const s = await sentinel.sample();
    expect(s).not.toBeNull();
    expect(s?.hex).toBe("0x00FF00");

    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("09-tray-controller.ts exports tray controller with dynamic menu building", async () => {
    const mod = await import("../../examples/showdown/09-tray-controller");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.TrayController).toBe("function");

    const controller = new mod.TrayController();
    await controller.setIcon("*");
    const menu = await controller.buildMenu([
      { name: "Item 1" },
      { name: "Item 2", checked: true },
    ]);
    expect(menu).toBeDefined();

    await controller.dispose();
    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("10-daemon-supervisor.ts exports process watchdog with auto-restart supervisor", async () => {
    const mod = await import("../../examples/showdown/10-daemon-supervisor");
    expect(typeof mod.main).toBe("function");
    expect(typeof mod.DaemonSupervisor).toBe("function");

    const supervisor = new mod.DaemonSupervisor({
      command: "notepad.exe",
      processName: "notepad.exe",
      maxRestarts: 2,
      checkIntervalMs: 10,
    });

    const status = await supervisor.checkAndHeal();
    expect(status.targetProcessName).toBe("notepad.exe");
    expect(status.maxRestarts).toBe(2);
    expect(typeof status.isAlive).toBe("boolean");

    await expect(mod.main()).resolves.toBeUndefined();
  });

  test("all showdown executions completed with zero global unhandled failures", () => {
    const failure = (globalThis as { __rim_failure?: string }).__rim_failure;
    expect(failure).toBeUndefined();
  });
});
