/** Options for {@link sortLines}; each field mirrors an AHK `Sort` option letter. */
export interface SortLinesOptions {
  /** AHK `C`: compare case-sensitively. Default false (case-insensitive). */
  caseSensitive?: boolean;
  /** AHK `N`: numeric comparison via ATOF. Default false. */
  numeric?: boolean;
  /** AHK `R`: reverse the order. Ties keep the documented AHK stability rules. */
  descending?: boolean;
  /** AHK `U`: drop duplicates adjacent after sorting. Default false. */
  unique?: boolean;
  /** AHK `D`: single-character separator. Default "\n". */
  separator?: string;
  /** AHK `P`: 1-based column from which the sort key is taken. Default 1. */
  column?: number;
  /** AHK `Z`: keep a trailing blank item when the text ends with the separator. */
  keepTrailingBlank?: boolean;
  /** AHK function option: takes precedence over caseSensitive/numeric/descending. */
  compare?: (a: string, b: string) => number;
}

/** Result of {@link regexMatch}. `pos` is 0-based (AHK returns 1-based positions). */
export interface RegexMatchResult {
  /** Start of the whole match in UTF-16 code units, 0-based. */
  pos: number;
  /** Length of the whole match in UTF-16 code units. */
  len: number;
  /** The whole match; identical to `groups[0]`. */
  value: string;
  /** `[0]` is the whole match, `[1..]` are captures; unmatched groups are undefined. */
  groups: Array<string | undefined>;
  /** Named captures by pattern declaration; present only if the pattern declares names. */
  named?: Record<string, string | undefined>;
  /** Number of capturing groups in the pattern (`groups.length - 1`). */
  count: number;
  /** Reserved; never set (mark verbs are rejected). */
  mark?: string;
}

/** Options for {@link regexReplace}. */
export interface RegexReplaceOptions {
  /** Maximum number of replacements; 0 replaces nothing. Default: replace all. */
  limit?: number;
  /** 0-based offset in `text` at which matching starts; defaults to 0. */
  start?: number;
  /** Regex flags; see {@link regexMatch}. */
  flags?: string;
}

/** Result of {@link splitPath}. */
export interface SplitPathResult {
  /** File name including extension (or URL path tail). */
  name: string;
  /** Directory portion including its trailing separator, or "" when absent. */
  dir: string;
  /** Extension without the dot, or "" when absent. */
  ext: string;
  /** Name without the extension. */
  stem: string;
  /** Drive, UNC root, or URL authority (e.g. "C:", "\\\\srv", "http://host"), or "". */
  drive: string;
}

/** Time unit accepted by {@link addTime} and {@link diffTime}. */
export type TimeUnit = "s" | "m" | "h" | "d";

function unsupported(message: string): Error {
  const error = new Error(message);
  error.name = "Unsupported";
  return error;
}

function describeValue(value: unknown): string {
  if (typeof value === "string") return JSON.stringify(value);
  if (value === null) return "null";
  if (value === undefined) return "undefined";
  if (typeof value === "number" || typeof value === "boolean") return String(value);
  if (typeof value === "bigint") return `${value}n`;
  if (typeof value === "symbol") return value.toString();
  if (typeof value === "function") return "function";
  if (Array.isArray(value)) return "array";
  return "object";
}

function requireString(value: unknown, label: string): string {
  if (typeof value !== "string") {
    throw new TypeError(`${label} must be a string, got ${describeValue(value)}`);
  }
  return value;
}

function isDigit(ch: string): boolean {
  return ch >= "0" && ch <= "9";
}

function pad(value: number, width: number): string {
  const sign = value < 0 ? "-" : "";
  return sign + String(Math.abs(value)).padStart(width, "0");
}

/**
 * Rounds half away from zero at the given decimal precision (AHK `Round`).
 * Positive `digits` returns a number, negative `digits` rounds to a power of ten.
 */
export function round(value: number, digits?: number): number {
  if (typeof value !== "number") {
    throw new TypeError(`round: value must be a number, got ${describeValue(value)}`);
  }
  if (!Number.isFinite(value)) {
    throw new RangeError(`round: value must be finite, got ${value}`);
  }
  let precision = 0;
  if (digits !== undefined) {
    if (typeof digits !== "number") {
      throw new TypeError(`round: digits must be a number, got ${describeValue(digits)}`);
    }
    if (!Number.isFinite(digits)) {
      throw new RangeError(`round: digits must be finite, got ${digits}`);
    }
    precision = Math.trunc(digits);
  }
  const multiplier = Math.pow(10, precision);
  if (!Number.isFinite(multiplier) || multiplier === 0) {
    throw new RangeError(`round: digits out of supported range, got ${precision}`);
  }
  const scaled = value >= 0 ? Math.floor(value * multiplier + 0.5) : Math.ceil(value * multiplier - 0.5);
  const rounded = scaled / multiplier;
  if (!Number.isFinite(rounded)) {
    throw new RangeError(`round: result is not finite for value ${value} with digits ${precision}`);
  }
  if (precision > 0) return rounded + 0;
  return Math.trunc(rounded + (rounded > 0 ? 0.2 : -0.2)) + 0;
}

interface FormatSpec {
  minus: boolean;
  plus: boolean;
  space: boolean;
  zero: boolean;
  alt: boolean;
  width: number | undefined;
  precision: number | undefined;
}

const MAX_FORMAT_WIDTH = 1000000;
const MAX_FORMAT_PRECISION = 100;

function parseFormatSpec(text: string): FormatSpec {
  const spec: FormatSpec = {
    minus: false,
    plus: false,
    space: false,
    zero: false,
    alt: false,
    width: undefined,
    precision: undefined,
  };
  let i = 0;
  while (i < text.length && "-+0 #".includes(text[i])) {
    const flag = text[i];
    if (flag === "-") spec.minus = true;
    else if (flag === "+") spec.plus = true;
    else if (flag === " ") spec.space = true;
    else if (flag === "0") spec.zero = true;
    else spec.alt = true;
    i++;
  }
  const widthStart = i;
  while (i < text.length && isDigit(text[i])) i++;
  if (i > widthStart) {
    const widthDigits = text.slice(widthStart, i);
    const width = Number(widthDigits);
    if (!Number.isSafeInteger(width) || width > MAX_FORMAT_WIDTH) {
      throw new RangeError(`format: width out of supported range, got ${widthDigits}`);
    }
    spec.width = width;
  }
  if (i < text.length && text[i] === ".") {
    i++;
    const precStart = i;
    while (i < text.length && isDigit(text[i])) i++;
    const precision = i > precStart ? Number(text.slice(precStart, i)) : 0;
    if (!Number.isSafeInteger(precision) || precision > MAX_FORMAT_PRECISION) {
      throw new RangeError(`format: precision out of supported range, got ${precision}`);
    }
    spec.precision = precision;
  }
  return spec;
}

function applyWidth(prefix: string, digits: string, spec: FormatSpec, padZero: boolean): string {
  const body = prefix + digits;
  const width = spec.width;
  if (width === undefined || body.length >= width) return body;
  const count = width - body.length;
  if (spec.minus) return body + " ".repeat(count);
  if (padZero) return prefix + "0".repeat(count) + digits;
  return " ".repeat(count) + body;
}

