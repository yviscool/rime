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

/** One injected keyboard transition as accepted by `input.send`. */
export interface SendKeyStep {
  /** Virtual key 1..254, or a UTF-16 code unit when {@link unicode} is set. */
  vk: number;
  down: boolean;
  /** `vk` is a UTF-16 code unit sent with KEYEVENTF_UNICODE (AHK SendUnicodeChar). */
  unicode?: boolean;
}

// Per-side modifier masks (AHK modLR_type, same bit layout as MOD_* in AHK).
export const MOD_LCONTROL = 0x01;
export const MOD_RCONTROL = 0x02;
export const MOD_LSHIFT = 0x04;
export const MOD_RSHIFT = 0x08;
export const MOD_LALT = 0x10;
export const MOD_RALT = 0x20;
export const MOD_LWIN = 0x40;
export const MOD_RWIN = 0x80;

const MOD_CONTROL = MOD_LCONTROL | MOD_RCONTROL;
const MOD_SHIFT = MOD_LSHIFT | MOD_RSHIFT;
const MOD_ALT = MOD_LALT | MOD_RALT;
const MOD_WIN = MOD_LWIN | MOD_RWIN;
const MOD_ALL = 0xff;
const MOD_LMASK = MOD_LCONTROL | MOD_LSHIFT | MOD_LALT | MOD_LWIN;
const MOD_RMASK = MOD_RCONTROL | MOD_RSHIFT | MOD_RALT | MOD_RWIN;

// Virtual keys used by the compiler (winuser.h values).
const VK_BACK = 0x08;
const VK_TAB = 0x09;
const VK_RETURN = 0x0d;
const VK_SHIFT = 0x10;
const VK_CONTROL = 0x11;
const VK_MENU = 0x12;
const VK_PAUSE = 0x13;
const VK_CAPITAL = 0x14;
const VK_ESCAPE = 0x1b;
const VK_SPACE = 0x20;
const VK_PRIOR = 0x21;
const VK_NEXT = 0x22;
const VK_END = 0x23;
const VK_HOME = 0x24;
const VK_LEFT = 0x25;
const VK_UP = 0x26;
const VK_RIGHT = 0x27;
const VK_DOWN = 0x28;
const VK_SNAPSHOT = 0x2c;
const VK_INSERT = 0x2d;
const VK_DELETE = 0x2e;
const VK_LWIN = 0x5b;
const VK_RWIN = 0x5c;
const VK_NUMLOCK = 0x90;
const VK_SCROLL = 0x91;
const VK_LSHIFT = 0xa0;
const VK_RSHIFT = 0xa1;
const VK_LCONTROL = 0xa2;
const VK_RCONTROL = 0xa3;
const VK_LMENU = 0xa4;
const VK_RMENU = 0xa5;

const EVENT_DOWN = 0;
const EVENT_UP = 1;
const EVENT_BOTH = 2;

/** Send mode; mirrors AHK's aSendRaw state (SCM_RAW / SCM_RAW_TEXT). */
export type SendMode = "send" | "raw" | "text";

/** Snapshot state one `compileSend` call starts from. */
export interface SendCompileContext {
  /** Per-side modifier mask read from `input.modifiers()` before the batch. */
  state: number;
  /** Script-held persistent mask carried over from earlier sends (AHK sModifiersLR_persistent). */
  persistent: number;
}

/** Compiled batch plus the state the caller must carry across calls. */
export interface SendCompileResult {
  steps: SendKeyStep[];
  /** Updated persistent mask to store for the next send. */
  persistent: number;
  /** Modifier mask the injection must leave behind (blind rules included). */
  endState: number;
}

// Mouse VKs inside Send strings resolve to real keys in AHK ({Click}-style
// mouse clicks); this stage refuses them explicitly.
const MOUSE_KEYS = new Set(["lbutton", "rbutton", "mbutton", "xbutton1", "xbutton2"]);

/**
 * Named keys: case-insensitive merge of AHK's `g_key_to_vk` (globaldata.cpp:284)
 * and `g_key_to_sc` (globaldata.cpp:438). The SC-only names (Delete, Up,
 * PgUp, ...) map to their virtual keys because this substrate injects VKs;
 * NumpadEnter collapses onto Enter (documented collision).
 */
