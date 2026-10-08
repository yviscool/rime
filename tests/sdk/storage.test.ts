import { beforeEach, expect, mock, test } from "bun:test";
import { ActionError, type NativeActionOptions } from "../../sdk/src/action";
import {
  File,
  storage,
  type DirEntry,
  type DriveInfo,
  type FileInfo,
  type StorageBridge,
  type StorageSelectFileOptions,
  type StorageWritePayload,
} from "../../sdk/src/storage";

// Realism: L3 - the real sdk/src/storage.ts facade and the real File class
// run against an instrumented rime:storage bridge (only the native module is
// replaced). The bridge below is an in-memory byte store: it is the
// environment, never the unit under test. Everything asserted here is the
// facade's own behaviour - which arguments it forwarded, which result it
// unwrapped, how a coded rejection became an ActionError, and the byte-level
// layout the File codecs produce. Encoding expectations are written out as
// literal byte arrays rather than derived from the codec, so a bug in the
// codec cannot make its own expectation pass.

type CodedRejection = { code: string; message: string };

const codedError = ({ code, message }: CodedRejection) =>
  Object.assign(new Error(message), { code });

// ---- bridge state ----------------------------------------------------------

let sessionEncoding = "utf-8";
let textFiles = new Map<string, string>();
let binFiles = new Map<string, number[]>();
let handles = new Map<number, { data: number[]; pos: number }>();
let nextHandle = 1;

let readCalls: Array<[string, NativeActionOptions | undefined]> = [];
let statCalls: Array<[string, NativeActionOptions | undefined]> = [];
let openCalls: Array<[string, string, NativeActionOptions | undefined]> = [];
let driveCalls: Array<[string, string | undefined, NativeActionOptions | undefined]> = [];
let writeCalls: Array<[StorageWritePayload, NativeActionOptions | undefined]> = [];
let selectFileCalls: StorageSelectFileOptions[] = [];
let selectDirCalls: unknown[] = [];
let setEncodingCalls: string[] = [];

let readRejection: CodedRejection | null = null;
let writeRejection: CodedRejection | null = null;
let openRejection: CodedRejection | null = null;

const failIfSet = (rejection: CodedRejection | null): void => {
  if (rejection) throw codedError(rejection);
};

function reset(): void {
  sessionEncoding = "utf-8";
  textFiles = new Map();
  binFiles = new Map();
  handles = new Map();
  nextHandle = 1;
  readCalls = [];
  statCalls = [];
  openCalls = [];
  driveCalls = [];
  writeCalls = [];
  selectFileCalls = [];
  selectDirCalls = [];
  setEncodingCalls = [];
  readRejection = null;
  writeRejection = null;
  openRejection = null;
}

function seed(path: string, bytes: number[]): void {
  binFiles.set(path, bytes.slice());
}

function stored(path: string): number[] {
  return binFiles.get(path) ?? [];
}

