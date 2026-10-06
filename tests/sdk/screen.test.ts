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
let caretCalls: Array<NativeActionOptions | undefined> = [];
let sysGetCalls: Array<[number, NativeActionOptions | undefined]> = [];
let ipCalls: Array<NativeActionOptions | undefined> = [];

let pixelResult: { color: number } = { color: 0x00030540 };
let pixelSearchResult: PixelResult = { found: true, x: 2, y: 3 };
let imageSearchResult: PixelResult = { found: false };
let caretResult: PixelResult = { found: true, x: 45, y: 67 };
let sysGetResult: { metric: number } = { metric: 2560 };
let ipResult: { addresses: string[] } = { addresses: ["192.168.1.7", "127.0.0.1"] };
let pixelRejection: CodedRejection | null = null;

function reset(): void {
  countCalls = [];
  monitorCalls = [];
  pixelCalls = [];
  pixelSearchCalls = [];
  imageSearchCalls = [];
  caretCalls = [];
  sysGetCalls = [];
  ipCalls = [];
  pixelResult = { color: 0x00030540 };
  pixelSearchResult = { found: true, x: 2, y: 3 };
  imageSearchResult = { found: false };
  caretResult = { found: true, x: 45, y: 67 };
  sysGetResult = { metric: 2560 };
  ipResult = { addresses: ["192.168.1.7", "127.0.0.1"] };
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
  async caret(options?: NativeActionOptions) {
    caretCalls.push(options);
    return caretResult;
  },
  async sysGet(index: number, options?: NativeActionOptions) {
    sysGetCalls.push([index, options]);
    return sysGetResult;
  },
  async sysGetIPAddresses(options?: NativeActionOptions) {
    ipCalls.push(options);
    return ipResult;
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

test("caret forwards the action options and unwraps the hit", async () => {
  const result = await screen.caret({ deadlineMs: 100 });
  expect(caretCalls).toEqual([{ deadlineMs: 100 }]);
  expect(result).toEqual({ found: true, x: 45, y: 67 });
});

test("caret resolves a miss without coordinates", async () => {
  caretResult = { found: false };
  const result = await screen.caret();
  expect(caretCalls).toEqual([undefined]);
  expect(result).toEqual({ found: false });
  expect("x" in result).toBe(false);
});

test("sysGet forwards the index and unwraps the metric", async () => {
  await expect(screen.sysGet(0)).resolves.toBe(2560);
  expect(sysGetCalls).toEqual([[0, undefined]]);

  // Windows answers 0 for an index it does not know, and the facade must
  // hand that 0 through instead of treating it as a failure.
  sysGetResult = { metric: 0 };
  await expect(screen.sysGet(-1, { deadlineMs: 10 })).resolves.toBe(0);
  expect(sysGetCalls.at(-1)).toEqual([-1, { deadlineMs: 10 }]);
});

test("sysGetIPAddresses forwards the options and unwraps the address list", async () => {
  await expect(screen.sysGetIPAddresses({ deadlineMs: 40 })).resolves.toEqual([
    "192.168.1.7",
    "127.0.0.1",
  ]);
  expect(ipCalls).toEqual([{ deadlineMs: 40 }]);

  ipResult = { addresses: ["127.0.0.1"] };
  await expect(screen.sysGetIPAddresses()).resolves.toEqual(["127.0.0.1"]);
  expect(ipCalls.at(-1)).toBe(undefined);
});
