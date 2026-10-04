import { expect, test } from "bun:test";
import {
  addTime,
  compareVersions,
  diffTime,
  format,
  formatTime,
  regexMatch,
  regexReplace,
  round,
  sortLines,
  splitPath,
} from "../../sdk/src/runtime-language";

function caught(fn: () => unknown): Error {
  try {
    fn();
  } catch (error) {
    if (error instanceof Error) return error;
    throw error;
  }
  throw new Error("expected the call to throw");
}

test("formatTime ISO week matches GetISOWeekNumber (util.cpp:43-74, macro anchor util.cpp:50-54)", () => {
  expect(formatTime("20261004123045", "yweek")).toBe("202640");
  expect(formatTime("20210101120000", "yweek")).toBe("202053");
  expect(formatTime("20040105", "yweek")).toBe("200402");
});

test("formatTime keywords render through renderDateTime, not the raw pattern (string.cpp:148-174)", () => {
  expect(formatTime("20261004123045", "shortdate")).toBe("10/4/2026");
  expect(formatTime("20261004123045", "longdate")).toBe("Sunday, October 4, 2026");
  expect(formatTime("20261004123045", "yearmonth")).toBe("October 2026");
  expect(formatTime("20261004123045", "time")).toBe("12:30 PM");
  expect(formatTime("20261004123045", "YWeek")).toBe("202640");
});

test("formatTime empty format renders the AHK default time-first long date (string.cpp:54)", () => {
  expect(formatTime("20261004123045", "")).toBe("12:30 PM Sunday, October 4, 2026");
  expect(formatTime("20261004123045")).toBe("12:30 PM Sunday, October 4, 2026");
  expect(formatTime("20261004123045", "yyyy-MM-dd HH:mm:ss")).toBe("2026-10-04 12:30:45");
  expect(formatTime("20261004123045", " ddd")).toBe(" Sun");
});

test("formatTime calendar keywords (string.cpp:148-174, yday via util.cpp:24-38)", () => {
  expect(formatTime("20261004123045", "yday")).toBe("277");
  expect(formatTime("20260105", "yday0")).toBe("005");
  expect(formatTime("20261004123045", "wday")).toBe("1");
  expect(formatTime("20261004000000", "h:mm tt")).toBe("12:00 AM");
  expect(formatTime("20261004230506", "h:mm tt")).toBe("11:05 PM");
});

test("formatTime trims leading whitespace and splits date from options (string.cpp:58, 68-72)", () => {
  expect(formatTime("  20261004123045", "yyyy")).toBe("2026");
  expect(formatTime("20261004123045  ", "yyyy")).toBe("2026");
  expect(formatTime("", "yyyy")).toBe(String(new Date().getFullYear()));
  expect(formatTime("   ", "yyyy")).toBe(String(new Date().getFullYear()));
});

test("formatTime rejects options text AHK would parse into wall-clock fallback (string.cpp:59-63, residual)", () => {
  expect(() => formatTime("20261004123045 yyyy", "yyyy")).toThrow(RangeError);
  expect(caught(() => formatTime("20261004123045 D1", "yyyy")).message).toContain(
    "date/time options are not supported",
  );
  expect(caught(() => formatTime("D1", "yyyy")).message).toContain("date/time options are not supported");
  expect(caught(() => formatTime("now")).message).toContain("date/time options are not supported");
});

test("formatTime enforces format length, even digit runs, and civil ranges (string.cpp:37, util.cpp:153-157)", () => {
  expect(formatTime("20261004123045", "x".repeat(2000)).length).toBe(2000);
  expect(() => formatTime("20261004123045", "x".repeat(2001))).toThrow(RangeError);
  expect(() => formatTime("2026104", "yyyy")).toThrow(RangeError);
  expect(() => formatTime("2026100412304500", "yyyy")).toThrow(RangeError);
  expect(() => formatTime("20269999", "yyyy")).toThrow(RangeError);
  expect(caught(() => formatTime("1300000000", "yyyy")).message).toContain("year must be in [1601, 9999]");
  expect(() => formatTime(123 as never)).toThrow(TypeError);
  expect(() => formatTime("20261004123045", 5 as never)).toThrow(TypeError);
});