const NAMED_KEYS: Record<string, number> = {
  numpad0: 0x60, numpad1: 0x61, numpad2: 0x62, numpad3: 0x63, numpad4: 0x64,
  numpad5: 0x65, numpad6: 0x66, numpad7: 0x67, numpad8: 0x68, numpad9: 0x69,
  numpadmult: 0x6a, numpaddiv: 0x6f, numpadadd: 0x6b, numpadsub: 0x6d,
  numpaddot: 0x6e, numpadenter: VK_RETURN, numpaddel: VK_DELETE,
  numpadins: VK_INSERT, numpadclear: 0x0c, numpadup: VK_UP, numpaddown: VK_DOWN,
  numpadleft: VK_LEFT, numpadright: VK_RIGHT, numpadhome: VK_HOME,
  numpadend: VK_END, numpadpgup: VK_PRIOR, numpadpgdn: VK_NEXT,
  numlock: VK_NUMLOCK, scrolllock: VK_SCROLL, capslock: VK_CAPITAL,
  escape: VK_ESCAPE, esc: VK_ESCAPE, tab: VK_TAB, space: VK_SPACE,
  backspace: VK_BACK, bs: VK_BACK, enter: VK_RETURN, return: VK_RETURN,
  delete: VK_DELETE, del: VK_DELETE, insert: VK_INSERT, ins: VK_INSERT,
  up: VK_UP, down: VK_DOWN, left: VK_LEFT, right: VK_RIGHT,
  home: VK_HOME, end: VK_END, pageup: VK_PRIOR, pgup: VK_PRIOR,
  pagedown: VK_NEXT, pgdn: VK_NEXT,
  printscreen: VK_SNAPSHOT, ctrlbreak: 0x03, pause: VK_PAUSE, help: 0x2f,
  sleep: 0x5f, appskey: 0x5d,
  lcontrol: VK_LCONTROL, rcontrol: VK_RCONTROL, lctrl: VK_LCONTROL,
  rctrl: VK_RCONTROL, lshift: VK_LSHIFT, rshift: VK_RSHIFT,
  lalt: VK_LMENU, ralt: VK_RMENU, lwin: VK_LWIN, rwin: VK_RWIN,
  control: VK_CONTROL, ctrl: VK_CONTROL, alt: VK_MENU, shift: VK_SHIFT,
  f1: 0x70, f2: 0x71, f3: 0x72, f4: 0x73, f5: 0x74, f6: 0x75, f7: 0x76,
  f8: 0x77, f9: 0x78, f10: 0x79, f11: 0x7a, f12: 0x7b, f13: 0x7c, f14: 0x7d,
  f15: 0x7e, f16: 0x7f, f17: 0x80, f18: 0x81, f19: 0x82, f20: 0x83,
  f21: 0x84, f22: 0x85, f23: 0x86, f24: 0x87,
  browser_back: 0xa6, browser_forward: 0xa7, browser_refresh: 0xa8,
  browser_stop: 0xa9, browser_search: 0xaa, browser_favorites: 0xab,
  browser_home: 0xac, volume_mute: 0xad, volume_down: 0xae, volume_up: 0xaf,
  media_next: 0xb0, media_prev: 0xb1, media_stop: 0xb2,
  media_play_pause: 0xb3, launch_mail: 0xb4, launch_media: 0xb5,
  launch_app1: 0xb6, launch_app2: 0xb7,
};

/**
 * US-layout printable characters: char -> [vk, needsShift]. Mirrors what
 * `VkKeyScanEx` returns on a US keyboard (letters, digits, punctuation).
 * Characters outside this table inject as Unicode (AHK SendKeySpecial).
 */
const CHAR_KEYS: Record<string, [number, boolean]> = {
  " ": [VK_SPACE, false],
  "`": [0xc0, false], "~": [0xc0, true],
  "1": [0x31, false], "!": [0x31, true],
  "2": [0x32, false], "@": [0x32, true],
  "3": [0x33, false], "#": [0x33, true],
  "4": [0x34, false], "$": [0x34, true],
  "5": [0x35, false], "%": [0x35, true],
  "6": [0x36, false], "^": [0x36, true],
  "7": [0x37, false], "&": [0x37, true],
  "8": [0x38, false], "*": [0x38, true],
  "9": [0x39, false], "(": [0x39, true],
  "0": [0x30, false], ")": [0x30, true],
  "-": [0xbd, false], _: [0xbd, true],
  "=": [0xbb, false], "+": [0xbb, true],
  "[": [0xdb, false], "{": [0xdb, true],
  "]": [0xdd, false], "}": [0xdd, true],
  "\\": [0xdc, false], "|": [0xdc, true],
  ";": [0xba, false], ":": [0xba, true],
  "'": [0xde, false], '"': [0xde, true],
  ",": [0xbc, false], "<": [0xbc, true],
  ".": [0xbe, false], ">": [0xbe, true],
  "/": [0xbf, false], "?": [0xbf, true],
};
for (let code = 0x41; code <= 0x5a; ++code) {
  const upper = String.fromCharCode(code);
  const lower = String.fromCharCode(code + 0x20);
  CHAR_KEYS[lower] = [code, false];
  CHAR_KEYS[upper] = [code, true];
}

/**
 * Static US scan-code -> virtual-key table for numeric `{scXXX}` items
 * (Set 1, non-extended preference, aligned with AHK's SC_* constants; a live
 * `MapVirtualKey` is not available to the pure compiler).
 */