const bridge: StorageBridge = {
  async readText(path, options) {
    readCalls.push([path, options]);
    failIfSet(readRejection);
    return { text: textFiles.get(path) ?? "" };
  },
  async readBytes(path, options) {
    readCalls.push([path, options]);
    failIfSet(readRejection);
    return { bytes: stored(path).slice() };
  },
  async stat(path, options): Promise<FileInfo> {
    statCalls.push([path, options]);
    failIfSet(readRejection);
    const data = binFiles.get(path);
    if (!data) throw codedError({ code: "execution_failed", message: `file not found: ${path}` });
    return { size: data.length, mtimeMs: 1700000000000, attrib: "A", isDir: false };
  },
  async list(path, options): Promise<{ entries: DirEntry[] }> {
    readCalls.push([path, options]);
    failIfSet(readRejection);
    return { entries: [{ name: "child.txt", isDir: false }] };
  },
  async envGet(name, options) {
    readCalls.push([name, options]);
    failIfSet(readRejection);
    return { value: name === "RIME_TEST_SET" ? "yes" : "" };
  },
  async iniRead(path, section, key, options) {
    readCalls.push([path, options]);
    failIfSet(readRejection);
    return { value: `${section}/${key}` };
  },
  async shortcut(path, options) {
    readCalls.push([path, options]);
    failIfSet(readRejection);
    return {
      target: "C:\\Windows\\notepad.exe",
      workingDir: "C:\\Windows",
      args: "--rime",
      icon: "shell32.dll,0",
    };
  },
  async version(path, options) {
    readCalls.push([path, options]);
    failIfSet(readRejection);
    return { version: "10.0.26100.1" };
  },
  async driveGet(field, letter, options): Promise<DriveInfo> {
    driveCalls.push([field, letter, options]);
    failIfSet(readRejection);
    return {
      letter: letter ? `${letter}:` : "",
      filesystem: "NTFS",
      label: "System",
      type: "Fixed",
      status: "Ready",
      totalBytes: 1000,
      freeBytes: 400,
      serial: 42,
      list: letter ? [] : ["C"],
      capacityPercent: 60,
    };
  },
  async open(path, mode, options) {
    openCalls.push([path, mode, options]);
    failIfSet(openRejection);
    if (mode === "r" && !binFiles.has(path)) {
      throw codedError({ code: "execution_failed", message: `file not found: ${path}` });
    }
    if (mode === "w") binFiles.set(path, []);
    if (!binFiles.has(path)) binFiles.set(path, []);
    const data = binFiles.get(path) as number[];
    const handle = nextHandle++;
    handles.set(handle, { data, pos: mode === "a" ? data.length : 0 });
    return { handle, length: data.length };
  },
  async fileRead(handle, count, options) {
    const file = handles.get(handle);
    if (!file) throw codedError({ code: "invalid_state", message: "file handle is not open" });
    const remaining = Math.max(0, file.data.length - file.pos);
    const take = Math.min(count, remaining);
    const bytes = file.data.slice(file.pos, file.pos + take);
    file.pos += take;
    return { bytes, eof: take < count };
  },
  async fileSeek(handle, offset, whence, options) {
    const file = handles.get(handle);
    if (!file) throw codedError({ code: "invalid_state", message: "file handle is not open" });
    const target =
      whence === 0 ? offset : whence === 1 ? file.pos + offset : file.data.length + offset;
    if (target < 0) {
      throw codedError({
        code: "execution_failed",
        message: "file seek moved before the start of the file",
      });
    }
    file.pos = target;
    return { pos: file.pos };
  },
  async fileStat(handle, options) {
    const file = handles.get(handle);
    if (!file) throw codedError({ code: "invalid_state", message: "file handle is not open" });
    return { pos: file.pos, length: file.data.length };
  },
  async fileClose(handle, options) {
    if (!handles.has(handle)) {
      throw codedError({ code: "invalid_state", message: "file handle is not open" });
    }
    handles.delete(handle);
    return {};
  },
  async write(payload, options) {
    writeCalls.push([payload, options]);
    failIfSet(writeRejection);
    if (payload.op === "handleWrite") {
      const file = handles.get(payload.handle);
      if (!file) throw codedError({ code: "invalid_state", message: "file handle is not open" });
      const bytes =
        typeof payload.data === "string"
          ? Array.from(payload.data, (ch) => ch.charCodeAt(0) & 0xff)
          : payload.data;
      while (file.data.length < file.pos) file.data.push(0);
      for (const byte of bytes) {
        if (file.pos < file.data.length) file.data[file.pos] = byte;
        else file.data.push(byte);
        file.pos++;
      }
    }
    return { op: payload.op };
  },
  encoding() {
    return { encoding: sessionEncoding as "utf-8" };
  },
  setEncoding(encoding) {
    setEncodingCalls.push(encoding);
    sessionEncoding = encoding.toLowerCase();
    return { encoding: sessionEncoding as "utf-8" };
  },
  async download(url, path, options) {
    readCalls.push([url, options]);
    failIfSet(readRejection);
    return { bytes: 1234 };
  },
  async selectFile(options) {
    selectFileCalls.push(options ?? {});
    failIfSet(readRejection);
    return { paths: ["C:\\picked\\file.txt"] };
  },
  async selectDir(options) {
    selectDirCalls.push(options ?? {});
    failIfSet(readRejection);
    return { path: "C:\\picked\\dir" };
  },
};

mock.module("rime:storage", () => ({ storage: bridge }));

const { storage: facade } = await import("../../sdk/src/storage");

beforeEach(reset);

// ---- facade ----------------------------------------------------------------

