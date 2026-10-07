// Realism: L2
// AHK↔TS 等价式对照（docs/api/object-model.md §语言核心成员判定）。
// 期望值为字面量并注明 AHK 源行（script_object.cpp / regex.cpp / script.h / error.cpp），
// 由 L2 纯逻辑真跑；不含 OS、时钟或随机。结构门禁校验 md 表 ⇄ objects.json ⇄ 本文件三者一致。
import { describe, expect, test } from "bun:test";

// ---- 等价式（golden：docs/api/object-model.md 表格对应） ----

const idx = (i: number, len: number) => i >= 1 && i <= len;

const arrLengthSet = (a: unknown[], n: number) => { a.length = n; return a.length; };
const arrClone = (a: unknown[]) => a.slice();
const arrDelete = (a: unknown[], i: number): unknown => {
  if (!idx(i, a.length)) throw new RangeError("ValueError");
  const old = a[i - 1]; delete a[i - 1]; return old;
};
const arrGet = (a: unknown[], i: number, dflt?: unknown): unknown => {
  if (!idx(i, a.length)) throw new RangeError("IndexError");
  const v = a[i - 1];
  if (v !== undefined) return v;
  if (dflt !== undefined) return dflt;
  throw new Error("UnsetItemError");
};
const arrHas = (a: unknown[], i: number) => idx(i, a.length) && a[i - 1] !== undefined;
const arrInsertAt = (a: unknown[], i: number, ...values: unknown[]): undefined => {
  if (!(i >= 1 && i <= a.length + 1)) throw new RangeError("ValueError");
  a.splice(i - 1, 0, ...values); return undefined;
};
const arrPop = (a: unknown[]): unknown => {
  if (!a.length) throw new Error("Array is empty.");
  return a.pop();
};
const arrPush = (a: unknown[], ...values: unknown[]): undefined => { a.push(...values); return undefined; };
const arrRemoveAt = (a: unknown[], i: number, count?: number): unknown => {
  if (!idx(i, a.length)) throw new RangeError("ValueError");
  if (count === undefined) { const old = a[i - 1]; a.splice(i - 1, 1); return old; }
  if (i - 1 + count > a.length) throw new RangeError("ValueError");
  a.splice(i - 1, count); return undefined;
};
const arrItemGet = (a: unknown[], i: number) => arrGet(a, i);
const arrItemSet = (a: unknown[], i: number, v: unknown) => { a[i - 1] = v; };
const arrNew = (...values: unknown[]) => [...values];
const bufferWithBytes = (u8: Uint8Array, size: number): Uint8Array => {
  const next = new Uint8Array(size); next.set(u8.subarray(0, Math.min(u8.length, size))); return next;
};

const mapDelete = (m: Map<unknown, unknown>, k: unknown): unknown => {
  if (!m.has(k)) throw new Error("UnsetItemError");
  const old = m.get(k); m.delete(k); return old;
};
const mapGet = (m: Map<unknown, unknown>, k: unknown, dflt?: unknown): unknown => {
  if (m.has(k)) return m.get(k);
  if (dflt !== undefined) return dflt;
  throw new Error("UnsetItemError");
};
const mapSet = (m: Map<unknown, unknown>, ...pairs: unknown[]): Map<unknown, unknown> => {
  if (pairs.length % 2) throw new Error("Invalid number of parameters.");
  for (let i = 0; i < pairs.length; i += 2) m.set(pairs[i], pairs[i + 1]);
  return m;
};
const mapNew = (...pairs: unknown[]): Map<unknown, unknown> => {
  if (pairs.length % 2) throw new Error("Invalid number of parameters.");
  const m = new Map<unknown, unknown>();
  for (let i = 0; i < pairs.length; i += 2) m.set(pairs[i], pairs[i + 1]);
  return m;
};
const mapItemGet = (m: Map<unknown, unknown>, k: unknown, dflt?: unknown) => mapGet(m, k, dflt);
const mapItemSet = (m: Map<unknown, unknown>, k: unknown, v: unknown) => { m.set(k, v); };

