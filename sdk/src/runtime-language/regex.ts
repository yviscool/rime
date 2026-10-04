import { describeValue, requireString, unsupported } from "./shared";

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
