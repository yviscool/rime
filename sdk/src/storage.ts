import { ActionError, runAction, type ActionOptions, type NativeActionOptions } from "./action";

/**
 * File open modes accepted by `storage.open` and {@link storage.openFile}.
 * `"r"` requires the file to exist, `"a"` opens-or-creates and starts at end
 * of file, `"w"` truncates-or-creates.
 */
export type FileMode = "r" | "a" | "w";

/** Session encodings the native storage service accepts (case-insensitive). */
export type SessionEncoding =
  | "utf-8"
  | "utf-8-bom"
  | "utf-16"
  | "utf-16-be"
  | "cp0"
  | "cp1252"
  | "latin1";

/**
 * Encodings {@link File} can actually decode. `cp0` (the ANSI code page) is
 * deliberately absent: this SDK ships no code-page tables, so opening a file
 * while the session encoding is `cp0` rejects with a TypeError.
 */
export type FileEncoding = Exclude<SessionEncoding, "cp0">;

/** Result of `storage.stat`: size, last write time, AHK attribute letters. */
export interface FileInfo {
  size: number;
  /** Last write time, milliseconds since the Unix epoch (UTC). */
  mtimeMs: number;
  /** Attribute letters in AHK's `RASHNDOCTL` order (a plain file reads `"A"`). */
  attrib: string;
  isDir: boolean;
}

/** One immediate child of a directory listing. */
export interface DirEntry {
  name: string;
  isDir: boolean;
}

/** Result of `storage.driveGet`; only the queried field is populated. */
export interface DriveInfo {
  /** `"C:"` for letter-based fields, `""` for the `list` field. */
  letter: string;
  filesystem: string;
  label: string;
  /** `Unknown|Removable|Fixed|Network|CDROM|RAMDisk`. */
  type: string;
  /** `Ready|Invalid|NotReady|ReadOnly|Unknown`. */
  status: string;
  totalBytes: number;
  freeBytes: number;
  serial: number;
  /** Drive letters spelled `"A"`..`"Z"`; filled by the `list` field only. */
  list: string[];
  /** `0..100`, or `-1` when the query did not read capacity. */
  capacityPercent: number;
}

/** Result of `storage.open`: an opaque handle id plus the file length. */
export interface OpenedFile {
  /** Monotonic id owned by the service. Never a raw OS HANDLE. */
  handle: number;
  length: number;
}

/** Result of one `storage.write` action: the op that was applied. */
export interface StorageWriteResult {
  op: string;
}

/** Payload of one `storage.write` action. The executor rejects any field the op does not declare. */
export type StorageWritePayload =
  | { op: "append" | "write"; path: string; text: string }
  | {
      op: "copy" | "move" | "install" | "dircopy" | "dirmove";
      src: string;
      dst: string;
      overwrite?: boolean;
    }
  | { op: "delete" | "mkdir" | "recycle"; path: string }
  | { op: "rmdir"; path: string; recursive?: boolean }
  | { op: "setAttrib"; path: string; add?: string; remove?: string }
  | { op: "setTime"; path: string; which: "mtime" | "atime" | "ctime"; unixMs: number }
  | { op: "recycleEmpty"; root?: string }
  | {
      op: "shortcut";
      path: string;
      target: string;
      args?: string;
      workdir?: string;
      icon?: string;
      description?: string;
    }
  | { op: "env"; name: string; value: string }
  | { op: "iniWrite"; path: string; section: string; key: string; value: string }
  | { op: "iniDelete"; path: string; section: string; key?: string }
  | { op: "driveLabel"; letter: string; label: string }
  | { op: "driveLock" | "driveUnlock" | "driveEject" | "driveRetract"; letter: string }
  | { op: "handleWrite"; handle: number; data: number[] | string };

/** Options accepted by `storage.selectFile`. */
export interface StorageSelectFileOptions extends NativeActionOptions {
  /** `Name (*.patterns)` or a bare pattern list; never empty. */
  filter?: string;
  defaultName?: string;
  multi?: boolean;
}

/** Options accepted by `storage.selectDir`. */
export interface StorageSelectDirOptions extends NativeActionOptions {
  caption?: string;
}

/** Options accepted by {@link storage.openFile}. */
export interface FileOpenOptions extends ActionOptions {
  /** Overrides the session encoding for this file only. */
  encoding?: FileEncoding;
}

/**
 * Bridge of the `rime:storage` module - the exact shape the native module
 * exports as `storage`. `write` is an Action (queued, traced, capability
 * `filesystem.write`); everything else is a direct service call that resolves
 * without passing through the Action kernel.
 */
