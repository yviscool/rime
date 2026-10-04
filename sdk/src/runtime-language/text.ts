import { describeValue, isDigit, requireString } from "./shared";

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