test("the facade unwraps every wire result into the documented shape", async () => {
  textFiles.set("C:/data.txt", "hello");
  seed("C:/data.txt", [0x68, 0x69]);

  expect(await facade.readFile("C:/data.txt", "utf-8")).toBe("hello");
  expect(await facade.readFile("C:/data.txt")).toStrictEqual(new Uint8Array([0x68, 0x69]));
  expect(await facade.stat("C:/data.txt")).toEqual({
    size: 2,
    mtimeMs: 1700000000000,
    attrib: "A",
    isDir: false,
  });
  expect(await facade.readdir("C:/")).toEqual([{ name: "child.txt", isDir: false }]);
  expect(await facade.envGet("RIME_TEST_SET")).toBe("yes");
  expect(await facade.envGet("RIME_TEST_UNSET")).toBe("");
  expect(await facade.iniRead("C:/a.ini", "sec", "key")).toBe("sec/key");
  expect(await facade.driveGet("capacity", "C")).toMatchObject({
    filesystem: "NTFS",
    capacityPercent: 60,
  });
  expect(await facade.download("https://example.test/a", "C:/a.bin")).toBe(1234);
  expect(await facade.selectFile()).toEqual(["C:\\picked\\file.txt"]);
  expect(await facade.selectDir()).toBe("C:\\picked\\dir");
  expect(await facade.shortcut("C:\\links\\target.lnk")).toEqual({
    target: "C:\\Windows\\notepad.exe",
    workingDir: "C:\\Windows",
    args: "--rime",
    icon: "shell32.dll,0",
  });
  // The wire carries {version}; the facade unwraps it like read/envGet/download.
  expect(await facade.version("C:/Windows/notepad.exe")).toBe("10.0.26100.1");

  expect(readCalls.some(([path]) => path === "C:/data.txt")).toBe(true);
  expect(readCalls.some(([path]) => path === "C:\\links\\target.lnk")).toBe(true);
  expect(driveCalls.at(-1)).toEqual(["capacity", "C", undefined]);
});

test("shortcut and version upgrade a coded rejection to ActionError", async () => {
  readRejection = { code: "target_gone", message: "file not found: C:/gone.lnk" };
  const shortcutError = await facade.shortcut("C:/gone.lnk").catch((e: unknown) => e);
  expect(shortcutError).toBeInstanceOf(ActionError);
  expect((shortcutError as ActionError).code).toBe("target_gone");

  readRejection = { code: "execution_failed", message: "no version resource" };
  const versionError = await facade.version("C:/gone.dll").catch((e: unknown) => e);
  expect(versionError).toBeInstanceOf(ActionError);
  expect((versionError as ActionError).code).toBe("execution_failed");
  expect((versionError as ActionError).message).toBe("no version resource");
});

test("a coded rejection surfaces as ActionError carrying the same code", async () => {
  readRejection = { code: "target_gone", message: "file not found: C:/gone.txt" };
  const error = await facade.readFile("C:/gone.txt", "utf-8").catch((e: unknown) => e);
  expect(error).toBeInstanceOf(ActionError);
  expect((error as ActionError).code).toBe("target_gone");
  expect((error as ActionError).message).toBe("file not found: C:/gone.txt");
});

test("write forwards the payload unchanged and resolves {op}", async () => {
  const payload: StorageWritePayload = { op: "mkdir", path: "C:/new" };
  const result = await facade.write(payload, { deadlineMs: 50 });
  expect(result).toEqual({ op: "mkdir" });
  expect(writeCalls).toEqual([[payload, { deadlineMs: 50 }]]);
});

test("write rejects a capability refusal as ActionError with its code", async () => {
  writeRejection = {
    code: "capability_denied",
    message: "required capability was not granted: filesystem.write",
  };
  const error = await facade.write({ op: "delete", path: "C:/x" }).catch((e: unknown) => e);
  expect(error).toBeInstanceOf(ActionError);
  expect((error as ActionError).code).toBe("capability_denied");
  expect((error as ActionError).message).toContain("filesystem.write");
});

test("an already-aborted signal short-circuits before the bridge", async () => {
  const reason = new Error("signal already aborted");
  const signal = { aborted: true, reason };
  await expect(
    facade.write({ op: "delete", path: "C:/x" }, { signal }),
  ).rejects.toThrow("signal already aborted");
  await expect(facade.readFile("C:/data.txt", { signal })).rejects.toThrow("signal already aborted");
  expect(writeCalls).toHaveLength(0);
  expect(readCalls).toHaveLength(0);
});