test("round is half-away-from-zero like AHK Round (math.cpp:44-45)", () => {
  expect(round(3.14159)).toBe(3);
  expect(round(3.14159, 2)).toBe(3.14);
  expect(round(10.99)).toBe(11);
  expect(round(-10.99)).toBe(-11);
  expect(round(0.5)).toBe(1);
  expect(round(-0.5)).toBe(-1);
  expect(round(2.5)).toBe(3);
  expect(round(-2.5)).toBe(-3);
  expect(round(-0.6)).toBe(-1);
  expect(round(1234.5678, -1)).toBe(1230);
});

test("round rejects bad types and non-finite values", () => {
  expect(() => round("x" as never)).toThrow(TypeError);
  expect(() => round(1, "x" as never)).toThrow(TypeError);
  expect(() => round(NaN)).toThrow(RangeError);
  expect(() => round(Infinity, 2)).toThrow(RangeError);
  expect(() => round(1e308, 3)).toThrow(RangeError);
});

test("format %# alt-o prefixes after precision padding (string.cpp:1436-1448, C '#' flag)", () => {
  expect(format("{:#.3o}", 5)).toBe("005");
  expect(format("{:#o}", 5)).toBe("05");
  expect(format("{:#.0o}", 0)).toBe("0");
  expect(format("{:#o}", 0)).toBe("0");
  expect(format("{:#.3o}", 0)).toBe("000");
  expect(format("{:#x}", 5)).toBe("0x5");
  expect(format("{:#.3x}", 5)).toBe("0x005");
  expect(format("{:#X}", 255)).toBe("0XFF");
});

test("format accepts {:} and {:width} with the default string conversion (string.cpp:1468-1476, 1502)", () => {
  expect(format("{:}", "abc")).toBe("abc");
  expect(format("{:5}", "abc")).toBe("  abc");
  expect(format("{:}", 42)).toBe("42");
  expect(format("{:-}", 1)).toBe("1");
  expect(format("{:+}", 1)).toBe("1");
  expect(format("{: #}", 1)).toBe("1");
  expect(format("{:10}", "hi")).toBe("        hi");
  expect(format("{:10.2}", "hi")).toBe("        hi");
});

test("format case conversions cover ULlTt and require the optional s (string.cpp:1470-1476, util.h:97-124)", () => {
  expect(format("{:T}", "hello world")).toBe("Hello World");
  expect(format("{:U}", "hello")).toBe("HELLO");
  expect(format("{:l}", "HELLO")).toBe("hello");
  expect(format("{:t}", "HELLO WORLD")).toBe("Hello World");
  expect(format("{:Ls}", "hello")).toBe("hello");
  expect(format("{:Ts}", "hello world")).toBe("Hello World");
  expect(format("{:Lt}", "hello")).toBe("{:Lt}");
});

test("format placeholders: positional, escapes, literals, and passthrough (string.cpp:1409-1427)", () => {
  expect(format("{}", 42)).toBe("42");
  expect(format("{} {}", 42, 43)).toBe("42 43");
  expect(format("{0}", 42)).toBe("{0}");
  expect(format("{{}")).toBe("{");
  expect(format("{}}")).toBe("}");
  expect(format("}}")).toBe("}}");
  expect(format("{", 42)).toBe("{");
  expect(format("{0", 42)).toBe("{0");
  expect(format("{2}", 42)).toBe("{2}");
  expect(format("literal", 1, 2)).toBe("literal");
});

test("format integer conversions truncate like ATOI64 (script2.cpp:3383-3394, strtoll stops at first non-digit)", () => {
  expect(format("{:d}", 123)).toBe("123");
  expect(format("{:d}", 3.7)).toBe("3");
  expect(format("{:d}", -3.7)).toBe("-3");
  expect(format("{:d}", true)).toBe("1");
  expect(format("{:d}", "1e3")).toBe("1");
  expect(format("{:d}", " 42")).toBe("42");
  expect(format("{:d}", "0x1F")).toBe("31");
  expect(format("{:i}", 5)).toBe("5");
  expect(format("{:u}", 5)).toBe("5");
  expect(format("{:05d}", 42)).toBe("00042");
  expect(format("{:+d}", 42)).toBe("+42");
  expect(format("{: d}", 42)).toBe(" 42");
  expect(format("{:c}", 65)).toBe("A");
});

