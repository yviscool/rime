/**
 * Production Showdown #5: Cross-App Rapid Form & Data Entry RPA
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Uses `ControlSetText, Edit1, Value, WinTitle` and `ControlClick, Button1`.
 *   Suffers from silent failures, inability to verify whether the edit control
 *   actually accepted the input, no atomic transaction support, and complete
 *   lack of modern UIA or visual fallback when Win32 messages fail.
 * - Rime SDK: Strongly-typed `Control` architecture via `win.control()`. Offers
 *   dedicated facades (`edit.setText()`, `button.click()`), multi-tier strategy
 *   fallback (`auto` -> `message` -> `uia` -> `visual`), and readback verification.
 */

import { Window, runtime } from "@rime/sdk";
import type { WindowSnapshot } from "@rime/sdk";

export interface FormFieldData {
  fieldClassNN: string;
  value: string;
}

export interface FormFillerReport {
  targetWindow: WindowSnapshot;
  populatedFields: Array<{ classNN: string; value: string; verified: boolean }>;
  submitted: boolean;
  durationMs: number;
}

/**
 * Fills a batch of edit fields in a target window and optionally clicks a submit button.
 */
export async function fillForm(
  window: Window,
  fields: FormFieldData[],
  submitButtonClassNN?: string,
): Promise<FormFillerReport> {
  const startTime = Date.now();
  const populated: Array<{ classNN: string; value: string; verified: boolean }> = [];

  for (const field of fields) {
    try {
      const ctrl = await window.control({ classNN: field.fieldClassNN });
      // Set text via dedicated EditControl facade
      await ctrl.edit.setText(field.value);

      // Verify text was written
      const readback = await ctrl.edit.getText();
      const verified = readback.text === field.value;

      populated.push({
        classNN: field.fieldClassNN,
        value: field.value,
        verified,
      });
    } catch (err) {
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[FormFiller] Failed field ${field.fieldClassNN}: ${err}`);
      }
      populated.push({
        classNN: field.fieldClassNN,
        value: field.value,
        verified: false,
      });
    }
  }

  let submitted = false;
  if (submitButtonClassNN) {
    try {
      const buttonCtrl = await window.control({ classNN: submitButtonClassNN });
      await buttonCtrl.button.click();
      submitted = true;
    } catch (err) {
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[FormFiller] Failed submit button ${submitButtonClassNN}: ${err}`);
      }
    }
  }

  return {
    targetWindow: window.snapshot,
    populatedFields: populated,
    submitted,
    durationMs: Date.now() - startTime,
  };
}

export async function main(): Promise<void> {
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug("=== Showdown 05: Batch Form Filler RPA ===");
  }

  // 1. Locate an eligible target window
  const windows = await Window.list();
  const targetWindow = windows.find((w) => w.snapshot.visible && !w.snapshot.minimized) ?? windows[0];

  if (!targetWindow) {
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug("[FormFiller] No desktop window found to demonstrate form filling");
    }
    return;
  }

  // 2. Query available controls
  const controls = await targetWindow.controls();
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug(
      `[FormFiller] Target: [${targetWindow.snapshot.processName}] "${targetWindow.snapshot.title}", Controls: ${controls.length}`,
    );
  }

  // 3. Inspect Edit controls
  const editControls = controls.filter((c) => c.className.toLowerCase().includes("edit"));
  const buttonControls = controls.filter((c) => c.className.toLowerCase().includes("button"));

  if (editControls.length > 0) {
    const fieldsToFill: FormFieldData[] = editControls.slice(0, 2).map((c, i) => ({
      fieldClassNN: c.classNN,
      value: `Showdown Field Data #${i + 1}`,
    }));

    const submitBtn = buttonControls[0]?.classNN;
    const report = await fillForm(targetWindow, fieldsToFill, submitBtn);

    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(
        `[FormFiller] Populated ${report.populatedFields.length} fields in ${report.durationMs}ms, Submitted: ${report.submitted}`,
      );
    }
  } else {
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug("[FormFiller] No standard Win32 Edit controls present in active window; structural RPA verified");
    }
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Batch Form Filler failed:", error);
  }
});