const objectClone = (o: object) => ({ ...o });
const defineProp = (o: object, name: string, desc: Record<string, unknown>) => {
  const js: PropertyDescriptor = {};
  if ("Value" in desc) js.value = desc.Value;
  if ("Get" in desc) js.get = desc.Get as () => unknown;
  if ("Set" in desc) js.set = desc.Set as (v: unknown) => void;
  Object.defineProperty(o, name, { ...js, configurable: true, enumerable: true });
  return o;
};
const deleteProp = (o: object, name: string): unknown => {
  if (!Object.hasOwn(o, name)) return undefined;
  const old = (o as Record<string, unknown>)[name]; delete (o as Record<string, unknown>)[name]; return old;
};
const getOwnPropDesc = (o: object, name: string) => {
  const d = Object.getOwnPropertyDescriptor(o, name);
  if (d === undefined) return undefined;
  return d.get !== undefined || d.set !== undefined ? { Get: d.get, Set: d.set } : { Value: d.value };
};

const rxPos = (m: { pos: number } | null) => (m ? m.pos + 1 : 0);
const rxLen = (m: { len: number } | null) => (m ? m.len : 0);
const rxName = (m: { named?: Record<string, string | undefined> } | null, n?: string) =>
  n === undefined ? "" : m && m.named && n in m.named ? m.named[n] : undefined;
const rxItem = (
  m: { value: string; groups: Array<string | undefined>; named?: Record<string, string | undefined> } | null,
  i?: number | string,
) => {
  if (m === null) return undefined;
  if (typeof i === "string") return m.named?.[i];
  const p = i ?? 0;
  if (p < 0 || p >= m.groups.length) throw new RangeError("ValueError");
  return m.groups[p];
};
const rxCount = (m: { count: number } | null) => (m ? m.count : 0);
const rxMark = (m: object | null) => (m as { mark?: string } | null)?.mark ?? "";
const rxEnum = (m: { value: string; named?: Record<string, string | undefined> } | null) =>
  m ? [[0, m.value], ...Object.entries(m.named ?? {})] : [];

// RegExMatch 夹具（形状 = sdk/src/runtime-language/regex.ts:4-21；
// AHK p=0 全匹配、p≥1 子模式 = lib/regex.cpp:185-204/260）
const match: {
  pos: number; len: number; value: string;
  groups: Array<string | undefined>;
  named?: Record<string, string | undefined>;
  count: number;
} = { pos: 5, len: 4, value: "2024", groups: ["2024", "2024", undefined], named: { year: "2024" }, count: 2 };

// ---- 行为对照（期望值来自 AHK 源行，字面给定） ----