test("selectFile defaults the filter the native service refuses to be without", async () => {
  await facade.selectFile();
  expect(selectFileCalls.at(-1)?.filter).toBe("All Files (*.*)");

  await facade.selectFile({ filter: "*.txt", multi: true, defaultName: "a.txt" });
  expect(selectFileCalls.at(-1)).toMatchObject({
    filter: "*.txt",
    multi: true,
    defaultName: "a.txt",
  });
});

test("encoding reads the session and setEncoding validates before it awaits", async () => {
  expect(await facade.encoding()).toBe("utf-8");
  expect(await facade.setEncoding("latin1")).toBe("latin1");
  expect(setEncodingCalls).toEqual(["latin1"]);
  // The name check runs synchronously, so it throws instead of rejecting.
  expect(() => facade.setEncoding("bogus" as never)).toThrow(TypeError);
  expect(setEncodingCalls).toEqual(["latin1"]);
});

test("openFile rejects an ANSI session encoding instead of silently guessing", async () => {
  sessionEncoding = "cp0";
  const error = await facade.openFile("C:/cp.txt", "r").catch((e: unknown) => e);
  expect(error).toBeInstanceOf(TypeError);
  expect((error as Error).message).toContain("cp0 is unsupported");
  // The encoding is resolved first, so the refusal never opens a handle that
  // nothing would close.
  expect(openCalls).toHaveLength(0);
  sessionEncoding = "utf-8";
});

// ---- File shape ------------------------------------------------------------

const FILE_MEMBERS = [
  "atEof",
  "close",
  "encoding",
  "handle",
  "length",
  "pos",
  "readBytes",
  "readFloat32",
  "readFloat64",
  "readInt16",
  "readInt32",
  "readInt64",
  "readInt8",
  "readLine",
  "readUInt16",
  "readUInt32",
  "readUInt8",
  "read",
  "seek",
  "writeBytes",
  "writeFloat32",
  "writeFloat64",
  "writeInt16",
  "writeInt32",
  "writeInt64",
  "writeInt8",
  "writeLine",
  "writeUInt16",
  "writeUInt32",
  "writeUInt8",
  "write",
];

test("File exposes exactly the 31 documented members and nothing else", () => {
  // The full prototype surface is listed, internals included, so adding or
  // renaming any member has to be a deliberate edit of this expectation.
  const internals = [
    "putBytes",
    "putText",
    "enqueue",
    "ensureOpen",
    "fixedRead",
    "fixedWrite",
    "readUpTo",
    "ready",
  ];
  const surface = Object.getOwnPropertyNames(File.prototype)
    .filter((name) => name !== "constructor")
    .sort();
  expect(surface).toEqual([...FILE_MEMBERS, ...internals].sort());
  expect(FILE_MEMBERS).toHaveLength(31);
});

// ---- File behaviour --------------------------------------------------------

test("Read decodes literal byte sequences and advances only past what it consumed", async () => {
  seed("C:/abc.txt", [0x41, 0x42, 0x43]);
  const file = await facade.openFile("C:/abc.txt", "r");
  expect(await file.read()).toBe("ABC");
  expect(await file.atEof).toBe(true);

  seed("C:/abc2.txt", [0x41, 0x42, 0x43, 0x44]);
  const two = await facade.openFile("C:/abc2.txt", "r");
  expect(await two.read(2)).toBe("AB");
  expect(await two.read()).toBe("CD");

  // U+4E2D in UTF-8 is three bytes; one code unit must consume all three.
  seed("C:/cjk.txt", [0xe4, 0xb8, 0xad, 0x41]);
  const cjk = await facade.openFile("C:/cjk.txt", "r");
  expect(await cjk.read(1)).toBe("\u4e2d");
  expect(await cjk.read()).toBe("A");
});

test("a BOM is detected on open and never leaks into the decoded text", async () => {
  seed("C:/bom.txt", [0xef, 0xbb, 0xbf, 0x48, 0x69]);
  const file = await facade.openFile("C:/bom.txt", "r");
  expect(await file.read()).toBe("Hi");
  // The sniff runs lazily on the first queued operation, so Encoding is only
  // the detected name once something has been read.
  expect(file.encoding).toBe("utf-8-bom");
  expect(await file.pos).toBe(5);
});

