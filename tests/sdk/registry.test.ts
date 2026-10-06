import { beforeEach, expect, mock, test } from "bun:test";
import { ActionError, type NativeActionOptions } from "../../sdk/src/action";
import type {
  RegistryBridge,
  RegistryValue,
  RegistryWritePayload,
  RegistryWriteResult,
} from "../../sdk/src/registry";

// Realism: L3 - the real sdk/src/registry.ts facade runs against an
// instrumented rime:registry bridge (only the native module is replaced).
// sdk/src/registry.ts only ever talks to the native module it dynamically
// imports, so that module is what the mock below replaces (the environment).
// Everything asserted is the facade's own behaviour: which arguments it
// forwarded, which result it resolved with, and how a coded rejection became
// an ActionError. Nothing here asserts on the mock itself.

type ReadCall = [string, string | undefined, NativeActionOptions | undefined];
type WriteCall = [RegistryWritePayload, NativeActionOptions | undefined];
type CodedRejection = { code: string; message: string };

let readCalls: ReadCall[] = [];
let writeCalls: WriteCall[] = [];
let viewCalls: string[] = [];
let setViewCalls: string[] = [];
let readResult: RegistryValue = { type: "sz", value: "from-bridge" };
let writeResult = { key: "HKCU\\Software\\Rime", op: "set" as const };
let readRejection: CodedRejection | null = null;
let writeRejection: CodedRejection | null = null;
let view = "default";

const codedError = ({ code, message }: CodedRejection) =>
  Object.assign(new Error(message), { code });

mock.module("rime:registry", () => ({
  registry: {
    read: (key: string, name?: string, options?: NativeActionOptions): Promise<RegistryValue> => {
      readCalls.push([key, name, options]);
      if (readRejection) return Promise.reject(codedError(readRejection));
      return Promise.resolve(readResult);
    },
    write: (
      payload: RegistryWritePayload,
      options?: NativeActionOptions,
    ): Promise<RegistryWriteResult> => {
      writeCalls.push([payload, options]);
      if (writeRejection) return Promise.reject(codedError(writeRejection));
      return Promise.resolve<RegistryWriteResult>(writeResult);
    },
    view: () => {
      viewCalls.push("view");
      return { view };
    },
    setView: (next: string) => {
      setViewCalls.push(next);
      view = next;
      return { view };
    },
  },
}));

const { registry } = await import("../../sdk/src/registry");

beforeEach(() => {
  readCalls = [];
  writeCalls = [];
  viewCalls = [];
  setViewCalls = [];
  readResult = { type: "sz", value: "from-bridge" };
  writeResult = { key: "HKCU\\Software\\Rime", op: "set" };
  readRejection = null;
  writeRejection = null;
  view = "default";
});

test("read forwards key, name and options and resolves the wire value", async () => {
  readResult = { type: "dword", value: 42 };
  const value = await registry.read("HKCU\\Software\\Rime", "counter", { deadlineMs: 1234 });
  expect(value).toEqual({ type: "dword", value: 42 });
  expect(readCalls).toEqual([["HKCU\\Software\\Rime", "counter", { deadlineMs: 1234 }]]);
});

test("read without a name still passes the options slot through", async () => {
  await registry.read("HKCU\\Software\\Rime", undefined, { deadlineMs: 7 });
  expect(readCalls.at(-1)).toEqual(["HKCU\\Software\\Rime", undefined, { deadlineMs: 7 }]);
  await registry.read("HKCU\\Software\\Rime");
  expect(readCalls.at(-1)).toEqual(["HKCU\\Software\\Rime", undefined, undefined]);
});

test("a coded rejection surfaces as ActionError carrying the same code", async () => {
  readRejection = {
    code: "target_gone",
    message: "registry key does not exist: HKCU\\Software\\Rime\\Gone",
  };
  const error = await registry.read("HKCU\\Software\\Rime\\Gone").catch((e: unknown) => e);
  expect(error).toBeInstanceOf(ActionError);
  expect((error as ActionError).code).toBe("target_gone");
  expect((error as ActionError).message).toBe(
    "registry key does not exist: HKCU\\Software\\Rime\\Gone",
  );
});

test("write forwards the payload unchanged and resolves {key, op}", async () => {
  const payload: RegistryWritePayload = {
    op: "set",
    key: "HKCU\\Software\\Rime",
    name: "counter",
    type: "multi_sz",
    value: ["a", "b"],
  };
  const result = await registry.write(payload, { deadlineMs: 50 });
  expect(result).toEqual({ key: "HKCU\\Software\\Rime", op: "set" });
  expect(writeCalls).toEqual([[payload, { deadlineMs: 50 }]]);
});

test("write rejects a capability refusal as ActionError with its code", async () => {
  writeRejection = {
    code: "capability_denied",
    message: "required capability was not granted: registry.write",
  };
  const error = await registry
    .write({ op: "createKey", key: "HKCU\\Software\\Rime" })
    .catch((e: unknown) => e);
  expect(error).toBeInstanceOf(ActionError);
  expect((error as ActionError).code).toBe("capability_denied");
  expect((error as ActionError).message).toContain("registry.write");
});

test("an already-aborted signal short-circuits before the bridge", async () => {
  const reason = new Error("signal already aborted");
  const signal = { aborted: true, reason };
  await expect(
    registry.write({ op: "deleteKey", key: "HKCU\\Software\\Rime" }, { signal }),
  ).rejects.toThrow("signal already aborted");
  expect(writeCalls).toHaveLength(0);
});

test("view and setView resolve the bridge's view value", async () => {
  expect(await registry.view()).toBe("default");
  expect(viewCalls).toEqual(["view"]);
  expect(await registry.setView("64")).toBe("64");
  expect(setViewCalls).toEqual(["64"]);
  expect(await registry.view()).toBe("64");
});

test("payload types refuse what the contract does not define", () => {
  // Negative cases are asserted at compile time: if the union ever widens,
  // `bun run typecheck` fails on the unused directives below.
  const submit = (payload: RegistryWritePayload): RegistryWritePayload => payload;

  // @ts-expect-error - "rename" is not one of the four ops
  submit({ op: "rename", key: "HKCU\\Software\\Rime" });
  // @ts-expect-error - dword takes a number, not a string
  submit({ op: "set", key: "HKCU\\Software\\Rime", type: "dword", value: "42" });
  // @ts-expect-error - set without a value is not a payload
  submit({ op: "set", key: "HKCU\\Software\\Rime", type: "sz" });

  expect(
    submit({ op: "deleteKey", key: "HKCU\\Software\\Rime" }),
  ).toEqual({ op: "deleteKey", key: "HKCU\\Software\\Rime" });
});

test("the bridge interface the module must export is satisfied", () => {
  // Compile-time only: keeps sdk/src/registry.ts's RegistryBridge honest
  // against what the runtime module declaration and this mock agree on.
  const bridge: RegistryBridge = {
    read: (): Promise<RegistryValue> => Promise.resolve({ type: "sz", value: "" }),
    write: (): Promise<RegistryWriteResult> => Promise.resolve({ key: "", op: "set" }),
    view: () => ({ view: "default" }),
    setView: (next) => ({ view: next }),
  };
  expect(typeof bridge.read).toBe("function");
});
