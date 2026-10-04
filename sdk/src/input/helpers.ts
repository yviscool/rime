import { MOD_LCONTROL, MOD_LALT, MOD_LSHIFT, MOD_LWIN, MOD_RCONTROL, MOD_RALT, MOD_RSHIFT, MOD_RWIN } from "../send";
import type { WindowsBridge, WindowsListOptions } from "../window";
import type { ModifiersSnapshot, MouseButton, MouseCoordSpace } from "./types";

export function modifierMaskOf(snapshot: ModifiersSnapshot): number {
  let mask = 0;
  if (snapshot.lcontrol) mask |= MOD_LCONTROL;
  if (snapshot.rcontrol) mask |= MOD_RCONTROL;
  if (snapshot.lshift) mask |= MOD_LSHIFT;
  if (snapshot.rshift) mask |= MOD_RSHIFT;
  if (snapshot.lalt) mask |= MOD_LALT;
  if (snapshot.ralt) mask |= MOD_RALT;
  if (snapshot.lwin) mask |= MOD_LWIN;
  if (snapshot.rwin) mask |= MOD_RWIN;
  return mask;
}

export function requireFiniteNumber(value: unknown, field: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new TypeError(`${field} must be a finite number, got ${String(value)}`);
  }
  return value;
}

/** Coordinates are int32 screen pixels: exact integers, same bound the native payload enforces. */
export function requireCoordinate(value: unknown, field: string): number {
  const number = requireFiniteNumber(value, field);
  if (!Number.isInteger(number) || number < -2147483648 || number > 2147483647) {
    throw new TypeError(`${field} must be an int32 integer coordinate, got ${String(value)}`);
  }
  return number;
}

export function validateSpeed(speed: number | undefined, field: string): number | undefined {
  if (speed === undefined) return undefined;
  const value = requireFiniteNumber(speed, field);
  if (!Number.isInteger(value) || value < 0 || value > 100) {
    throw new TypeError(`${field} must be an integer 0..100, got ${speed}`);
  }
  return value;
}

export function validateButton(button: number | undefined, field: string): MouseButton {
  if (button === undefined) return 1;
  if (button !== 1 && button !== 2 && button !== 3) {
    throw new TypeError(`${field} button must be 1 (left), 2 (right) or 3 (middle), got ${String(button)}`);
  }
  return button;
}

export function validateCount(count: number | undefined, field: string): number {
  if (count === undefined) return 1;
  return Math.trunc(requireFiniteNumber(count, field));
}

/** Validates an optional point pair: both coordinates or neither, each an int32 pixel. */
export function validateOptionalPoint(x: unknown, y: unknown, field: string): void {
  if ((x === undefined) !== (y === undefined)) {
    throw new TypeError(`${field}: x and y must be given together (a partial point is rejected; AHK silently returns)`);
  }
  if (x !== undefined) requireCoordinate(x, `${field}.x`);
  if (y !== undefined) requireCoordinate(y, `${field}.y`);
}

export function validateCoordSpace(value: unknown, field: string): MouseCoordSpace | undefined {
  if (value === undefined) return undefined;
  if (value !== "screen" && value !== "window" && value !== "client") {
    throw new TypeError(`${field}.coords must be "screen"|"window"|"client", got ${String(value)}`);
  }
  return value;
}

/**
 * Screen-space origin of `coords`: null for the default `"screen"` space
 * (no translation needed), otherwise the target window's outer or client
 * origin — both already expressed in screen coordinates on the snapshot.
 */
export async function coordOrigin(
  coords: MouseCoordSpace,
  query: WindowsListOptions | undefined,
  field: string,
): Promise<{ x: number; y: number } | null> {
  if (coords === "screen") return null;
  let module: { windows: WindowsBridge };
  try {
    module = await import("rime:window");
  } catch {
    throw new Error(`${field}: coords "${coords}" needs the rime:window module`);
  }
  const snapshot = query ? (await module.windows.list(query))[0] : await module.windows.active();
  if (!snapshot) {
    throw new Error(`${field}: no ${query ? "matching" : "active"} window for coords "${coords}"`);
  }
  const rect = coords === "client" ? snapshot.clientRect : snapshot.rect;
  return { x: rect.left, y: rect.top };
}
