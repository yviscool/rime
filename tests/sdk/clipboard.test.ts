import { beforeEach, expect, mock, test } from "bun:test";
import { ActionError, type NativeActionOptions } from "../../sdk/src/action";

// Realism: L3 - the real sdk/src/clipboard.ts facade and the real ClipboardAll
// class run against an instrumented rime:clipboard bridge: the native module
// is the environment, never the unit under test. Every expectation below is a
// literal written out by hand, so the facade cannot make its own contract
// pass by agreeing with itself.

type CodedRejection = { code: string; message: string };

const codedError = ({ code, message }: CodedRejection) =>
  Object.assign(new Error(message), { code });

let saveCalls: Array<NativeActionOptions | undefined> = [];
let restoreCalls: Array<[number[], NativeActionOptions | undefined]> = [];
let waitCalls: Array<(NativeActionOptions & { anyData?: boolean }) | undefined> = [];
let writeCalls: Array<[string, NativeActionOptions | undefined]> = [];

let savedBytes: number[] = [];
let saveRejection: CodedRejection | null = null;
let restoreRejection: CodedRejection | null = null;
let restoredFormats = 2;

function reset(): void {
  saveCalls = [];
  restoreCalls = [];
  waitCalls = [];
  writeCalls = [];
  // magic 'RIMB', version 1, reserved 0, one CF_UNICODETEXT record of 2 bytes.
  savedBytes = [82, 73, 77, 66, 1, 0, 0, 0, 0, 0, 0, 0, 13, 0, 0, 0, 2, 0, 0, 0, 120, 0, 0, 0, 0];
  saveRejection = null;
  restoreRejection = null;
  restoredFormats = 2;
}

const failIfSet = (rejection: CodedRejection | null): void => {
  if (rejection) throw codedError(rejection);
};

const bridge = {
  async read(options?: NativeActionOptions): Promise<{ text: string }> {
    void options;
    return { text: "from-bridge" };
  },
  async write(text: string, options?: NativeActionOptions): Promise<{ text: string }> {
    writeCalls.push([text, options]);
    return { text };
  },
  async wait(options?: NativeActionOptions & { anyData?: boolean }) {
    waitCalls.push(options);
    return { ready: true as const };
  },
  async saveAll(options?: NativeActionOptions) {
    saveCalls.push(options);
    failIfSet(saveRejection);
    return { bytes: savedBytes.slice() };
  },
  async restoreAll(bytes: number[], options?: NativeActionOptions) {
    restoreCalls.push([bytes, options]);
    failIfSet(restoreRejection);
    return { formats: restoredFormats };
  },
};

mock.module("rime:clipboard", () => ({ clipboard: bridge }));

const { ClipboardAll, clipboard: facade } = await import("../../sdk/src/clipboard");

beforeEach(reset);

// ---- ClipboardAll.__New ---------------------------------------------------

test("ClipboardAll with no data is an empty snapshot", () => {
  const snapshot = new ClipboardAll();
  expect(snapshot.size).toBe(0);
  expect(snapshot.bytes).toEqual([]);
});

test("ClipboardAll copies the data it is given", () => {
  const source = [82, 73, 77, 66];
  const snapshot = new ClipboardAll(source);
  source.push(99);
  expect(snapshot.size).toBe(4);
  expect(snapshot.bytes).toEqual([82, 73, 77, 66]);

  const copy = snapshot.bytes;
  copy.length = 0;
  expect(snapshot.size).toBe(4);
  expect(snapshot.bytes).toEqual([82, 73, 77, 66]);
});

test("ClipboardAll(data, size) keeps only the first size bytes", () => {
  const snapshot = new ClipboardAll([82, 73, 77, 66, 1], 4);
  expect(snapshot.size).toBe(4);
  expect(snapshot.bytes).toEqual([82, 73, 77, 66]);
});

test("ClipboardAll rejects a size that does not fit and a size that is not a count", () => {
  // AHK trusts the caller's size and reads past the end of a short buffer;
  // the bounds check here is the documented deviation.
  expect(() => new ClipboardAll([82, 73], 4)).toThrow(RangeError);
  expect(() => new ClipboardAll([82, 73], -1)).toThrow(TypeError);
  expect(() => new ClipboardAll([82, 73], 1.5)).toThrow(TypeError);
});

test("ClipboardAll rejects data that is not an array of bytes", () => {
  expect(() => new ClipboardAll("nope" as unknown as number[])).toThrow(TypeError);
  expect(() => new ClipboardAll([0, 256])).toThrow(RangeError);
  expect(() => new ClipboardAll([0, 1.5])).toThrow(RangeError);
  expect(() => new ClipboardAll([0, -1])).toThrow(RangeError);
});

// ---- facade ---------------------------------------------------------------

test("saveAll resolves a ClipboardAll holding exactly what the bridge returned", async () => {
  const snapshot = await facade.saveAll({ deadlineMs: 250 });
  expect(snapshot).toBeInstanceOf(ClipboardAll);
  expect(snapshot.size).toBe(25);
  expect(snapshot.bytes).toEqual([
    82, 73, 77, 66, 1, 0, 0, 0, 0, 0, 0, 0, 13, 0, 0, 0, 2, 0, 0, 0, 120, 0, 0, 0, 0,
  ]);
  expect(saveCalls).toEqual([{ deadlineMs: 250 }]);

  const bytes = snapshot.bytes;
  bytes.length = 0;
  expect(snapshot.size).toBe(25);
});

test("restoreAll forwards the snapshot bytes and unwraps the format count", async () => {
  const snapshot = new ClipboardAll([1, 2, 3, 4]);
  const result = await facade.restoreAll(snapshot, { deadlineMs: 100 });
  expect(result).toEqual({ formats: 2 });
  expect(restoreCalls).toEqual([
    [[1, 2, 3, 4], { deadlineMs: 100 }],
  ]);
});

test("restoreAll accepts a raw byte array the same way", async () => {
  const result = await facade.restoreAll([9, 8]);
  expect(result).toEqual({ formats: 2 });
  expect(restoreCalls).toEqual([[[9, 8], undefined]]);
});

test("a coded bridge rejection becomes an ActionError with the same code", async () => {
  saveRejection = { code: "capability_denied", message: "windows.clipboard.read" };
  const saveError = await facade.saveAll().then(
    () => null,
    (error) => error,
  );
  expect(saveError).toBeInstanceOf(ActionError);
  expect((saveError as ActionError).code).toBe("capability_denied");

  restoreRejection = { code: "execution_failed", message: "clipboard is busy" };
  const restoreError = await facade.restoreAll([1]).then(
    () => null,
    (error) => error,
  );
  expect(restoreError).toBeInstanceOf(ActionError);
  expect((restoreError as ActionError).code).toBe("execution_failed");
  expect((restoreError as ActionError).message).toBe("clipboard is busy");
});

test("wait forwards ClipWait's anyData flag alongside the deadline", async () => {
  await expect(facade.wait({ deadlineMs: 40, anyData: true })).resolves.toEqual({
    ready: true,
  });
  expect(waitCalls).toEqual([{ deadlineMs: 40, anyData: true }]);
});

test("write forwards the text unchanged and no options when none are given", async () => {
  await expect(facade.write("hello")).resolves.toEqual({ text: "hello" });
  expect(writeCalls).toEqual([["hello", undefined]]);
  await expect(facade.read()).resolves.toEqual({ text: "from-bridge" });
});