test("format float conversions follow printf delegation (string.cpp:1457-1460)", () => {
  expect(format("{:.3f}", 1.239)).toBe("1.239");
  expect(format("{:.2e}", 1234.5)).toBe("1.23e+03");
  expect(format("{:f}", 42)).toBe("42.000000");
  expect(format("{:g}", 100)).toBe("100");
  expect(format("{:g}", 0.5)).toBe("0.5");
  expect(format("{:g}", 1e20)).toBe("1e+20");
  expect(format("{:g}", 123456789)).toBe("1.23457e+08");
  expect(format("{:#.3g}", 100)).toBe("100.");
  expect(format("{:#.1g}", 1e20)).toBe("1.e+20");
  expect(format("{:#g}", 0.5)).toBe("0.500000");
});

test("format throws TypeError for ill-typed arguments (string.cpp:1484-1492)", () => {
  expect(() => format(5 as never)).toThrow(TypeError);
  expect(() => format("{:d}", "abc")).toThrow(TypeError);
  expect(() => format("{:d}", undefined)).toThrow(TypeError);
  expect(() => format("{:d}", null)).toThrow(TypeError);
  expect(() => format("{:f}", "abc")).toThrow(TypeError);
  expect(() => format("{:c}", "A")).toThrow(TypeError);
  expect(() => format("{:s}", {})).toThrow(TypeError);
});

test("format guards width and precision and rejects unsupported conversions (MAX_FORMAT_* runtime-language.ts, string.cpp:1450)", () => {
  expect(() => format("{:.101f}", 1)).toThrow(RangeError);
  expect(() => format("{:#.101g}", 1)).toThrow(RangeError);
  expect(() => format("{:1000001d}", 1)).toThrow(RangeError);
  expect(() => format("{:99999999999d}", 1)).toThrow(RangeError);
  const error = caught(() => format("{:a}", 1));
  expect(error.name).toBe("Unsupported");
  expect(() => format("{:A}", 1)).toThrow(/unsupported format spec/);
  expect(() => format("{:p}", 1)).toThrow(/unsupported format spec/);
  expect(() => format("{:z}", 1)).toThrow(/unsupported format spec/);
});

test("regexReplace follows the documented AHK zero-width loop (regex.cpp:1108-1113 \"zzyz\")", () => {
  expect(regexReplace("xy", "x?", "z")).toBe("zzyz");
  expect(regexReplace("ABC", "x?", "z")).toBe("zAzBzCz");
  expect(regexReplace("hello world", "o", "0")).toBe("hell0 w0rld");
  expect(regexReplace("", "x?", "y")).toBe("y");
  expect(regexReplace("xaxbx", "x", "y")).toBe("yayby");
});

test("regexReplace keeps the same-position non-empty divergence documented as a residual (regex.cpp:1124-1127)", () => {
  // AHK produces 5 replacements ("xxxBxCx") via PCRE_NOTEMPTY|PCRE_ANCHORED;
  // our copy-one retry produces 4 ("xAxBxCx") - disclosed residual.
  expect(regexReplace("ABC", "Z*|A", "x")).toBe("xAxBxCx");
  expect(regexReplace("bab", "a", "x")).toBe("bxb");
});

test("regexReplace supports JS-style replacement templates", () => {
  expect(regexReplace("abc123", "(\\d+)", "[$1]")).toBe("abc[123]");
  expect(regexReplace("abc", "(b)", "$&")).toBe("abc");
  expect(regexReplace("abc", "(b)", "$$1")).toBe("a$1c");
  expect(regexReplace("hello", "(?<w>h)", "[$<w>]")).toBe("[h]ello");
  expect(regexReplace("abc", "(b)", "$2")).toBe("a$2c");
  expect(regexReplace("abc", "b", "$0")).toBe("a$0c");
});

test("regexReplace honours limit and start options", () => {
  expect(regexReplace("aaa", "a", "x", { limit: 2 })).toBe("xxa");
  expect(regexReplace("aaa", "a", "x", { limit: 0 })).toBe("aaa");
  expect(regexReplace("aaa", "a", "x", { start: 1 })).toBe("axx");
  expect(regexReplace("xyz", "x?", "z", { start: 2 })).toBe("xyzzz");
  expect(regexReplace("abc", "b", "x", { limit: 1 })).toBe("axc");
});