const SCAN_KEYS: Record<number, number> = {
  0x01: VK_ESCAPE, 0x02: 0x31, 0x03: 0x32, 0x04: 0x33, 0x05: 0x34,
  0x06: 0x35, 0x07: 0x36, 0x08: 0x37, 0x09: 0x38, 0x0a: 0x39,
  0x0b: 0x30, 0x0c: 0xbd, 0x0d: 0xbb, 0x0e: VK_BACK, 0x0f: VK_TAB,
  0x10: 0x51, 0x11: 0x57, 0x12: 0x45, 0x13: 0x52, 0x14: 0x54,
  0x15: 0x59, 0x16: 0x55, 0x17: 0x49, 0x18: 0x4f, 0x19: 0x50,
  0x1a: 0xdb, 0x1b: 0xdd, 0x1c: VK_RETURN, 0x1d: VK_LCONTROL,
  0x1e: 0x41, 0x1f: 0x53, 0x20: 0x44, 0x21: 0x46, 0x22: 0x47,
  0x23: 0x48, 0x24: 0x4a, 0x25: 0x4b, 0x26: 0x4c, 0x27: 0xba,
  0x28: 0xde, 0x29: 0xc0, 0x2a: VK_LSHIFT, 0x2b: 0xdc, 0x2c: 0x5a,
  0x2d: 0x58, 0x2e: 0x43, 0x2f: 0x56, 0x30: 0x42, 0x31: 0x4e,
  0x32: 0x4d, 0x33: 0xbc, 0x34: 0xbe, 0x35: 0xbf, 0x36: VK_RSHIFT,
  0x37: 0x6a, 0x38: VK_MENU, 0x39: VK_SPACE, 0x3a: VK_CAPITAL,
  0x3b: 0x70, 0x3c: 0x71, 0x3d: 0x72, 0x3e: 0x73, 0x3f: 0x74,
  0x40: 0x75, 0x41: 0x76, 0x42: 0x77, 0x43: 0x78, 0x44: 0x79,
  0x45: 0x90, 0x46: VK_PAUSE, 0x47: VK_HOME, 0x48: VK_UP, 0x49: VK_PRIOR,
  0x4a: 0x6d, 0x4b: VK_LEFT, 0x4c: 0x0c, 0x4d: VK_RIGHT, 0x4e: 0x6b,
  0x4f: VK_END, 0x50: VK_DOWN, 0x51: VK_NEXT, 0x52: VK_INSERT,
  0x53: VK_DELETE, 0x57: 0x7a, 0x58: 0x7b,
};

function isDigit(char: string): boolean {
  return char >= "0" && char <= "9";
}

function isHexDigit(char: string): boolean {
  return isDigit(char) || (char >= "a" && char <= "f") || (char >= "A" && char <= "F");
}

function parseHex(text: string): number | null {
  if (text.length === 0) return null;
  let value = 0;
  for (const char of text) {
    if (!isHexDigit(char)) return null;
    value = value * 16 + Number.parseInt(char, 16);
  }
  return value;
}

/** AHK ATOI: leading optional sign plus digits, 0 when there are none. */
function atoi(text: string): number {
  let index = 0;
  let negative = false;
  if (text.startsWith("-")) {
    negative = true;
    index = 1;
  } else if (text.startsWith("+")) {
    index = 1;
  }
  let value = 0;
  let digits = false;
  while (index < text.length && isDigit(text[index])) {
    value = value * 10 + (text.charCodeAt(index) - 0x30);
    digits = true;
    ++index;
  }
  if (!digits) return 0;
  return negative ? -value : value;
}

function isModifierKey(vk: number): number {
  switch (vk) {
    case VK_SHIFT:
      return MOD_LSHIFT; // neutral Shift behaves as the left key (AHK KeyToModifiersLR)
    case VK_LSHIFT:
      return MOD_LSHIFT;
    case VK_RSHIFT:
      return MOD_RSHIFT;
    case VK_CONTROL:
      return MOD_LCONTROL;
    case VK_LCONTROL:
      return MOD_LCONTROL;
    case VK_RCONTROL:
      return MOD_RCONTROL;
    case VK_MENU:
      return MOD_LALT;
    case VK_LMENU:
      return MOD_LALT;
    case VK_RMENU:
      return MOD_RALT;
    case VK_LWIN:
      return MOD_LWIN;
    case VK_RWIN:
      return MOD_RWIN;
    default:
      return 0;
  }
}

/**
 * Resolves one `{...}` key name to a virtual key. Returns `undefined` when the
 * name does not resolve (the caller refuses it as an unsupported item) and
 * throws for names that resolve to mouse keys, which Send cannot express in
 * this stage.
 */