test("Write emits the encoding's own bytes, with a BOM only into an empty file", async () => {
  seed("C:/w.txt", []);
  const utf8 = await facade.openFile("C:/w.txt", "w");
  expect(await utf8.write("Hi")).toBe(2);
  expect(stored("C:/w.txt")).toEqual([0x48, 0x69]);

  seed("C:/bomw.txt", []);
  const bom = await facade.openFile("C:/bomw.txt", "w", { encoding: "utf-8-bom" });
  expect(await bom.write("A")).toBe(4);
  expect(stored("C:/bomw.txt")).toEqual([0xef, 0xbb, 0xbf, 0x41]);
  expect(await bom.write("B")).toBe(1);
  expect(stored("C:/bomw.txt")).toEqual([0xef, 0xbb, 0xbf, 0x41, 0x42]);

  seed("C:/cp.txt", []);
  const cp = await facade.openFile("C:/cp.txt", "w", { encoding: "cp1252" });
  await cp.write("\u20ac");
  expect(stored("C:/cp.txt")).toEqual([0x80]);

  // U+0041 in UTF-16LE is the two bytes 41 00; a BOM-less file still decodes
  // once the file itself says which encoding to use.
  seed("C:/u16.txt", [0x41, 0x00]);
  const le = await facade.openFile("C:/u16.txt", "r", { encoding: "utf-16" });
  expect(await le.read()).toBe("A");
  expect(await le.atEof).toBe(true);
});

test("ReadLine consumes one terminator and returns the line without it", async () => {
  seed("C:/lines.txt", Array.from("one\r\ntwo\nthree", (ch) => ch.charCodeAt(0)));
  const file = await facade.openFile("C:/lines.txt", "r");
  expect(await file.readLine()).toBe("one");
  expect(await file.readLine()).toBe("two");
  expect(await file.readLine()).toBe("three");
  expect(await file.readLine()).toBe("");
  expect(await file.atEof).toBe(true);
});

test("fixed-width reads and writes use little-endian layout", async () => {
  seed("C:/ints.bin", [
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff,
  ]);
  const file = await facade.openFile("C:/ints.bin", "r");
  expect(await file.readInt32()).toBe(1);
  expect(await file.readUInt32()).toBe(1);
  expect(await file.readInt32()).toBe(-1);
  expect(await file.readUInt32()).toBe(4294967295);
  expect(await file.atEof).toBe(true);

  seed("C:/small.bin", [0x41, 0xff, 0xff, 0xff, 0xff, 0xff]);
  const small = await facade.openFile("C:/small.bin", "r");
  expect(await small.readUInt8()).toBe(0x41);
  expect(await small.readInt8()).toBe(-1);
  expect(await small.readUInt16()).toBe(0xffff);
  expect(await small.readInt16()).toBe(-1);
  expect(await small.atEof).toBe(true);

  // 1.5 is 0x3FF8000000000000 as a double and 0x3FC00000 as a float.
  seed("C:/double.bin", [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f]);
  const doubles = await facade.openFile("C:/double.bin", "r");
  expect(await doubles.readFloat64()).toBe(1.5);

  seed("C:/float.bin", [0x00, 0x00, 0xc0, 0x3f]);
  const floats = await facade.openFile("C:/float.bin", "r");
  expect(await floats.readFloat32()).toBeCloseTo(1.5, 6);

  seed("C:/i64.bin", [0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]);
  const big = await facade.openFile("C:/i64.bin", "r");
  expect(await big.readInt64()).toBe(1);
});

test("fixed-width writes land as the literal byte layout", async () => {
  seed("C:/out.bin", []);
  const file = await facade.openFile("C:/out.bin", "w");
  expect(await file.writeInt32(-1)).toBe(4);
  expect(await file.writeUInt16(0x1234)).toBe(2);
  expect(await file.writeFloat64(1.5)).toBe(8);
  expect(stored("C:/out.bin")).toEqual([
    0xff, 0xff, 0xff, 0xff, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f,
  ]);
});

test("a short fixed read rejects with the end-of-file message", async () => {
  seed("C:/short.bin", [0x01, 0x02]);
  const file = await facade.openFile("C:/short.bin", "r");
  const error = await file.readInt32().catch((e: unknown) => e);
  expect(error).toBeInstanceOf(ActionError);
  expect((error as ActionError).code).toBe("execution_failed");
  expect((error as ActionError).message).toBe("unexpected end of file: needed 4 bytes, got 2");
});

