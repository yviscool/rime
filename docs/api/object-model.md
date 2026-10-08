# Object、GUI、COM 与宿主对象模型

状态：GUI/COM 等大对象 M6+；M6 第一批五项对话框/托盘动词（`MsgBox`/`InputBox`/`ToolTip`/`TraySetIcon`/`TrayTip`）已实现于 `@rime/ui`（见 [`gui-menu.md`](./gui-menu.md)）；语言核心 59 成员已定档为 `js-native`（42）、`unsupported-by-policy`（16）与 `contract-only`（1，`Map.CaseSense`），见下。

## TS 边界

Runtime 不把 AHK 的 C++ 对象、`IUnknown*`、`HMENU`、`HGLOBAL`、缓冲区裸指针或内部对象地址交给 QuickJS。上层得到的是 Runtime-owned opaque ID、不可变快照和显式 `Subscription`。拥有状态的对象只能在所属 lane 操作。

### Window 对象模型（P1-4，已完成替换）

`sdk/src/window.ts` 的唯一窗口表面是 `Window` 类（以 `Control` 类为模板；
函数命名空间与 `ActiveWindowRequest` 已删除，不保留向后兼容）。
`Window` 只持有稳定 `WindowId` + 上次不可变快照，每个动词经 service
重解析 id 并用返回快照更新缓存；窗口死亡一律表现为 service 的标准
`target_gone` ActionError，不新增客户端错误种类。`list/find/active/
fromSnapshot/refresh/isAlive` 见 `tests/sdk/windows.test.ts` 与
`tests/sdk/window-handle.test.ts`（L3）。

## AHK 对象到 TS 的映射

| AHK 类型 | TS 方向 | 所属 lane | 关键问题 |
|---|---|---|---|
| Object/Array/Map/Func | 原生 TS 对象、数组、Map、函数 | JS | 还原保留时需逐项定义并测试 AHK 的索引、ByRef、枚举和异常差异 |
| Buffer | `BufferRef` + `Uint8Array` 拷贝 | Worker/JS | 不暴露地址；跨线程只能传复制或受控共享快照 |
| Gui/GuiCtrl | `GuiRef`/`ControlRef` | UI | HWND 只在 UI lane；事件回调回到 JS scheduler |
| Menu/MenuBar | `MenuRef` | UI | HMENU 不出 native；显示是可取消的 UI action |
| InputHook | `InputSubscription` | Hook/UI | Stop、Wait、EndReason、EndKey 和回调计数必须可观察 |
| File | `FileRef` | IO | HANDLE/FILE* 不出 JS；close 和 runtime shutdown 必须确定性执行 |
| ComObject | 受权限的 typed facade | Automation MTA | COM apartment、VARIANT、SAFEARRAY 和事件连接不可跨线程裸传 |
| Host Script/Funcs/Vars/Labels | 序列化检查描述 | JS Host | 检查不能触发脚本；执行必须显式提交 Action |

## GUI/菜单成员矩阵要求

`GuiType::sMembers`、`GuiControlType::sMembers*`、`UserMenu::sMembers`、`FileObject::sMembers`、`InputObject::sMembers` 和 `ComObject::s*Members` 必须逐个提取成员名、参数、返回值、源码实现和对象生命周期，另建 `objects.json`。仅列出 `Gui*` 函数不足以覆盖这些方法。

## 语言核心成员判定（Round 5，59 项）

> 定位（2026-10-08，与 `AGENTS.md` 一致）：下表是**冻结的迁移参照**，
> 记录 AHK 语义与 TS 形态的对照，供移植脚本查阅。它不是规范——现代语义以
> `tests/sdk/object-model.test.ts` 为准（缺键 `undefined`、越界 `RangeError`、
> 0-based 索引），测试不再断言 AHK 怪癖（1-based、`UnsetItemError` 等错误名、
> unset 置洞）。新增 API 不得引用下表做行为依据。

`objects.json` 中 7 个非控制类对象（`Array`/`Buffer`/`ComObject`/`Func`/`Map`/`Object`/`RegExMatchObject`）共 59 个成员逐项定档：**42 项 `js-native` + 16 项 `unsupported-by-policy`**；另 `Map.CaseSense` 判 `contract-only`（set 要求空表 + `On/Off/Locale`，`script_object.cpp:1934-1976`——有用户价值的可观察差异，按分诊规则留 L4 还原路径，不进等价表），`GuiControl.Get` 同留 `contract-only`（GUI 侧，`script_gui.cpp:GuiControlType::sMembersTV`）。每条 `js-native` 行给出等价 TS 表达式与可观察差异，等价式在 `tests/sdk/object-model.test.ts`（L2）真跑，期望值取自本表引证的 AHK 源行，不由测试逻辑自算；`unsupported-by-policy` 行的理由与替代在 `stdlib.md` §5（第 1/4/5/7 条）与 `native-interop.md`，入口级拒绝由 `policy-refusal.mjs` 守 `Ptr`/`Handle`/`Hwnd`。

