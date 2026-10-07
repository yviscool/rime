import { expect, test } from "bun:test";
import { channels, describePixel, FROZEN_BANNER, hex } from "./model";

// Pure-logic units (L2): no OS, clock or randomness. Literals only, so a
// formatter that mis-splits channels, drops padding or forgets the banner
// fails here.

test("channels mask without sign extension", () => {
  expect(channels(0xff8040)).toStrictEqual({ r: 255, g: 128, b: 64 });
  expect(channels(0x000000)).toStrictEqual({ r: 0, g: 0, b: 0 });
  expect(channels(0xffffff)).toStrictEqual({ r: 255, g: 255, b: 255 });
});

test("hex is uppercase and zero-padded", () => {
  expect(hex(0xff8040)).toBe("#FF8040");
  expect(hex(0x000a05)).toBe("#000A05");
  expect(hex(0)).toBe("#000000");
});

test("describePixel formats two lines; frozen prepends the banner", () => {
  expect(describePixel(10, 20, 0xff8040, false)).toBe("#FF8040\nR255 G128 B64 @(10,20)");
  expect(describePixel(10, 20, 0xff8040, true).split("\n")[0]).toBe(FROZEN_BANNER);
});