test("RawRead and RawWrite move bytes without any encoding", async () => {
  seed("C:/raw.bin", [0x01, 0x02, 0x03]);
  const file = await facade.openFile("C:/raw.bin", "a");
  expect(await file.readBytes()).toEqual([]);
  expect(await file.writeBytes([0x09, 0x08])).toBe(2);
  expect(await file.writeBytes("A")).toBe(1);
  expect(stored("C:/raw.bin")).toEqual([0x01, 0x02, 0x03, 0x09, 0x08, 0x41]);
  file.pos = 0;
  expect(await file.readBytes()).toEqual([0x01, 0x02, 0x03, 0x09, 0x08, 0x41]);
});

test("Pos reads, writes, and surfaces a failed set on the next read", async () => {
  seed("C:/pos.txt", [0x61, 0x62, 0x63]);
  const file = await facade.openFile("C:/pos.txt", "r");
  expect(await file.pos).toBe(0);
  file.pos = 2;
  expect(await file.pos).toBe(2);
  expect(() => {
    file.pos = -1;
  }).toThrow(TypeError);
  expect(() => {
    file.pos = 1e99;
  }).toThrow(RangeError);
});

test("Seek reports only a move before the start of the file as false", async () => {
  seed("C:/seek.txt", [0x61, 0x62, 0x63]);
  const file = await facade.openFile("C:/seek.txt", "r");
  expect(await file.seek(2, 0)).toBe(true);
  expect(await file.seek(1, 1)).toBe(true); // 2 -> 3
  expect(await file.seek(-10, 2)).toBe(false); // end (3) - 10 is before the file
  expect(await file.seek(-1, 0)).toBe(false);
  expect(await file.seek(-1, 1)).toBe(true); // 3 -> 2
  expect(await file.pos).toBe(2);
  expect(await file.seek(1, 0)).toBe(true);
  expect(await file.pos).toBe(1);
  await expect(file.seek(0, 5)).rejects.toThrow(TypeError);
});

test("Length is readable but refuses a setter", async () => {
  seed("C:/len.txt", [0x61, 0x62, 0x63]);
  const file = await facade.openFile("C:/len.txt", "r");
  expect(await file.length).toBe(3);
  expect(() => {
    file.length = 1;
  }).toThrow(TypeError);
});

test("Encoding is synchronous, rejects an unknown name, and rejects cp0", async () => {
  seed("C:/enc.txt", [0x61]);
  const file = await facade.openFile("C:/enc.txt", "r");
  expect(file.encoding).toBe("utf-8");
  file.encoding = "latin1";
  expect(file.encoding).toBe("latin1");
  expect(() => {
    file.encoding = "bogus";
  }).toThrow(TypeError);
  expect(() => {
    file.encoding = "cp0";
  }).toThrow(TypeError);
  expect(file.encoding).toBe("latin1");
});

test("Handle always refuses to expose the OS handle", async () => {
  seed("C:/h.txt", [0x61]);
  const file = await facade.openFile("C:/h.txt", "r");
  const read = () => file.handle;
  expect(read).toThrow(ActionError);
  expect(read).toThrow("unsupported by policy");
});

test("Close is idempotent and every member then reports the file is closed", async () => {
  seed("C:/c.txt", [0x61]);
  const file = await facade.openFile("C:/c.txt", "r");
  await file.close();
  await file.close();

  const error = await file.read().catch((e: unknown) => e);
  expect(error).toBeInstanceOf(ActionError);
  expect((error as ActionError).code).toBe("invalid_state");
  expect((error as ActionError).message).toBe("file is closed");

  const lineError = await file.readLine().catch((e: unknown) => e);
  expect(lineError).toBeInstanceOf(ActionError);
  expect((lineError as ActionError).message).toBe("file is closed");
  expect(() => file.encoding).toThrow("file is closed");
  expect(() => file.handle).toThrow("file is closed");
  expect(() => {
    file.pos = 0;
  }).toThrow("file is closed");
  expect(() => {
    file.length = 0;
  }).toThrow("file is closed");
});