function resolveKeyName(name: string): number | undefined {
  const lower = name.toLowerCase();
  if (MOUSE_KEYS.has(lower)) {
    throw new TypeError(
      `Send: mouse keys are not supported inside {..}: ${name} (use mouseClick()/mouseClickDrag())`,
    );
  }
  const named = NAMED_KEYS[lower];
  if (named !== undefined) return named;
  if (lower.length > 2 && lower.startsWith("vk")) {
    const parsed = parseHex(name.slice(2));
    if (parsed !== null && parsed > 0 && parsed <= 0xff) return parsed;
    return undefined;
  }
  if (lower.length > 2 && lower.startsWith("sc")) {
    const parsed = parseHex(name.slice(2));
    if (parsed !== null && parsed > 0) {
      const mapped = SCAN_KEYS[parsed];
      if (mapped !== undefined) return mapped;
      throw new TypeError(`Send: unknown scan code: ${name}`);
    }
    return undefined;
  }
  if (name.length === 1) {
    const entry = CHAR_KEYS[name];
    if (entry !== undefined) return entry[0];
    return undefined;
  }
  return undefined;
}

/** `{name}` items whose single char form needs Shift (AHK CharToVKAndModifiers). */
function resolveChar(char: string): { vk: number; shift: boolean } | undefined {
  const entry = CHAR_KEYS[char];
  if (entry !== undefined) return { vk: entry[0], shift: entry[1] };
  return undefined;
}

/** True for a `vkXX`/`vkXXscXXX` explicit item (both may carry hex digits). */
function parseVkSc(name: string): { vk: number; sc: number | null } | undefined {
  if (name.length <= 2 || name.slice(0, 2).toLowerCase() !== "vk") return undefined;
  let index = 2;
  let vkHex = "";
  while (index < name.length && isHexDigit(name[index])) {
    vkHex += name[index];
    ++index;
  }
  if (vkHex.length === 0) return undefined;
  const vk = parseHex(vkHex);
  if (vk === null || vk <= 0 || vk > 0xff) return undefined;
  if (index === name.length) return { vk, sc: null };
  if (name.slice(index, index + 2).toLowerCase() !== "sc") return undefined;
  index += 2;
  let scHex = "";
  while (index < name.length && isHexDigit(name[index])) {
    scHex += name[index];
    ++index;
  }
  if (scHex.length === 0 || index !== name.length) return undefined;
  const sc = parseHex(scHex);
  if (sc === null) return undefined;
  return { vk, sc };
}

class Compiler {
  private readonly steps: SendKeyStep[] = [];
  private current: number;
  private mode: SendMode;
  private readonly blind: boolean;
  private readonly blindExclusions: number;
  private persistent: number; // persistent_eff: this-batch persistent mask
  private modulePersistent: number; // sModifiersLR_persistent (carried across calls)
  private readonly state: number;
  private modsForNextKey = 0;
  // DisguiseWinAltIfNeeded bookkeeping (AHK sPrevVK/sPrevEventType/
  // sPrevEventModifierDown): prevents a Win/Alt down+up pair from opening the
  // Start menu / menu bar.
  private prevEventType = -1;
  private prevVK = 0;
  private prevEventModifierDown = 0;
  private thisEventModifierDown = 0;
  // Blind-mode selective release end state (AHK mods_released_for_selective_blind).
  private readonly blindReleased: number;

  constructor(context: SendCompileContext, blind: boolean, exclusions: number, mode: SendMode) {
    this.state = context.state & MOD_ALL;
    this.blind = blind;
    this.blindExclusions = exclusions & MOD_ALL;
    this.mode = mode;
    this.modulePersistent = context.persistent & this.state; // AHK: sModifiersLR_persistent &= mods_current
    this.blindReleased = blind ? this.state ^ (this.state & ~this.blindExclusions) : 0;
    this.persistent = blind
      ? this.state & ~this.blindExclusions
      : this.modulePersistent;
    this.current = this.state;
  }

  compile(keys: string): SendCompileResult {
    let index = 0;
    while (index < keys.length) {
      const char = keys[index];
      if (this.mode === "send" && "^+!#{}".includes(char)) {
        if (char === "^" || char === "+" || char === "!" || char === "#") {
          const bit =
            char === "^"
              ? MOD_LCONTROL
              : char === "+"
                ? MOD_LSHIFT
                : char === "!"
                  ? MOD_LALT
                  : MOD_LWIN;
          // Prefixes that the persistent set already covers are dropped so the
          // post-key step never releases them (AHK SendKeys prefix handling).
          if ((this.persistent & bit) === 0) this.modsForNextKey |= bit;
          ++index;
          continue;
        }
        if (char === "}") {
          // AHK ignores a stray `}` without resetting pending prefixes.
          ++index;
          continue;
        }
        const end = keys.indexOf("}", index + 1);
        if (end < 0) {
          throw new TypeError("Send: unmatched '{' (every {item} needs a closing '}')");
        }
        index = this.braceItem(keys, index, end) ? end + 1 : end + 1;
        continue;
      }
      this.plainChar(char, index + 1 < keys.length ? keys[index + 1] : "");
      if (char === "\r" && keys[index + 1] === "\n") ++index;
      ++index;
    }
    this.finish();
    return {
      steps: this.steps,
      persistent: this.modulePersistent,
      endState: this.endState(),
    };
  }