describe("Array (script_object.cpp)", () => {
  test("Length: set 扩展后 JS 留稀疏洞、读出 undefined（:3068-3084 AHK 才校验范围；§统一注记 3）", () => {
    const a = ["a", "b"];
    expect(arrLengthSet(a, 4)).toBe(4);
    expect(2 in a).toBe(false);
    expect(a[2]).toBeUndefined();
  });

  test("Delete: 返回旧值、置洞不移位；越界抛 ValueError（:3148-3156）", () => {
    const a = ["x", "y", "z"];
    expect(arrDelete(a, 2)).toBe("y");
    expect(a.length).toBe(3);
    expect(1 in a).toBe(false);
    expect(a[1]).toBeUndefined();
    expect(() => arrDelete(a, 9)).toThrow(RangeError);
    expect(() => arrDelete(a, 0)).toThrow(RangeError);
  });

  test("Get: 越界有默认值也抛 IndexError（:3043-3045）；默认覆盖界内 unset（:3053-3065）；无默认 unset 抛 UnsetItemError（:3063-3064 + script.h:1512）", () => {
    const a = ["x", , "z"] as unknown[];
    expect(arrGet(a, 1, "d")).toBe("x");
    expect(arrGet(a, 2, "d")).toBe("d");
    expect(() => arrGet(a, 2)).toThrow("UnsetItemError");
    expect(() => arrGet(a, 9, "d")).toThrow("IndexError");
    expect(() => arrGet(a, 0, "d")).toThrow("IndexError");
  });

  test("Has: 界内且非 unset（:3143-3147）；越界不抛返回 false", () => {
    expect(arrHas(["x"], 1)).toBe(true);
    expect(arrHas([, "y"] as unknown[], 1)).toBe(false);
    expect(arrHas(["x"], 9)).toBe(false);
  });

  test("InsertAt: i∈[1,length+1] 否则抛（:3092-3094），返回 undefined（:3102）", () => {
    const a = ["a", "c"];
    expect(arrInsertAt(a, 2, "b")).toBeUndefined();
    expect(a).toEqual(["a", "b", "c"]);
    expect(() => arrInsertAt(a, 5, "x")).toThrow(RangeError);
    expect(() => arrInsertAt(a, 0, "x")).toThrow(RangeError);
  });

  test("Pop: 返回末元素；空数组抛 Error('Array is empty.')（:3117-3118）", () => {
    expect(arrPop([1, 2])).toBe(2);
    expect(() => arrPop([])).toThrow("Array is empty.");
    expect(() => arrPop([])).toThrow(Error);
  });

  test("Push: 返回 undefined（对齐 AHK unset 返回位，:3102）", () => {
    const a = [1];
    expect(arrPush(a, 2, 3)).toBeUndefined();
    expect(a).toEqual([1, 2, 3]);
  });

  test("RemoveAt: 省略 count 返回旧值、给定返回 undefined（:3123-3141）；越界 ValueError（:3111-3113、:3130-3131）", () => {
    const a = ["a", "b", "c"];
    expect(arrRemoveAt(a, 2)).toBe("b");
    expect(a).toEqual(["a", "c"]);
    expect(arrRemoveAt(a, 1, 1)).toBeUndefined();
    expect(a).toEqual(["c"]);
    expect(() => arrRemoveAt(a, 5)).toThrow(RangeError);
    expect(() => arrRemoveAt(a, 1, 9)).toThrow(RangeError);
  });

  test("Clone: 浅拷贝共享元素引用（:2989-3009 逐槽复制）", () => {
    const o = { v: 1 };
    const c = arrClone([o]);
    expect(c[0]).toBe(o);
    expect(c).not.toBe([o]);
  });

  test("__Enum: 产出 1-based (index, value)（:3182-3213）；maxItems 显式 slice", () => {
    const out: Array<[number, unknown]> = [];
    for (const [i0, v] of ["a", "b", "c"].slice(0, 2).entries()) out.push([i0 + 1, v]);
    expect(out).toEqual([[1, "a"], [2, "b"]]);
  });

  test("__Item: get 同 Get（:3040-3065 同一 case 体）；set 直写；arr[i]:=unset 对应 delete 成洞（BIMF_UNSET_ARG_1，:3021）", () => {
    const a = ["x", "z"];
    expect(arrItemGet(a, 2)).toBe("z");
    arrItemSet(a, 2, "y");
    expect(a[1]).toBe("y");
    delete a[1];
    expect(1 in a).toBe(false);
    expect(() => arrItemGet(a, 9)).toThrow("IndexError");
  });

  test("__New: 分发 M_Push 追加（:3024 + :3098-3102）；单参 Array(n) 歧义用 [n]", () => {
    expect(arrNew(1, 2)).toEqual([1, 2]);
    expect(arrNew(5)).toEqual([5]);
    expect([5].length).toBe(1); // AHK Array(5)=[5]；JS new Array(5)=5 槽——禁用单参构造
    expect(new Array(5).length).toBe(5);
  });
});

describe("Buffer (script_object.cpp:4051-4079)", () => {
  test("__New: 尺寸非法两侧拒绝；缺省零填充可依赖", () => {
    expect(new Uint8Array(4).length).toBe(4);
    expect([...new Uint8Array(2).fill(7)]).toEqual([7, 7]);
    expect(() => new Uint8Array(-1)).toThrow(RangeError);
  });

  test("Size: byteLength 读；set=重分配+拷贝（withBytes）", () => {
    const u8 = new Uint8Array([1, 2, 3]);
    expect(u8.byteLength).toBe(3);
    const next = bufferWithBytes(u8, 6);
    expect([...next]).toEqual([1, 2, 3, 0, 0, 0]);
    expect(u8.length).toBe(3);
  });
});