test("regexReplace iterates every match with the global flag (regression: missing g hung the loop)", () => {
  expect(regexReplace("aXbXc", "X", "-")).toBe("a-b-c");
  expect(regexReplace("a.b", ".", "x")).toBe("xxx");
});

test("regexReplace validates options with typed errors", () => {
  expect(() => regexReplace("abc", "b", "x", { limit: -1 })).toThrow(RangeError);
  expect(() => regexReplace("abc", "b", "x", { limit: 1.5 })).toThrow(RangeError);
  expect(() => regexReplace("abc", "b", "x", { start: 99 })).toThrow(RangeError);
  expect(() => regexReplace("abc", "b", "x", { limit: "2" as never })).toThrow(TypeError);
  expect(() => regexReplace("abc", "b", "x", { start: "2" as never })).toThrow(TypeError);
  expect(() => regexReplace("abc", "b", "x", "no" as never)).toThrow(TypeError);
  expect(() => regexReplace(1 as never, "b", "x")).toThrow(TypeError);
  expect(() => regexReplace("abc", 1 as never, "x")).toThrow(TypeError);
  expect(() => regexReplace("abc", "b", 1 as never)).toThrow(TypeError);
  expect(caught(() => regexReplace("abc", "b", "x", { flags: "Q" })).message).toContain("unknown flag");
});

test("regexMatch returns the AHK-shaped result", () => {
  const match = regexMatch("hello world", "o w");
  expect(match).toEqual({ pos: 4, len: 3, value: "o w", groups: ["o w"], count: 0 });
  expect(match).not.toHaveProperty("mark");
  expect(match).not.toHaveProperty("named");
  const full = regexMatch("abc", "(a)(b)(c)");
  expect(full?.groups).toEqual(["abc", "a", "b", "c"]);
  expect(full?.count).toBe(3);
  expect(regexMatch("abc", "x")).toBeNull();
  expect(regexMatch("abc123", "[a-z]+(\\d+)")).toMatchObject({ pos: 0, len: 6, value: "abc123", count: 1 });
});

test("regexMatch exposes named groups only when declared and unmatched groups as undefined", () => {
  const named = regexMatch("hello", "(?<who>h)(?<mid>e)");
  expect(named?.named).toEqual({ who: "h", mid: "e" });
  expect(named?.groups).toEqual(["he", "h", "e"]);
  const partial = regexMatch("ac", "a(z)?c");
  expect(partial?.groups).toEqual(["ac", undefined]);
  expect(partial?.groups[1]).toBeUndefined();
  expect(partial?.count).toBe(1);
  const mapped = regexMatch("hello", "(?<w>h)");
  expect(mapped?.named).toEqual({ w: "h" });
  const precompiled = regexMatch("aXb", "(?P<n>a)|(?P<n>b)");
  expect(precompiled?.named).toEqual({ n: "a" });
});

test("regexMatch flags mirror AHK flag letters (regex.cpp:510-519)", () => {
  expect(regexMatch("ABC", "b", "i")?.value).toBe("B");
  expect(regexMatch("a\nb", "a.b", "s")?.value).toBe("a\nb");
  expect(regexMatch("x\nabc", "^abc$", "m")?.value).toBe("abc");
  expect(regexMatch("abc", "b", "A")).toBeNull();
  expect(regexMatch("abc", "b", "D")?.value).toBe("b");
  expect(regexMatch("abc", "b", "O")?.value).toBe("b");
  expect(regexMatch("abc", "b", "S")?.value).toBe("b");
  expect(regexMatch("abc", "b # c", "x")?.value).toBe("b");
  expect(regexMatch("abc", "b", "im")?.value).toBe("b");
  expect(regexMatch("ABC", "b", "iD")?.value).toBe("B");
});