const NUMERIC_STRING = /^[ \t]*[+-]?(?:0[xX][0-9a-fA-F]+|(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)[ \t]*$/;

function numericStringToBigInt(text: string): bigint {
  const trimmed = text.trim();
  let body = trimmed;
  let negative = false;
  if (body[0] === "-" || body[0] === "+") {
    negative = body[0] === "-";
    body = body.slice(1);
  }
  const sign = negative ? -1n : 1n;
  if (body[0] === "0" && (body[1] === "x" || body[1] === "X")) {
    return sign * BigInt(`0x${body.slice(2)}`);
  }
  // Match ATOI64 (strtoll): parse leading digits only; "1e3" -> 1, not 1000 (script2.cpp:3383).
  const leading = /^\d+/.exec(body);
  return sign * BigInt(leading ? leading[0] : "0");
}

function numericStringToDouble(text: string): number {
  const trimmed = text.trim();
  const body = trimmed[0] === "-" || trimmed[0] === "+" ? trimmed.slice(1) : trimmed;
  if (body[0] === "0" && (body[1] === "x" || body[1] === "X")) {
    const magnitude = Number(BigInt(`0x${body.slice(2)}`));
    return trimmed[0] === "-" ? -magnitude : magnitude;
  }
  return parseFloat(trimmed);
}

function toBigIntArg(value: unknown, conv: string, index: number): bigint {
  if (typeof value === "bigint") return value;
  if (typeof value === "boolean") return value ? 1n : 0n;
  if (typeof value === "number") {
    if (!Number.isFinite(value)) {
      throw new RangeError(`format: %${conv} requires a finite number, got ${value}`);
    }
    return BigInt(Math.trunc(value));
  }
  if (typeof value === "string" && NUMERIC_STRING.test(value)) {
    return numericStringToBigInt(value);
  }
  throw new TypeError(`format: argument ${index} must be a Number for %${conv}, got ${describeValue(value)}`);
}

function toDoubleArg(value: unknown, conv: string, index: number): number {
  let result: number;
  if (typeof value === "number") result = value;
  else if (typeof value === "bigint") result = Number(value);
  else if (typeof value === "boolean") result = value ? 1 : 0;
  else if (typeof value === "string" && NUMERIC_STRING.test(value)) {
    result = numericStringToDouble(value);
    if (!Number.isFinite(result)) {
      throw new RangeError(`format: %${conv} requires a finite number, got ${value}`);
    }
    return result;
  } else {
    throw new TypeError(`format: argument ${index} must be a Number for %${conv}, got ${describeValue(value)}`);
  }
  if (!Number.isFinite(result)) {
    throw new RangeError(`format: %${conv} requires a finite number, got ${result}`);
  }
  return result;
}

function formatInteger(value: unknown, spec: FormatSpec, conv: string, index: number): string {
  const n = toBigIntArg(value, conv, index);
  const signed = conv === "d" || conv === "i";
  let prefix = "";
  let magnitude: bigint;
  if (signed) {
    if (n < 0n) prefix = "-";
    else if (spec.plus) prefix = "+";
    else if (spec.space) prefix = " ";
    magnitude = n < 0n ? -n : n;
  } else {
    magnitude = n < 0n ? n + (1n << 64n) : n;
  }
  const base = conv === "o" ? 8 : conv === "x" || conv === "X" ? 16 : 10;
  let digits = magnitude.toString(base);
  if (conv === "X") digits = digits.toUpperCase();
  if (spec.alt && (conv === "x" || conv === "X") && magnitude !== 0n) {
    prefix += conv === "X" ? "0X" : "0x";
  }
  if (spec.precision !== undefined) {
    if (spec.precision === 0 && magnitude === 0n) digits = "";
    else if (digits.length < spec.precision) digits = "0".repeat(spec.precision - digits.length) + digits;
    // %#o forces a leading zero only after precision padding (%#.3o of 5 -> "005").
    if (spec.alt && conv === "o" && !digits.startsWith("0")) prefix = "0" + prefix;
    return applyWidth(prefix, digits, spec, false);
  }
  if (spec.alt && conv === "o" && !digits.startsWith("0")) prefix += "0";
  return applyWidth(prefix, digits, spec, spec.zero && !spec.minus);
}

function normalizeExponent(text: string): string {
  return text.replace(/e([+-])(\d)$/, (_m, sign: string, digit: string) => `e${sign}0${digit}`);
}

function stripMantissaZeros(text: string): string {
  const at = text.indexOf("e");
  const mantissa = text.slice(0, at);
  if (!mantissa.includes(".")) return text;
  return mantissa.replace(/0+$/, "").replace(/\.$/, "") + text.slice(at);
}

function stripTrailingZeros(text: string): string {
  if (!text.includes(".")) return text;
  return text.replace(/0+$/, "").replace(/\.$/, "");
}

/** %# conversions force a decimal point into the mantissa (string.cpp: `printf` '#' flag). */
function ensureDot(text: string): string {
  const at = text.indexOf("e");
  const mantissa = at < 0 ? text : text.slice(0, at);
  if (mantissa.includes(".")) return text;
  const dotted = `${mantissa}.`;
  return at < 0 ? dotted : dotted + text.slice(at);
}

function formatG(magnitude: number, precision: number, alt: boolean): string {
  const exponential = magnitude.toExponential(Math.min(precision - 1, 100));
  const exponent = Number(exponential.slice(exponential.indexOf("e") + 1));
  if (exponent < -4 || exponent >= precision) {
    const normalized = normalizeExponent(exponential);
    return alt ? ensureDot(normalized) : stripMantissaZeros(normalized);
  }
  const fraction = Math.min(Math.max(precision - 1 - exponent, 0), 100);
  const fixed = magnitude.toFixed(fraction);
  return alt ? ensureDot(fixed) : stripTrailingZeros(fixed);
}

function formatFloat(value: unknown, spec: FormatSpec, conv: string, index: number): string {
  const n = toDoubleArg(value, conv, index);
  const sign = n < 0 || Object.is(n, -0) ? "-" : spec.plus ? "+" : spec.space ? " " : "";
  const magnitude = Math.abs(n);
  let body: string;
  if (conv === "e" || conv === "E") {
    const precision = spec.precision ?? 6;
    body = normalizeExponent(magnitude.toExponential(precision));
    if (spec.alt && precision === 0) {
      const at = body.indexOf("e");
      if (!body.slice(0, at).includes(".")) body = `${body.slice(0, at)}.${body.slice(at)}`;
    }
    if (conv === "E") body = body.toUpperCase();
  } else if (conv === "f") {
    const precision = spec.precision ?? 6;
    body = magnitude.toFixed(precision);
    if (spec.alt && precision === 0 && !body.includes(".")) body += ".";
  } else {
    const precision = spec.precision === 0 ? 1 : spec.precision ?? 6;
    body = formatG(magnitude, precision, spec.alt);
    if (conv === "G") body = body.toUpperCase();
  }
  return applyWidth(sign, body, spec, spec.zero && !spec.minus);
}

function formatChar(value: unknown, spec: FormatSpec, conv: string, index: number): string {
  const n = toBigIntArg(value, conv, index);
  if (n < 0n || n > 0x10ffffn) {
    throw new RangeError(`format: %${conv} requires a character code in [0, 0x10FFFF], got ${n}`);
  }
  return applyWidth("", String.fromCodePoint(Number(n)), spec, false);
}

function toTitleCase(text: string): string {
  let out = "";
  let upperNext = true;
  for (const ch of text) {
    if (/\p{L}/u.test(ch)) {
      out += upperNext ? ch.toUpperCase() : ch.toLowerCase();
      upperNext = false;
    } else {
      out += ch;
      if (/[ \t\n\r\f\v]/.test(ch)) upperNext = true;
    }
  }
  return out;
}

function stringifyArg(value: unknown, fn: string, what: string): string {
  if (typeof value === "string") return value;
  if (typeof value === "number" || typeof value === "bigint") return String(value);
  if (typeof value === "boolean") return value ? "1" : "0";
  throw new TypeError(`${fn}: ${what} must be a String, got ${describeValue(value)}`);
}

function formatString(value: unknown, spec: FormatSpec, caseOption: string | undefined, index: number): string {
  let body = stringifyArg(value, "format", `argument ${index}`);
  if (spec.precision !== undefined && body.length > spec.precision) body = body.slice(0, spec.precision);
  if (caseOption === "U") body = body.toUpperCase();
  else if (caseOption === "L") body = body.toLowerCase();
  else if (caseOption === "T") body = toTitleCase(body);
  return applyWidth("", body, spec, false);
}

/**
 * Formats with the AHK `Format` placeholder dialect: literal text, `{{}`/`}}}`
 * escapes, `{}`/`{n}` arguments, and `{:flags width .precision conv}` specs.
 * Malformed placeholders pass through literally; unsupported conversions throw
 * an error with `name === "Unsupported"`.
 */
export function format(template: string, ...args: unknown[]): string {
  requireString(template, "format: template");
  let out = "";
  let cursor = 0;
  let literalStart = 0;
  let lastParam = 0;
  while (cursor < template.length) {
    if (template[cursor] !== "{") {
      cursor++;
      continue;
    }
    const brace = cursor;
    out += template.slice(literalStart, brace);
    let p = brace + 1;
    if ((template[p] === "{" || template[p] === "}") && template[p + 1] === "}") {
      out += template[p];
      cursor = p + 2;
      literalStart = cursor;
      continue;
    }
    const digitStart = p;
    while (p < template.length && isDigit(template[p])) p++;
    let param: number;
    if (p > digitStart) param = Number(template.slice(digitStart, p));
    else param = lastParam + 1;
    if (param < 1 || param > args.length) {
      literalStart = brace;
      cursor = p;
      continue;
    }
    if (template[p] === ":") {
      const specStart = p + 1;
      let q = specStart;
      while (q < template.length && "-+0 #".includes(template[q])) q++;
      while (q < template.length && isDigit(template[q])) q++;
      if (q < template.length && template[q] === ".") {
        q++;
        while (q < template.length && isDigit(template[q])) q++;
      }
      const specStr = template.slice(specStart, q);
      const conv = q < template.length ? template[q] : "";
      let body: string | undefined;
      if (conv === "") {
        literalStart = brace;
        cursor = q;
        continue;
      }
      if (conv === "}") {
        // {:} / {:5}: no conversion letter; default to %s with "}" as the terminator
        // (string.cpp:1468-1476 defaults spec to 's'; string.cpp:1502 consumes the "}").
        body = formatString(args[param - 1], parseFormatSpec(specStr), undefined, param);
        q += 1;
      } else if ("diouxXeEfgGcC".includes(conv)) {
        if (template[q + 1] === "}") {
          const spec = parseFormatSpec(specStr);
          const value = args[param - 1];
          body = "diouxX".includes(conv)
            ? formatInteger(value, spec, conv, param)
            : "eEfgG".includes(conv)
              ? formatFloat(value, spec, conv, param)
              : formatChar(value, spec, conv, param);
          q += 2;
        }
      } else if (conv === "s") {
        if (template[q + 1] === "}") {
          body = formatString(args[param - 1], parseFormatSpec(specStr), undefined, param);
          q += 2;
        }
      } else if ("ULTlt".includes(conv)) {
        const caseOption = conv.toUpperCase();
        let r = q + 1;
        if (template[r] === "s") r++;
        if (template[r] === "}") {
          body = formatString(args[param - 1], parseFormatSpec(specStr), caseOption, param);
          q = r + 1;
        }
      } else {
        throw unsupported(`format: unsupported format spec %{${specStr}${conv}`);
      }
      if (body === undefined) {
        literalStart = brace;
        cursor = q;
        continue;
      }
      out += body;
      lastParam = param;
      cursor = q;
      literalStart = cursor;
      continue;
    }
    if (template[p] === "}") {
      out += stringifyArg(args[param - 1], "format", `argument ${param}`);
      lastParam = param;
      cursor = p + 1;
      literalStart = cursor;
      continue;
    }
    literalStart = brace;
    cursor = p;
  }
  out += template.slice(literalStart);
  return out;
}

interface Civil {
  year: number;
  month: number;
  day: number;
  hour: number;
  minute: number;
  second: number;
  epochDay: number;
}

function isLeapYear(year: number): boolean {
  return (year % 4 === 0 && year % 100 !== 0) || year % 400 === 0;
}

function daysInMonth(year: number, month: number): number {
  if (month === 2) return isLeapYear(year) ? 29 : 28;
  return [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month - 1];
}

function daysFromCivil(year: number, month: number, day: number): number {
  const y = year - (month <= 2 ? 1 : 0);
  const era = Math.floor(y / 400);
  const yoe = y - era * 400;
  const doy = Math.floor((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5) + day - 1;
  const doe = yoe * 365 + Math.floor(yoe / 4) - Math.floor(yoe / 100) + doy;
  return era * 146097 + doe - 719468;
}

function civilFromDays(epochDay: number): { year: number; month: number; day: number } {
  const z = epochDay + 719468;
  const era = Math.floor(z / 146097);
  const doe = z - era * 146097;
  const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365);
  const y = yoe + era * 400;
  const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100));
  const mp = Math.floor((5 * doy + 2) / 153);
  const d = doy - Math.floor((153 * mp + 2) / 5) + 1;
  const m = mp + (mp < 10 ? 3 : -9);
  return { year: y + (m <= 2 ? 1 : 0), month: m, day: d };
}

