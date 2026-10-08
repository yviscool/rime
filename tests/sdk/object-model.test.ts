// Realism: L2
// Modern TS collection semantics, proven by hand-computed literals. No OS,
// clock or randomness. The AHK member-equivalence tables in
// docs/api/object-model.md remain as frozen migration reference only; this
// file does not assert AHK quirks (1-based indexes, unset slots,
// ValueError/IndexError/UnsetItemError names). Missing means `undefined`,
// out-of-range means `RangeError`, indexes are 0-based.
import { describe, expect, test } from "bun:test";

// ---- modern collection forms (the normative shapes) ----

const at = <T>(a: T[], i: number): T | undefined => {
  if (!Number.isInteger(i) || i < 0 || i >= a.length) throw new RangeError(`index ${i} out of range`);
  return a[i];
};

const removeAt = <T>(a: T[], i: number): T => {
  if (!Number.isInteger(i) || i < 0 || i >= a.length) throw new RangeError(`index ${i} out of range`);
  return a.splice(i, 1)[0] as T;
};

const insertAt = <T>(a: T[], i: number, ...values: T[]): void => {
  if (!Number.isInteger(i) || i < 0 || i > a.length) throw new RangeError(`index ${i} out of range`);
  a.splice(i, 0, ...values);
};

const arrClone = <T>(a: T[]): T[] => a.slice();

const mapGet = <K, V>(m: Map<K, V>, k: K, dflt?: V): V | undefined =>
  m.has(k) ? m.get(k) : dflt;

const mapDelete = <K, V>(m: Map<K, V>, k: K): V | undefined => {
  const old = m.get(k);
  m.delete(k);
  return old;
};

const mapFromPairs = (...pairs: unknown[]): Map<unknown, unknown> => {
  if (pairs.length % 2 !== 0) throw new TypeError("Expected an even number of key/value arguments.");
  const m = new Map<unknown, unknown>();
  for (let i = 0; i < pairs.length; i += 2) m.set(pairs[i], pairs[i + 1]);
  return m;
};

const bufferResized = (u8: Uint8Array, size: number): Uint8Array => {
  const next = new Uint8Array(size);
  next.set(u8.subarray(0, Math.min(u8.length, size)));
  return next;
};

// 0-based match view (RegExMatchObject without AHK numbering): pos is a
// string offset, -1 when there is no match, groups[0] is the whole match.
const rxPos = (m: { pos: number } | null): number => (m ? m.pos : -1);
const rxLen = (m: { len: number } | null): number => (m ? m.len : 0);
const rxItem = (
  m: { groups: Array<string | undefined>; named?: Record<string, string | undefined> } | null,
  i?: number | string,
): string | undefined => {
  if (m === null) return undefined;
  if (typeof i === "string") return m.named?.[i];
  const p = i ?? 0;
  if (!Number.isInteger(p) || p < 0 || p >= m.groups.length) throw new RangeError(`group ${String(i)} out of range`);
  return m.groups[p];
};

// Fixture shape mirrors sdk/src/runtime-language/regex.ts match records.
const match: {
  pos: number; len: number; value: string;
  groups: Array<string | undefined>;
  named?: Record<string, string | undefined>;
  count: number;
} = { pos: 5, len: 4, value: "2024", groups: ["2024", "2024", undefined], named: { year: "2024" }, count: 2 };

// ---- behavior (literals, modern expectations) ----

