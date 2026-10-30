/**
 * Production Showdown #1: Auto Window Grid Tiler & Manager
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Relies on `WinGet, id, List`, `SysGet, Mon, MonitorWorkArea`, and `WinMove`.
 *   Suffers from 1-based pseudo-arrays, lack of typing, untracked coordinate math,
 *   and inability to filter complex window styles (Progman, Shell_TrayWnd, hidden toolbars)
 *   without cryptic BitAND operations on GWL_STYLE/EXSTYLE.
 * - Rime SDK: Provides typed `screen.monitor()` with exact virtual-desktop work-area geometry,
 *   structured `Window.list()` returning strongly-typed `WindowSnapshot` records,
 *   pure-function grid arithmetic, and transactional `win.move({ x, y, w, h })` actions.
 */

import { screen, Window, runtime } from "@rime/sdk";
import type { WindowSnapshot } from "@rime/sdk";

export interface GridLayout {
  cols: number;
  rows: number;
  cellWidth: number;
  cellHeight: number;
  startX: number;
  startY: number;
}

export interface WindowTileTarget {
  window: Window;
  rect: { x: number; y: number; w: number; h: number };
}

/**
 * Pure calculation of a non-overlapping uniform grid layout within a work area.
 */
export function calculateGridLayout(
  windowCount: number,
  workArea: { left: number; top: number; right: number; bottom: number },
): GridLayout {
  if (windowCount <= 0) {
    return {
      cols: 0,
      rows: 0,
      cellWidth: 0,
      cellHeight: 0,
      startX: workArea.left,
      startY: workArea.top,
    };
  }

  const workWidth = Math.max(100, workArea.right - workArea.left);
  const workHeight = Math.max(100, workArea.bottom - workArea.top);

  let cols = 1;
  let rows = 1;

  if (windowCount === 2) {
    cols = 2;
    rows = 1;
  } else if (windowCount === 3 || windowCount === 4) {
    cols = 2;
    rows = 2;
  } else {
    cols = Math.ceil(Math.sqrt(windowCount));
    rows = Math.ceil(windowCount / cols);
  }

  const cellWidth = Math.floor(workWidth / cols);
  const cellHeight = Math.floor(workHeight / rows);

  return {
    cols,
    rows,
    cellWidth,
    cellHeight,
    startX: workArea.left,
    startY: workArea.top,
  };
}

/**
 * Filters visible top-level application windows suitable for tiling,
 * ignoring desktop, taskbar, tooltips, and hidden background shells.
 */
export function filterTileableWindows(windows: Window[]): Window[] {
  const ignoredClasses = new Set([
    "Progman",
    "WorkerW",
    "Shell_TrayWnd",
    "Shell_SecondaryTrayWnd",
    "Windows.UI.Core.CoreWindow",
  ]);

  return windows.filter((win) => {
    const snap = win.snapshot;
    if (!snap.visible || snap.minimized) return false;
    if (snap.title.trim().length === 0) return false;
    if (ignoredClasses.has(snap.className)) return false;
    // Exclude zero-sized or utility windows
    const width = snap.rect.right - snap.rect.left;
    const height = snap.rect.bottom - snap.rect.top;
    if (width < 50 || height < 50) return false;
    return true;
  });
}

/**
 * Tiles given windows into a clean grid across the primary monitor work area.
 */
export async function tileWindows(maxWindows = 6): Promise<WindowTileTarget[]> {
  const monitor = await screen.monitor();
  const allWindows = await Window.list();
  const candidates = filterTileableWindows(allWindows).slice(0, maxWindows);

  if (candidates.length === 0) {
    return [];
  }

  const layout = calculateGridLayout(candidates.length, monitor.work);
  const results: WindowTileTarget[] = [];

  for (let index = 0; index < candidates.length; index++) {
    const col = index % layout.cols;
    const row = Math.floor(index / layout.cols);
    const x = layout.startX + col * layout.cellWidth;
    const y = layout.startY + row * layout.cellHeight;
    const w = layout.cellWidth;
    const h = layout.cellHeight;

    const win = candidates[index];
    try {
      await win.move({ x, y, w, h });
      results.push({ window: win, rect: { x, y, w, h } });
    } catch (moveError) {
      // Individual window move failures (hung window, access denied) do not abort the grid
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`Failed to move window [${win.snapshot.title}]: ${moveError}`);
      }
    }
  }

  return results;
}

export async function main(): Promise<void> {
  const mon = await screen.monitor();
  const workStr = `(${mon.work.left}, ${mon.work.top}) -> (${mon.work.right}, ${mon.work.bottom})`;
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug(`[Monitor] Primary: ${mon.name}, Work Area: ${workStr}`);
  }

  const tiled = await tileWindows(4);
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug(`[Tiler] Tiled ${tiled.length} windows into a structured grid`);
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Window Tiler failed:", error);
  }
});