function dayOfWeek(civil: Civil): number {
  return (((civil.epochDay % 7) + 4) % 7 + 7) % 7;
}

function validateCivil(
  year: number,
  month: number,
  day: number,
  hour: number,
  minute: number,
  second: number,
  fn: string,
): void {
  if (year < 1601 || year > 9999) {
    throw new RangeError(`${fn}: year must be in [1601, 9999], got ${year}`);
  }
  if (month < 1 || month > 12) {
    throw new RangeError(`${fn}: month must be in [1, 12], got ${month}`);
  }
  const dim = daysInMonth(year, month);
  if (day < 1 || day > dim) {
    throw new RangeError(`${fn}: day must be in [1, ${dim}] for ${year}-${month}, got ${day}`);
  }
  if (hour < 0 || hour > 23) {
    throw new RangeError(`${fn}: hour must be in [0, 23], got ${hour}`);
  }
  if (minute < 0 || minute > 59) {
    throw new RangeError(`${fn}: minute must be in [0, 59], got ${minute}`);
  }
  if (second < 0 || second > 59) {
    throw new RangeError(`${fn}: second must be in [0, 59], got ${second}`);
  }
}

function civilFromDigits(text: string, fn: string): Civil {
  const year = Number(text.slice(0, 4));
  const month = text.length > 4 ? Number(text.slice(4, 6)) : 1;
  const day = text.length > 6 ? Number(text.slice(6, 8)) : 1;
  const hour = text.length > 8 ? Number(text.slice(8, 10)) : 0;
  const minute = text.length > 10 ? Number(text.slice(10, 12)) : 0;
  const second = text.length > 12 ? Number(text.slice(12, 14)) : 0;
  validateCivil(year, month, day, hour, minute, second, fn);
  return { year, month, day, hour, minute, second, epochDay: daysFromCivil(year, month, day) };
}

// Strict YYYYMMDDHH24MISS parse for DateAdd/DateDiff: no trimming and no options
// split, because YYYYMMDDToSystemTime validates the whole string (util.cpp:153-157:
// _tcslen length even 4..14, then IsNumeric which rejects any whitespace for
// DateAdd/DateDiff inputs).
function parseTimestampDigits(text: string, fn: string): Civil {
  if (!/^\d+$/.test(text)) {
    throw new RangeError(`${fn}: value must be a YYYYMMDDHH24MISS timestamp, got ${describeValue(text)}`);
  }
  if (text.length < 4 || text.length > 14 || text.length % 2 !== 0) {
    throw new RangeError(
      `${fn}: value must be an even number of digits between 4 and 14, got ${describeValue(text)}`,
    );
  }
  return civilFromDigits(text, fn);
}

