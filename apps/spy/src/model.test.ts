import { expect, test } from "bun:test";
import { describeProbe, FROZEN_BANNER, type SpyProbe } from "./model";
import type { WindowControl, WindowId, WindowSnapshot } from "@rime/sdk";
import type { ProcessId } from "@rime/sdk";

// Pure-logic unit (L2): describeProbe has no OS, clock or randomness.
// Every expectation is a hand-written literal, so a formatter that drops
// the control line, mismeasures the frame or forgets the banner fails here.

const win = (overrides?: Partial<WindowSnapshot>): WindowSnapshot => ({
  id: 7 as WindowId,
  title: "Slice Window",
  className: "Static",
  processName: "rime_tests.exe",
  processPath: "C:\\build\\tests\\rime_tests.exe",
  rect: { left: 100, top: 200, right: 900, bottom: 800 },
  clientRect: { left: 108, top: 231, right: 900, bottom: 760 },
  visible: true,
  minimized: false,
  state: "normal",
  processId: 42 as ProcessId,
  style: 13565952,
  exStyle: 65536,
  enabled: true,
  alwaysOnTop: false,
  minMax: 0,
  transparent: -1,
  transColor: "",
  region: "",
  ...overrides,
});

const control = (overrides?: Partial<WindowControl>): WindowControl => ({
  id: 9 as WindowId,
  className: "Edit",
  classNN: "Edit1",
  ...overrides,
});

test("desktop probe formats position only", () => {
  const probe: SpyProbe = { x: 5, y: 6, window: null, control: null };
  expect(describeProbe(probe, false)).toBe("desktop @(5,6)");
});

test("window probe formats title, class, process and frame", () => {
  const probe: SpyProbe = { x: 150, y: 250, window: win(), control: null };
  expect(describeProbe(probe, false)).toBe(
    "Slice Window [Static]\npid 42 (rime_tests.exe) 800x600 @(100,200)",
  );
});

test("untitled windows and flags render explicitly", () => {
  const probe: SpyProbe = {
    x: 0,
    y: 0,
    window: win({ title: "", minimized: true, alwaysOnTop: true }),
    control: null,
  };
  expect(describeProbe(probe, false)).toBe(
    "(untitled) [Static]\npid 42 (rime_tests.exe) 800x600 @(100,200) [min] [top]",
  );
});

test("control appends its own line; frozen prepends the banner", () => {
  const probe: SpyProbe = { x: 150, y: 250, window: win(), control: control() };
  expect(describeProbe(probe, false)).toBe(
    "Slice Window [Static]\npid 42 (rime_tests.exe) 800x600 @(100,200)\ncontrol: Edit1 [Edit]",
  );
  expect(describeProbe(probe, true).split("\n")[0]).toBe(FROZEN_BANNER);
});
