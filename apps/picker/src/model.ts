/** Frozen banner line shown while the readout is held. */
export const FROZEN_BANNER = "-- frozen (F2 resume, Esc quit) --";

/** Split a `0xRRGGBB` pixel into channels. Masks, never shifts sign. */
export function channels(color: number): { r: number; g: number; b: number } {
  return { r: (color >> 16) & 0xff, g: (color >> 8) & 0xff, b: color & 0xff };
}

/** Canonical `#RRGGBB` (uppercase, zero-padded). */
export function hex(color: number): string {
  const { r, g, b } = channels(color >>> 0);
  const pad = (n: number): string => n.toString(16).toUpperCase().padStart(2, "0");
  return `#${pad(r)}${pad(g)}${pad(b)}`;
}

/**
 * Formats one sample into at most three tooltip lines. Pure: no OS, no
 * clock, no randomness — every expectation in the unit test is a literal.
 */
export function describePixel(x: number, y: number, color: number, frozen: boolean): string {
  const { r, g, b } = channels(color >>> 0);
  const lines: string[] = [];
  if (frozen) lines.push(FROZEN_BANNER);
  lines.push(hex(color));
  lines.push(`R${r} G${g} B${b} @(${x},${y})`);
  return lines.join("\n");
}