test("regexMatch rejects unsupported and unknown flags", () => {
  for (const flag of ["C", "J", "U", "X"]) {
    const error = caught(() => regexMatch("abc", "b", flag));
    expect(error.name).toBe("Unsupported");
    expect(error.message).toContain("flag");
  }
  expect(caught(() => regexMatch("abc", "b", "Z")).name).toBe("RangeError");
  expect(() => regexMatch("abc", "b", 5 as never)).toThrow(TypeError);
  expect(caught(() => regexMatch("abc", "b", "\n\x07")).message).toContain("newline-mode flag");
});

test("regexMatch rejects PCRE-only constructs loudly (regex.cpp:510-519, PCRE 8.30)", () => {
  const cases: Array<[string, string]> = [
    ["(?R)", "recursion"],
    ["(*FAIL)", "backtrack verbs"],
    ["a\\Kb", "escape \\K"],
    ["[[:alpha:]]+", "POSIX character classes"],
    ["\\v", "escape \\v"],
    ["\\u0041", "escape \\u"],
    ["\\A", "escape \\A"],
    ["\\z", "escape \\z"],
    ["\\G", "escape \\G"],
    ["\\h", "escape \\h"],
    ["\\a", "escape \\a"],
    ["(?>a)", "atomic groups"],
    ["(?|a)", "branch reset"],
    ["(?#c)", "comments"],
    ["(?C1)", "callouts"],
    ["(?P>x)", "subroutine calls"],
    ["(?&x)", "subroutine calls"],
    ["a++", "possessive quantifier ++"],
    ["a{2,3}+", "possessive quantifier }+"],
  ];
  for (const [pattern, fragment] of cases) {
    const error = caught(() => regexMatch("aaa", pattern));
    expect(error.name).toBe("Unsupported");
    expect(error.message).toContain(fragment);
  }
});

test("regexMatch converts \\x{...} and guards malformed escapes (PCRE accepts \\x{...}, regex.cpp PCRE 8.30)", () => {
  expect(regexMatch("A", "\\x{41}")?.value).toBe("A");
  expect(regexMatch("AAA", "\\x{41}+")?.value).toBe("AAA");
  expect(caught(() => regexMatch("abc", "\\x{G}")).message).toContain("malformed");
  expect(caught(() => regexMatch("abc", "\\x{41")).message).toContain("malformed");
  expect(caught(() => regexMatch("abc", "\\x{110000}")).name).toBe("SyntaxError");
  expect(regexMatch("abc", "\\x0b")).toBeNull();
  const q = regexMatch("a.b", "\\Qa.b\\E");
  expect(q?.value).toBe("a.b");
  expect(regexMatch("axb", "\\Qa.b\\E")).toBeNull();
});

test("regexMatch wraps JS syntax errors and validates argument types", () => {
  expect(caught(() => regexMatch("abc", "(a")).name).toBe("SyntaxError");
  expect(() => regexMatch(123 as never, "b")).toThrow(TypeError);
  expect(() => regexMatch("abc", 5 as never)).toThrow(TypeError);
  expect(regexMatch("", "a*")).toEqual({ pos: 0, len: 0, value: "", groups: [""], count: 0 });
  expect(regexMatch("abc", "")).toEqual({ pos: 0, len: 0, value: "", groups: [""], count: 0 });
  expect(regexMatch("abc", "a(z)?c")).toBeNull();
});

test("addTime applies whole-unit deltas with truncation (math.cpp:384-416)", () => {
  expect(addTime("20260101000000", 1, "d")).toBe("20260102000000");
  expect(addTime("20260101000000", -1, "d")).toBe("20251231000000");
  expect(addTime("20260228000000", 1, "d")).toBe("20260301000000");
  expect(addTime("20261231235959", 1, "s")).toBe("20270101000000");
  expect(addTime("20260101000000", 1.7, "h")).toBe("20260101014200");
  expect(addTime("20260101000000", -1.7, "h")).toBe("20251231221800");
  expect(addTime("20260101", 1, "d")).toBe("20260102000000");
  expect(addTime("2026", 1, "d")).toBe("20260102000000");
  expect(addTime("2026010100", 1, "d")).toBe("20260102000000");
  expect(addTime(new Date(2026, 0, 1), 1, "d")).toBe("20260102000000");
});

