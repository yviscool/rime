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

export { automation } from "rime:automation";