function requireTimestampArg(value: string | Date, fn: string): void {
  if (typeof value !== "string" && !(value instanceof Date)) {
    throw new TypeError(`${fn}: value must be a string or Date, got ${describeValue(value)}`);
  }
}

function parseTimestamp(value: string | Date, fn: string): Civil {
  if (value instanceof Date) {
    if (Number.isNaN(value.getTime())) {
      throw new RangeError(`${fn}: value is an invalid Date`);
    }
    const year = value.getFullYear();
    const month = value.getMonth() + 1;
    const day = value.getDate();
    const hour = value.getHours();
    const minute = value.getMinutes();
    const second = value.getSeconds();
    validateCivil(year, month, day, hour, minute, second, fn);
    return { year, month, day, hour, minute, second, epochDay: daysFromCivil(year, month, day) };
  }
  if (typeof value !== "string") {
    throw new TypeError(`${fn}: value must be a string or Date, got ${describeValue(value)}`);
  }
  return parseTimestampDigits(value, fn);
}

function nowCivil(): Civil {
  const now = new Date();
  const year = now.getFullYear();
  const month = now.getMonth() + 1;
  const day = now.getDate();
  const hour = now.getHours();
  const minute = now.getMinutes();
  const second = now.getSeconds();
  return { year, month, day, hour, minute, second, epochDay: daysFromCivil(year, month, day) };
}

// FormatTime value parsing (string.cpp:54-80): empty -> wall clock; leading
// space/tab trimmed (line 58); a non-digit first character means options without
// a date (lines 59-63, AHK falls back to wall clock - we reject, documented);
// otherwise the date runs up to the first space/tab and trailing text is options
// (lines 68-72; AHK parses them, we reject with a specific RangeError).
function parseFormatTimeValue(value: string | Date, fn: string): Civil {
  if (value instanceof Date) return parseTimestamp(value, fn);
  if (typeof value !== "string") {
    throw new TypeError(`${fn}: value must be a string or Date, got ${describeValue(value)}`);
  }
  const text = value.replace(/^[ \t]+/, "");
  if (text.length === 0) return nowCivil();
  if (!isDigit(text[0])) {
    throw new RangeError(`${fn}: date/time options are not supported, got ${describeValue(value)}`);
  }
  const sep = text.search(/[ \t]/);
  if (sep >= 0) {
    const tail = text.slice(sep + 1).replace(/^[ \t]+/, "");
    if (tail.length > 0) {
      throw new RangeError(`${fn}: date/time options are not supported, got ${describeValue(value)}`);
    }
    return parseTimestampDigits(text.slice(0, sep), fn);
  }
  return parseTimestampDigits(text, fn);
}

const MONTHS_FULL = [
  "January",
  "February",
  "March",
  "April",
  "May",
  "June",
  "July",
  "August",
  "September",
  "October",
  "November",
  "December",
];
const MONTHS_ABBR = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
const DAYS_FULL = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
const DAYS_ABBR = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const DEFAULT_FORMAT = "h:mm tt dddd, MMMM d, yyyy";

function getYDay(civil: Civil): number {
  const offsets = isLeapYear(civil.year)
    ? [0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335]
    : [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334];
  return offsets[civil.month - 1] + civil.day;
}

function isoWeekString(civil: Civil): string {
  let year = civil.year;
  const yday = getYDay(civil) - 1;
  const wday = dayOfWeek(civil);
  const isoDays = (day: number, weekday: number): number => day - ((day - weekday + 382) % 7) + 3;
  let days = isoDays(yday, wday);
  if (days < 0) {
    year -= 1;
    days = isoDays(yday + 365 + (isLeapYear(year) ? 1 : 0), wday);
  } else {
    const nextYearDays = isoDays(yday - (365 + (isLeapYear(year) ? 1 : 0)), wday);
    if (nextYearDays >= 0) {
      year += 1;
      days = nextYearDays;
    }
  }
  const week = Math.floor(days / 7) + 1;
  return `${pad(year, 4)}${pad(week, 2)}`;
}

function formatToken(ch: string, run: number, civil: Civil): string {
  const dow = dayOfWeek(civil);
  switch (ch) {
    case "y":
      if (run === 2) return pad(civil.year % 100, 2);
      if (run >= 4) return pad(civil.year, 4);
      return String(civil.year);
    case "M":
      if (run === 1) return String(civil.month);
      if (run === 2) return pad(civil.month, 2);
      if (run === 3) return MONTHS_ABBR[civil.month - 1];
      return MONTHS_FULL[civil.month - 1];
    case "d":
      if (run === 1) return String(civil.day);
      if (run === 2) return pad(civil.day, 2);
      if (run === 3) return DAYS_ABBR[dow];
      return DAYS_FULL[dow];
    case "h": {
      const hour12 = civil.hour % 12 === 0 ? 12 : civil.hour % 12;
      return run === 1 ? String(hour12) : pad(hour12, 2);
    }
    case "H":
      return run === 1 ? String(civil.hour) : pad(civil.hour, 2);
    case "m":
      return run === 1 ? String(civil.minute) : pad(civil.minute, 2);
    case "s":
      return run === 1 ? String(civil.second) : pad(civil.second, 2);
    case "t": {
      const pm = civil.hour >= 12;
      return run === 1 ? (pm ? "P" : "A") : pm ? "PM" : "AM";
    }
    case "g":
      return "A.D.";
    default:
      return "";
  }
}

function renderDateTime(civil: Civil, fmt: string): string {
  let out = "";
  let i = 0;
  while (i < fmt.length) {
    const ch = fmt[i];
    if (ch === "'") {
      if (fmt[i + 1] === "'") {
        out += "'";
        i += 2;
        continue;
      }
      let j = i + 1;
      while (j < fmt.length && fmt[j] !== "'") j++;
      out += fmt.slice(i + 1, j);
      i = j < fmt.length ? j + 1 : j;
      continue;
    }
    if ("ydMhHmstg".includes(ch)) {
      let j = i;
      while (j < fmt.length && fmt[j] === ch) j++;
      out += formatToken(ch, j - i, civil);
      i = j;
      continue;
    }
    out += ch;
    i++;
  }
  return out;
}

/**
 * Formats a YYYYMMDDHH24MISS timestamp or Date with Win32-style date/time
 * pictures in fixed English (locale-independent), or an AHK keyword
 * (YWeek, YDay, YDay0, WDay, ShortDate, LongDate, YearMonth, Time).
 */
export function formatTime(value: string | Date, format?: string): string {
  if (format !== undefined && typeof format !== "string") {
    throw new TypeError(`formatTime: format must be a string, got ${describeValue(format)}`);
  }
  if (format !== undefined && format.length > 2000) {
    throw new RangeError(`formatTime: format must be at most 2000 characters, got ${format.length}`);
  }
  const civil = parseFormatTimeValue(value, "formatTime");
  const fmt = format ?? "";
  if (fmt.length === 0) return renderDateTime(civil, DEFAULT_FORMAT);
  const keyword = fmt.replace(/^[ \t]+/, "").toLowerCase();
  switch (keyword) {
    case "yweek":
      return isoWeekString(civil);
    case "yday":
      return String(getYDay(civil));
    case "yday0":
      return pad(getYDay(civil), 3);
    case "wday":
      return String(dayOfWeek(civil) + 1);
    case "shortdate":
      return renderDateTime(civil, "M/d/yyyy");
    case "longdate":
      return renderDateTime(civil, "dddd, MMMM d, yyyy");
    case "yearmonth":
      return renderDateTime(civil, "MMMM yyyy");
    case "time":
      return renderDateTime(civil, "h:mm tt");
    default:
      return renderDateTime(civil, fmt);
  }
}

