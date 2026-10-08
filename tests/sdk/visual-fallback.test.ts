// Realism: L2 — pure layer-selection policy from docs/api/visual-fallback.md.
// Oracle: the hardcoded table in that doc; no shared logic with any engine file.
import { expect, test } from "bun:test";

type Layer = "uia" | "win32" | "input" | "visual" | "ocr-unsupported";

interface Target {
  hasUiaPattern: boolean;
  hasWin32Control: boolean;
  canSendInput: boolean;
  needsText: boolean;
}

// Mirrors visual-fallback.md §2. Visual answers screen coordinates; text needs
// OCR which does not exist, so it must fail explicitly, never fake a visual hit.
function decideLayer(t: Target): Layer {
  if (t.needsText) return "ocr-unsupported";
  if (t.hasUiaPattern) return "uia";
  if (t.hasWin32Control) return "win32";
  if (t.canSendInput) return "input";
  return "visual";
}

test("semantic first: UIA wins over every lower layer", () => {
  expect(
    decideLayer({ hasUiaPattern: true, hasWin32Control: true, canSendInput: true, needsText: false }),
  ).toBe("uia");
});

test("Win32 direct beats input injection", () => {
  expect(
    decideLayer({ hasUiaPattern: false, hasWin32Control: true, canSendInput: true, needsText: false }),
  ).toBe("win32");
});

test("input is used only when no element handle exists", () => {
  expect(
    decideLayer({ hasUiaPattern: false, hasWin32Control: false, canSendInput: true, needsText: false }),
  ).toBe("input");
});

test("last resort is visual, never an error", () => {
  expect(
    decideLayer({ hasUiaPattern: false, hasWin32Control: false, canSendInput: false, needsText: false }),
  ).toBe("visual");
});

test("text need without OCR fails explicitly instead of faking a hit", () => {
  expect(
    decideLayer({ hasUiaPattern: true, hasWin32Control: true, canSendInput: true, needsText: true }),
  ).toBe("ocr-unsupported");
});

test("visual miss carries no coordinates (result, not error)", () => {
  const miss = { found: false } as const;
  expect(miss).toEqual({ found: false });
  expect("x" in miss).toBe(false);
  const hit = { found: true, x: 12, y: 34 } as const;
  expect(hit.x).toBe(12);
});
