import { describeValue, isDigit, pad } from "./shared";

/** Time unit accepted by {@link addTime} and {@link diffTime}. */
export type TimeUnit = "s" | "m" | "h" | "d";

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
