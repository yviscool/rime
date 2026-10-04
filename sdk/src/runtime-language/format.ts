import { describeValue, isDigit, requireString, unsupported } from "./shared";

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
