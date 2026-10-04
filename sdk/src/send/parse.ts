import {
  CHAR_KEYS,
  EVENT_BOTH,
  EVENT_DOWN,
  EVENT_UP,
  MOD_ALL,
  MOD_ALT,
  MOD_CONTROL,
  MOD_LALT,
  MOD_LCONTROL,
  MOD_LMASK,
  MOD_LSHIFT,
  MOD_LWIN,
  MOD_RALT,
  MOD_RCONTROL,
  MOD_RMASK,
  MOD_RSHIFT,
  MOD_RWIN,
  MOD_SHIFT,
  MOD_WIN,
  MOUSE_KEYS,
  NAMED_KEYS,
  SCAN_KEYS,
  VK_BACK,
  VK_CONTROL,
  VK_LCONTROL,
  VK_LMENU,
  VK_LSHIFT,
  VK_LWIN,
  VK_MENU,
  VK_RCONTROL,
  VK_RETURN,
  VK_RMENU,
  VK_RSHIFT,
  VK_RWIN,
  VK_SHIFT,
  VK_TAB,
} from "./tables";
import type { SendCompileContext, SendCompileResult, SendKeyStep, SendMode } from "./types";

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
