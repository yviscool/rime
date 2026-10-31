import { beforeEach, expect, mock, test } from "bun:test";
import type { NativeActionOptions } from "../../sdk/src/action";
import type { ScreenRect } from "../../sdk/src/screen";
import type { WindowSnapshot, WindowId } from "../../sdk/src/window";
import type { ProcessId } from "../../sdk/src/process";

// Realism: L3 - tests visual semantic OCR text finder in sdk/src/screen.ts,
// sdk/src/automation.ts, and sdk/src/window.ts against instrumented screen,
// input, and automation bridges.

interface OcrItem {
  text: string;
  rect: ScreenRect;
}

let ocrItems: OcrItem[] = [];
let mousePayloads: any[] = [];
let automationElements: any[] = [];
let ocrEnabled = true;

const screenBridgeMock = {
  monitorCount: async () => ({ count: 1 }),
  monitor: async () => ({
    index: 1,
    primary: true,
    name: "\\\\.\\DISPLAY1",
    bounds: { left: 0, top: 0, right: 1920, bottom: 1080 },
    work: { left: 0, top: 0, right: 1920, bottom: 1040 },
  }),
  pixel: async () => ({ color: 0xffffff }),
  pixelSearch: async () => ({ found: false as const }),
  imageSearch: async () => ({ found: false as const }),
  caret: async () => ({ found: false as const }),
  sysGet: async () => ({ metric: 0 }),
  sysGetIPAddresses: async () => ({ addresses: ["127.0.0.1"] }),
  ocr: async (_area?: ScreenRect, _options?: NativeActionOptions) => {
    if (!ocrEnabled) throw new Error("OCR not available");
    return ocrItems;
  },
};

mock.module("rime:screen", () => ({ screen: screenBridgeMock }));

mock.module("rime:input", () => ({
  input: {
    mouse: async (payload: any) => {
      mousePayloads.push(payload);
      return { sent: payload.steps?.length ?? 1 };
    },
  },
}));

mock.module("rime:automation", () => ({
  automation: {
    find: async () => ({ elements: automationElements }),
    read: async (id: number) => ({ id, name: "mock" }),
    invoke: async () => ({ invoked: true as const }),
    release: () => true,
  },
}));

const { screen } = await import("../../sdk/src/screen");
const { automation } = await import("../../sdk/src/automation");
const { Window } = await import("../../sdk/src/window");

function reset(): void {
  ocrEnabled = true;
  ocrItems = [
    { text: "File", rect: { left: 10, top: 10, right: 50, bottom: 30 } },
    { text: "Save As...", rect: { left: 100, top: 200, right: 300, bottom: 250 } },
    { text: "Cancel Order", rect: { left: 400, top: 500, right: 600, bottom: 560 } },
    { text: "Submit Button", rect: { left: 11, top: 21, right: 101, bottom: 81 } },
  ];
  mousePayloads = [];
  automationElements = [];
}

beforeEach(reset);

test("screen.findText validates arguments", async () => {
  // @ts-expect-error non-string text
  await expect(screen.findText(123)).rejects.toThrow(TypeError);
  // @ts-expect-error null text
  await expect(screen.findText(null)).rejects.toThrow(TypeError);
  const emptyRes = await screen.findText("");
  expect(emptyRes).toBeNull();
});

test("screen.findText semantics: exact match vs substring match", async () => {
  // Substring match (default)
  const partialMatch = await screen.findText("Cancel");
  expect(partialMatch).not.toBeNull();
  expect(partialMatch?.text).toBe("Cancel Order");

  // Exact match - should miss "Cancel" because text is "Cancel Order"
  const exactMiss = await screen.findText("Cancel", { exact: true });
  expect(exactMiss).toBeNull();

  // Exact match - should hit with full text
  const exactHit = await screen.findText("Cancel Order", { exact: true });
  expect(exactHit).not.toBeNull();
  expect(exactHit?.text).toBe("Cancel Order");
});

test("screen.findText semantics: case sensitivity", async () => {
  // Case-insensitive (default)
  const insensitiveHit = await screen.findText("save as...");
  expect(insensitiveHit).not.toBeNull();
  expect(insensitiveHit?.text).toBe("Save As...");

  // Case-sensitive - should miss when case doesn't match
  const sensitiveMiss = await screen.findText("save as...", { caseSensitive: true });
  expect(sensitiveMiss).toBeNull();

  // Case-sensitive - should hit when case matches exactly
  const sensitiveHit = await screen.findText("Save As...", { caseSensitive: true });
  expect(sensitiveHit).not.toBeNull();
  expect(sensitiveHit?.text).toBe("Save As...");
});

test("screen.findText semantics: area filtering", async () => {
  // Target "File" is at (10, 10, 50, 30)
  // Searching within area including (10, 10, 50, 30)
  const inAreaHit = await screen.findText("File", {
    area: { left: 0, top: 0, right: 80, bottom: 50 },
  });
  expect(inAreaHit).not.toBeNull();
  expect(inAreaHit?.text).toBe("File");

  // Searching within area excluding (10, 10, 50, 30)
  const outAreaMiss = await screen.findText("File", {
    area: { left: 100, top: 100, right: 400, bottom: 400 },
  });
  expect(outAreaMiss).toBeNull();
});