export interface StorageBridge {
  readText(path: string, options?: NativeActionOptions): Promise<{ text: string }>;
  readBytes(path: string, options?: NativeActionOptions): Promise<{ bytes: number[] }>;
  stat(path: string, options?: NativeActionOptions): Promise<FileInfo>;
  /** Decoded .lnk fields; unset fields read back empty. */
  shortcut(
    path: string,
    options?: NativeActionOptions,
  ): Promise<{ target: string; workingDir: string; args: string; icon: string }>;
  /** File version resource as "M.m.b.r"; "" when the file carries none. */
  version(path: string, options?: NativeActionOptions): Promise<{ version: string }>;
  list(path: string, options?: NativeActionOptions): Promise<{ entries: DirEntry[] }>;
  envGet(name: string, options?: NativeActionOptions): Promise<{ value: string }>;
  iniRead(
    path: string,
    section: string,
    key: string,
    options?: NativeActionOptions,
  ): Promise<{ value: string }>;
  driveGet(
    field: string,
    letter?: string,
    options?: NativeActionOptions,
  ): Promise<DriveInfo>;
  open(path: string, mode: FileMode, options?: NativeActionOptions): Promise<OpenedFile>;
  fileRead(
    handle: number,
    count: number,
    options?: NativeActionOptions,
  ): Promise<{ bytes: number[]; eof: boolean }>;
  fileSeek(
    handle: number,
    offset: number,
    whence: 0 | 1 | 2,
    options?: NativeActionOptions,
  ): Promise<{ pos: number }>;
  fileStat(handle: number, options?: NativeActionOptions): Promise<{ pos: number; length: number }>;
  fileClose(handle: number, options?: NativeActionOptions): Promise<Record<string, never>>;
  write(payload: StorageWritePayload, options?: NativeActionOptions): Promise<StorageWriteResult>;
  /** Synchronous in the native module; the SDK facade is async only because the module loads dynamically. */
  encoding(): { encoding: SessionEncoding };
  /** Synchronous in the native module; rejects an unknown name with a TypeError. */
  setEncoding(encoding: string): { encoding: SessionEncoding };
  download(url: string, path: string, options?: NativeActionOptions): Promise<{ bytes: number }>;
  selectFile(options?: StorageSelectFileOptions): Promise<{ paths: string[] }>;
  selectDir(options?: StorageSelectDirOptions): Promise<{ path: string }>;
}

// ---- encoding tables -------------------------------------------------------

const SESSION_ENCODINGS: readonly SessionEncoding[] = [
  "utf-8",
  "utf-8-bom",
  "utf-16",
  "utf-16-be",
  "cp0",
  "cp1252",
  "latin1",
];

const FILE_ENCODINGS: readonly FileEncoding[] = [
  "utf-8",
  "utf-8-bom",
  "utf-16",
  "utf-16-be",
  "cp1252",
  "latin1",
];

// Windows-1252 bytes 0x80..0x9F (the rest of the page is ISO-8859-1).
// Bytes the standard leaves undefined decode to their C1 control so the
// mapping stays a bijection.
const CP1252_C1 =
  "\u20ac\u0081\u201a\u0192\u201e\u2026\u2020\u2021\u02c6\u2030\u0160\u2039\u0152\u008d\u017e\u008f" +
  "\u0090\u2018\u2019\u201c\u201d\u2022\u2013\u2014\u02dc\u2122\u0161\u203a\u0153\u009d\u017e\u0178";

const CP1252_ENCODE = new Map<number, number>();
for (let index = 0; index < CP1252_C1.length; index++) {
  CP1252_ENCODE.set(CP1252_C1.charCodeAt(index), 0x80 + index);
}

const REPLACEMENT = 0xfffd;
const UNMAPPABLE = 0x3f; // "?"

function normalizeSessionEncoding(value: unknown, slot: string): SessionEncoding {
  if (typeof value !== "string") {
    throw new TypeError(`${slot} must be a string`);
  }
  const name = value.toLowerCase() as SessionEncoding;
  if (SESSION_ENCODINGS.indexOf(name) < 0) {
    throw new TypeError(
      `${slot} must be one of ${SESSION_ENCODINGS.join(", ")}`,
    );
  }
  return name;
}

function toFileEncoding(value: unknown, slot: string): FileEncoding {
  const name = normalizeSessionEncoding(value, slot);
  if (name === "cp0") {
    throw new TypeError(
      `${slot} must be one of ${FILE_ENCODINGS.join(", ")}: ` +
        "cp0 is unsupported because the File API cannot decode the ANSI code page",
    );
  }
  return name;
}

function bomOf(encoding: FileEncoding): number[] {
  if (encoding === "utf-8-bom") return [0xef, 0xbb, 0xbf];
  if (encoding === "utf-16") return [0xff, 0xfe];
  if (encoding === "utf-16-be") return [0xfe, 0xff];
  return [];
}

function detectBom(bytes: number[]): { encoding: FileEncoding; length: number } | null {
  if (bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) {
    return { encoding: "utf-8-bom", length: 3 };
  }
  if (bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xfe) {
    return { encoding: "utf-16", length: 2 };
  }
  if (bytes.length >= 2 && bytes[0] === 0xfe && bytes[1] === 0xff) {
    return { encoding: "utf-16-be", length: 2 };
  }
  return null;
}

/** Worst-case source bytes one UTF-16 code unit can occupy. */
function bytesPerUnit(encoding: FileEncoding): number {
  if (encoding === "utf-16" || encoding === "utf-16-be") return 2;
  if (encoding === "cp1252" || encoding === "latin1") return 1;
  return 4;
}

interface Decoded {
  text: string;
  /** Bytes actually decoded; the input may hold a partial trailing unit. */
  consumed: number;
  /**
   * `offsets[u]` is the byte index where code unit `u` starts; the last entry
   * is `consumed`. Line scanning uses it to rewind to a line terminator
   * without decoding the stream twice.
   */
  offsets: number[];
}

function invalidUtf8(at: number): never {
  throw new ActionError(
    "invalid_contract",
    `storage file is not valid UTF-8 at byte offset ${at}`,
  );
}

