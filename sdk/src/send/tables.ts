// Per-side modifier masks (AHK modLR_type, same bit layout as MOD_* in AHK).
export const MOD_LCONTROL = 0x01;
export const MOD_RCONTROL = 0x02;
export const MOD_LSHIFT = 0x04;
export const MOD_RSHIFT = 0x08;
export const MOD_LALT = 0x10;
export const MOD_RALT = 0x20;
export const MOD_LWIN = 0x40;
export const MOD_RWIN = 0x80;

export const MOD_CONTROL = MOD_LCONTROL | MOD_RCONTROL;
export const MOD_SHIFT = MOD_LSHIFT | MOD_RSHIFT;
export const MOD_ALT = MOD_LALT | MOD_RALT;
export const MOD_WIN = MOD_LWIN | MOD_RWIN;
export const MOD_ALL = 0xff;
export const MOD_LMASK = MOD_LCONTROL | MOD_LSHIFT | MOD_LALT | MOD_LWIN;
export const MOD_RMASK = MOD_RCONTROL | MOD_RSHIFT | MOD_RALT | MOD_RWIN;

// Virtual keys used by the compiler (winuser.h values).
export const VK_BACK = 0x08;
export const VK_TAB = 0x09;
export const VK_RETURN = 0x0d;
export const VK_SHIFT = 0x10;
export const VK_CONTROL = 0x11;
export const VK_MENU = 0x12;
export const VK_PAUSE = 0x13;
export const VK_CAPITAL = 0x14;
export const VK_ESCAPE = 0x1b;
export const VK_SPACE = 0x20;
export const VK_PRIOR = 0x21;
export const VK_NEXT = 0x22;
export const VK_END = 0x23;
export const VK_HOME = 0x24;
export const VK_LEFT = 0x25;
export const VK_UP = 0x26;
export const VK_RIGHT = 0x27;
export const VK_DOWN = 0x28;
export const VK_SNAPSHOT = 0x2c;
export const VK_INSERT = 0x2d;
export const VK_DELETE = 0x2e;
export const VK_LWIN = 0x5b;
export const VK_RWIN = 0x5c;
export const VK_NUMLOCK = 0x90;
export const VK_SCROLL = 0x91;
export const VK_LSHIFT = 0xa0;
export const VK_RSHIFT = 0xa1;
export const VK_LCONTROL = 0xa2;
export const VK_RCONTROL = 0xa3;
export const VK_LMENU = 0xa4;
export const VK_RMENU = 0xa5;

export const EVENT_DOWN = 0;
export const EVENT_UP = 1;
export const EVENT_BOTH = 2;

// Mouse VKs inside Send strings resolve to real keys in AHK ({Click}-style
// mouse clicks); this stage refuses them explicitly.
export const MOUSE_KEYS = new Set(["lbutton", "rbutton", "mbutton", "xbutton1", "xbutton2"]);

/**
 * Named keys: case-insensitive merge of AHK's `g_key_to_vk` (globaldata.cpp:284)
 * and `g_key_to_sc` (globaldata.cpp:438). The SC-only names (Delete, Up,
 * PgUp, ...) map to their virtual keys because this substrate injects VKs;
 * NumpadEnter collapses onto Enter (documented collision).
 */
export const NAMED_KEYS: Record<string, number> = {
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
export const CHAR_KEYS: Record<string, [number, boolean]> = {
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
export const SCAN_KEYS: Record<number, number> = {
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