test("screen.findText center coordinate calculation", async () => {
  // "Save As..." rect is { left: 100, top: 200, right: 300, bottom: 250 }
  // center.x = (100 + 300) / 2 = 200
  // center.y = (200 + 250) / 2 = 225
  const match = await screen.findText("Save As...");
  expect(match).not.toBeNull();
  expect(match?.rect).toEqual({ left: 100, top: 200, right: 300, bottom: 250 });
  expect(match?.center).toEqual({ x: 200, y: 225 });

  // "Submit Button" rect is { left: 11, top: 21, right: 101, bottom: 81 }
  // center.x = Math.round((11 + 101) / 2) = 56
  // center.y = Math.round((21 + 81) / 2) = 51
  const submitMatch = await screen.findText("Submit Button");
  expect(submitMatch).not.toBeNull();
  expect(submitMatch?.center).toEqual({ x: 56, y: 51 });
});

test("TextMatch.click() executes mouse click at center coordinates", async () => {
  const match = await screen.findText("Save As...");
  expect(match).not.toBeNull();

  const clickResult = await match!.click();
  expect(clickResult).toEqual({ clicks: 1 });

  // Verify mouse was injected at the center coordinates (200, 225)
  expect(mousePayloads.length).toBe(1);
  const steps = mousePayloads[0].steps;
  expect(steps).toBeDefined();
  expect(steps[0]).toEqual({ action: "move", x: 200, y: 225 });
  expect(steps[1]).toEqual({ action: "down", button: 1 });
  expect(steps[2]).toEqual({ action: "up", button: 1 });
});

test("screen.findText respects timeoutMs when waiting for dynamic text", async () => {
  // Clear OCR initially
  ocrItems = [];

  // Add the item after 20ms
  setTimeout(() => {
    ocrItems.push({
      text: "Dynamic Alert",
      rect: { left: 50, top: 50, right: 150, bottom: 90 },
    });
  }, 20);

  const match = await screen.findText("Dynamic Alert", { timeoutMs: 150 });
  expect(match).not.toBeNull();
  expect(match?.text).toBe("Dynamic Alert");
});

test("screen.findText falls back to UIA automation when OCR is unavailable", async () => {
  ocrEnabled = false;
  automationElements = [
    {
      id: 42,
      name: "Confirm Dialog",
      controlType: "button",
      automationId: "btn_confirm",
      enabled: true,
      x: 300,
      y: 400,
      width: 100,
      height: 40,
    },
  ];

  const match = await screen.findText("Confirm Dialog");
  expect(match).not.toBeNull();
  expect(match?.text).toBe("Confirm Dialog");
  expect(match?.rect).toEqual({ left: 300, top: 400, right: 400, bottom: 440 });
  expect(match?.center).toEqual({ x: 350, y: 420 });

  const clickRes = await match!.click();
  expect(clickRes).toEqual({ clicks: 1 });
  expect(mousePayloads.length).toBe(1);
  expect(mousePayloads[0].steps[0]).toEqual({ action: "move", x: 350, y: 420 });
});

test("window.findText searches within window client bounds", async () => {
  const fakeSnapshot: WindowSnapshot = {
    id: 101 as WindowId,
    title: "App Window",
    className: "AppClass",
    processName: "app.exe",
    processPath: "C:\\app.exe",
    rect: { left: 100, top: 100, right: 800, bottom: 600 },
    clientRect: { left: 110, top: 140, right: 790, bottom: 590 },
    visible: true,
    minimized: false,
    state: "normal",
    processId: 1000 as ProcessId,
    style: 0,
    exStyle: 0,
    enabled: true,
    alwaysOnTop: false,
    minMax: 0,
    transparent: -1,
    transColor: "",
    region: "",
  };

  const win = Window.fromSnapshot(fakeSnapshot);

  // "File" at (10, 10, 50, 30) is completely outside clientRect (110, 140, 790, 590)
  const fileMatch = await win.findText("File");
  expect(fileMatch).toBeNull();

  // "Save As..." at (100, 200, 300, 250) overlaps/intersects clientRect
  const saveMatch = await win.findText("Save As...");
  expect(saveMatch).not.toBeNull();
  expect(saveMatch?.text).toBe("Save As...");
  expect(saveMatch?.center).toEqual({ x: 200, y: 225 });

  // Calling .click() on the window match
  const clickRes = await saveMatch!.click();
  expect(clickRes).toEqual({ clicks: 1 });
});

test("automation.findText and automation.visual.findText delegate to visual tier", async () => {
  const match1 = await automation.findText?.("File");
  expect(match1).not.toBeNull();
  expect(match1?.text).toBe("File");

  const match2 = await automation.visual?.findText("Cancel Order");
  expect(match2).not.toBeNull();
  expect(match2?.text).toBe("Cancel Order");
});