function decodeUtf8(bytes: number[], maxUnits: number): Decoded {
  const text: string[] = [];
  const offsets: number[] = [];
  let index = 0;
  let units = 0;
  while (index < bytes.length && units < maxUnits) {
    const start = index;
    const first = bytes[index];
    let code = 0;
    let length = 0;
    let extraMin = 0;
    if (first < 0x80) {
      code = first;
      length = 1;
    } else if (first >= 0xc2 && first <= 0xdf) {
      code = first & 0x1f;
      length = 2;
      extraMin = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      code = first & 0x0f;
      length = 3;
      extraMin = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code = first & 0x07;
      length = 4;
      extraMin = 0x10000;
    } else {
      invalidUtf8(index);
    }
    if (index + length > bytes.length) break; // partial trailing sequence
    for (let step = 1; step < length; step++) {
      const next = bytes[index + step];
      if ((next & 0xc0) !== 0x80) invalidUtf8(index + step);
      code = (code << 6) | (next & 0x3f);
    }
    if (code < extraMin) invalidUtf8(index);
    if (code >= 0xd800 && code <= 0xdfff) invalidUtf8(index);
    if (code > 0x10ffff) invalidUtf8(index);
    index += length;
    if (code > 0xffff) {
      if (units + 2 > maxUnits) {
        index = start; // never split a surrogate pair
        break;
      }
      const high = 0xd800 + ((code - 0x10000) >> 10);
      const low = 0xdc00 + ((code - 0x10000) & 0x3ff);
      text.push(String.fromCharCode(high), String.fromCharCode(low));
      offsets.push(start, start);
      units += 2;
    } else {
      text.push(String.fromCharCode(code));
      offsets.push(start);
      units += 1;
    }
  }
  offsets.push(index);
  return { text: text.join(""), consumed: index, offsets };
}

function readUnit16(bytes: number[], at: number, bigEndian: boolean): number {
  const high = bytes[at];
  const low = bytes[at + 1];
  return bigEndian ? (high << 8) | low : (low << 8) | high;
}

function decodeUtf16(bytes: number[], maxUnits: number, bigEndian: boolean): Decoded {
  const text: string[] = [];
  const offsets: number[] = [];
  let index = 0;
  let units = 0;
  while (units < maxUnits) {
    if (index + 1 >= bytes.length) break;
    const start = index;
    const high = readUnit16(bytes, index, bigEndian);
    let pair = false;
    if (high >= 0xd800 && high <= 0xdbff && index + 3 < bytes.length) {
      pair = readUnit16(bytes, index + 2, bigEndian) >= 0xdc00 &&
        readUnit16(bytes, index + 2, bigEndian) <= 0xdfff;
    }
    if (pair && units + 2 > maxUnits) break; // never split a surrogate pair
    if (pair) {
      const low = readUnit16(bytes, index + 2, bigEndian);
      text.push(String.fromCharCode(high, low));
      offsets.push(start, start);
      index += 4;
      units += 2;
    } else {
      text.push(String.fromCharCode(high));
      offsets.push(start);
      index += 2;
      units += 1;
    }
  }
  offsets.push(index);
  return { text: text.join(""), consumed: index, offsets };
}

function decodeSingleByte(
  bytes: number[],
  maxUnits: number,
  encoding: "cp1252" | "latin1",
): Decoded {
  const text: string[] = [];
  const offsets: number[] = [];
  const limit = Math.min(bytes.length, maxUnits);
  for (let index = 0; index < limit; index++) {
    const byte = bytes[index];
    text.push(
      encoding === "cp1252" && byte >= 0x80 && byte <= 0x9f
        ? CP1252_C1.charAt(byte - 0x80)
        : String.fromCharCode(byte),
    );
    offsets.push(index);
  }
  offsets.push(limit);
  return { text: text.join(""), consumed: limit, offsets };
}

function decodePrefix(bytes: number[], encoding: FileEncoding, maxUnits: number): Decoded {
  if (encoding === "utf-8" || encoding === "utf-8-bom") return decodeUtf8(bytes, maxUnits);
  if (encoding === "utf-16") return decodeUtf16(bytes, maxUnits, false);
  if (encoding === "utf-16-be") return decodeUtf16(bytes, maxUnits, true);
  return decodeSingleByte(bytes, maxUnits, encoding);
}

function encodeUtf8(text: string): number[] {
  const out: number[] = [];
  for (let index = 0; index < text.length; index++) {
    let code = text.charCodeAt(index);
    if (code >= 0xd800 && code <= 0xdbff && index + 1 < text.length) {
      const low = text.charCodeAt(index + 1);
      if (low >= 0xdc00 && low <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        index++;
      }
    }
    if (code >= 0xd800 && code <= 0xdfff) code = REPLACEMENT;
    if (code < 0x80) {
      out.push(code);
    } else if (code < 0x800) {
      out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
      out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    } else {
      out.push(
        0xf0 | (code >> 18),
        0x80 | ((code >> 12) & 0x3f),
        0x80 | ((code >> 6) & 0x3f),
        0x80 | (code & 0x3f),
      );
    }
  }
  return out;
}

function encodeUtf16(text: string, bigEndian: boolean): number[] {
  const out: number[] = [];
  for (let index = 0; index < text.length; index++) {
    const unit = text.charCodeAt(index);
    const high = unit >> 8;
    const low = unit & 0xff;
    if (bigEndian) out.push(high, low);
    else out.push(low, high);
  }
  return out;
}

function encodeSingleByte(text: string, encoding: "cp1252" | "latin1"): number[] {
  const out: number[] = [];
  for (let index = 0; index < text.length; index++) {
    const unit = text.charCodeAt(index);
    const mapped = encoding === "cp1252" ? CP1252_ENCODE.get(unit) : undefined;
    if (mapped !== undefined) out.push(mapped);
    else if (unit <= 0xff) out.push(unit);
    else out.push(UNMAPPABLE);
  }
  return out;
}

