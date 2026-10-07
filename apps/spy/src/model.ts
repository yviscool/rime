import type { WindowControl, WindowSnapshot } from "@rime/sdk";

/** The cursor sample the spy formats: position plus what is under it. */
export interface SpyProbe {
  x: number;
  y: number;
  window: WindowSnapshot | null;
  control: WindowControl | null;
}

/** Frozen banner line shown while the readout is held. */
export const FROZEN_BANNER = "-- frozen (F2 resume, Esc quit) --";

/**
 * Formats one probe into at most three tooltip lines. Pure: no OS, no
 * clock, no randomness — every expectation in the unit test is a literal.
 */
export function describeProbe(probe: SpyProbe, frozen: boolean): string {
  const lines: string[] = [];
  if (frozen) lines.push(FROZEN_BANNER);
  if (!probe.window) {
    lines.push(`desktop @(${probe.x},${probe.y})`);
    return lines.join("\n");
  }
  const win = probe.window;
  const title = win.title === "" ? "(untitled)" : win.title;
  lines.push(`${title} [${win.className}]`);
  const width = win.rect.right - win.rect.left;
  const height = win.rect.bottom - win.rect.top;
  lines.push(
    `pid ${win.processId} (${win.processName}) ${width}x${height} @(${win.rect.left},${win.rect.top})` +
      ` ${win.minimized ? "[min] " : ""}${win.alwaysOnTop ? "[top]" : ""}`.trimEnd(),
  );
  if (probe.control) {
    lines.push(`control: ${probe.control.classNN} [${probe.control.className}]`);
  }
  return lines.join("\n");
}