  private endState(): number {
    return this.blind
      ? this.persistent | (this.state & this.blindExclusions)
      : this.persistent | this.state;
  }

  /** True when the item was consumed (always true; unsupported items throw). */
  private braceItem(keys: string, start: number, end: number): boolean {
    // Skip leading whitespace inside the braces (AHK v1.0.43).
    let bodyStart = start + 1;
    while (bodyStart < end && (keys[bodyStart] === " " || keys[bodyStart] === "\t")) {
      ++bodyStart;
    }
    const body = keys.slice(bodyStart, end);
    if (body.length === 0) {
      if (end + 1 < keys.length && keys[end + 1] === "}") {
        // `{}}` is a literal "}" (AHK SendKeys brace_case_end special case).
        this.plainChar("}", "");
        this.modsForNextKey = 0;
        return true;
      }
      throw new TypeError("Send: empty {} item is not supported");
    }
    if (body.toLowerCase() === "raw") {
      this.mode = "raw";
      this.modsForNextKey = 0;
      return true;
    }
    if (body.toLowerCase() === "text") {
      this.mode = "text";
      this.modsForNextKey = 0;
      return true;
    }
    this.namedItem(body);
    this.modsForNextKey = 0;
    return true;
  }

  private namedItem(body: string): void {
    // Split "name suffix" on the first space/tab (AHK StrChrAny + ATOI).
    let split = -1;
    for (let index = 0; index < body.length; ++index) {
      if (body[index] === " " || body[index] === "\t") {
        split = index;
        break;
      }
    }
    const name = split < 0 ? body : body.slice(0, split);
    let eventType = EVENT_BOTH;
    let repeat = 1;
    let downType = 0; // 0 = n/a, 1 = persistent, 2 = temp/remap
    if (split >= 0) {
      let suffix = body.slice(split + 1);
      while (suffix.startsWith(" ") || suffix.startsWith("\t")) suffix = suffix.slice(1);
      if (suffix.toLowerCase().startsWith("down")) {
        eventType = EVENT_DOWN;
        const rest = suffix.slice(4);
        if (rest.toLowerCase().startsWith("temp") || rest.startsWith("R")) {
          downType = 2; // DownTemp / DownR: not carried across sends
        } else {
          downType = 1;
        }
      } else if (suffix.toLowerCase() === "up") {
        eventType = EVENT_UP;
      } else {
        repeat = atoi(suffix);
        if (repeat < 1) return; // AHK: repeat < 1 sends nothing (mods still reset)
      }
    }

    const explicit = parseVkSc(name);
    let vk = 0;
    let sc: number | null = null;
    if (explicit) {
      vk = explicit.vk;
      sc = explicit.sc;
    } else {
      const resolved = resolveKeyName(name);
      if (resolved === undefined) {
        throw new TypeError(
          `Send: unsupported {..} item: {${body}} ` +
            "(supported: named keys, vkXX, scXXX, Raw, Text; " +
            "{Click}, {ASC n}, {U+...}, {LButton} and unknown items are refused)",
        );
      }
      vk = resolved;
    }

    const modifierBit = isModifierKey(vk);
    if (modifierBit !== 0) {
      if (eventType === EVENT_DOWN) {
        this.thisEventModifierDown = vk;
        if (downType === 1) this.modulePersistent |= modifierBit;
        this.persistent |= modifierBit;
      } else if (eventType === EVENT_UP) {
        this.disguiseWinAltIfNeeded(vk);
        this.modulePersistent &= ~modifierBit;
        this.persistent &= ~modifierBit;
      }
      // AHK SendKey: keys that are themselves modifiers skip the modifier
      // pre/post steps entirely.
      this.key(vk, eventType, false);
      return;
    }

    // {Name} / {vkXX}: optional prefix shift from a single-char name.
    const singleChar = name.length === 1 ? resolveChar(name) : undefined;
    if (singleChar !== undefined && singleChar.shift && (this.persistent & MOD_SHIFT) === 0) {
      this.modsForNextKey |= MOD_LSHIFT;
    }
    if (repeat < 1) return;
    this.sendKey(vk, repeat, eventType, 0);
  }