function encodeText(text: string, encoding: FileEncoding): number[] {
  if (encoding === "utf-8" || encoding === "utf-8-bom") return encodeUtf8(text);
  if (encoding === "utf-16") return encodeUtf16(text, false);
  if (encoding === "utf-16-be") return encodeUtf16(text, true);
  return encodeSingleByte(text, encoding);
}

// ---- argument helpers ------------------------------------------------------

const MAX_SAFE = 9007199254740991; // 2^53 - 1

function requireInteger(value: unknown, slot: string, low: number, high: number): number {
  if (typeof value !== "number" || !Number.isFinite(value) || !Number.isInteger(value)) {
    throw new TypeError(`${slot} must be an integer`);
  }
  if (value < low || value > high) {
    throw new RangeError(`${slot} must be between ${low} and ${high}`);
  }
  return value;
}

function requireNonNegative(value: unknown, slot: string): number {
  if (
    typeof value !== "number" ||
    !Number.isFinite(value) ||
    !Number.isInteger(value) ||
    value < 0
  ) {
    throw new TypeError(`${slot} must be a non-negative integer`);
  }
  if (value > MAX_SAFE) throw new RangeError(`${slot} is out of range`);
  return value;
}

function coerceBytes(value: unknown, slot: string): number[] {
  if (typeof value === "string") return encodeUtf8(value);
  if (!Array.isArray(value)) {
    throw new TypeError(`${slot} must be a string or an array of 0..255`);
  }
  const out: number[] = [];
  for (const item of value) {
    if (typeof item !== "number" || !Number.isInteger(item) || item < 0 || item > 255) {
      throw new TypeError(`${slot} elements must be integers 0..255`);
    }
    out.push(item);
  }
  return out;
}

function textArgument(value: unknown, slot: string): string {
  if (typeof value === "string") return value;
  if (typeof value === "number" && Number.isFinite(value)) return String(value);
  throw new TypeError(`${slot} must be a string or a number`);
}

function viewOf(bytes: number[]): DataView {
  const view = new DataView(new ArrayBuffer(bytes.length));
  for (let index = 0; index < bytes.length; index++) view.setUint8(index, bytes[index]);
  return view;
}

function bytesOf(view: DataView): number[] {
  const out: number[] = [];
  for (let index = 0; index < view.byteLength; index++) out.push(view.getUint8(index));
  return out;
}

type FixedKind = "int8" | "uint8" | "int16" | "uint16" | "int32" | "uint32" | "int64" | "float32" | "float64";

function fixedSize(kind: FixedKind): number {
  if (kind === "int8" || kind === "uint8") return 1;
  if (kind === "int16" || kind === "uint16") return 2;
  if (kind === "int32" || kind === "uint32" || kind === "float32") return 4;
  return 8;
}

function readFixed(bytes: number[], kind: FixedKind): number {
  const view = viewOf(bytes);
  switch (kind) {
    case "int8":
      return view.getInt8(0);
    case "uint8":
      return view.getUint8(0);
    case "int16":
      return view.getInt16(0, true);
    case "uint16":
      return view.getUint16(0, true);
    case "int32":
      return view.getInt32(0, true);
    case "uint32":
      return view.getUint32(0, true);
    case "int64":
      return Number(view.getBigInt64(0, true));
    case "float32":
      return view.getFloat32(0, true);
    default:
      return view.getFloat64(0, true);
  }
}

function writeFixed(value: number, kind: FixedKind): number[] {
  const size = fixedSize(kind);
  const view = new DataView(new ArrayBuffer(size));
  switch (kind) {
    case "int8":
      view.setInt8(0, value);
      break;
    case "uint8":
      view.setUint8(0, value);
      break;
    case "int16":
      view.setInt16(0, value, true);
      break;
    case "uint16":
      view.setUint16(0, value, true);
      break;
    case "int32":
      view.setInt32(0, value, true);
      break;
    case "uint32":
      view.setUint32(0, value, true);
      break;
    case "int64":
      view.setBigInt64(0, BigInt(Math.trunc(value)), true);
      break;
    case "float32":
      view.setFloat32(0, value, true);
      break;
    default:
      view.setFloat64(0, value, true);
      break;
  }
  return bytesOf(view);
}

// ---- File ------------------------------------------------------------------

/** The maximum number of code units one {@link File.readLine} call returns. */
const MAX_LINE_UNITS = 65535;
/** The maximum a single native handle read may request (the service's cap). */
const MAX_READ_CHUNK = 64 * 1024 * 1024;

/**
 * An open file over the storage service's handle table.
 *
 * All members are serialized through one internal queue, so two overlapping
 * calls keep the order in which they were issued - the same guarantee the
 * native service gives per handle. Property getters return promises (they
 * must ask the service); `Encoding` and `Handle` are synchronous.
 *
 * Constructing one directly requires an already-open handle; prefer
 * {@link storage.openFile}, which resolves the encoding and runs the BOM
 * sniff for you.
 */
export class File {
  private readonly bridge: StorageBridge;
  private readonly handleId: number;
  private readonly mode: FileMode;
  private readonly options: NativeActionOptions | undefined;
  private currentEncoding: FileEncoding;
  private isClosed = false;
  private sniffed = false;
  private tail: Promise<unknown> = Promise.resolve();
  private pendingPosWrite: Promise<void> | null = null;

  constructor(
    bridge: StorageBridge,
    handle: number,
    mode: FileMode,
    encoding: FileEncoding,
    options?: NativeActionOptions,
  ) {
    this.bridge = bridge;
    this.handleId = handle;
    this.mode = mode;
    this.currentEncoding = encoding;
    this.options = options;
  }

