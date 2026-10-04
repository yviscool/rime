import { runAction, type ActionOptions } from "../action";
import type {
  MouseClickOptions,
  MouseCoordOptions,
  MouseDragOptions,
  MouseGetPosResult,
  MouseMoveOptions,
  MousePayload,
  MouseStep,
} from "./types";
import {
  coordOrigin,
  requireCoordinate,
  validateButton,
  validateCoordSpace,
  validateCount,
  validateOptionalPoint,
  validateSpeed,
} from "./helpers";
import { input } from "rime:input";

function runMouse(payload: MousePayload, options?: ActionOptions): Promise<{ sent: number }> {
  return runAction(options, (native) => input.mouse(payload, native));
}

/**
 * Mouse family: `move`/`click`/`drag` compile to ordered `input.mouse` steps
 * (one SendInput batch, self-injected like the keyboard path), `getPos` is
 * the `windows.input.read` cursor/window read.
 *
 * Deviations from AHK, all deliberate and pinned by tests: a partial point
 * (only one of x/y) throws TypeError instead of silently returning; X1/X2
 * and wheel buttons are refused; the title-bar click-down workaround
 * (`keyboard_mouse.cpp:2193-2291`) is not replicated; `speed` is validated
 * but ignored at injection (AHK SendInput does the same). `CoordMode` maps
 * to the per-call `coords`/`window` options (AHK's process-global mode is a
 * §5.3-style implicit global): `"window"`/`"client"` translate through
 * `rime:window` before the existing screen-pixel payload, and `getPos`
 * translates the read back into the requested space.
 */