统一注记：

- AHK 索引 1-based；`ParamToZeroIndex`（`script_object.cpp:3170-3179`）把负索引从尾部包装、`0` 解释为首个未用槽。下表表达式按正索引书写，负索引行为差异逐行注出。
- 缺键/缺属性的错误方向以 **v2.0 口径抛 `UnsetItemError`** 为准：`g_script.BackCompatMode()` 默认真（`script.h:1512` `// true = requires v2.0`）与现行文档一致（"an UnsetItemError is thrown"）；仅 v2.1 草案模式返回 unset（`Map::Delete` `script_object.cpp:1884-1890`、`Array::Get`/`Map.__Item` 的 `Default` 回落 `:3060-3065`/`:1313-1318`、`GuiType::get___Item` `script_gui.cpp:445-446`）。等价式按 v2.0 目标抛 `Error("UnsetItemError")`；`objects.json` 错误栏相应用 `UnsetItemError` 类名。
- AHK 的 unset 槽与 JS 的稀疏洞/显式 `undefined` 读出一致（均 `undefined`）；AHK 只有 unset、JS 可显式写入 `undefined`，需要区分时表达式以 `!== undefined` 判定。

### `js-native`（42）

| 成员 | 等价 TS 表达式 | 可观察差异（AHK 源引证） |
|---|---|---|
| `Array.Length`（get/set） | get：`arr.length`；set：`arr.length = n` | 同为逻辑长度读写；AHK setter 校验类型与范围（非数 TypeError、负数/超 MaxIndex ValueError，`script_object.cpp:3068-3084` `_o_throw_value`），JS 静默转换；AHK 扩展填 unset、JS 留稀疏洞（读出均 undefined） |
| `Array.__Enum` | `for (const [i0, v] of arr.slice(0, maxItems ?? arr.length).entries()) cb(i0 + 1, v)` | 产出 (1-based 索引, 值)，单变量形态只产值（`script_object.cpp:3182-3213`）；`maxItems` 需显式 slice；界内 unset 槽与 JS 洞同读作 undefined |
| `Array.Clone` | `arr.slice()` | 同为浅拷贝、共享元素引用（`script_object.cpp:2989-3009` 逐槽复制）；AHK 保留 unset 槽、JS 保留稀疏洞，读出等价 |
| `Array.Delete` | `i >= 1 && i <= arr.length ? (old => (delete arr[i - 1], old))(arr[i - 1]) : (() => { throw new RangeError("ValueError") })()` | 返回旧值且**不移位**——置洞、长度不变（`script_object.cpp:3148-3156`）；越界/非数经 `_o_throw_param(0)`（`:3151-3152` → `error.cpp:1019-1057` 无期望类型 → `ErrorPrototype::Value`，与文档 "A ValueError is thrown" 一致）抛 ValueError，表达式以 RangeError 承载；负索引经尾部包装（`:3170-3179`）表达式未含 |
| `Array.Get` | `i >= 1 && i <= arr.length ? (arr[i - 1] !== undefined ? arr[i - 1] : dflt !== undefined ? dflt : (() => { throw new Error("UnsetItemError") })()) : (() => { throw new RangeError("IndexError") })()` | 越界**有默认值也抛** IndexError——默认值分支在越界检查之后（`script_object.cpp:3043-3045` `ErrorPrototype::Index`）；默认仅覆盖界内 unset（`:3053-3065`）；无默认命中 unset 时按 `Default` 属性回落，v2.0 默认抛 UnsetItemError（`:3063-3064` + `script.h:1512`）、v2.1 草案返回 unset |
| `Array.Has` | `i >= 1 && i <= arr.length && arr[i - 1] !== undefined` | 界内且非 unset（`script_object.cpp:3143-3147`）；JS 显式 `undefined` 值与洞不可区分（AHK 只有 unset），表达式按此对齐 |
| `Array.InsertAt` | 先校验 `i >= 1 && i <= arr.length + 1` 否则抛，再 `arr.splice(i - 1, 0, ...values)` | 越界/非数抛参数错（`script_object.cpp:3092-3094`）；负索引尾部包装（`:3170-3179`）；返回 unset（`:3102`），需丢弃 splice 结果 |
| `Array.Pop` | `arr.length ? arr.pop() : (() => { throw new Error("Array is empty.") })()` | 同为移除并返回末元素；AHK 空数组抛错（`script_object.cpp:3117-3119`），JS `pop` 静默返回 undefined，表达式补抛 |
| `Array.Push` | `(arr.push(...values), undefined)` | 追加语义一致；AHK 返回 unset（`script_object.cpp:3102`）、JS 返回新长度，表达式丢弃对齐 |
| `Array.RemoveAt` | `count === undefined ? (old => (arr.splice(i - 1, 1), old))(arr[i - 1]) : (arr.splice(i - 1, count), undefined)`（先做越界校验） | 仅省略 count 返回被删值，给定 count 返回 unset（`script_object.cpp:3123-3141`）；`i` 或 `i + count` 越界经 `_o_throw_param` 抛 ValueError（`:3111-3113`、`:3130-3131` → `error.cpp:1019-1057`），与文档一致；旧值必须在 splice 前读取 |
| `Array.__Item`（get/set） | get 同 `Array.Get`：界内 `arr[i - 1]`（unset 回落 `Default`/抛 `Error("UnsetItemError")`），越界 `throw new RangeError("IndexError")`；set：`arr[i - 1] = v`，`v` 为 unset 时 `delete arr[i - 1]` | 与 `Get` 共用 case（`script_object.cpp:3040-3065` `P___Item`/`M_Get` 同一 case 体）；set 走 `Assign` 失败抛 MemoryError（`:3047-3051`）、`BIMF_UNSET_ARG_1`（`:3021`）允许 `arr[i] := unset` 置洞；JS 显式 `undefined` 赋值写入值而非洞——区分须用 `delete` |
| `Array.__New` | 构造 `[v1, v2, ...]`；实例 `arr.__New(...values)` → `(arr.push(...values), undefined)` | 分发到 `M_Push` 追加并返回 unset（`script_object.cpp:3024` + `:3098-3102`）；`Array(n)` 单数字形态 AHK 是单元素数组、JS `new Array(n)` 是稀疏长度——移植脚本禁用单参 `Array(n)`，用 `[n]`；失败仅 OOM |
| `Buffer.__New` | `fill === undefined ? new Uint8Array(size) : new Uint8Array(size).fill(fill)` | 尺寸非法同样拒绝（TypeError/ValueError，`objects.json` 错误栏；源 `_o_throw_param(0,"Number")`/`_o_throw_value`，`script_object.cpp:4051-4079`）；缺省内容 AHK 不初始化（不可依赖）、JS 零填充（可依赖，对移植脚本更安全）；AHK 有关闭态 InvalidState、JS 无 dispose |
| `Buffer.Size`（get/set） | get：`u8.byteLength`；set：`u8 = withBytes(n, u8)`（`withBytes` 分配新 `Uint8Array(n)`、拷贝重叠前缀） | get 同为字节数；AHK set 原位 realloc 保址（`script_object.cpp:4057-4081` case `P_Size` + `Resize :4085-4098`），等价式重分配+拷贝——地址不暴露（`stdlib.md` §5.1）故差异不可观察；共享引用者必须把返回值赋回（AHK 原位生效） |
| `Map.__Enum` | `for (const [k, v] of m) { if (max !== null && i++ >= max) break; cb(k, v); }` | 同为插入序 (key, value)（`script_object.cpp:3413-3438`、`:2048-2052`）；AHK 单变量形态只产值，JS 元组取值需 `for (const [, v] of m)` |
| `Map.Clear` | `m.clear()` | 同为清空全部（`script_object.cpp:704`、`:1918-1921`）；receiver 非 Map 时方法调用自然抛 TypeError |
| `Map.Clone` | `new Map(m)` | 同为浅拷贝、插入序保留（`script_object.cpp:2078-2084` → CloneTo） |
| `Map.Delete` | `m.has(k) ? (old => (m.delete(k), old))(m.get(k)) : (() => { throw new Error("UnsetItemError") })()` | 返回被删值；缺键 v2.0 默认抛 UnsetItemError（`script_object.cpp:1884-1890` `_o_throw(..., ErrorPrototype::UnsetItem)` + `script.h:1512`），v2.1 草案返回 unset（`:1890`）；等价式按 v2.0 目标抛错 |
| `Map.Has` | `m.has(k)` | 键存在性一致（`script_object.cpp:2059-2066`）；键按类型区分（两侧 Map 语义相同） |
| `Map.Set` | 单对：`m.set(k, v)`；多对：`for (const [k, v] of pairs) m.set(k, v)` | 返回 Map 本身可链（`script_object.cpp:1339-1347` `_o_return(this)`）；奇数参数经 `_o_throw(ERR_PARAM_COUNT_INVALID)` 抛**基础 `Error`**（`:1341-1342` → `script.h:421`/`:391` `Error(msg)` 默认原型），JS 单对签名由调用形态保证 |
| `Map.__Item`（get/set） | get 同 `Map.Get`；set：`m.set(k, v)`，`v` 为 unset 时 `m.delete(k)` | 与 `Get` 共路（`script_object.cpp:1305-1336` case `P___Item` + `:1296` `Object_Member(Get, __Item, 0, IT_CALL, 1, 2)`）；缺键先查脚本自定义 `Default` 属性（`:1313-1316`），v2.0 抛 UnsetItemError（`:1316-1317` + `script.h:1512`）、v2.1 返回 unset（`:1318`）；set 收到 unset 参数删除键（`:1331-1332`）、`SetItem` 失败抛 MemoryError（`:1333-1334`） |
| `Map.Count` | `m.size` | 同为条目数（`script_object.cpp:1929-1932` `_o_return(mCount)`）；无实质差异 |
| `Map.__New` | `new Map([[k1, v1], [k2, v2], ...])`；奇数参数形态 `(() => { throw new Error("Invalid number of parameters.") })()` | 分发到 `Map::Set` 逐对插入（`script_object.cpp:1295` + `:1339-1347`）；奇数参数抛基础 Error（`:1341-1342`）；返回 Map 本身可链——JS 构造器返回实例同效 |
| `Map.Get` | `m.has(k) ? m.get(k) : dflt !== undefined ? dflt : (() => { throw new Error("UnsetItemError") })()` | `Get` 即 `__Item` 的 IT_CALL 形态（`script_object.cpp:1296`，参数 1–2）；缺键查 `Default` 属性→v2.0 抛 UnsetItemError（`:1313-1317`）、v2.1 返回 unset（`:1318`）；`Default` 是脚本自定义属性而非内置成员——移植脚本可定义同名 getter 还原 |
| `Object.Clone` | `{ ...o }` | 同为浅拷贝；AHK class 实例（native base ≠ Object 原型）抛 TypeError（`script_object.cpp:2068-2076`），JS 不抛；JS 仅拷可枚举自有字符串键、原型链不随 spread 复刻 |
| `Object.DefineProp` | `Object.defineProperty(o, name, translate(desc))`（键翻译 `Value→value`、`Get→get`、`Set→set`） | 返回 receiver 可链（`script_object.cpp:2345`、`:2368`）；AHK 描述符独有 `Call` 方法属性与 typed-field `Type`/`Pack`/`Offset`（`:2357`、`:2321-2352`）不恢复；描述符缺全部键抛参数错（`:2359-2362`） |
| `Object.DeleteProp` | `Object.hasOwn(o, name) ? (old => (delete o[name], old))(o[name]) : undefined` | 删除并返回旧值、缺属性返回 unset 不抛（`script_object.cpp:1865-1872`）；残余差异：不可配置属性在严格模式（模块默认）下 `delete` 直接抛 TypeError、读写前须查 `configurable`，AHK 字段无不可配置概念总能删——删除结果一律以 `Object.hasOwn` 复核 |
| `Object.GetOwnPropDesc` | `d = Object.getOwnPropertyDescriptor(o, name); d === undefined ? undefined : d.get !== undefined || d.set !== undefined ? { Get: d.get, Set: d.set } : { Value: d.value }` | AHK 键为 `Value`/`Get`/`Set`/`Call`（`script_object.cpp:2407-2423`）、缺属性返回 unset（`:2397-2401`）；JS 描述符带 `writable`/`enumerable`/`configurable` 而 AHK 无；typed-field 的 `Type`/`Offset`（`:2411-2417`）与 `Call` 不恢复 |
| `Object.HasOwnProp` | `Object.hasOwn(o, name)` | 自有字段存在即真（`script_object.cpp:2054-2057` FindField）；无实质差异 |
| `Object.OwnProps` | `for (const [name, value] of Object.entries(o)) cb(name, value)` | 名在前值在后（`script_object.cpp:3256-3265`）；AHK 跳过 NoEnumGet 动态属性（`:3256-3275`），JS 只列可枚举键，同向；键序差异：JS 整数样式键前置、AHK 字段插入序 |
| `Func.Bind` | `fn.bind(null, ...params)` | 左起填充、返回新函数一致（`script_object.cpp:3763-3766`）；无实质差异（AHK 函数无接收者，thisArg 恒 null） |
| `Func.Call` | `fn.call(null, ...args)` | 直调并传播错误（`script_object.cpp:3759-3761`）；差异仅 JS 另有 thisArg 概念（AHK 无） |
| `Func.MinParams` | `fn.length` | 必需形参个数即 `mMinParams`（`script_object.cpp:3797`）；JS 语法禁必填跟在可选之后，合法函数上两值恒等；rest 参数不计（两侧一致） |
| `Func.Name` | `fn.name` | 函数名（`script_object.cpp:3796`）；差异：匿名函数/`new Function` 构造为空串或推断名，AHK 全局函数必有名 |
| `RegExMatchObject.Pos` | `m ? m.pos + 1 : 0`（全匹配，n=0） | 1-based、无匹配 0（`lib/regex.cpp:261` `mOffset+1`）；Rime `pos` 为 0-based、无匹配 `null`（`regex.ts:5-6`），等价式消解；按子模式序号 `Pos(n≥1)` 不可恢复（见下方注记） |
| `RegExMatchObject.Len` | `m ? m.len : 0` | 子模式长度、无匹配 0（`lib/regex.cpp:262`）；按序号形态同上不可恢复 |
| `RegExMatchObject.Name` | 按名恒等：`m && m.named && n in m.named ? n : undefined`；无参 `Name()` 恒 `""` | 无参 = 全匹配，源注释"0 never has a name"（`lib/regex.cpp:263` + `script_object.h:948`）；按名查子模式返回该名本身（名解析 `lib/regex.cpp:223-250`，命中即 `mPatternName[p]`）；未命中 AHK 抛 ValueError（`objects.json` 错误栏），Rime 结果对象无该成员 → 显式 TypeError |
| `RegExMatchObject.__Get`（`m.year`） | `m && m.named && name in m.named ? m.named[name] : undefined` | 命名捕获直取（`lib/regex.cpp:254-260` + 名解析 `:223-250`）；AHK 未命中抛 ValueError、等价式返回 undefined——静默差异，移植时以 `in` 前置判断消解；非字符串下标抛用法错（`:254-257`）无对应形态 |
| `RegExMatchObject.__Enum` | `m ? [[0, m.value], ...Object.entries(m.named ?? {})] : []`（1 变量形态取各对第二项） | 子模式枚举 (名或序号, 值)，全匹配键恒 0（`script_object.cpp:3440-3464` + 名解析 `:223-250`）；AHK 还枚举**未命名**捕获（键=子模式序号），Rime `named` 只存命名组、未命名分组仅可按下标读 `groups`（残余差异）；无匹配时 AHK 仍产出空值项、Rime `null` → 空枚举 |
| `RegExMatchObject.__Item`（get） | `m ? (i >= 0 && i < m.groups.length ? m.groups[i] : (() => { throw new RangeError("ValueError") })()) : undefined`（名形态 `m.named?.[name]`，未命中语义见右） | 数组下标与 AHK p 同轴（`groups[0]` = 全匹配，`regex.ts:13-14` ↔ `lib/regex.cpp:260` `mOffset[p*2]`）；名查找**大小写不敏感**且重名优先已匹配者（`_tcsicmp` `:234`、`:236-246`），JS 键精确匹配——移植须归一大小写；越界/负下标抛 ValueError（`:249-250` `_o_throw_param`）；名未命中 AHK 抛 ValueError 而等价式返回 undefined（静默差异，同 `__Get` 行）；未参与捕获组两侧读值口径需对齐（AHK 以 `mOffset` 拼子串、Rime 存 `undefined`） |
| `RegExMatchObject.Count` | `m ? m.count : 0` | 即捕获组数 `mPatternCount - 1`（`lib/regex.cpp:217`，p=0 全匹配不计），与 `regex.ts:17-18` `count` 同义；无匹配 Rime `null` → 0 |
| `RegExMatchObject.Mark` | `m?.mark ?? ""` | 源 `mMark` 无选项写入、恒空串（`lib/regex.cpp:218`）；Rime `mark` 保留字段恒缺（`regex.ts:20-21`，mark 动词被拒）→ 两侧恒 `""`，无实质差异 |