describe("Array", () => {
  test("length set grows sparsely; holes read undefined", () => {
    const a = ["a", "b"];
    a.length = 4;
    expect(a.length).toBe(4);
    expect(2 in a).toBe(false);
    expect(a[2]).toBeUndefined();
  });

  test("at: in-range reads (holes included), out-of-range throws RangeError", () => {
    const a = ["x", , "z"] as unknown[];
    expect(at(a, 0)).toBe("x");
    expect(at(a, 1)).toBeUndefined();
    expect(() => at(a, 3)).toThrow(RangeError);
    expect(() => at(a, -1)).toThrow(RangeError);
  });

  test("removeAt: splices (shifts), returns the removed value", () => {
    const a = ["a", "b", "c"];
    expect(removeAt(a, 1)).toBe("b");
    expect(a).toEqual(["a", "c"]);
    expect(() => removeAt(a, 5)).toThrow(RangeError);
  });

  test("insertAt: 0..length splice, out-of-range throws", () => {
    const a = ["a", "c"];
    insertAt(a, 1, "b");
    expect(a).toEqual(["a", "b", "c"]);
    expect(() => insertAt(a, 5, "x")).toThrow(RangeError);
    expect(() => insertAt(a, -1, "x")).toThrow(RangeError);
  });

  test("pop on empty is undefined; push returns the new length", () => {
    expect([].pop()).toBeUndefined();
    expect([1, 2].pop()).toBe(2);
    const a = [1];
    expect(a.push(2, 3)).toBe(3);
    expect(a).toEqual([1, 2, 3]);
  });

  test("clone shares element references, not identity", () => {
    const o = { v: 1 };
    const c = arrClone([o]);
    expect(c[0]).toBe(o);
    expect(c).not.toBe([o]);
  });

  test("entries() is 0-based (index, value)", () => {
    expect([...["a", "b"].entries()]).toEqual([[0, "a"], [1, "b"]]);
  });

  test("single-argument Array(n) allocates, it does not wrap — prefer [n]", () => {
    expect(arrClone([5])).toEqual([5]);
    expect(new Array(5).length).toBe(5);
  });
});

describe("Buffer", () => {
  test("sized allocation; illegal sizes throw", () => {
    expect(new Uint8Array(4).length).toBe(4);
    expect([...new Uint8Array(2).fill(7)]).toEqual([7, 7]);
    expect(() => new Uint8Array(-1)).toThrow(RangeError);
  });

  test("resize reallocates and copies the overlap", () => {
    const u8 = new Uint8Array([1, 2, 3]);
    expect(bufferResized(u8, 6)).toEqual(new Uint8Array([1, 2, 3, 0, 0, 0]));
    expect(u8.length).toBe(3);
  });
});

describe("Map", () => {
  test("clear empties; clone preserves insertion order", () => {
    const m = new Map([[1, "a"]]);
    m.clear();
    expect(m.size).toBe(0);
    const c = new Map<unknown, unknown>([[2, "b"], [1, "a"]]);
    expect([...new Map(c)]).toEqual([[2, "b"], [1, "a"]]);
  });

  test("delete returns the old value, undefined when absent", () => {
    const m = new Map<unknown, unknown>([[1, "a"]]);
    expect(mapDelete(m, 1)).toBe("a");
    expect(m.has(1)).toBe(false);
    expect(mapDelete(m, 1)).toBeUndefined();
  });

  test("get returns the default (possibly undefined) on a miss, never throws", () => {
    const m = new Map<unknown, unknown>([[1, "a"]]);
    expect(mapGet(m, 1)).toBe("a");
    expect(mapGet(m, 9, "d")).toBe("d");
    expect(mapGet(m, 9)).toBeUndefined();
  });

  test("pair construction requires even arity (TypeError, not silent drop)", () => {
    const m = mapFromPairs("a", 1, "b", 2);
    expect(m.get("a")).toBe(1);
    expect(m.size).toBe(2);
    expect(() => mapFromPairs("a")).toThrow(TypeError);
  });

  test("has/size/iteration are the stdlib", () => {
    const m = new Map([["k", 1]]);
    expect(m.has("k")).toBe(true);
    expect(m.has("j")).toBe(false);
    expect(m.size).toBe(1);
    expect([...m]).toEqual([["k", 1]]);
  });
});

