import { automation as nativeAutomation } from "rime:automation";
import type { NativeActionOptions } from "./action";

/** One element from `find`/`read`: a stable id plus a value snapshot. */
export interface AutomationElement {
  /** Positive for the lifetime of the registration; reused by nobody. */
  id: number;
  name: string;
  /** Lowercase UIA control type, e.g. "button" or "edit". */
  controlType: string;
  automationId: string;
  enabled: boolean;
  x: number;
  y: number;
  width: number;
  height: number;
}

/** Every field is optional; at least one of name/controlType/automationId. */
export interface FindQuery {
  name?: string;
  controlType?: string;
  automationId?: string;
  /** Subtree root; omit to search the whole desktop. */
  fromId?: number;
  /** 1..64, default 8. */
  maxResults?: number;
  /**
   * Explicit opt-in for whole-desktop search (P0-3). When `fromId` is absent
   * and this is not `true`, `automation.find` still runs but logs a one-time
   * `console.warn`: desktop-root traversal costs ~143ms (see
   * `docs/performance/RESULTS.md` l4-uia) and is almost never what a scoped
   * automation wants. Pass `fromId` (e.g. from a resolved window element) or
   * set this to `true` to silence the warning. Never forwarded to native.
   */
  allowDesktopRoot?: boolean;
}

/** Bridge of the `rime:automation` module (Windows UI Automation). */
export interface AutomationBridge {
  /**
   * Finds elements and registers them. Throws TypeError for shape mistakes
   * and an Error naming `windows.automation.find` when the capability is
   * missing; rejects with `target_gone` when `fromId` names a dead element.
   */
  find(query: FindQuery, options?: NativeActionOptions): Promise<{ elements: AutomationElement[] }>;
  /** Re-reads a registered element; rejects `target_gone` when it died. */
  read(elementId: number, options?: NativeActionOptions): Promise<AutomationElement>;
  /** Presses the element (UIA invoke pattern), e.g. a button click. */
  invoke(elementId: number, options?: NativeActionOptions): Promise<{ invoked: true }>;
  /** Drops our element reference. False for unknown ids or a stopped service. */
  release(elementId: number): boolean;
}

/**
 * Scoped facade over the native `rime:automation` bridge (P0-3). Read and
 * invoke pass through untouched; `find` strips the SDK-only
 * `allowDesktopRoot` key before crossing the bridge and warns once per
 * process when the caller searches the whole desktop without opting in.
 */
let desktopRootWarned = false;

export const automation: AutomationBridge = {
  find(
    query: FindQuery,
    options?: NativeActionOptions,
  ): Promise<{ elements: AutomationElement[] }> {
    const { allowDesktopRoot, ...bridgeQuery } = query;
    if (bridgeQuery.fromId === undefined && allowDesktopRoot !== true && !desktopRootWarned) {
      desktopRootWarned = true;
      console.warn(
        "[rime] automation.find without fromId searches the whole desktop " +
          "(~143ms, see docs/performance/RESULTS.md l4-uia). Pass fromId to scope " +
          "the search to a window subtree, or set allowDesktopRoot: true to silence this warning.",
      );
    }
    return nativeAutomation.find(bridgeQuery, options);
  },
  read(elementId: number, options?: NativeActionOptions): Promise<AutomationElement> {
    return nativeAutomation.read(elementId, options);
  },
  invoke(elementId: number, options?: NativeActionOptions): Promise<{ invoked: true }> {
    return nativeAutomation.invoke(elementId, options);
  },
  release(elementId: number): boolean {
    return nativeAutomation.release(elementId);
  },
};

