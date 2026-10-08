/**
 * Pure compiler for the AHK `Send` string language (stage M2-A).
 *
 * Translates `Send`/`SendInput`/`SendEvent`/`SendPlay`/`SendText` argument
 * strings into the ordered key-step list `input.send` injects through
 * SendInput. No `rime:*` imports: the grammar and modifier state machine are
 * deterministic and unit-testable anywhere.
 *
 * Source of truth is the AutoHotkey-alpha SendKeys pipeline
 * (`rime-research/AutoHotkey-alpha/source/keyboard_mouse.cpp`):
 *
 * - main parse loop: `{^+!#{}` special chars, `{...}` items, raw/text modes
 *   (`SendKeys`, line ~460-830);
 * - `{Blind...}` first-item options with `<`/`>` side exclusions (line ~150);
 * - `SetModifierLRState` press/release ordering, Win/Alt disguise taps and
 *   defer rules (line ~3026);
 * - `SendKey` pre/post modifier steps and lazy Win/Alt release (line ~1035);
 * - `SendUnicodeChar` KEYEVENTF_UNICODE packets (line ~92);
 * - key tables `g_key_to_vk`/`g_key_to_sc` (`globaldata.cpp:284/438`).
 *
 * Deliberate deviations (documented in docs/api/input.md):
 *
 * - Control characters map uniformly to Enter/Backspace/Tab in every mode
 *   (AHK sends `\r`/`\t`/`\b` as Unicode packets outside `{Text}`);
 * - Unrecognized `{...}` items throw a TypeError instead of AHK silently
 *   skipping them (`{Click}`, `{ASC n}`, `{U+...}`, `{LButton}`, mid-string
 *   `{Blind}` and unknown names are refused);
 * - An unmatched `{` throws instead of being ignored;
 * - Chars without a US-layout virtual key inject through KEYEVENTF_UNICODE
 *   (AHK `SendKeySpecial`), so text is layout-independent and deterministic;
 * - Numeric `{scXXX}` resolves through a static US scan-code table instead of
 *   a live `MapVirtualKey`;
 * - CapsLock is never pre-toggled (AHK `StoreCapslockMode`);
 * - SendLevel, `{Click}`, `{ASC}`, `{U+}`, mouse keys inside Send, and
 *   cross-call physical/logical split detection are out of scope.
 */
export type { SendKeyStep, SendCompileContext, SendCompileResult, SendMode } from "./types";
export { MOD_LCONTROL, MOD_RCONTROL, MOD_LSHIFT, MOD_RSHIFT, MOD_LALT, MOD_RALT, MOD_LWIN, MOD_RWIN } from "./tables";
// Key-name vocabulary shared by the legacy string compiler and structured
// `keyboard.press()` (US-layout names, case-insensitive).
export { CHAR_KEYS, NAMED_KEYS, VK_CONTROL, VK_LWIN, VK_MENU, VK_SHIFT } from "./tables";
export { compileSend } from "./parse";