  private plainChar(char: string, next: string): void {
    if (this.mode === "text") {
      if (char === "\r") {
        this.sendKey(VK_RETURN, 1, EVENT_BOTH, 0);
        return;
      }
      if (char === "\n") {
        this.sendKey(VK_RETURN, 1, EVENT_BOTH, 0);
        return;
      }
      if (char === "\b") {
        this.sendKey(VK_BACK, 1, EVENT_BOTH, 0);
        return;
      }
      if (char === "\t") {
        this.sendKey(VK_TAB, 1, EVENT_BOTH, 0);
        return;
      }
      this.unicodeChar(char, this.persistent);
      this.modsForNextKey = 0;
      return;
    }
    // Send/raw modes: uniform control-char mapping (documented deviation).
    if (char === "\r") {
      this.sendKey(VK_RETURN, 1, EVENT_BOTH, 0);
      if (next === "\n") return; // caller skips the LF of a CRLF pair
      return;
    }
    if (char === "\n") {
      this.sendKey(VK_RETURN, 1, EVENT_BOTH, 0);
      return;
    }
    if (char === "\b") {
      this.sendKey(VK_BACK, 1, EVENT_BOTH, 0);
      return;
    }
    if (char === "\t") {
      this.sendKey(VK_TAB, 1, EVENT_BOTH, 0);
      return;
    }
    const resolved = resolveChar(char);
    if (resolved === undefined) {
      // AHK CharToVK failure falls through to SendKeySpecial (Unicode).
      this.unicodeChar(char, this.modsForNextKey | this.persistent);
      this.modsForNextKey = 0;
      return;
    }
    if (resolved.shift && (this.persistent & MOD_SHIFT) === 0) {
      this.modsForNextKey |= MOD_LSHIFT;
    }
    this.sendKey(resolved.vk, 1, EVENT_BOTH, 0);
    this.modsForNextKey = 0;
  }

  /**
   * AHK SendKey: pre-step SetModifierLRState, key events per repeat, then the
   * lazy Win/Alt-only release. `keyAsModifiers` marks modifier keys, which
   * skip both modifier steps.
   */
  private sendKey(vk: number, repeat: number, eventType: number, keyAsModifiers: number): void {
    const specified = this.modsForNextKey | this.persistent;
    for (let count = 0; count < repeat; ++count) {
      if (keyAsModifiers === 0) {
        this.setModifiers(specified, false, true);
      }
      this.key(vk, eventType, true);
      if (keyAsModifiers === 0) {
        // AHK SendKey lazy Win/Alt-only release (keyboard_mouse.cpp:1175-1189):
        // SetModifierLRState(0, win_alt_to_be_released, ..., true, false) —
        // release-only (a subset target computes no presses), disguise-down
        // true, disguise-up false so an already-modified key needs no tap.
        const release = (this.current & ~this.persistent) & (MOD_WIN | MOD_ALT);
        if (release !== 0) this.setModifiers(this.current & ~release, true, false);
      }
    }
  }

  /**
   * AHK SendUnicodeChar: modifiers are set to exactly `mods`, then the char
   * goes out as KEYEVENTF_UNICODE down/up per UTF-16 code unit (surrogate
   * pairs become two units, like AHK).
   */
  private unicodeChar(text: string, mods: number): void {
    this.setModifiers(mods, false, true);
    for (let index = 0; index < text.length; ++index) {
      const unit = text.charCodeAt(index);
      this.steps.push({ vk: unit, down: true, unicode: true });
      this.steps.push({ vk: unit, down: false, unicode: true });
      this.noteEvent(EVENT_BOTH, unit);
    }
  }

  private key(vk: number, eventType: number, track: boolean): void {
    if (eventType === EVENT_DOWN || eventType === EVENT_BOTH) {
      this.steps.push({ vk, down: true });
      if (track) this.noteEvent(EVENT_DOWN, vk);
    }
    if (eventType === EVENT_UP || eventType === EVENT_BOTH) {
      this.steps.push({ vk, down: false });
      if (track) this.noteEvent(EVENT_UP, vk);
    }
  }

  private noteEvent(eventType: number, vk: number): void {
    this.prevEventType = eventType;
    this.prevVK = vk;
  }

  /** AHK DisguiseWinAltIfNeeded: mask a Win/Alt up that follows another Win/Alt down. */
  private disguiseWinAltIfNeeded(vk: number): void {
    const isWin = vk === VK_LWIN || vk === VK_RWIN;
    const isAlt = vk === VK_LMENU || vk === VK_RMENU;
    const prevWin = this.prevVK === VK_LWIN || this.prevVK === VK_RWIN;
    const prevAlt = this.prevVK === VK_LMENU || this.prevVK === VK_RMENU;
    if (
      this.prevEventType === EVENT_DOWN &&
      this.thisEventModifierDown !== vk &&
      !this.blind &&
      ((isWin && prevWin) || (isAlt && prevAlt))
    ) {
      this.steps.push({ vk: VK_CONTROL, down: true });
      this.steps.push({ vk: VK_CONTROL, down: false });
    }
  }

  /** Menu-mask tap (KeyEventMenuMask): neutral Ctrl down+up. */
  private disguiseTapDown(): void {
    this.steps.push({ vk: VK_CONTROL, down: true });
  }

  private disguiseTapUp(): void {
    this.steps.push({ vk: VK_CONTROL, down: false });
  }

  private disguiseTap(): void {
    this.disguiseTapDown();
    this.disguiseTapUp();
  }