function optionalBoolean(value: unknown, name: string, fallback: boolean): boolean {
  if (value === undefined) return fallback;
  if (typeof value !== "boolean") {
    throw new TypeError(`sortLines: options.${name} must be a boolean, got ${describeValue(value)}`);
  }
  return value;
}

function atof(text: string): number {
  const trimmed = text.replace(/^[ \t]+/, "");
  const hex = /^([+-]?)0[xX]([0-9a-fA-F]+)/.exec(trimmed);
  if (hex) {
    const magnitude = Number(BigInt(`0x${hex[2]}`));
    return hex[1] === "-" ? -magnitude : magnitude;
  }
  const numberMatch = /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/.exec(trimmed);
  if (numberMatch) return Number(numberMatch[0]);
  const lower = trimmed.toLowerCase();
  if (lower.startsWith("nan")) return Number.NaN;
  if (lower.startsWith("inf")) return trimmed[0] === "-" ? -Infinity : Infinity;
  return 0;
}

/**
 * Sorts lines the way AHK `Sort` does: string, numeric, or custom comparator;
 * optional case sensitivity, descending order, unique filtering, column keys,
 * and trailing-blank handling.
 */
export function sortLines(text: string, options: SortLinesOptions = {}): string {
  if (typeof text !== "string") {
    throw new TypeError(`sortLines: text must be a string, got ${describeValue(text)}`);
  }
  if (typeof options !== "object" || options === null || Array.isArray(options)) {
    throw new TypeError(`sortLines: options must be an object, got ${describeValue(options)}`);
  }
  const caseSensitive = optionalBoolean(options.caseSensitive, "caseSensitive", false);
  const numeric = optionalBoolean(options.numeric, "numeric", false);
  const descending = optionalBoolean(options.descending, "descending", false);
  const unique = optionalBoolean(options.unique, "unique", false);
  const keepTrailingBlank = optionalBoolean(options.keepTrailingBlank, "keepTrailingBlank", false);
  let separator = "\n";
  if (options.separator !== undefined) {
    if (typeof options.separator !== "string") {
      throw new TypeError(`sortLines: options.separator must be a string, got ${describeValue(options.separator)}`);
    }
    if (options.separator.length !== 1) {
      throw new RangeError(
        `sortLines: options.separator must be a single character, got ${describeValue(options.separator)}`,
      );
    }
    separator = options.separator;
  }
  let columnOffset = 0;
  if (options.column !== undefined) {
    if (typeof options.column !== "number") {
      throw new TypeError(`sortLines: options.column must be a number, got ${describeValue(options.column)}`);
    }
    if (!Number.isFinite(options.column)) {
      throw new RangeError(`sortLines: options.column must be finite, got ${options.column}`);
    }
    columnOffset = Math.max(0, Math.trunc(options.column) - 1);
  }
  const compare = options.compare;
  if (compare !== undefined && typeof compare !== "function") {
    throw new TypeError(`sortLines: options.compare must be a function, got ${describeValue(compare)}`);
  }
  if (text.length === 0) return "";
  const endsWithSeparator = text.endsWith(separator);
  let source = text;
  let crlfTemp = false;
  let terminate = false;
  if (endsWithSeparator && !keepTrailingBlank) {
    terminate = true;
  } else if (separator === "\n" && !endsWithSeparator) {
    const firstBreak = text.indexOf("\n");
    if (firstBreak > 0 && text[firstBreak - 1] === "\r") {
      crlfTemp = true;
      terminate = true;
      source = `${text}\r\n`;
    }
  }
  const items = source.split(separator);
  if (terminate) items.pop();
  if (items.length <= 1) return text;
  const entries = items.map((value, index) => ({ value, index }));
  if (compare) {
    entries.sort((a, b) => {
      const raw = compare(a.value, b.value);
      if (typeof raw !== "number" || Number.isNaN(raw)) {
        throw new TypeError(`sortLines: options.compare must return a number, got ${describeValue(raw)}`);
      }
      if (raw !== 0) return raw > 0 ? 1 : -1;
      return a.index < b.index ? -1 : 1;
    });
  } else {
    entries.sort((a, b) => {
      const left = columnOffset > 0 ? a.value.slice(columnOffset) : a.value;
      const right = columnOffset > 0 ? b.value.slice(columnOffset) : b.value;
      if (numeric) {
        const diff = atof(left) - atof(right);
        if (!diff) return a.index - b.index;
        const result = diff > 0 ? 1 : -1;
        return descending ? -result : result;
      }
      let result: number;
      if (caseSensitive) {
        result = left < right ? -1 : left > right ? 1 : 0;
      } else {
        const upperLeft = left.toUpperCase();
        const upperRight = right.toUpperCase();
        result = upperLeft < upperRight ? -1 : upperLeft > upperRight ? 1 : 0;
      }
      if (result === 0) result = a.index < b.index ? -1 : 1;
      return descending ? -result : result;
    });
  }
  const kept: string[] = [];
  let previous: string | null = null;
  for (const entry of entries) {
    let keep = true;
    if (unique && previous !== null) {
      if (numeric && columnOffset === 0) keep = atof(entry.value) !== atof(previous);
      else if (caseSensitive) keep = entry.value !== previous;
      else keep = entry.value.toUpperCase() !== previous.toUpperCase();
    }
    if (keep) {
      kept.push(entry.value);
      previous = entry.value;
    }
  }
  let out = kept.join(separator);
  if (terminate) out += separator;
  if (crlfTemp) out = out.slice(0, -2);
  return out;
}

/**
 * Splits a path, UNC path, or URL into name, directory, extension, stem, and
 * drive/server, following AHK `SplitPath` rules (including dotfile stems).
 */
export function splitPath(path: string): SplitPathResult {
  requireString(path, "splitPath: path");
  let name = "";
  let nameDelimiter: number | null = null;
  let driveEnd: number | null = null;
  const scheme = path.indexOf("://");
  if (scheme >= 0) {
    let scan = scheme + 3;
    while (scan < path.length && path[scan] !== "/" && path[scan] !== "\\") scan++;
    driveEnd = scan;
    nameDelimiter = driveEnd;
    if (driveEnd < path.length && driveEnd + 1 < path.length) {
      let slash = path.lastIndexOf("/");
      if (slash === scheme + 2) slash = path.lastIndexOf("\\");
      if (slash >= 0) {
        nameDelimiter = slash;
        name = path.slice(slash + 1);
      }
    }
  } else {
    let start = 0;
    while (start < path.length && (path[start] === " " || path[start] === "\t")) start++;
    if (path[start] === "\\" && path[start + 1] === "\\") {
      const next = path.indexOf("\\", start + 2);
      driveEnd = next < 0 ? path.length : next;
    } else if (path[start + 1] === ":") {
      driveEnd = start + 2;
    }
    let delimiter = path.lastIndexOf("\\");
    if (delimiter < 0) delimiter = path.lastIndexOf(":");
    nameDelimiter = delimiter < 0 ? null : delimiter;
    name = nameDelimiter === null ? path : path.slice(nameDelimiter + 1);
  }
  let dir = "";
  if (nameDelimiter !== null) {
    const ch = nameDelimiter < path.length ? path[nameDelimiter] : "";
    dir = ch === "\\" || ch === "/" ? path.slice(0, nameDelimiter) : path.slice(0, nameDelimiter + 1);
  }
  const drive = driveEnd === null ? "" : path.slice(0, driveEnd);
  const dot = name.lastIndexOf(".");
  const ext = dot < 0 ? "" : name.slice(dot + 1);
  const stem = dot < 0 ? name : name.slice(0, dot);
  return { name, dir, ext, stem, drive };
}