  // ---- lifecycle ----------------------------------------------------------

  private enqueue<T>(work: () => Promise<T>): Promise<T> {
    const run = this.tail.then(work);
    this.tail = run.then(
      () => undefined,
      () => undefined,
    );
    return run;
  }

  private ensureOpen(): void {
    if (this.isClosed) throw new ActionError("invalid_state", "file is closed");
  }

  private async ready(): Promise<void> {
    this.ensureOpen();
    if (this.sniffed) return;
    this.sniffed = true;
    if (this.mode === "r") {
      const probe = await this.bridge.fileRead(this.handleId, 4, this.options);
      const bom = detectBom(probe.bytes);
      await this.bridge.fileSeek(this.handleId, bom ? bom.length : 0, 0, this.options);
      if (bom) this.currentEncoding = bom.encoding;
      return;
    }
    // "a"/"w": a BOM is written before the first text write into a file that
    // starts out empty; an appended-to file keeps the bytes it already has.
    // The check lives in putText, so there is nothing to prepare here.
  }

  private async readUpTo(limit: number): Promise<number[]> {
    const out: number[] = [];
    let got = 0;
    while (got < limit) {
      const want = Math.min(limit - got, MAX_READ_CHUNK);
      const chunk = await this.bridge.fileRead(this.handleId, want, this.options);
      for (const byte of chunk.bytes) out.push(byte);
      got += chunk.bytes.length;
      if (chunk.eof || chunk.bytes.length === 0) break;
    }
    return out;
  }

  private async putBytes(bytes: number[]): Promise<void> {
    await this.bridge.write(
      { op: "handleWrite", handle: this.handleId, data: bytes },
      this.options,
    );
  }

  /** Writes the encoding's BOM when the file is still empty, then `bytes`. */
  private async putText(bytes: number[]): Promise<number> {
    if (bytes.length === 0) return 0;
    const stat = await this.bridge.fileStat(this.handleId, this.options);
    let payload = bytes;
    if (stat.pos === 0 && stat.length === 0) {
      const bom = bomOf(this.currentEncoding);
      if (bom.length > 0) payload = bom.concat(bytes);
    }
    await this.putBytes(payload);
    return payload.length;
  }

  // ---- properties ---------------------------------------------------------

  /** True once the stream has no unread bytes left. */
  get AtEOF(): Promise<boolean> {
    return this.enqueue(async () => {
      await this.ready();
      const stat = await this.bridge.fileStat(this.handleId, this.options);
      return stat.pos >= stat.length;
    });
  }

  /** File length in bytes (the whole file, not the unread remainder). */
  get Length(): Promise<number> {
    return this.enqueue(async () => {
      await this.ready();
      const stat = await this.bridge.fileStat(this.handleId, this.options);
      return stat.length;
    });
  }

  set Length(_value: number) {
    this.ensureOpen();
    throw new TypeError(
      "Length has no setter: truncation is unsupported because the storage service exposes no truncate primitive",
    );
  }

  /** Current read/write position, measured in bytes from the start of the file. */
  get Pos(): Promise<number> {
    return this.enqueue(async () => {
      await this.ready();
      if (this.pendingPosWrite) await this.pendingPosWrite;
      const stat = await this.bridge.fileStat(this.handleId, this.options);
      return stat.pos;
    });
  }

  set Pos(value: number) {
    this.ensureOpen();
    const target = requireNonNegative(value, "Pos");
    const pending = this.enqueue(async () => {
      await this.ready();
      await this.bridge.fileSeek(this.handleId, target, 0, this.options);
    });
    pending.catch(() => undefined);
    this.pendingPosWrite = pending;
  }

  /**
   * Always throws: exposing a raw OS HANDLE would break the service's rule
   * that no Win32 handle ever leaves the native layer.
   */
  get Handle(): number {
    this.ensureOpen();
    throw new ActionError("unsupported", "raw OS HANDLE exposure is unsupported by policy");
  }

  /** This file's encoding. Assigning an unknown name is a synchronous TypeError. */
  get Encoding(): FileEncoding {
    this.ensureOpen();
    return this.currentEncoding;
  }

  set Encoding(value: FileEncoding | SessionEncoding | string) {
    this.ensureOpen();
    this.currentEncoding = toFileEncoding(value, "Encoding");
  }

  // ---- reads --------------------------------------------------------------

  /**
   * Reads up to `chars` UTF-16 code units (all remaining when omitted) and
   * advances the position by exactly the bytes those units consumed.
   */
  async Read(chars?: number): Promise<string> {
    const units = chars === undefined ? Number.MAX_SAFE_INTEGER : requireNonNegative(chars, "Read(chars?): chars");
    return this.enqueue(async () => {
      await this.ready();
      const stat = await this.bridge.fileStat(this.handleId, this.options);
      const remaining = stat.length - stat.pos;
      if (remaining <= 0 || units === 0) return "";
      if (chars === undefined) {
        const bytes = await this.readUpTo(remaining);
        return decodePrefix(bytes, this.currentEncoding, Number.MAX_SAFE_INTEGER).text;
      }
      const probe = Math.min(remaining, bytesPerUnit(this.currentEncoding) * units + 4);
      const chunk = await this.readUpTo(probe);
      const decoded = decodePrefix(chunk, this.currentEncoding, units);
      const unread = chunk.length - decoded.consumed;
      if (unread > 0) await this.bridge.fileSeek(this.handleId, -unread, 1, this.options);
      return decoded.text;
    });
  }

