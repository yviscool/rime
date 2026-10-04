import { beforeEach, expect, mock, test } from "bun:test";
import type { MousePayload } from "../../sdk/src/input";

// mouse's CoordMode mapping translates through rime:window before the
// screen-pixel payload, so the facade is imported against scripted
// rime:input + rime:window mocks (send.test.ts pattern: register mocks
// first, pull the facade in with a dynamic import below).

const activeSnapshot = {
  rect: { left: 50, top: 60, right: 550, bottom: 460 },
  clientRect: { left: 10, top: 20, right: 540, bottom: 450 },
};
const matchSnapshot = {
  rect: { left: 500, top: 600, right: 900, bottom: 900 },
  clientRect: { left: 470, top: 630, right: 890, bottom: 890 },
};

let mouseCalls: MousePayload[] = [];
let activeCalls = 0;
let activeResult: unknown = activeSnapshot;
let listQueries: unknown[] = [];
let listResults: unknown[] = [];
let getPosResult = { x: 100, y: 200, window: null, control: null };

mock.module("rime:input", () => ({
  input: {
    mouse: (payload: MousePayload) => {
      mouseCalls.push(payload);
      return Promise.resolve({ sent: payload.steps.length });
    },
    mouseGetPos: () => Promise.resolve({ ...getPosResult }),
  },
}));

mock.module("rime:window", () => ({
  windows: {
    active: () => {
      activeCalls += 1;
      return Promise.resolve(activeResult);
    },
    list: (query: unknown) => {
      listQueries.push(query);
      return Promise.resolve(listResults);
    },
  },
}));

const { mouse } = await import("../../sdk/src/input");

beforeEach(() => {
  mouseCalls = [];
  activeCalls = 0;
  activeResult = activeSnapshot;
  listQueries = [];
  listResults = [matchSnapshot];
  getPosResult = { x: 100, y: 200, window: null, control: null };
});

test("screen space is the untouched default: no window resolution", async () => {
  const result = await mouse.move(10, 20);
  expect(result).toEqual({ sent: 1 });
  expect(mouseCalls).toEqual([{ steps: [{ action: "move", x: 10, y: 20 }] }]);
  expect(activeCalls).toBe(0);
  expect(listQueries).toHaveLength(0);
});

test("window space offsets by the active window's outer origin", async () => {
  await mouse.move(1, 2, { coords: "window" });
  expect(activeCalls).toBe(1);
  expect(mouseCalls[0].steps).toEqual([{ action: "move", x: 51, y: 62 }]);
});

test("client space offsets by the client-area origin", async () => {
  await mouse.move(1, 2, { coords: "client" });
  expect(mouseCalls[0].steps).toEqual([{ action: "move", x: 11, y: 22 }]);
});

test("a window query goes through list, not active", async () => {
  await mouse.move(0, 0, { coords: "window", window: { ahkExe: "notepad.exe" } });
  expect(activeCalls).toBe(0);
  expect(listQueries).toEqual([{ ahkExe: "notepad.exe" }]);
  expect(mouseCalls[0].steps).toEqual([{ action: "move", x: 500, y: 600 }]);
});

test("window is ignored for screen space", async () => {
  await mouse.move(7, 8, { coords: "screen", window: { ahkExe: "notepad.exe" } });
  expect(mouseCalls[0].steps).toEqual([{ action: "move", x: 7, y: 8 }]);
  expect(activeCalls).toBe(0);
  expect(listQueries).toHaveLength(0);
});

test("an empty match rejects before any injection", async () => {
  activeResult = null;
  await expect(mouse.move(1, 2, { coords: "client" })).rejects.toThrow(/no active window/);
  listResults = [];
  await expect(
    mouse.move(1, 2, { coords: "client", window: { ahkExe: "gone.exe" } }),
  ).rejects.toThrow(/no matching window/);
  expect(mouseCalls).toHaveLength(0);
});

test("an unresolvable translation rejects with the translated int32 bound", async () => {
  activeSnapshot.rect.left = 2147483647;
  await expect(mouse.move(1, 0, { coords: "window" })).rejects.toThrow(/int32/);
  expect(mouseCalls).toHaveLength(0);
  activeSnapshot.rect.left = 50;
});

test("click translates its point, leaving the no-point case untouched", async () => {
  await mouse.click({ x: 3, y: 4, coords: "client", count: 1 });
  expect(mouseCalls[0].steps).toEqual([
    { action: "move", x: 13, y: 24 },
    { action: "down", button: 1 },
    { action: "up", button: 1 },
  ]);
  mouseCalls = [];
  activeCalls = 0;
  await mouse.click({ coords: "client", count: 1 });
  expect(activeCalls).toBe(0);
  expect(mouseCalls[0].steps).toEqual([
    { action: "down", button: 1 },
    { action: "up", button: 1 },
  ]);
});

test("count below 1 never resolves the origin", async () => {
  const result = await mouse.click({ x: 1, y: 2, coords: "client", count: 0 });
  expect(result).toEqual({ sent: 0 });
  expect(activeCalls).toBe(0);
  expect(mouseCalls).toHaveLength(0);
});

test("drag translates both the start and the destination", async () => {
  await mouse.drag({ x: 1, y: 2, to: { x: 3, y: 4 }, coords: "window" });
  expect(mouseCalls[0].steps).toEqual([
    { action: "move", x: 51, y: 62 },
    { action: "down", button: 1 },
    { action: "move", x: 53, y: 64 },
    { action: "up", button: 1 },
  ]);
  await mouse.drag({ to: { x: 3, y: 4 }, coords: "window" });
  expect(mouseCalls[1].steps).toEqual([
    { action: "down", button: 1 },
    { action: "move", x: 53, y: 64 },
    { action: "up", button: 1 },
  ]);
});

test("getPos translates the read out of screen space", async () => {
  const plain = await mouse.getPos();
  expect(plain.x).toBe(100);
  expect(plain.y).toBe(200);
  expect(activeCalls).toBe(0);
  const client = await mouse.getPos({ coords: "client" });
  expect(client.x).toBe(90);
  expect(client.y).toBe(180);
  const window_ = await mouse.getPos({ coords: "window", window: { ahkExe: "x.exe" } });
  expect(window_.x).toBe(-400);
  expect(window_.y).toBe(-400);
});

test("malformed coords spaces throw synchronously", () => {
  expect(() => (mouse.move as (x: number, y: number, o?: unknown) => unknown)(1, 2, { coords: "pixel" })).toThrow(
    TypeError,
  );
  expect(() => (mouse.getPos as (o?: unknown) => unknown)({ coords: 5 })).toThrow(TypeError);
  expect(() => (mouse.drag as (o: unknown) => unknown)({ to: { x: 1, y: 2 }, coords: "screen-ish" })).toThrow(
    TypeError,
  );
  expect(mouseCalls).toHaveLength(0);
});