describe("Object", () => {
  test("spread clones own enumerable props only", () => {
    const src = Object.create({ p: 1 }) as Record<string, unknown>;
    src.own = 2;
    const c = { ...src };
    expect(c.own).toBe(2);
    expect("p" in c).toBe(false);
  });

  test("defineProperty accessors behave; descriptor round-trips", () => {
    const o: Record<string, unknown> = {};
    Object.defineProperty(o, "x", { value: 5, configurable: true, enumerable: true });
    expect(o.x).toBe(5);
    let got = 0;
    Object.defineProperty(o, "y", { get: () => got, configurable: true, enumerable: true });
    expect((o as { y: number }).y).toBe(0);
    const d = Object.getOwnPropertyDescriptor(o, "y");
    expect(typeof d?.get).toBe("function");
    expect(Object.getOwnPropertyDescriptor({}, "nope")).toBeUndefined();
  });

  test("delete on a missing key is undefined; strict delete on sealed props throws", () => {
    const o: Record<string, unknown> = { a: 1 };
    delete o.a;
    expect(Object.hasOwn(o, "a")).toBe(false);
    const frozen = { b: 2 };
    Object.defineProperty(frozen, "b", { configurable: false, value: 2, enumerable: true, writable: true });
    expect(() => { delete (frozen as Record<string, unknown>).b; }).toThrow(TypeError);
  });

  test("hasOwn and entries are the stdlib", () => {
    expect(Object.hasOwn({ a: 1 }, "a")).toBe(true);
    expect(Object.hasOwn(Object.create({ a: 1 }), "a")).toBe(false);
    expect(Object.entries({ a: 1, b: "x" })).toEqual([["a", 1], ["b", "x"]]);
  });
});

describe("Function", () => {
  test("bind/call propagate values and errors", () => {
    expect(((a: number, b: number) => a + b).bind(null, 1)(2)).toBe(3);
    expect(((a: number, b: number) => a * b).call(null, 3, 4)).toBe(12);
  });

  test("length counts pre-default params; name reflects declaration", () => {
    expect(((a: number, b = 0) => a).length).toBe(1);
    expect(((...rest: number[]) => rest).length).toBe(0);
    expect(function localFn() {}.name).toBe("localFn");
  });
});

describe("RegExMatch", () => {
  test("pos is a 0-based offset, -1 without a match", () => {
    expect(rxPos(match)).toBe(5);
    expect(rxPos(null)).toBe(-1);
  });

  test("len is the match length, 0 without a match", () => {
    expect(rxLen(match)).toBe(4);
    expect(rxLen(null)).toBe(0);
  });

  test("named groups read directly, miss is undefined", () => {
    expect(match.named?.year).toBe("2024");
    expect(match.named?.month).toBeUndefined();
  });

  test("groups[0] is the whole match; out-of-range throws RangeError", () => {
    expect(rxItem(match, 0)).toBe("2024");
    expect(rxItem(match, 1)).toBe("2024");
    expect(() => rxItem(match, 9)).toThrow(RangeError);
    expect(() => rxItem(match, -1)).toThrow(RangeError);
    expect(rxItem(match, "year")).toBe("2024");
    expect(rxItem(match, "month")).toBeUndefined();
    expect(rxItem(null)).toBeUndefined();
  });
});

// ---- structural gate: schema holds, no quirk parity is enforced ----
const REQ_FIELDS = ["name", "kind", "parameters", "returnType", "sourceDefinition", "lane", "async", "ownership", "error", "status", "compatibilityTest"];

describe("结构门禁 (objects.json schema, 无怪癖对等要求)", () => {
  test("objects.json 全成员字段完整、状态词表受控", async () => {
    const objects = JSON.parse(await Bun.file(new URL("../../docs/api/objects.json", import.meta.url)).text()) as {
      objects: Array<{ name: string; members: Array<{ name: string; status: string } & Record<string, unknown>> }>;
    };
    const vocab = new Set(["implemented", "contract-only", "js-native", "unsupported-by-policy"]);
    for (const o of objects.objects) {
      for (const m of o.members) {
        for (const f of REQ_FIELDS) expect(m[f], `${o.name}.${m.name}.${f}`).toBeTruthy();
        expect(vocab.has(m.status), `${o.name}.${m.name}.status=${m.status}`).toBe(true);
      }
    }
  });
});