export const mouse = {
  /**
   * AHK `MouseMove X, Y, Speed`: absolute move in one step. With
   * `coords: "window"|"client"` the point is translated into screen pixels
   * first (see {@link MouseCoordOptions}).
   */
  move(x: number, y: number, options?: MouseMoveOptions): Promise<{ sent: number }> {
    requireCoordinate(x, "mouse.move x");
    requireCoordinate(y, "mouse.move y");
    const speed = validateSpeed(options?.speed, "mouse.move speed");
    const coords = validateCoordSpace(options?.coords, "mouse.move");
    if (coords === undefined || coords === "screen") {
      const payload: MousePayload = { steps: [{ action: "move", x, y }] };
      if (speed !== undefined) payload.speed = speed;
      return runMouse(payload);
    }
    return coordOrigin(coords, options?.window, "mouse.move").then((origin) => {
      if (!origin) throw new Error("mouse.move: unresolved coordinate origin");
      const payload: MousePayload = {
        steps: [
          {
            action: "move",
            x: requireCoordinate(x + origin.x, "mouse.move x"),
            y: requireCoordinate(y + origin.y, "mouse.move y"),
          },
        ],
      };
      if (speed !== undefined) payload.speed = speed;
      return runMouse(payload);
    });
  },
  /**
   * AHK `MouseClick WhichButton, X, Y, Count, Speed`: optional move first,
   * then `count` down/up pairs. `count < 1` does nothing, not even a move.
   * With `coords: "window"|"client"` a given point is translated first;
   * without a point the click lands on the current cursor, so no origin is
   * resolved (no `rime:window` dependency, no capability demand).
   */
  click(options?: MouseClickOptions): Promise<{ sent: number }> {
    const button = validateButton(options?.button, "mouse.click");
    const speed = validateSpeed(options?.speed, "mouse.click speed");
    const count = validateCount(options?.count, "mouse.click count");
    validateOptionalPoint(options?.x, options?.y, "mouse.click");
    const coords = validateCoordSpace(options?.coords, "mouse.click");
    if (count < 1) return Promise.resolve({ sent: 0 });
    const px = options?.x;
    const py = options?.y;
    const buildSteps = (point?: { x: number; y: number }): MouseStep[] => {
      const steps: MouseStep[] = [];
      if (point) steps.push({ action: "move", x: point.x, y: point.y });
      for (let index = 0; index < count; ++index) {
        steps.push({ action: "down", button }, { action: "up", button });
      }
      return steps;
    };
    if (px !== undefined && py !== undefined && coords !== undefined && coords !== "screen") {
      return coordOrigin(coords, options?.window, "mouse.click").then((origin) => {
        if (!origin) throw new Error("mouse.click: unresolved coordinate origin");
        const payload: MousePayload = {
          steps: buildSteps({
            x: requireCoordinate(px + origin.x, "mouse.click x"),
            y: requireCoordinate(py + origin.y, "mouse.click y"),
          }),
        };
        if (speed !== undefined) payload.speed = speed;
        return runMouse(payload);
      });
    }
    const payload: MousePayload = {
      steps: buildSteps(px !== undefined && py !== undefined ? { x: px, y: py } : undefined),
    };
    if (speed !== undefined) payload.speed = speed;
    return runMouse(payload);
  },
  /**
   * AHK `MouseClickDrag WhichButton, X1, Y1, X2, Y2, Speed`: optional move to
   * the start, button down, move to `to`, button up — always as separate
   * events inside one batch (AHK `keyboard_mouse.cpp:2082-2106`).
   */
  drag(options: MouseDragOptions): Promise<{ sent: number }> {
    if (options === null || typeof options !== "object") {
      throw new TypeError("mouse.drag(options): options must be an object with to { x, y }");
    }
    const button = validateButton(options.button, "mouse.drag");
    const speed = validateSpeed(options.speed, "mouse.drag speed");
    validateOptionalPoint(options.x, options.y, "mouse.drag");
    if (options.to === null || typeof options.to !== "object") {
      throw new TypeError("mouse.drag: to { x, y } is required");
    }
    const toX = requireCoordinate(options.to.x, "mouse.drag to.x");
    const toY = requireCoordinate(options.to.y, "mouse.drag to.y");
    const coords = validateCoordSpace(options.coords, "mouse.drag");
    const buildSteps = (from: { x: number; y: number } | undefined, to: { x: number; y: number }): MouseStep[] => {
      const steps: MouseStep[] = [];
      if (from) steps.push({ action: "move", x: from.x, y: from.y });
      steps.push({ action: "down", button });
      steps.push({ action: "move", x: to.x, y: to.y });
      steps.push({ action: "up", button });
      return steps;
    };
    const fromX = options.x;
    const fromY = options.y;
    if (coords !== undefined && coords !== "screen") {
      return coordOrigin(coords, options.window, "mouse.drag").then((origin) => {
        if (!origin) throw new Error("mouse.drag: unresolved coordinate origin");
        const payload: MousePayload = {
          steps: buildSteps(
            fromX !== undefined && fromY !== undefined
              ? {
                  x: requireCoordinate(fromX + origin.x, "mouse.drag x"),
                  y: requireCoordinate(fromY + origin.y, "mouse.drag y"),
                }
              : undefined,
            {
              x: requireCoordinate(toX + origin.x, "mouse.drag to.x"),
              y: requireCoordinate(toY + origin.y, "mouse.drag to.y"),
            },
          ),
        };
        if (speed !== undefined) payload.speed = speed;
        return runMouse(payload);
      });
    }
    const payload: MousePayload = {
      steps: buildSteps(
        fromX !== undefined && fromY !== undefined ? { x: fromX, y: fromY } : undefined,
        { x: toX, y: toY },
      ),
    };
    if (speed !== undefined) payload.speed = speed;
    return runMouse(payload);
  },
  /**
   * AHK `MouseGetPos`: cursor position plus the window and child control
   * under it. Rejects with `capability_denied` naming `windows.input.read`.
   * AHK's OutputVarX/Y/Win/Control and flags 0x01/0x02 (simple mode) are not
   * exposed; the full snapshot and control id are returned instead. With
   * `coords: "window"|"client"` the read position is translated out of
   * screen space (the inverse of the move-side offset).
   */
  getPos(options?: ActionOptions & MouseCoordOptions): Promise<MouseGetPosResult> {
    const coords = validateCoordSpace(options?.coords, "mouse.getPos");
    const read = runAction(options, (native) => input.mouseGetPos(native));
    if (coords === undefined || coords === "screen") return read;
    return Promise.all([read, coordOrigin(coords, options?.window, "mouse.getPos")]).then(
      ([result, origin]) => {
        if (!origin) throw new Error("mouse.getPos: unresolved coordinate origin");
        return {
          ...result,
          x: requireCoordinate(result.x - origin.x, "mouse.getPos x"),
          y: requireCoordinate(result.y - origin.y, "mouse.getPos y"),
        };
      },
    );
  },
};