  /**
   * Reads one line: everything up to `CR`, `LF` or `CRLF`, none of the
   * terminator. Returns `""` at end of file and stops after 65535 code units,
   * which is the native stream's line limit.
   */
  async ReadLine(): Promise<string> {
    return this.enqueue(async () => {
      await this.ready();
      let text = "";
      const offsets: number[] = []; // byte starts, relative to the line's start position
      let consumed = 0;
      let term = -1;
      let termUnits = 0;
      let truncated = false;
      while (term < 0) {
        const chunk = await this.readUpTo(65536);
        if (chunk.length === 0) break;
        const decoded = decodePrefix(chunk, this.currentEncoding, Number.MAX_SAFE_INTEGER);
        const unread = chunk.length - decoded.consumed;
        if (unread > 0) {
          await this.bridge.fileSeek(this.handleId, -unread, 1, this.options);
        }
        // A partial trailing code unit never decodes (truncated or corrupt
        // tail): stop instead of re-reading the same bytes forever.
        if (decoded.consumed === 0) break;
        const from = Math.max(0, text.length - 1);
        text += decoded.text;
        for (const offset of decoded.offsets.slice(0, decoded.text.length)) {
          offsets.push(offset + consumed);
        }
        consumed += decoded.consumed;
        term = findTerminator(text, from);
        if (term >= 0) {
          termUnits = text.charCodeAt(term) === 0x0d && text.charAt(term + 1) === "\n" ? 2 : 1;
        } else if (text.length > MAX_LINE_UNITS) {
          text = text.slice(0, MAX_LINE_UNITS);
          truncated = true;
          break;
        }
      }
      offsets.push(consumed);
      if (term < 0 && !truncated && text.length > 0 && text.charCodeAt(text.length - 1) === 0x0d) {
        // A lone CR at end of file still terminates its line.
        term = text.length - 1;
        termUnits = 1;
      }
      if (term < 0) {
        const target = truncated ? MAX_LINE_UNITS : text.length;
        await this.bridge.fileSeek(this.handleId, offsets[target] - consumed, 1, this.options);
        return text;
      }
      const line = text.slice(0, term);
      await this.bridge.fileSeek(
        this.handleId,
        offsets[term + termUnits] - consumed,
        1,
        this.options,
      );
      return line;
    });
  }

  /** Reads up to `bytes` raw bytes (all remaining when omitted); no decoding. */
  async RawRead(bytes?: number): Promise<number[]> {
    const limit = bytes === undefined ? -1 : requireNonNegative(bytes, "RawRead(bytes?): bytes");
    return this.enqueue(async () => {
      await this.ready();
      if (limit >= 0) return this.readUpTo(limit);
      const stat = await this.bridge.fileStat(this.handleId, this.options);
      return this.readUpTo(Math.max(0, stat.length - stat.pos));
    });
  }

  private async fixedRead(size: number, kind: FixedKind): Promise<number> {
    return this.enqueue(async () => {
      await this.ready();
      const bytes = await this.readUpTo(size);
      if (bytes.length < size) {
        throw new ActionError(
          "execution_failed",
          `unexpected end of file: needed ${size} bytes, got ${bytes.length}`,
        );
      }
      return readFixed(bytes, kind);
    });
  }

  /** Reads one unsigned byte. */
  ReadUChar(): Promise<number> {
    return this.fixedRead(1, "uint8");
  }

  /** Reads one signed byte. */
  ReadChar(): Promise<number> {
    return this.fixedRead(1, "int8");
  }

  /** Reads a little-endian signed 16-bit integer. */
  ReadShort(): Promise<number> {
    return this.fixedRead(2, "int16");
  }

  /** Reads a little-endian unsigned 16-bit integer. */
  ReadUShort(): Promise<number> {
    return this.fixedRead(2, "uint16");
  }

  /** Reads a little-endian signed 32-bit integer. */
  ReadInt(): Promise<number> {
    return this.fixedRead(4, "int32");
  }

  /** Reads a little-endian unsigned 32-bit integer. */
  ReadUInt(): Promise<number> {
    return this.fixedRead(4, "uint32");
  }

  /**
   * Reads a little-endian signed 64-bit integer. Values outside the double
   * range lose precision - JavaScript has no 64-bit integer number type.
   */
  ReadInt64(): Promise<number> {
    return this.fixedRead(8, "int64");
  }

  /** Reads a little-endian IEEE-754 single. */
  ReadFloat(): Promise<number> {
    return this.fixedRead(4, "float32");
  }

  /** Reads a little-endian IEEE-754 double. */
  ReadDouble(): Promise<number> {
    return this.fixedRead(8, "float64");
  }

  // ---- writes -------------------------------------------------------------

  /**
   * Writes text in this file's encoding and returns the number of bytes
   * written (the BOM of a still-empty file counts).
   */
  async Write(value: string | number): Promise<number> {
    const text = textArgument(value, "Write(value)");
    return this.enqueue(async () => {
      await this.ready();
      return this.putText(encodeText(text, this.currentEncoding));
    });
  }

  /** Writes text plus a `\n` and returns the number of bytes written. */
  async WriteLine(value?: string | number): Promise<number> {
    const text =
      value === undefined
        ? "\n"
        : textArgument(value, "WriteLine(value?)") + "\n";
    return this.enqueue(async () => {
      await this.ready();
      return this.putText(encodeText(text, this.currentEncoding));
    });
  }

  /** Writes raw bytes (a string is UTF-8 encoded first); no encoding, no BOM. */
  async RawWrite(data: string | number[], bytes?: number): Promise<number> {
    const source = coerceBytes(data, "RawWrite(data, bytes?)");
    const take = bytes === undefined ? source.length : requireNonNegative(bytes, "RawWrite(data, bytes?): bytes");
    const payload = source.slice(0, Math.min(take, source.length));
    return this.enqueue(async () => {
      await this.ready();
      if (payload.length === 0) return 0;
      await this.putBytes(payload);
      return payload.length;
    });
  }