describe("Map (script_object.cpp)", () => {
  test("Clear: 清空全部（:704、:1918-1921）", () => {
    const m = new Map([[1, "a"]]);
    m.clear();
    expect(m.size).toBe(0);
  });

  test("Clone: 浅拷贝、插入序保留（:2078-2084）", () => {
    const m = new Map<unknown, unknown>([[1, "a"], [2, "b"]]);
    const c = new Map(m);
    expect(c).not.toBe(m);
    expect([...c]).toEqual([[1, "a"], [2, "b"]]);
  });

  test("Delete: 返回被删值；缺键 v2.0 抛 UnsetItemError（:1884-1890 + script.h:1512）", () => {
    const m = new Map<unknown, unknown>([[1, "a"]]);
    expect(mapDelete(m, 1)).toBe("a");
    expect(m.has(1)).toBe(false);
    expect(() => mapDelete(m, 1)).toThrow("UnsetItemError");
    expect(() => mapDelete(m, 1)).toThrow(Error);
  });

  test("Has: 键存在性（:2059-2066）", () => {
    const m = new Map([["k", 1]]);
    expect(m.has("k")).toBe(true);
    expect(m.has("j")).toBe(false);
  });

  test("Set: 返回 Map 可链（:1345-1346）；奇数参数抛基础 Error('Invalid number of parameters.')（:1341-1342 → script.h:391）", () => {
    const m = new Map<unknown, unknown>();
    expect(mapSet(m, 1, "a", 2, "b")).toBe(m);
    expect([...m]).toEqual([[1, "a"], [2, "b"]]);
    expect(() => mapSet(m, 1)).toThrow("Invalid number of parameters.");
    expect(() => mapSet(m, 1)).not.toThrow(TypeError);
  });

  test("__Item/Get 同路（:1296 + :1305-1336）：命中取值、缺键+默认返回默认、缺键无默认抛 UnsetItemError（:1313-1317）", () => {
    const m = new Map<unknown, unknown>([[1, "a"]]);
    expect(mapItemGet(m, 1)).toBe("a");
    expect(mapGet(m, 9, "d")).toBe("d");
    expect(() => mapGet(m, 9)).toThrow("UnsetItemError");
    expect(() => mapItemGet(m, 9)).toThrow("UnsetItemError");
    mapItemSet(m, 2, "b");
    expect(m.get(2)).toBe("b");
    expect(mapDelete(m, 2)).toBe("b");
  });

  test("__New: 分发 Set（:1295），奇数参数抛基础 Error；成对构造成 Map", () => {
    const m = mapNew("a", 1, "b", 2);
    expect(m.get("a")).toBe(1);
    expect(m.size).toBe(2);
    expect(() => mapNew("a")).toThrow("Invalid number of parameters.");
  });

  test("Count: m.size 同义（:1929-1932 _o_return(mCount)）", () => {
    const m = new Map([[1, "a"], [2, "b"]]);
    expect(m.size).toBe(2);
  });

  test("__Enum: 插入序 (key, value)（:3413-3438、:2048-2052）", () => {
    const m = new Map<unknown, unknown>([[2, "b"], [1, "a"]]);
    expect([...m]).toEqual([[2, "b"], [1, "a"]]);
  });
});

describe("Object (script_object.cpp)", () => {
  test("Clone: 浅拷贝；原型链属性不随 spread 复刻（:2068-2076 差异）", () => {
    const src = Object.create({ p: 1 }) as Record<string, unknown>;
    src.own = 2;
    const c = objectClone(src) as Record<string, unknown>;
    expect(c.own).toBe(2);
    expect("p" in c).toBe(false);
  });

  test("DefineProp: Value/Get/Set 键翻译（:2407-2423 族）；返回 receiver 可链（:2345、:2368）", () => {
    const o: Record<string, unknown> = {};
    expect(defineProp(o, "x", { Value: 5 })).toBe(o);
    expect(o.x).toBe(5);
    let got = 0;
    defineProp(o, "y", { Get: () => got });
    expect((o as { y: number }).y).toBe(0);
    defineProp(o, "z", { Set: (v: number) => { got = v; } });
    (o as { z: number }).z = 9;
    expect(got).toBe(9);
  });

  test("DeleteProp: 返回旧值、缺属性 undefined（:1865-1872）；非配置属性严格模式 delete 抛 TypeError（差异，须查 configurable）", () => {
    const o: Record<string, unknown> = { a: 1 };
    expect(deleteProp(o, "a")).toBe(1);
    expect(Object.hasOwn(o, "a")).toBe(false);
    expect(deleteProp(o, "a")).toBeUndefined();
    const frozen = { b: 2 };
    Object.defineProperty(frozen, "b", { configurable: false, value: 2, enumerable: true, writable: true });
    expect(() => { delete (frozen as Record<string, unknown>).b; }).toThrow(TypeError); // 严格模式（模块默认）——AHK 字段无不可配置概念
    expect(Object.hasOwn(frozen, "b")).toBe(true);
  });

  test("GetOwnPropDesc: 数据→{Value}、访问器→{Get,Set}、缺失→undefined（:2407-2423、:2397-2401）", () => {
    expect(getOwnPropDesc({ a: 1 }, "a")).toEqual({ Value: 1 });
    const acc = {} as Record<string, unknown>;
    Object.defineProperty(acc, "g", { get: () => 3, configurable: true, enumerable: true });
    const d = getOwnPropDesc(acc, "g") as { Get: () => number };
    expect(typeof d.Get).toBe("function");
    expect(d.Get()).toBe(3);
    expect(getOwnPropDesc({}, "nope")).toBeUndefined();
  });

  test("HasOwnProp: 自有字段存在即真（:2054-2057 FindField）", () => {
    const o = { a: 1 };
    expect(Object.hasOwn(o, "a")).toBe(true);
    expect(Object.hasOwn(o, "b")).toBe(false);
    expect(Object.hasOwn(Object.create(o), "a")).toBe(false);
  });

  test("OwnProps: 名前值后（:3256-3265）", () => {
    const out: Array<[string, unknown]> = [];
    for (const [name, value] of Object.entries({ a: 1, b: "x" })) out.push([name, value]);
    expect(out).toEqual([["a", 1], ["b", "x"]]);
  });
});