function parseVersionDecimal(text: string, start: number): { value: number; end: number } {
  let index = start;
  while (index < text.length && /[ \t\n\r]/.test(text[index])) index++;
  let negative = false;
  if (text[index] === "-" || text[index] === "+") {
    negative = text[index] === "-";
    index++;
  }
  const digitStart = index;
  while (index < text.length && isDigit(text[index])) index++;
  if (index === digitStart) return { value: 0, end: start };
  let value = Number(text.slice(digitStart, index));
  if (negative) value = -value;
  if (value > 2147483647) value = 2147483647;
  if (value < -2147483648) value = -2147483648;
  return { value, end: index };
}

function compareVersionParts(a: string, b: string): number {
  const INT_MAX = 2147483647;
  let i = 0;
  let j = 0;
  while (i < a.length || j < b.length) {
    let numericA: number;
    let numericB: number;
    let pointerA: number;
    let pointerB: number;
    if (a[i] === "+") {
      numericA = 0;
      pointerA = i;
    } else {
      const parsed = parseVersionDecimal(a, i);
      numericA = parsed.value;
      pointerA = parsed.end;
    }
    if (b[j] === "+") {
      numericB = 0;
      pointerB = j;
    } else {
      const parsed = parseVersionDecimal(b, j);
      numericB = parsed.value;
      pointerB = parsed.end;
    }
    const inSetA = pointerA >= a.length || ".-+".includes(a[pointerA]);
    const inSetB = pointerB >= b.length || ".-+".includes(b[pointerB]);
    if (!inSetA) numericA = INT_MAX;
    else i = pointerA;
    if (!inSetB) numericB = INT_MAX;
    else j = pointerB;
    if (numericA < numericB) return -1;
    if (numericA > numericB) return 1;
    const charA = i < a.length ? a[i] : "";
    const charB = j < b.length ? b[j] : "";
    if (charA === "." || charB === ".") {
      if (charA === ".") i++;
      if (charB === ".") j++;
      continue;
    }
    if (charA === "-" && charB !== "-") return -1;
    if (charB === "-" && charA !== "-") return 1;
    for (;; i++, j++) {
      const rawA = i < a.length ? a[i] : "";
      const rawB = j < b.length ? b[j] : "";
      const ac = rawA === "+" || rawA === "." ? "" : rawA;
      const bc = rawB === "+" || rawB === "." ? "" : rawB;
      if (ac !== bc) return ac < bc ? -1 : 1;
      if (ac === "") {
        if (rawA === "." || rawB === ".") break;
        return 0;
      }
    }
  }
  return 0;
}

/**
 * Compares version strings the way AHK `VerCompare` does with no operator:
 * numeric components, dot separators, `-prerelease` precedence, and ignored
 * `+metadata`. A leading `v` is stripped; operator prefixes in `b` are rejected.
 */
export function compareVersions(a: string, b: string): number {
  requireString(a, "compareVersions: a");
  requireString(b, "compareVersions: b");
  if (b.length > 0 && (b[0] === "<" || b[0] === ">" || b[0] === "=")) {
    throw new RangeError(`compareVersions: operator prefixes in b are not supported, got ${describeValue(b)}`);
  }
  const left = a[0] === "v" ? a.slice(1) : a;
  const right = b[0] === "v" ? b.slice(1) : b;
  return compareVersionParts(left, right);
}

const TIME_UNIT_SECONDS: Record<string, number | undefined> = { s: 1, m: 60, h: 3600, d: 86400 };

function requireUnitType(unit: TimeUnit, fn: string): void {
  if (typeof unit !== "string") {
    throw new TypeError(`${fn}: unit must be a string, got ${describeValue(unit)}`);
  }
}

function timeUnitFactor(unit: TimeUnit, fn: string): number {
  requireUnitType(unit, fn);
  const factor: number | undefined = TIME_UNIT_SECONDS[unit];
  if (factor === undefined) {
    throw new RangeError(`${fn}: unit must be one of "s", "m", "h", "d", got ${describeValue(unit)}`);
  }
  return factor;
}

function timestampSeconds(civil: Civil): number {
  return civil.epochDay * 86400 + civil.hour * 3600 + civil.minute * 60 + civil.second;
}

/** Adds `amount` units to a timestamp and returns a YYYYMMDDHH24MISS string. */
export function addTime(value: string | Date, amount: number, unit: TimeUnit): string {
  requireTimestampArg(value, "addTime");
  if (typeof amount !== "number") {
    throw new TypeError(`addTime: amount must be a number, got ${describeValue(amount)}`);
  }
  requireUnitType(unit, "addTime");
  const civil = parseTimestamp(value, "addTime");
  const factor = timeUnitFactor(unit, "addTime");
  if (!Number.isFinite(amount)) {
    throw new RangeError(`addTime: amount must be finite, got ${amount}`);
  }
  const delta = Math.trunc(amount * factor);
  if (!Number.isSafeInteger(delta)) {
    throw new RangeError(`addTime: amount ${amount} is out of supported range for unit ${describeValue(unit)}`);
  }
  const total = timestampSeconds(civil) + delta;
  if (!Number.isSafeInteger(total)) {
    throw new RangeError(`addTime: result is out of supported range for amount ${amount}`);
  }
  const epochDay = Math.floor(total / 86400);
  const secondOfDay = total - epochDay * 86400;
  const result = civilFromDays(epochDay);
  if (result.year < 1601 || result.year > 9999) {
    throw new RangeError(`addTime: result year ${result.year} is outside [1601, 9999]`);
  }
  const hour = Math.floor(secondOfDay / 3600);
  const minute = Math.floor((secondOfDay % 3600) / 60);
  const second = secondOfDay % 60;
  return (
    `${pad(result.year, 4)}${pad(result.month, 2)}${pad(result.day, 2)}` +
    `${pad(hour, 2)}${pad(minute, 2)}${pad(second, 2)}`
  );
}

/** Returns whole `unit`s from `from` to `to`, truncating toward zero. */
export function diffTime(from: string | Date, to: string | Date, unit: TimeUnit): number {
  requireTimestampArg(from, "diffTime");
  requireTimestampArg(to, "diffTime");
  requireUnitType(unit, "diffTime");
  const start = parseTimestamp(from, "diffTime");
  const end = parseTimestamp(to, "diffTime");
  const factor = timeUnitFactor(unit, "diffTime");
  return Math.trunc((timestampSeconds(end) - timestampSeconds(start)) / factor);
}

interface RegexFlags {
  js: string;
  extended: boolean;
}

const UNSUPPORTED_REGEX_FLAGS = new Set(["C", "J", "U", "X"]);
const REGEX_ESCAPE_ALLOWLIST = new Set([
  "d",
  "D",
  "w",
  "W",
  "s",
  "S",
  "f",
  "n",
  "r",
  "t",
  "b",
  "B",
  "c",
  "x",
  "u",
  "p",
  "P",
  "k",
  "Q",
]);

function parseRegexFlags(flags: string | undefined, fn: string): RegexFlags {
  if (flags === undefined) return { js: "", extended: false };
  requireString(flags, `${fn}: flags`);
  let js = "";
  let extended = false;
  for (const ch of flags) {
    if (ch === "i" || ch === "m" || ch === "s") {
      if (!js.includes(ch)) js += ch;
    } else if (ch === "x") {
      extended = true;
    } else if (ch === "A") {
      if (!js.includes("y")) js += "y";
    } else if (ch === "D" || ch === "S" || ch === "O") {
      continue;
    } else if (UNSUPPORTED_REGEX_FLAGS.has(ch)) {
      throw unsupported(`${fn}: flag ${describeValue(ch)} is not supported`);
    } else if (ch === "\x07" || ch === "\n" || ch === "\r") {
      throw unsupported(`${fn}: newline-mode flag ${describeValue(ch)} is not supported`);
    } else {
      throw new RangeError(`${fn}: unknown flag ${describeValue(ch)}`);
    }
  }
  return { js, extended };
}