test("addTime raises type errors before body errors in parameter order", () => {
  expect(() => addTime(123 as never, 1, "d")).toThrow(TypeError);
  expect(() => addTime("bad", "x" as never, "d")).toThrow(TypeError);
  expect(() => addTime("20260101000000", "x" as never, "d")).toThrow(TypeError);
  expect(() => addTime("bad", 1, 5 as never)).toThrow(TypeError);
  expect(() => addTime("20260101000000", 1, 5 as never)).toThrow(TypeError);
  expect(() => addTime("bad", 1, "d")).toThrow(RangeError);
  expect(() => addTime("bad", NaN, "d")).toThrow(RangeError);
});

test("addTime reports date, unit, then amount range failures in body order (math.cpp:384 then 391)", () => {
  expect(caught(() => addTime("bad", 1, "days" as never)).message).toContain("timestamp");
  expect(caught(() => addTime("20260101000000", NaN, "days" as never)).message).toContain("unit must be one of");
  expect(caught(() => addTime("20260101000000", NaN, "d")).message).toContain("amount must be finite");
  expect(caught(() => addTime("20260101000000", Infinity, "d")).message).toContain("amount must be finite");
  expect(caught(() => addTime("20260101000000", 1, "days" as never)).message).toContain("unit must be one of");
  expect(caught(() => addTime("20261301000000", 1, "d")).message).toContain("month must be in");
  expect(caught(() => addTime("15991231", 1, "d")).message).toContain("year must be in [1601, 9999]");
  expect(caught(() => addTime("20260101000000", 1e9, "d")).message).toContain("result year");
  expect(() => addTime(new Date(NaN), 1, "d")).toThrow(RangeError);
});

test("addTime rejects whitespace timestamps AHK's field-aligned parser accepts (util.cpp:153-157, residual)", () => {
  expect(() => addTime("20260101  ", 1, "d")).toThrow(RangeError);
  expect(() => addTime(" 2026010100", 1, "d")).toThrow(RangeError);
});

test("diffTime returns to - from; AHK DateDiff returns aTime1 - aTime2 (math.cpp:430, util.cpp:267-268)", () => {
  expect(diffTime("20260101000000", "20260102120000", "d")).toBe(1);
  expect(diffTime("20260102120000", "20260101000000", "h")).toBe(-36);
  expect(diffTime("20260101000000", "20260101000059", "s")).toBe(59);
  expect(diffTime("20260101000000", "20260101000059", "m")).toBe(0);
  expect(diffTime("20260101000000", "20260101000000", "d")).toBe(0);
  expect(diffTime(new Date(2026, 0, 1), new Date(2026, 0, 2), "d")).toBe(1);
});

test("diffTime validates types before parsing and keeps unit shape strict", () => {
  expect(() => diffTime(1 as never, "20260101000000", "d")).toThrow(TypeError);
  expect(() => diffTime("bad", "20260101000000", 5 as never)).toThrow(TypeError);
  expect(() => diffTime("20260101000000", "20260102000000", 5 as never)).toThrow(TypeError);
  expect(() => diffTime("bad", "20260101000000", "d")).toThrow(RangeError);
  expect(() => diffTime("20260101000000", "bad", "d")).toThrow(RangeError);
  expect(caught(() => diffTime("20260101000000", "20260102000000", "days" as never)).message).toContain(
    "unit must be one of",
  );
  expect(() => diffTime("", "20260101000000", "d")).toThrow(RangeError);
});

test("sortLines sorts like AHK Sort with case, numeric, and reverse options (string.cpp:777-870)", () => {
  expect(sortLines("b\na\nc")).toBe("a\nb\nc");
  expect(sortLines("b\na\nc", { descending: true })).toBe("c\nb\na");
  expect(sortLines("10\n9\n2")).toBe("10\n2\n9");
  expect(sortLines("10\n9\n2", { numeric: true })).toBe("2\n9\n10");
  expect(sortLines("a\nA\nb")).toBe("a\nA\nb");
  expect(sortLines("a\nA\nb", { caseSensitive: true })).toBe("A\na\nb");
  expect(sortLines("a\na\nb", { unique: true })).toBe("a\nb");
  expect(sortLines("bb\na", { compare: (a, b) => a.length - b.length })).toBe("a\nbb");
});