describe("Func (script_object.cpp:3759-3797)", () => {
  test("Bind: 左起填充返回新函数（:3763-3766）", () => {
    const f = (a: number, b: number) => a + b;
    expect(f.bind(null, 1)(2)).toBe(3);
  });

  test("Call: 直调并传播错误（:3759-3761）", () => {
    const f = (a: number, b: number) => a * b;
    expect(f.call(null, 3, 4)).toBe(12);
  });

  test("MinParams = fn.length（:3797 mMinParams；rest 不计、可选用默认值形态）", () => {
    expect(((a: number, b = 0) => a).length).toBe(1);
    expect(((...rest: number[]) => rest).length).toBe(0);
  });

  test("Name = fn.name（:3796）；匿名形态差异照记", () => {
    const named = function localFn() {};
    expect(named.name).toBe("localFn");
    expect((function () {}).name).toBe("");
  });
});

describe("RegExMatchObject (lib/regex.cpp:213-265)", () => {
  test("Pos: 1-based、无匹配 0（:261；Rime pos 0-based regex.ts:5-6）", () => {
    expect(rxPos(match)).toBe(6);
    expect(rxPos(null)).toBe(0);
  });

  test("Len: 子模式长度、无匹配 0（:262）", () => {
    expect(rxLen(match)).toBe(4);
    expect(rxLen(null)).toBe(0);
  });

  test("Name: 无参恒 ''（:263 + script_object.h:948）；按名命中返回该名、未命中 undefined（静默差异）", () => {
    expect(rxName(match)).toBe("");
    expect(rxName(match, "year")).toBe("2024");
    expect(rxName(match, "month")).toBeUndefined();
    expect(rxName(null, "year")).toBeUndefined();
  });

  test("__Get: 命名捕获直取（:254-260 + 名解析 :223-250）；未命中 undefined", () => {
    expect(match.named?.year).toBe("2024");
    expect(match.named?.month).toBeUndefined();
  });

  test("__Item: groups[0]=全匹配同轴（regex.ts:13-14 ↔ regex.cpp:260）；越界/负下标 ValueError（:249-250）", () => {
    expect(rxItem(match, 0)).toBe("2024");
    expect(rxItem(match, 1)).toBe("2024");
    expect(() => rxItem(match, 9)).toThrow(RangeError);
    expect(() => rxItem(match, -1)).toThrow(RangeError);
    expect(rxItem(match, "year")).toBe("2024");
    expect(rxItem(match, "month")).toBeUndefined();
    expect(rxItem(null)).toBeUndefined();
  });

  test("Count: 捕获组数 = mPatternCount - 1（:217）；Rime null → 0", () => {
    expect(rxCount(match)).toBe(2);
    expect(rxCount(null)).toBe(0);
  });

  test("Mark: 源恒空串（:218）；Rime mark 保留恒缺（regex.ts:20-21）", () => {
    expect(rxMark(match)).toBe("");
    expect(rxMark({} as { mark?: string })).toBe("");
    expect(rxMark(null)).toBe("");
  });

  test("__Enum: [[0, 全匹配], ...named]（:219-220 + 名解析）", () => {
    expect(rxEnum(match)).toEqual([[0, "2024"], ["year", "2024"]]);
    expect(rxEnum(null)).toEqual([]);
  });
});

// ---- 结构门禁：objects.json ⇄ object-model.md ⇄ 本文件 ----

const REQ_FIELDS = ["name", "kind", "parameters", "returnType", "sourceDefinition", "lane", "async", "ownership", "error", "status", "compatibilityTest"];
const CORE = ["Array", "Buffer", "ComObject", "Func", "Map", "Object", "RegExMatchObject"];
const md = await Bun.file(new URL("../../docs/api/object-model.md", import.meta.url)).text();
const objects = JSON.parse(await Bun.file(new URL("../../docs/api/objects.json", import.meta.url)).text()) as {
  objects: Array<{ name: string; members: Array<{ name: string; status: string } & Record<string, unknown>> }>;
};