function stripExtended(pattern: string): string {
  let out = "";
  let inClass = false;
  let classStart = false;
  let i = 0;
  while (i < pattern.length) {
    const ch = pattern[i];
    if (ch === "\\") {
      const next = pattern[i + 1];
      if (next === "Q") {
        const end = pattern.indexOf("\\E", i + 2);
        const stop = end < 0 ? pattern.length : end + 2;
        out += pattern.slice(i, stop);
        i = stop;
        continue;
      }
      out += ch + (next ?? "");
      i += next !== undefined ? 2 : 1;
      continue;
    }
    if (inClass) {
      if (classStart && ch === "^") {
        out += ch;
        i++;
        continue;
      }
      if (ch === "]" && !classStart) {
        inClass = false;
        out += ch;
        i++;
        continue;
      }
      classStart = false;
      out += ch;
      i++;
      continue;
    }
    if (ch === "[") {
      inClass = true;
      classStart = true;
      out += ch;
      i++;
      continue;
    }
    if (ch === " " || ch === "\t" || ch === "\n" || ch === "\r" || ch === "\f" || ch === "\v") {
      i++;
      continue;
    }
    if (ch === "#") {
      while (i < pattern.length && pattern[i] !== "\n") i++;
      continue;
    }
    out += ch;
    i++;
  }
  return out;
}

function escapeLiteral(text: string, inClass: boolean): string {
  if (inClass) return text.replace(/[\\^\[\]{}()*+?.|$~-]/g, "\\$&");
  return text.replace(/[\\^$.*+?()[\]{}|/]/g, "\\$&");
}