  private async fixedWrite(value: number, kind: FixedKind): Promise<number> {
    const bytes = writeFixed(value, kind);
    return this.enqueue(async () => {
      await this.ready();
      await this.putBytes(bytes);
      return bytes.length;
    });
  }

  WriteChar(value: number): Promise<number> {
    requireInteger(value, "WriteChar(value)", -128, 127);
    return this.fixedWrite(value, "int8");
  }

  WriteUChar(value: number): Promise<number> {
    requireInteger(value, "WriteUChar(value)", 0, 255);
    return this.fixedWrite(value, "uint8");
  }

  WriteShort(value: number): Promise<number> {
    requireInteger(value, "WriteShort(value)", -32768, 32767);
    return this.fixedWrite(value, "int16");
  }

  WriteUShort(value: number): Promise<number> {
    requireInteger(value, "WriteUShort(value)", 0, 65535);
    return this.fixedWrite(value, "uint16");
  }

  WriteInt(value: number): Promise<number> {
    requireInteger(value, "WriteInt(value)", -2147483648, 2147483647);
    return this.fixedWrite(value, "int32");
  }

  WriteUInt(value: number): Promise<number> {
    requireInteger(value, "WriteUInt(value)", 0, 4294967295);
    return this.fixedWrite(value, "uint32");
  }

  WriteInt64(value: number): Promise<number> {
    requireInteger(value, "WriteInt64(value)", -MAX_SAFE, MAX_SAFE);
    return this.fixedWrite(value, "int64");
  }

  WriteFloat(value: number): Promise<number> {
    if (typeof value !== "number" || !Number.isFinite(value)) {
      throw new TypeError("WriteFloat(value): value must be a finite number");
    }
    return this.fixedWrite(value, "float32");
  }

  WriteDouble(value: number): Promise<number> {
    if (typeof value !== "number" || !Number.isFinite(value)) {
      throw new TypeError("WriteDouble(value): value must be a finite number");
    }
    return this.fixedWrite(value, "float64");
  }

  // ---- positioning --------------------------------------------------------

  /**
   * Moves the position. Returns `false` when the move would start before the
   * beginning of the file; every other failure (a closed handle, a denied
   * capability) still rejects. `origin` defaults to `SEEK_SET` for a
   * non-negative distance and `SEEK_END` for a negative one.
   */
  async Seek(distance: number, origin?: number): Promise<boolean> {
    if (typeof distance !== "number" || !Number.isInteger(distance)) {
      throw new TypeError("Seek(distance, origin?): distance must be an integer");
    }
    const whence = origin === undefined ? (distance < 0 ? 2 : 0) : origin;
    if (whence !== 0 && whence !== 1 && whence !== 2) {
      throw new TypeError("Seek(distance, origin?): origin must be 0, 1 or 2");
    }
    const mode = whence as 0 | 1 | 2;
    return this.enqueue(async () => {
      await this.ready();
      try {
        await this.bridge.fileSeek(this.handleId, distance, mode, this.options);
        return true;
      } catch (error) {
        // The bridge rejects with a coded plain object (the native module
        // throws `{code, message}`); `runAction` is what upgrades those to
        // ActionError, and File talks to the bridge directly. Match both.
        const coded =
          typeof error === "object" && error !== null
            ? (error as { code?: unknown; message?: unknown })
            : null;
        if (
          coded !== null &&
          coded.code === "execution_failed" &&
          typeof coded.message === "string" &&
          coded.message.includes("file seek moved before the start of the file")
        ) {
          return false;
        }
        throw error;
      }
    });
  }

  /** Closes the handle. Idempotent: closing twice resolves both times. */
  async Close(): Promise<void> {
    return this.enqueue(async () => {
      if (this.isClosed) return;
      this.ensureOpen();
      await this.bridge.fileClose(this.handleId, this.options);
      this.isClosed = true;
    });
  }
}

/** Returns the index of the first `CR`/`LF` that is not itself the last character read. */
function findTerminator(text: string, from: number): number {
  for (let index = from; index < text.length; index++) {
    const unit = text.charCodeAt(index);
    if (unit !== 0x0a && unit !== 0x0d) continue;
    if (unit === 0x0d && index + 1 === text.length) return -1; // need the next chunk
    return index;
  }
  return -1;
}

async function storageBridge(): Promise<StorageBridge> {
  const module = await import("rime:storage");
  return module.storage;
}

const DEFAULT_FILE_FILTER = "All Files (*.*)";