  /**
   * AHK SetModifierLRState: release/press the eight per-side modifier keys in
   * the source order (Win, shift-part1, Alt, Control, shift-part2, deferred
   * Win/Alt) with the same disguise and defer rules. AltGr branches are
   * omitted (US layout, documented). `current` becomes `target` afterwards;
   * disguise taps are emitted but do not change the tracked state.
   */
  private setModifiers(target: number, disguiseDown: boolean, disguiseUp: boolean): void {
    const now = this.current;
    target &= MOD_ALL;
    if (now === target) return;
    const union = now | target;
    const ctrlNotDown = (now & MOD_CONTROL) === 0;
    const ctrlWillNotBeDown = (target & MOD_CONTROL) === 0;
    const ctrlNorShiftNorAltDown =
      ctrlNotDown && (now & (MOD_SHIFT | MOD_ALT)) === 0;
    const ctrlOrShiftOrAltWillBeDown =
      !ctrlWillNotBeDown || (target & (MOD_SHIFT | MOD_ALT)) !== 0;
    const deferWinRelease = ctrlNorShiftNorAltDown && ctrlOrShiftOrAltWillBeDown;
    const deferAltRelease = ctrlNotDown && !ctrlWillNotBeDown;
    const releaseShiftBeforeAlt =
      deferAltRelease || ((now & MOD_ALT) === 0 && (target & MOD_ALT) !== 0);
    const disguiseAltDown = disguiseDown && ctrlNotDown && ctrlWillNotBeDown;
    const disguiseWinDown =
      disguiseDown &&
      ctrlNotDown &&
      ctrlWillNotBeDown &&
      (union & MOD_SHIFT) === 0 &&
      (union & MOD_ALT) === 0;

    const relLWin = (now & MOD_LWIN) !== 0 && (target & MOD_LWIN) === 0;
    const relRWin = (now & MOD_RWIN) !== 0 && (target & MOD_RWIN) === 0;
    const pressLWin = (now & MOD_LWIN) === 0 && (target & MOD_LWIN) !== 0;
    const pressRWin = (now & MOD_RWIN) === 0 && (target & MOD_RWIN) !== 0;
    const relLAlt = (now & MOD_LALT) !== 0 && (target & MOD_LALT) === 0;
    const relRAlt = (now & MOD_RALT) !== 0 && (target & MOD_RALT) === 0;
    const pressLAlt = (now & MOD_LALT) === 0 && (target & MOD_LALT) !== 0;
    const pressRAlt = (now & MOD_RALT) === 0 && (target & MOD_RALT) !== 0;
    const relLShift = (now & MOD_LSHIFT) !== 0 && (target & MOD_LSHIFT) === 0;
    const relRShift = (now & MOD_RSHIFT) !== 0 && (target & MOD_RSHIFT) === 0;
    const pressLShift = (now & MOD_LSHIFT) === 0 && (target & MOD_LSHIFT) !== 0;
    const pressRShift = (now & MOD_RSHIFT) === 0 && (target & MOD_RSHIFT) !== 0;
    const relLCtrl = (now & MOD_LCONTROL) !== 0 && (target & MOD_LCONTROL) === 0;
    const relRCtrl = (now & MOD_RCONTROL) !== 0 && (target & MOD_RCONTROL) === 0;
    const pressLCtrl = (now & MOD_LCONTROL) === 0 && (target & MOD_LCONTROL) !== 0;
    const pressRCtrl = (now & MOD_RCONTROL) === 0 && (target & MOD_RCONTROL) !== 0;

    // 1) WIN first: its disguise may rely on Alt staying down.
    if (relLWin) {
      if (!deferWinRelease) {
        if (ctrlNorShiftNorAltDown && disguiseUp) this.disguiseTap();
        this.key(VK_LWIN, EVENT_UP, false);
      }
    } else if (pressLWin) {
      if (disguiseWinDown) this.disguiseTapDown();
      this.key(VK_LWIN, EVENT_DOWN, false);
      if (disguiseWinDown) this.disguiseTapUp();
    }
    if (relRWin) {
      if (!deferWinRelease) {
        if (ctrlNorShiftNorAltDown && disguiseUp) this.disguiseTap();
        this.key(VK_RWIN, EVENT_UP, false);
      }
    } else if (pressRWin) {
      if (disguiseWinDown) this.disguiseTapDown();
      this.key(VK_RWIN, EVENT_DOWN, false);
      if (disguiseWinDown) this.disguiseTapUp();
    }

    // 2) SHIFT part 1 (before Alt/Ctrl to avoid the OS language hotkey).
    if (releaseShiftBeforeAlt) {
      if (relLShift) this.key(VK_LSHIFT, EVENT_UP, false);
      if (relRShift) this.key(VK_RSHIFT, EVENT_UP, false);
    }

    // 3) ALT.
    if (relLAlt) {
      if (!deferAltRelease) {
        if (ctrlNotDown && disguiseUp) this.disguiseTap();
        this.key(VK_LMENU, EVENT_UP, false);
      }
    } else if (pressLAlt) {
      if (disguiseAltDown) this.disguiseTapDown();
      this.key(VK_LMENU, EVENT_DOWN, false);
      if (disguiseAltDown) this.disguiseTapUp();
    }
    if (relRAlt) {
      if (!deferAltRelease) {
        if (ctrlNotDown && disguiseUp) this.disguiseTap();
        this.key(VK_RMENU, EVENT_UP, false);
      }
    } else if (pressRAlt) {
      if (disguiseAltDown) this.disguiseTapDown();
      this.key(VK_RMENU, EVENT_DOWN, false);
      if (disguiseAltDown) this.disguiseTapUp();
    }

    // 4) CONTROL.
    if (relLCtrl) {
      this.key(VK_LCONTROL, EVENT_UP, false);
    } else if (pressLCtrl) {
      this.key(VK_LCONTROL, EVENT_DOWN, false);
    }
    if (relRCtrl) {
      this.key(VK_RCONTROL, EVENT_UP, false);
    } else if (pressRCtrl) {
      this.key(VK_RCONTROL, EVENT_DOWN, false);
    }

    // 5) SHIFT part 2 (after Ctrl/Alt).
    if (!releaseShiftBeforeAlt) {
      if (relLShift) {
        this.key(VK_LSHIFT, EVENT_UP, false);
      } else if (pressLShift) {
        this.key(VK_LSHIFT, EVENT_DOWN, false);
      }
      if (relRShift) {
        this.key(VK_RSHIFT, EVENT_UP, false);
      } else if (pressRShift) {
        this.key(VK_RSHIFT, EVENT_DOWN, false);
      }
    }

    // 6) Releases deferred until the disguise keys were in place.
    if (deferWinRelease) {
      if (relLWin) this.key(VK_LWIN, EVENT_UP, false);
      if (relRWin) this.key(VK_RWIN, EVENT_UP, false);
    }
    if (deferAltRelease) {
      if (relLAlt) this.key(VK_LMENU, EVENT_UP, false);
      if (relRAlt) this.key(VK_RMENU, EVENT_UP, false);
    }

    this.current = target;
  }