function rewritePattern(pattern: string, fn: string): { source: string; needsUnicode: boolean } {
  let out = "";
  let inClass = false;
  let classStart = false;
  let needsUnicode = false;
  let i = 0;
  while (i < pattern.length) {
    const ch = pattern[i];
    if (ch === "\\") {
      const next = pattern[i + 1];
      if (next === "Q") {
        const end = pattern.indexOf("\\E", i + 2);
        const literal = pattern.slice(i + 2, end < 0 ? pattern.length : end);
        out += escapeLiteral(literal, inClass);
        i = end < 0 ? pattern.length : end + 2;
        continue;
      }
      if (next === "u") {
        throw unsupported(`${fn}: escape \\u is not supported; use \\x{...} instead`);
      }
      if (next === "x" && pattern[i + 2] === "{") {
        const close = pattern.indexOf("}", i + 3);
        const hex = close < 0 ? "" : pattern.slice(i + 3, close);
        if (close < 0 || !/^[0-9A-Fa-f]+$/.test(hex)) {
          throw unsupported(`${fn}: malformed \\x{...} escape`);
        }
        out += `\\u{${hex}}`;
        needsUnicode = true;
        i = close + 1;
        continue;
      }
      if (next === "p" || next === "P" || next === "k") needsUnicode = true;
      out += next === undefined ? "\\" : `\\${next}`;
      i += next === undefined ? 1 : 2;
      continue;
    }
    if (inClass) {
      if (classStart && ch === "^") {
        out += ch;
        i++;
        continue;
      }
      if (ch === "]" && !classStart) {
        inClass = false;
        out += ch;
        i++;
        continue;
      }
      classStart = false;
      out += ch;
      i++;
      continue;
    }
    if (ch === "[") {
      inClass = true;
      classStart = true;
      out += ch;
      i++;
      continue;
    }
    if (ch === "(" && pattern[i + 1] === "?") {
      const rest = pattern.slice(i);
      const named = /^\(\?P<([A-Za-z_][0-9A-Za-z_]*)>/.exec(rest);
      if (named) {
        out += `(?<${named[1]}>`;
        i += named[0].length;
        continue;
      }
      const quoted = /^\(\?'([A-Za-z_][0-9A-Za-z_]*)'/.exec(rest);
      if (quoted) {
        out += `(?<${quoted[1]}>`;
        i += quoted[0].length;
        continue;
      }
      const backref = /^\(\?P=([A-Za-z_][0-9A-Za-z_]*)\)/.exec(rest);
      if (backref) {
        out += `\\k<${backref[1]}>`;
        needsUnicode = true;
        i += backref[0].length;
        continue;
      }
    }
    out += ch;
    i++;
  }
  return { source: out, needsUnicode };
}

function isQuantifierBrace(pattern: string, closeIndex: number): boolean {
  let open = -1;
  for (let k = closeIndex - 1; k >= 0; k--) {
    if (pattern[k] === "{") {
      const prev = k > 0 ? pattern[k - 1] : "";
      let beforePrev = 0;
      let m = k - 2;
      while (m >= 0 && pattern[m] === "\\") {
        beforePrev++;
        m--;
      }
      // \x{...} / \u{...}: the brace belongs to the escape, not to a quantifier.
      if ((prev === "x" || prev === "u") && beforePrev % 2 === 1) break;
      let backslashes = 0;
      m = k - 1;
      while (m >= 0 && pattern[m] === "\\") {
        backslashes++;
        m--;
      }
      if (backslashes % 2 === 0) open = k;
      break;
    }
  }
  if (open < 0) return false;
  const body = pattern.slice(open + 1, closeIndex);
  return /^\d+(,\d*)?$|^,\d+$/.test(body);
}

function assertSupportedPattern(pattern: string, fn: string): void {
  let inClass = false;
  let classStart = false;
  let i = 0;
  while (i < pattern.length) {
    const ch = pattern[i];
    if (ch === "\\") {
      const next = pattern[i + 1];
      if (next === undefined) break;
      if (next >= "0" && next <= "9") {
        i += 2;
        continue;
      }
      if (!/[a-zA-Z]/.test(next)) {
        i += 2;
        continue;
      }
      if (!REGEX_ESCAPE_ALLOWLIST.has(next)) {
        throw unsupported(`${fn}: escape \\${next} is not supported`);
      }
      if (next === "k") {
        const after = pattern[i + 2];
        if (after !== "<" && after !== "{") {
          throw unsupported(`${fn}: \\k must be followed by a group name`);
        }
      }
      i += 2;
      continue;
    }
    if (inClass) {
      if (classStart && ch === "^") {
        i++;
        continue;
      }
      if (ch === "]" && !classStart) {
        inClass = false;
        classStart = false;
        i++;
        continue;
      }
      if (ch === "[") {
        const posix = /^\[:[a-zA-Z]+:\]/.exec(pattern.slice(i));
        if (posix) {
          throw unsupported(`${fn}: POSIX character classes such as ${posix[0]} are not supported`);
        }
        if (pattern[i + 1] === "=" || pattern[i + 1] === ".") {
          throw unsupported(`${fn}: collation expressions like [${pattern[i + 1]}...${pattern[i + 1]}] are not supported`);
        }
      }
      classStart = false;
      i++;
      continue;
    }
    if (ch === "[") {
      inClass = true;
      classStart = true;
      i++;
      continue;
    }
    if (ch === "(") {
      const rest = pattern.slice(i);
      if (rest.startsWith("(?>")) throw unsupported(`${fn}: atomic groups (?>...) are not supported`);
      if (rest.startsWith("(?|")) throw unsupported(`${fn}: branch reset groups (?|...) are not supported`);
      if (rest.startsWith("(?(")) throw unsupported(`${fn}: conditional groups (?(...)...) are not supported`);
      if (rest.startsWith("(?C")) throw unsupported(`${fn}: callouts (?C...) are not supported`);
      if (rest.startsWith("(?#")) throw unsupported(`${fn}: comments (?#...) are not supported`);
      if (rest.startsWith("(?P>")) throw unsupported(`${fn}: subroutine calls (?P>...) are not supported`);
      if (rest.startsWith("(?&")) throw unsupported(`${fn}: subroutine calls (?&...) are not supported`);
      if (/^\(\?[R\d]/.test(rest)) throw unsupported(`${fn}: recursion (?R) is not supported`);
      if (/^\(\?[+-]\d/.test(rest)) throw unsupported(`${fn}: relative recursion is not supported`);
      if (rest.startsWith("(*")) throw unsupported(`${fn}: backtrack verbs like ${rest.slice(0, 10)} are not supported`);
      i++;
      continue;
    }
    if (ch === "*" || ch === "+" || ch === "?") {
      if (pattern[i + 1] === "+") {
        throw unsupported(`${fn}: possessive quantifier ${ch}+ is not supported`);
      }
      i++;
      continue;
    }
    if (ch === "}") {
      if (pattern[i + 1] === "+" && isQuantifierBrace(pattern, i)) {
        throw unsupported(`${fn}: possessive quantifier }+ is not supported`);
      }
      i++;
      continue;
    }
    i++;
  }
}

function collectGroupNames(source: string): string[] {
  const names: string[] = [];
  const pattern = /\(\?<([A-Za-z_][0-9A-Za-z_]*)>/g;
  let match = pattern.exec(source);
  while (match !== null) {
    names.push(match[1]);
    match = pattern.exec(source);
  }
  return names;
}

interface CompiledPattern {
  re: RegExp;
  names: string[];
}

function compilePattern(pattern: string, flags: string | undefined, fn: string, global = false): CompiledPattern {
  const flagSet = parseRegexFlags(flags, fn);
  const source = flagSet.extended ? stripExtended(pattern) : pattern;
  const rewritten = rewritePattern(source, fn);
  assertSupportedPattern(rewritten.source, fn);
  const names = collectGroupNames(rewritten.source);
  let jsFlags = flagSet.js;
  if (rewritten.needsUnicode && !jsFlags.includes("u")) jsFlags += "u";
  if (global && !jsFlags.includes("g") && !jsFlags.includes("y")) jsFlags += "g";
  try {
    return { re: new RegExp(rewritten.source, jsFlags), names };
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    throw new SyntaxError(`${fn}: invalid pattern ${describeValue(pattern)}: ${message}`);
  }
}

/**
 * Executes a regex and returns AHK-shaped match data: 0-based `pos`, `len`,
 * `value`, `groups` (with `[0]` the whole match), optional `named`, and
 * `count`. Returns null when the pattern does not match.
 */
export function regexMatch(text: string, pattern: string, flags?: string): RegexMatchResult | null {
  requireString(text, "regexMatch: text");
  requireString(pattern, "regexMatch: pattern");
  const compiled = compilePattern(pattern, flags, "regexMatch");
  const match = compiled.re.exec(text);
  if (match === null) return null;
  const groups: Array<string | undefined> = [];
  for (let g = 0; g < match.length; g++) groups.push(match[g]);
  const result: RegexMatchResult = {
    pos: match.index,
    len: match[0].length,
    value: match[0],
    groups,
    count: groups.length - 1,
  };
  if (compiled.names.length > 0) {
    const named: Record<string, string | undefined> = {};
    for (const name of compiled.names) named[name] = match.groups ? match.groups[name] : undefined;
    result.named = named;
  }
  return result;
}

function expandReplacement(template: string, match: RegExpExecArray): string {
  let out = "";
  let i = 0;
  while (i < template.length) {
    const ch = template[i];
    if (ch !== "$") {
      out += ch;
      i++;
      continue;
    }
    const next = template[i + 1];
    if (next === undefined) {
      out += "$";
      i++;
      continue;
    }
    if (next === "$") {
      out += "$";
      i += 2;
      continue;
    }
    if (next === "&") {
      out += match[0];
      i += 2;
      continue;
    }
    if (next === "<") {
      const close = template.indexOf(">", i + 2);
      if (close < 0) {
        out += "$<";
        i += 2;
        continue;
      }
      const name = template.slice(i + 2, close);
      if (match.groups && Object.prototype.hasOwnProperty.call(match.groups, name)) {
        out += match.groups[name] ?? "";
      }
      i = close + 1;
      continue;
    }
    if (next >= "1" && next <= "9") {
      let digits = next;
      const following = template[i + 2];
      if (following !== undefined && following >= "0" && following <= "9") {
        if (Number(digits + following) <= match.length - 1) digits += following;
      }
      const index = Number(digits);
      if (index <= match.length - 1) out += match[index] ?? "";
      else out += `$${digits}`;
      i += 1 + digits.length;
      continue;
    }
    out += `$${next}`;
    i += 2;
  }
  return out;
}

/**
 * Replaces regex matches following AHK `RegExReplace` loop semantics
 * (zero-width matches retry non-empty at the same position) with JS-style
 * `$1`/`$&`/`$$`/`$<name>` replacement templates.
 */
export function regexReplace(
  text: string,
  pattern: string,
  replacement: string,
  options?: RegexReplaceOptions,
): string {
  requireString(text, "regexReplace: text");
  requireString(pattern, "regexReplace: pattern");
  requireString(replacement, "regexReplace: replacement");
  if (options !== undefined && (typeof options !== "object" || options === null || Array.isArray(options))) {
    throw new TypeError(`regexReplace: options must be an object, got ${describeValue(options)}`);
  }
  const opts: RegexReplaceOptions = options ?? {};
  const limit = opts.limit;
  if (limit !== undefined) {
    if (typeof limit !== "number") {
      throw new TypeError(`regexReplace: options.limit must be a number, got ${describeValue(limit)}`);
    }
    if (!Number.isInteger(limit) || limit < 0) {
      throw new RangeError(`regexReplace: options.limit must be a non-negative integer, got ${limit}`);
    }
  }
  const start = opts.start;
  if (start !== undefined) {
    if (typeof start !== "number") {
      throw new TypeError(`regexReplace: options.start must be a number, got ${describeValue(start)}`);
    }
    if (!Number.isInteger(start) || start < 0 || start > text.length) {
      throw new RangeError(`regexReplace: options.start must be an integer in [0, ${text.length}], got ${start}`);
    }
  }
  const compiled = compilePattern(pattern, opts.flags, "regexReplace", true);
  if (limit === 0) return text;
  const offset = start ?? 0;
  const prefix = text.slice(0, offset);
  const rest = text.slice(offset);
  const max = limit ?? Number.POSITIVE_INFINITY;
  const re = compiled.re;
  let out = "";
  let lastCopied = 0;
  let index = 0;
  let count = 0;
  let notEmptyRetry = false;
  const copyOne = (): void => {
    const code = rest.codePointAt(index);
    const step = code !== undefined && code > 0xffff ? 2 : 1;
    out += rest.slice(lastCopied, index + step);
    lastCopied = index + step;
    index = index + step;
    notEmptyRetry = false;
  };
  for (;;) {
    if (count >= max) break;
    re.lastIndex = index;
    const match = re.exec(rest);
    if (match === null) {
      if (notEmptyRetry && index < rest.length) copyOne();
      else break;
      continue;
    }
    if (notEmptyRetry && (match.index !== index || match[0].length === 0)) {
      copyOne();
      continue;
    }
    out += rest.slice(lastCopied, match.index);
    out += expandReplacement(replacement, match);
    lastCopied = match.index + match[0].length;
    index = lastCopied;
    count++;
    notEmptyRetry = match[0].length === 0;
  }
  out += rest.slice(lastCopied);
  return prefix + out;
}
