import { beforeEach, expect, mock, test } from "bun:test";
import { ActionError, type NativeActionOptions } from "../../sdk/src/action";

// Realism: L3 - the real sdk/src/screen.ts facade runs against an
// instrumented rime:screen bridge; the native module is the environment, never
// the unit under test. Every expectation below is a literal written out by
// hand, so a facade that drops an option cannot pass by agreeing with itself.

type Area = { left: number; top: number; right: number; bottom: number };
type PixelResult = { found: false } | { found: true; x: number; y: number };
type ScreenOptions = NativeActionOptions & { variation?: number };
type CodedRejection = { code: string; message: string };

const codedError = ({ code, message }: CodedRejection) =>
  Object.assign(new Error(message), { code });

let countCalls: Array<NativeActionOptions | undefined> = [];
let monitorCalls: Array<[number | undefined, NativeActionOptions | undefined]> = [];
let pixelCalls: Array<[number, number, NativeActionOptions | undefined]> = [];
let pixelSearchCalls: Array<[Area, number, ScreenOptions | undefined]> = [];
let imageSearchCalls: Array<[Area, string, ScreenOptions | undefined]> = [];

let pixelResult: { color: number } = { color: 0x00030540 };
let pixelSearchResult: PixelResult = { found: true, x: 2, y: 3 };
let imageSearchResult: PixelResult = { found: false };
let pixelRejection: CodedRejection | null = null;

function reset(): void {
  countCalls = [];
  monitorCalls = [];
  pixelCalls = [];
  pixelSearchCalls = [];
  imageSearchCalls = [];
  pixelResult = { color: 0x00030540 };
  pixelSearchResult = { found: true, x: 2, y: 3 };
  imageSearchResult = { found: false };
  pixelRejection = null;
}

const bridge = {
  async monitorCount(options?: NativeActionOptions) {
    countCalls.push(options);
    return { count: 3 };
  },
  async monitor(index?: number | NativeActionOptions, options?: NativeActionOptions) {
    monitorCalls.push([typeof index === "number" ? index : undefined, options]);
    return {
      index: typeof index === "number" ? index : 1,
      primary: typeof index !== "number",
      name: "\\\\.\\DISPLAY1",
      bounds: { left: 0, top: 0, right: 1920, bottom: 1080 },
      work: { left: 0, top: 0, right: 1920, bottom: 1040 },
    };
  },
  async pixel(x: number, y: number, options?: NativeActionOptions) {
    pixelCalls.push([x, y, options]);
    if (pixelRejection) throw codedError(pixelRejection);
    return pixelResult;
  },
  async pixelSearch(area: Area, color: number, options?: ScreenOptions) {
    pixelSearchCalls.push([area, color, options]);
    return pixelSearchResult;
  },
  async imageSearch(area: Area, imagePath: string, options?: ScreenOptions) {
    imageSearchCalls.push([area, imagePath, options]);
    return imageSearchResult;
  },
};

mock.module("rime:screen", () => ({ screen: bridge }));

const { screen } = await import("../../sdk/src/screen");

beforeEach(reset);

const area: Area = { left: 0, top: 0, right: 7, bottom: 7 };
const outside: Area = { left: 10, top: 10, right: 11, bottom: 11 };

test("pixelSearch forwards variation alongside the action options", async () => {
  const result = await screen.pixelSearch(area, 0x00020340, { variation: 64, deadlineMs: 250 });
  // runAction rebuilds the bridge options from the action fields only, so the
  // facade has to merge the screen-domain option back in. Dropping it here
  // would silently widen or narrow every search.
  expect(pixelSearchCalls).toEqual([[area, 0x00020340, { deadlineMs: 250, variation: 64 }]]);
  expect(result).toEqual({ found: true, x: 2, y: 3 });
});

test("pixelSearch passes no variation when the caller gave none", async () => {
  await screen.pixelSearch(area, 0x112233);
  expect(pixelSearchCalls).toEqual([[area, 0x112233, undefined]]);

  await screen.pixelSearch(area, 0x112233, { deadlineMs: 50 });
  expect(pixelSearchCalls.at(-1)).toEqual([area, 0x112233, { deadlineMs: 50 }]);
});

test("pixelSearch resolves a miss without coordinates", async () => {
  pixelSearchResult = { found: false };
  const result = await screen.pixelSearch(area, 0x123456);
  expect(result).toEqual({ found: false });
  expect("x" in result).toBe(false);
});

test("imageSearch forwards the area, the path and the variation", async () => {
  const result = await screen.imageSearch(area, "C:\\tmp\\needle.bmp", { variation: 5 });
  expect(imageSearchCalls).toEqual([
    [area, "C:\\tmp\\needle.bmp", { variation: 5 }],
  ]);
  expect(result).toEqual({ found: false });
});

test("imageSearch forwards a hit the same way pixelSearch does", async () => {
  imageSearchResult = { found: true, x: 4, y: 6 };
  const result = await screen.imageSearch(outside, "needle.png", { deadlineMs: 100 });
  expect(imageSearchCalls.at(-1)).toEqual([
    outside,
    "needle.png",
    { deadlineMs: 100 },
  ]);
  expect(result).toEqual({ found: true, x: 4, y: 6 });
});

test("the facade unwraps the pixel colour and the monitor count", async () => {
  await expect(screen.pixel(3, 5)).resolves.toBe(0x00030540);
  expect(pixelCalls).toEqual([[3, 5, undefined]]);

  pixelResult = { color: 0x00020340 };
  await expect(screen.pixel(2, 3, { deadlineMs: 10 })).resolves.toBe(0x00020340);
  expect(pixelCalls.at(-1)).toEqual([2, 3, { deadlineMs: 10 }]);

  await expect(screen.monitorCount()).resolves.toBe(3);
  expect(countCalls).toEqual([undefined]);
});

test("monitor passes an index through and an omitted index as options only", async () => {
  const second = await screen.monitor(2, { deadlineMs: 250 });
  expect(second.index).toBe(2);
  expect(monitorCalls.at(-1)).toEqual([2, { deadlineMs: 250 }]);

  const primary = await screen.monitor();
  expect(primary.primary).toBe(true);
  expect(monitorCalls.at(-1)).toEqual([undefined, undefined]);
});

test("a coded bridge rejection becomes an ActionError with that code", async () => {
  pixelRejection = { code: "capability_denied", message: "required capability was not granted" };
  await expect(screen.pixel(0, 0)).rejects.toBeInstanceOf(ActionError);
  await expect(screen.pixel(0, 0)).rejects.toMatchObject({ code: "capability_denied" });
});