test("sortLines honours separator, character-offset column, and trailing-blank Z (string.cpp:836-869, 903-905)", () => {
  expect(sortLines("b|a", { separator: "|" })).toBe("a|b");
  expect(sortLines("b,2\na,1\nc,3", { column: 2 })).toBe("a,1\nb,2\nc,3");
  expect(sortLines("ab\nba", { column: 2 })).toBe("ba\nab");
  expect(sortLines("a\nb\n")).toBe("a\nb\n");
  expect(sortLines("a\nb\n", { keepTrailingBlank: true })).toBe("\na\nb");
  expect(sortLines("a\nb\n", { keepTrailingBlank: false })).toBe("a\nb\n");
  expect(sortLines("")).toBe("");
  expect(sortLines("x")).toBe("x");
});

test("sortLines validates options", () => {
  expect(() => sortLines(5 as never)).toThrow(TypeError);
  expect(() => sortLines("a\nb", null as never)).toThrow(TypeError);
  expect(() => sortLines("x", { separator: "ab" })).toThrow(RangeError);
  expect(() => sortLines("x", { separator: "" })).toThrow(RangeError);
  expect(() => sortLines("x", { caseSensitive: 1 as never })).toThrow(TypeError);
  expect(() => sortLines("x", { numeric: "y" as never })).toThrow(TypeError);
  expect(() => sortLines("x", { compare: 5 as never })).toThrow(TypeError);
});

test("splitPath matches AHK SplitPath including URL and UNC forms (string.cpp:537-651)", () => {
  expect(splitPath("C:\\dir\\file.txt")).toEqual({
    name: "file.txt",
    dir: "C:\\dir",
    ext: "txt",
    stem: "file",
    drive: "C:",
  });
  expect(splitPath("C:\\dir\\")).toEqual({ name: "", dir: "C:\\dir", ext: "", stem: "", drive: "C:" });
  expect(splitPath("file.txt")).toEqual({ name: "file.txt", dir: "", ext: "txt", stem: "file", drive: "" });
  expect(splitPath("\\\\srv\\share\\a.b.c")).toEqual({
    name: "a.b.c",
    dir: "\\\\srv\\share",
    ext: "c",
    stem: "a.b",
    drive: "\\\\srv",
  });
  expect(splitPath("http://host/p/q.html")).toEqual({
    name: "q.html",
    dir: "http://host/p",
    ext: "html",
    stem: "q",
    drive: "http://host",
  });
  expect(splitPath("/unix/path")).toEqual({ name: "/unix/path", dir: "", ext: "", stem: "/unix/path", drive: "" });
  expect(splitPath("a.")).toEqual({ name: "a.", dir: "", ext: "", stem: "a", drive: "" });
  expect(splitPath(".hidden")).toEqual({ name: ".hidden", dir: "", ext: "hidden", stem: "", drive: "" });
  expect(splitPath("")).toEqual({ name: "", dir: "", ext: "", stem: "", drive: "" });
  expect(() => splitPath(5 as never)).toThrow(TypeError);
});

test("compareVersions follows util.cpp CompareVersion semantics (util.cpp:3299-3338)", () => {
  expect(compareVersions("1.0.0", "1.0.1")).toBe(-1);
  expect(compareVersions("1.10", "1.9")).toBe(1);
  expect(compareVersions("1.0", "1.0.0")).toBe(0);
  expect(compareVersions("1.0.0", "1.0.0")).toBe(0);
  expect(compareVersions("01.02", "1.2")).toBe(0);
  expect(compareVersions("2.0", "1.999.999")).toBe(1);
  expect(compareVersions("1.2.3", "1.2.3.4")).toBe(-1);
  expect(compareVersions("1.0.0-beta", "1.0.0")).toBe(-1);
  expect(compareVersions("1.0.0+build", "1.0.0")).toBe(0);
  expect(compareVersions("a", "1")).toBe(1);
  expect(compareVersions("", "1")).toBe(-1);
  expect(compareVersions("1", "1")).toBe(0);
  expect(compareVersions("1.999999999999999999", "2")).toBe(-1);
  expect(compareVersions("v1.2", "1.2")).toBe(0);
  expect(() => compareVersions(5 as never, "1")).toThrow(TypeError);
  expect(() => compareVersions("1", 5 as never)).toThrow(TypeError);
});