`RegExMatchObject` 前置注记：AHK 的子模式 p=0 是全匹配、p≥1 是**命名捕获**（`lib/regex.cpp:185-204` "Copy subpattern names"，`mPatternName[0]=NULL`），不是多模式数组——`m.year` 形态成立；Rime 结果为单匹配 + `groups`/`named`（`sdk/src/runtime-language/regex.ts:4-19`），无匹配返回 `null`。按**序号**的形态（`Pos(2)`、`Len(2)`、分组偏移）依赖 JS RegExp 不暴露的分组偏移，成员不存在、调用显式失败（`stdlib.md` §6"显式失败不静默错配"）。

### `unsupported-by-policy`（16）

| 成员 | 裁剪条款 | 理由与替代 | compatibilityTest |
|---|---|---|---|
| `Array.Capacity` | `stdlib.md` §5.4 | 引擎分配槽位数是内部布局（与 `ObjGetCapacity` 同族）；替代 `arr.length` + 按需重分配 | `docs/api/stdlib.md` |
| `Buffer.Ptr` | `stdlib.md` §5.1 + §5.4 | 缓冲区裸地址不暴露；替代 `Uint8Array` 视图 + 显式拷贝 | `docs/api/stdlib.md` |
| `Map.Capacity` | `stdlib.md` §5.4 | 引擎容量槽位是内部布局（与 `ObjSetCapacity` 同族）；set 非数 TypeError、失败 MemoryError，无范围错误（`script_object.cpp:2003-2047`），替代 `m.size` + 按需重分配 | `docs/api/stdlib.md` |
| `ComObject.__Item` | `native-interop.md` §1-§3 | COM 面走隔离插件；下标/调用替代 `automation.find` 与输入订阅 | `docs/api/native-interop.md` |
| `ComObject.__Value` | `native-interop.md` §1-§3 | 同上——VARIANT 解包不出脚本面 | `docs/api/native-interop.md` |
| `ComObject.Ptr` | `native-interop.md` §1-§3（§5.1 第 1 条） | 裸 COM 指针不给 JS；替代 branded id | `docs/api/native-interop.md` |
| `ComObject.__Enum` | `native-interop.md` §1-§3 | SAFEARRAY 枚举面（`script_com.cpp:1309-1316` + `:1386-1393` `FResult`）走隔离插件；替代复制为快照后在 JS 侧迭代 | `docs/api/native-interop.md` |
| `ComObject.Clone` | `native-interop.md` §1-§3 | SAFEARRAY 拷贝面（`script_com.cpp:1396-1403` `SafeArrayCopy`）；替代导出显式快照 | `docs/api/native-interop.md` |
| `ComObject.MaxIndex` | `native-interop.md` §1-§3 | SAFEARRAY 上界读取面（`script_com.cpp:1406-1409` `SafeArrayGetUBound`）；替代维度元数据随快照导出 | `docs/api/native-interop.md` |
| `ComObject.MinIndex` | `native-interop.md` §1-§3 | SAFEARRAY 下界读取面（`script_com.cpp:1412-1415` `SafeArrayGetLBound`）；同上 | `docs/api/native-interop.md` |
| `Func.IsByRef` | `stdlib.md` §5.7 | ByRef 出参是 AHK 声明模型（`design-review.md`：不进内核）；替代显式返回值 | `docs/api/stdlib.md` |
| `Func.IsOptional` | `stdlib.md` §5.7 | 参数形状反射是编译期信息；替代类型声明与文档 | `docs/api/stdlib.md` |
| `Func.IsBuiltIn` | `stdlib.md` §5.7 | BIF/脚本函数分类是 AHK 声明模型；`[native code]` 启发式对 JS 包装函数必然误判（Rime 模块函数皆为包装） | `docs/api/stdlib.md` |
| `Func.IsVariadic` | `stdlib.md` §5.7 | 同 `IsOptional`；替代 rest 参数类型声明 | `docs/api/stdlib.md` |
| `Func.MaxParams` | `stdlib.md` §5.7 | 同 `IsOptional`；`fn.length` 只答必需参数（`MinParams` 有真等价，最大参数数无） | `docs/api/stdlib.md` |
| `Object.__Ref` | `stdlib.md` §5.7 | `PropRef`/`VarRef` 即 ByRef 引用对象，不进内核；替代显式返回值 | `docs/api/stdlib.md` |