test("members validate their arguments and leave the file untouched", async () => {
  seed("C:/args.txt", [0x61]);
  const file = await facade.openFile("C:/args.txt", "r");

  await expect(file.read(-1)).rejects.toThrow(TypeError);
  await expect(file.readBytes(1.5)).rejects.toThrow(TypeError);
  await expect(file.write({} as never)).rejects.toThrow(TypeError);
  await expect(file.writeLine([] as never)).rejects.toThrow(TypeError);
  await expect(file.writeBytes([300])).rejects.toThrow(TypeError);
  await expect(file.seek(1.5)).rejects.toThrow(TypeError);
  await expect(file.seek(0, 5)).rejects.toThrow(TypeError);
  expect(() => file.writeInt32(1.5)).toThrow(TypeError);
  expect(() => file.writeUInt16(1.5)).toThrow(TypeError);
  expect(() => file.writeUInt16(70000)).toThrow(RangeError);
  expect(() => file.writeFloat64(Number.NaN)).toThrow(TypeError);

  // None of the refusals enqueued anything, so the file still reads whole.
  expect(await file.read()).toBe("a");
});

// ---- contract types --------------------------------------------------------

test("payload types refuse what the contract does not define", () => {
  const submit = (payload: StorageWritePayload): StorageWritePayload => payload;

  // @ts-expect-error - "rename" is not one of the 24 ops
  submit({ op: "rename", path: "C:/x" });
  // @ts-expect-error - copy takes src/dst, not path
  submit({ op: "copy", path: "C:/x" });
  // @ts-expect-error - setTime requires a known `which`
  submit({ op: "setTime", path: "C:/x", which: "btime", unixMs: 0 });

  expect(submit({ op: "handleWrite", handle: 1, data: [65] })).toEqual({
    op: "handleWrite",
    handle: 1,
    data: [65],
  });
});

test("the bridge interface the module must export is satisfied", () => {
  // Compile-time only: keeps sdk/src/storage.ts's StorageBridge honest against
  // what the runtime module declaration and this mock agree on.
  const probe: StorageBridge = bridge;
  expect(typeof probe.readText).toBe("function");
  expect(typeof probe.encoding).toBe("function");
});

// ---- Node-named facade (delegates to the same ops) -------------------------

test("readFile/readdir delegate to the text read and listing", async () => {
  seed("C:/n.txt", [0x68, 0x69]);
  textFiles.set("C:/n.txt", "hi");
  await expect(facade.readFile("C:/n.txt", "utf-8")).resolves.toBe("hi");
  await expect(facade.readdir("C:/")).resolves.toEqual([{ name: "child.txt", isDir: false }]);
});

test("writeFile text goes through one write op; bytes through open/write/close", async () => {
  await facade.writeFile("C:/w.txt", "hi");
  expect(writeCalls.at(-1)?.[0]).toStrictEqual({ op: "write", path: "C:/w.txt", text: "hi" });

  seed("C:/b.bin", []);
  await facade.writeFile("C:/b.bin", [0x41]);
  expect(stored("C:/b.bin")).toEqual([0x41]);
});

test("appendFile/mkdir/rm/unlink/copyFile/rename map to ops", async () => {
  await facade.appendFile("C:/a.txt", "x");
  await facade.mkdir("C:/d");
  await facade.rm("C:/f.txt");
  await facade.rm("C:/d", { recursive: true });
  await facade.unlink("C:/g.txt");
  await facade.copyFile("C:/a.txt", "C:/b.txt", { overwrite: true });
  await facade.rename("C:/b.txt", "C:/c.txt");
  expect(writeCalls.map(([payload]) => payload)).toStrictEqual([
    { op: "append", path: "C:/a.txt", text: "x" },
    { op: "mkdir", path: "C:/d" },
    { op: "delete", path: "C:/f.txt" },
    { op: "rmdir", path: "C:/d", recursive: true },
    { op: "delete", path: "C:/g.txt" },
    { op: "copy", src: "C:/a.txt", dst: "C:/b.txt", overwrite: true },
    { op: "move", src: "C:/b.txt", dst: "C:/c.txt" },
  ]);
});

test("exists is true exactly when stat succeeds", async () => {
  seed("C:/here.txt", [1]);
  await expect(facade.exists("C:/here.txt")).resolves.toBe(true);
  await expect(facade.exists("C:/gone.txt")).resolves.toBe(false);
});