export const storage = {
  /**
   * Reads a whole file as text using the session encoding.
   * @throws ActionError with `capability_denied` / `execution_failed`.
   */
  read(path: string, options?: ActionOptions): Promise<string> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.readText(path, native).then((r) => r.text)),
    );
  },

  /** Reads a whole file as raw bytes (1 GiB cap). */
  readBytes(path: string, options?: ActionOptions): Promise<number[]> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.readBytes(path, native).then((r) => r.bytes)),
    );
  },

  /** Size, last write time, attribute letters and the directory flag. */
  stat(path: string, options?: ActionOptions): Promise<FileInfo> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.stat(path, native)),
    );
  },

  /**
   * Reads a .lnk shortcut (target, working dir, args, icon).
   * @throws ActionError with `capability_denied` / `execution_failed`.
   */
  shortcut(
    path: string,
    options?: ActionOptions,
  ): Promise<{ target: string; workingDir: string; args: string; icon: string }> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.shortcut(path, native)),
    );
  },

  /**
   * Reads the file version resource ("M.m.b.r", "" when absent).
   * @throws ActionError with `capability_denied` / `execution_failed`.
   */
  version(path: string, options?: ActionOptions): Promise<string> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.version(path, native).then((r) => r.version)),
    );
  },

  /** Immediate children of a directory; `.` and `..` never appear. */
  list(path: string, options?: ActionOptions): Promise<DirEntry[]> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.list(path, native).then((r) => r.entries)),
    );
  },

  /** Reads an environment variable; an unset name reads back `""`. */
  envGet(name: string, options?: ActionOptions): Promise<string> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.envGet(name, native).then((r) => r.value)),
    );
  },

  /** Reads one INI value. A missing file, section or key is an error, never `""`. */
  iniRead(path: string, section: string, key: string, options?: ActionOptions): Promise<string> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.iniRead(path, section, key, native).then((r) => r.value)),
    );
  },

  /**
   * Queries one drive field: `type|list|serial|spacefree|status|statuscd|
   * filesystem|label|capacity`. `letter` is the drive for every field except
   * `list`, where it is the type filter.
   */
  driveGet(field: string, letter?: string, options?: ActionOptions): Promise<DriveInfo> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.driveGet(field, letter, native)),
    );
  },

  /** Opens a handle and returns its id plus the file length. Use openFile for a File. */
  open(path: string, mode: FileMode, options?: ActionOptions): Promise<OpenedFile> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.open(path, mode, native)),
    );
  },

  /**
   * Opens a file and returns a {@link File} over it.
   * @throws TypeError when `mode` is not `"r"`/`"a"`/`"w"` (thrown by the
   *   module) or when the resolved encoding is `cp0`.
   */
  async openFile(path: string, mode: FileMode, options?: FileOpenOptions): Promise<File> {
    return runAction(options, async (native) => {
      const bridge = await storageBridge();
      // The encoding is resolved before the OS open: throwing afterwards would
      // strand a native handle that no File ever gets to close.
      const encoding =
        options?.encoding !== undefined
          ? toFileEncoding(options.encoding, "options.encoding")
          : toFileEncoding(bridge.encoding().encoding, "session encoding");
      const opened = await bridge.open(path, mode, native);
      return new File(bridge, opened.handle, mode, encoding, native);
    });
  },

  /** Raw handle read: up to `count` bytes at the current position. */
  fileRead(handle: number, count: number, options?: ActionOptions): Promise<{ bytes: number[]; eof: boolean }> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.fileRead(handle, count, native)),
    );
  },

  /** Raw handle seek: `whence` is 0 (begin), 1 (current) or 2 (end). */
  fileSeek(
    handle: number,
    offset: number,
    whence: 0 | 1 | 2,
    options?: ActionOptions,
  ): Promise<{ pos: number }> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.fileSeek(handle, offset, whence, native)),
    );
  },

  /** Raw handle position and length. */
  fileStat(handle: number, options?: ActionOptions): Promise<{ pos: number; length: number }> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.fileStat(handle, native)),
    );
  },

  /** Closes a raw handle. A stale id rejects with `invalid_state`. */
  fileClose(handle: number, options?: ActionOptions): Promise<Record<string, never>> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.fileClose(handle, native)),
    );
  },

  /** Runs one `storage.write` action through the traced, capability-checked queue. */
  write(payload: StorageWritePayload, options?: ActionOptions): Promise<StorageWriteResult> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.write(payload, native)),
    );
  },

  /**
   * Session encoding.
   * @throws TypeError synchronously when the name is not one of the seven
   *   session encodings (the native re-check still applies).
   */
  encoding(): Promise<SessionEncoding> {
    return storageBridge().then((bridge) => bridge.encoding().encoding);
  },

  /** Sets the session encoding; an unknown name throws synchronously. */
  setEncoding(encoding: SessionEncoding): Promise<SessionEncoding> {
    normalizeSessionEncoding(encoding, "encoding");
    return storageBridge().then((bridge) => bridge.setEncoding(encoding).encoding);
  },

  /** Downloads `url` to `path`; only `http`/`https`, partial files are removed. */
  download(url: string, path: string, options?: ActionOptions): Promise<number> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => bridge.download(url, path, native).then((r) => r.bytes)),
    );
  },

  /**
   * Opens the native file picker. Rejects with `invalid_state` on a headless
   * runtime and `cancelled` when the user backs out.
   * @remarks `filter` defaults to `"All Files (*.*)"`, because the native
   *   service rejects an empty filter.
   */
  selectFile(options?: StorageSelectFileOptions): Promise<string[]> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => {
        const payload: StorageSelectFileOptions = { ...(native ?? {}) };
        payload.filter = options?.filter ?? DEFAULT_FILE_FILTER;
        if (options?.defaultName !== undefined) payload.defaultName = options.defaultName;
        if (options?.multi !== undefined) payload.multi = options.multi;
        return bridge.selectFile(payload).then((r) => r.paths);
      }),
    );
  },

  /** Opens the native folder picker. Same rejection codes as {@link selectFile}. */
  selectDir(options?: StorageSelectDirOptions): Promise<string> {
    return runAction(options, (native) =>
      storageBridge().then((bridge) => {
        const payload: StorageSelectDirOptions = { ...(native ?? {}) };
        if (options?.caption !== undefined) payload.caption = options.caption;
        return bridge.selectDir(payload).then((r) => r.path);
      }),
    );
  },
};