  /** Final SetModifierLRState restoring endState with full disguise (AHK tail of SendKeys). */
  private finish(): void {
    this.setModifiers(this.endState(), true, true);
    // Reset per-item bookkeeping the same way SendKeys does between items.
    this.modsForNextKey = 0;
  }
}

/**
 * Parses the leading `{Blind...}` item (must be the first characters of the
 * string, AHK SendKeys line ~150). Returns the exclusion mask and the length
 * of the item including its closing `}`, or `null` when the string does not
 * start with `{Blind`.
 */
function parseBlindPrefix(keys: string): { length: number; exclusions: number } | null {
  if (keys.slice(0, 6).toLowerCase() !== "{blind") return null;
  let mask = MOD_ALL;
  let exclusions = 0;
  for (let index = 6; index < keys.length; ++index) {
    const char = keys[index];
    if (char === "}") return { length: index + 1, exclusions };
    let mod = 0;
    if (char === "<") {
      mask = MOD_LMASK;
      continue;
    }
    if (char === ">") {
      mask = MOD_RMASK;
      continue;
    }
    if (char === "^") mod = MOD_CONTROL;
    else if (char === "+") mod = MOD_SHIFT;
    else if (char === "!") mod = MOD_ALT;
    else if (char === "#") mod = MOD_WIN;
    else if (char === "\0") break;
    else {
      throw new TypeError(`Send: unsupported {Blind} option: ${char}`);
    }
    exclusions |= mod & mask;
    mask = MOD_ALL;
  }
  // AHK ignores an unterminated {Blind... by sending nothing at all.
  throw new TypeError("Send: unterminated {Blind...} item");
}

/**
 * Compiles one AHK Send string into ordered `input.send` steps.
 *
 * `context.state` is the live per-side modifier mask before the batch (from
 * `input.modifiers()`); `context.persistent` is the mask carried across sends.
 * Throws `TypeError` for unsupported items, unmatched braces and unknown keys.
 */
export function compileSend(
  keys: string,
  context: SendCompileContext,
  mode: SendMode = "send",
): SendCompileResult {
  if (typeof keys !== "string") throw new TypeError("send(keys): keys must be a string");
  let rest = keys;
  let blind = false;
  let exclusions = 0;
  if (mode === "send" && rest.slice(0, 6).toLowerCase() === "{blind") {
    const parsed = parseBlindPrefix(rest);
    if (parsed) {
      blind = true;
      exclusions = parsed.exclusions;
      rest = rest.slice(parsed.length);
    }
  }
  if (mode === "send" && rest.slice(0, 6) === "{Text}") {
    mode = "text";
    rest = rest.slice(6);
  }
  const compiler = new Compiler(context, blind, exclusions, mode);
  return compiler.compile(rest);
}
