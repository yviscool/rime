export function unsupported(message: string): Error {
  const error = new Error(message);
  error.name = "Unsupported";
  return error;
}

export function describeValue(value: unknown): string {
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

export function requireString(value: unknown, label: string): string {
  if (typeof value !== "string") {
    throw new TypeError(`${label} must be a string, got ${describeValue(value)}`);
  }
  return value;
}

export function isDigit(ch: string): boolean {
  return ch >= "0" && ch <= "9";
}

export function pad(value: number, width: number): string {
  const sign = value < 0 ? "-" : "";
  return sign + String(Math.abs(value)).padStart(width, "0");
}