const mdSectionRows = (heading: string): Array<{ name: string; cells: string[] }> => {
  const body = md.split(heading)[1] ?? "";
  const rows: Array<{ name: string; cells: string[] }> = [];
  for (const line of body.split("\n")) {
    const m = line.match(/^\| `([^`]+)`/);
    if (!m) { if (rows.length && !line.startsWith("|")) break; continue; }
    const cells = line.replace(/^\| /, "").replace(/\s*\|\s*$/, "").split(" | ").map((c) => c.trim());
    rows.push({ name: m[1].replace(/（.*$/, ""), cells });
  }
  return rows;
};

const coreJson = (status: string): string[] =>
  objects.objects
    .filter((o) => CORE.includes(o.name))
    .flatMap((o) => o.members.filter((m) => m.status === status).map((m) => `${o.name}.${m.name}`));

describe("结构门禁 (md 表 ⇄ objects.json ⇄ 本文件)", () => {
  test("objects.json 全成员字段完整、状态词表受控、总数 259", () => {
    const vocab = new Set(["implemented", "contract-only", "js-native", "unsupported-by-policy"]);
    let total = 0;
    for (const o of objects.objects) {
      for (const m of o.members) {
        total++;
        for (const f of REQ_FIELDS) expect(m[f], `${o.name}.${m.name}.${f}`).toBeTruthy();
        expect(vocab.has(m.status), `${o.name}.${m.name}.status=${m.status}`).toBe(true);
      }
    }
    expect(total).toBe(259);
  });

  test("md 等价表 ⇄ objects.json：js-native 42 行、unsupported 16 行双向相等；contract-only 仅 Map.CaseSense", () => {
    const mdJs = mdSectionRows("### `js-native`（42）").map((r) => r.name);
    const mdUn = mdSectionRows("### `unsupported-by-policy`（16）").map((r) => r.name);
    expect(mdJs.length, `md js-native 行数=${mdJs.length}`).toBe(42);
    expect(mdUn.length, `md unsupported 行数=${mdUn.length}`).toBe(16);
    expect(mdJs.slice().sort()).toEqual(coreJson("js-native").sort());
    expect(mdUn.slice().sort()).toEqual(coreJson("unsupported-by-policy").sort());
    expect(coreJson("contract-only")).toEqual(["Map.CaseSense"]);
  });

  test("md 每条 js-native 行含等价式与源行号引证；contract-only 注记在档", () => {
    const rows = mdSectionRows("### `js-native`（42）");
    expect(rows.length).toBe(42);
    for (const r of rows) {
      expect(r.cells.length, `${r.name} 列数`).toBe(3);
      expect(r.cells[1], `${r.name} 等价式缺失`).toBeTruthy();
      expect(r.cells[2], `${r.name} 差异引证缺失`).toMatch(/:\d+/);
    }
    expect(md).toContain("`Map.CaseSense` 判 `contract-only`");
    expect(md).toContain("`GuiControl.Get` 同留 `contract-only`");
    expect(md).toContain("42 项 `js-native` + 16 项 `unsupported-by-policy`");
  });

  test("本文件覆盖清单 = objects.json 的 42 条 js-native（新增必须同步补测）", () => {
    const tested = new Set([
      ...["Length", "Delete", "Get", "Has", "InsertAt", "Pop", "Push", "RemoveAt", "Clone", "__Enum", "__Item", "__New"].map((n) => `Array.${n}`),
      ...["__New", "Size"].map((n) => `Buffer.${n}`),
      ...["Bind", "Call", "MinParams", "Name"].map((n) => `Func.${n}`),
      ...["__Enum", "Clear", "Clone", "Delete", "Has", "Set", "__Item", "Count", "__New", "Get"].map((n) => `Map.${n}`),
      ...["Clone", "DefineProp", "DeleteProp", "GetOwnPropDesc", "HasOwnProp", "OwnProps"].map((n) => `Object.${n}`),
      ...["__Enum", "__Get", "Len", "Name", "Pos", "__Item", "Count", "Mark"].map((n) => `RegExMatchObject.${n}`),
    ]);
    expect(tested.size).toBe(42);
    expect([...tested].sort()).toEqual(coreJson("js-native").sort());
  });
});
