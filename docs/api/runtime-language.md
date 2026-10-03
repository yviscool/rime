# Runtime and Language Compatibility

状态：138 项已分类——51 `js-native`、62 `contract-only`（含 18 项 L4 还原保留候选）、5 `implemented`、20 `unsupported-by-policy`。判定真值见 §2 映射表；状态词汇的权威是 [`stdlib.md`](./stdlib.md)（§3 四状态口径、§5 黑名单、§6 L4 还原保留层）。

命名与参数形状不在本页裁定：公共 API 命名遵循 [`AHK-TS-WINDOWS-API-DESIGN.md`](../AHK-TS-WINDOWS-API-DESIGN.md) §0 的六个核心设计问题与 [`future-runtime.md`](./future-runtime.md)（stdlib.md 开篇）。本页只登记**判定、证据与语义差异**，不给出 API 形状；AHK 名称仅作能力研究样本与语义核对测试的 oracle（计划 §0.1：不存在 AHK 名称兼容层）。

范围：`core-builtins.json` 全部 101 项 + `coverage.json` 中 `domain=runtime-language` 的 35 项 + `RegExMatch`/`RegExReplace` 2 项，合计 138 项。台账原值 `sdk-owned`/`contract-only` 是过渡态（stdlib.md §3），在本页收敛为四状态判定；本页只做判定与证据登记，不改台账 JSON。

源码证据前缀 `rime-research/AutoHotkey-alpha/source/`（表中短路径即相对该前缀；仓库内文件给完整路径）。纯字符串、日期和正则优先使用 TS/ECMAScript 标准库并配对照原版的语义还原测试；影响 Runtime 生命周期、调度和全局状态的函数一律映射到显式 Runtime API，不复制 AHK 的隐式伪线程与全局可变设置。

## 1. 四状态术语

| 状态 | 本页判定规则 | 计为终态 |
|---|---|---|
| `implemented` | 已有 TS 服务表面 + contract 测试（测试 ID 入台账）；本页给出服务模块与改名注记 | 是 |
| `js-native` | L0 已覆盖：本页给等价 TS 表达式与可观察差异（无差异写“无”），零专属代码，映射记录替代测试 ID | 是 |
| `contract-only` | 过渡态：目标模块与计划阶段已定、待实现；含 §2.2.2 的 18 项 L4 还原保留候选（实现后 → `implemented`） | 否 |
| `unsupported-by-policy` | stdlib.md §5 黑名单或计划 §0.3 已拍板裁剪：本页给理由与等价物/替代 | 是（需理由与替代） |

终态公式（stdlib.md §3）：`终态 = implemented | js-native | unsupported-by-policy`。台账收敛规则：`sdk-owned` 收敛到 `contract-only`（差异有用户价值的进 §2.2.2 还原保留候选），`contract-only` 服务表面落地后收敛为 `implemented`，`unsupported-by-policy` 必须能指到 stdlib.md §5 的条目（三条已定案见 §5）。

## 2. 映射表（138 项）

台账标记：**CB** = `core-builtins.json`，**COV** = `coverage.json`。

### 2.1 `js-native`（51）

等价式为可直接使用的 TS/ECMAScript 表达式；差异列记录可观察行为不一致处（均不转入 §2.2.2 还原保留：差异集中在错误路径、区域设置或低频参数形态，按 stdlib.md §3 记录即可）。

| 函数 | 台账 | 等价 TS 表达式 | 可观察差异 | 证据 |
|---|---|---|---|---|
| `Abs` | CB | `Math.abs(v)` | 非数值/空串 AHK 抛 Type 参数错（math.cpp:199-200），JS 得 NaN；AHK 保留 int64/浮点 token 类型 | `lib/math.cpp:197` |
| `ACos` | CB | `Math.acos(v)` | 非数值/空串 AHK 按 0 参与计算，JS 得 NaN；输入落在 [-1,1] 之外时 AHK 抛参数错（math.cpp:249-252），JS 得 NaN | `lib/math.cpp:245` |
| `ASin` | CB | `Math.asin(v)` | 同 ACos | `lib/math.cpp:245` |
| `ATan` | CB | `Math.atan(v)` | 非数值/空串 AHK 按 0 计算，JS 得 NaN；Float NaN AHK 抛参数错 | `lib/math.cpp:263` |
| `ATan2` | CB | `Math.atan2(y, x)` | 两参各做 NaN 校验（math.cpp:275-276），非数值 AHK 按 0；JS 得 NaN | `lib/math.cpp:271` |
| `Ceil` | CB | `Math.ceil(v)` | 非数值/空串 AHK 记 0（math.cpp:102-105），JS 得 NaN；结果夹到 int64（118），超出 int64 可表示范围时与 JS 不同 | `lib/math.cpp:96` |
| `Chr` | CB | `String.fromCodePoint(n)` | AHK 先把参数截断为整数（string.cpp:1341），JS 非整数抛 RangeError；码点区间（<0 或 >0x10FFFF）两端都拒绝（1344-1345） | `lib/string.cpp:1338` |
| `Cos` | CB | `Math.cos(v)` | 非数值/空串 AHK 按 0 计算，JS 得 NaN；Float NaN AHK 抛参数错 | `lib/math.cpp:225` |
| `DefineProp` | CB | `Object.defineProperty(obj, key, desc)` | AHK 描述符字段名与 JS 不同（Enum/Editable vs enumerable/writable 等，script_object_bif.cpp:217-223），值/get/set 语义等价；目标必须是对象/原型 | `script_object_bif.cpp:217` |
| `Exp` | CB | `Math.exp(v)` | 非数值/空串 AHK 按 0 计算得 1，JS 得 NaN/Infinity | `lib/math.cpp:280` |
| `Floor` | CB | `Math.floor(v)` | 同 Ceil | `lib/math.cpp:96` |
| `GetMethod` | CB | `Object(o)[name]` | AHK 找不到返回 unset（script_object_bif.cpp:277-282），JS 得 undefined；COM 原型直接拒绝（255-256）；第 3 参做形参个数校验（261-273）无 JS 等价 | `script_object_bif.cpp:252` |
| `HasBase` | CB | `Object.prototype.isPrototypeOf.call(base, Object(value))` | AHK 对原始值取其值原型（script_object_bif.cpp:173-184），表达式用 Object() 包装后等价；参数为 unset/非对象时两端均判 false/抛错形态不同 | `script_object_bif.cpp:187` |
| `HasMethod` | CB | `typeof Object(o)[name] === "function"` | 同 GetMethod：可选第 3 参形参个数校验（script_object_bif.cpp:261-273）无内建等价，移植侧需自行校验 | `script_object_bif.cpp:252` |
| `HasProp` | CB | `name in Object(o)` | AHK 对 COM 对象抛错（script_object_bif.cpp:211-212）、原始值走值原型；表达式对普通对象等价（含原型链） | `script_object_bif.cpp:208` |
| `IsAlnum` | CB | `/^[0-9a-zA-Z]*$/.test(s)` | 仅 ASCII 字母数字；空串 AHK 记 true（script2.cpp:2187-2195）；CaseSense=Locale 时改走 IsCharAlphaNumeric（2190）JS 无等价；纯数字参数 AHK 抛 Type 错（2104-2107） | `script2.cpp:2086` |
| `IsAlpha` | CB | `/^[a-zA-Z]*$/.test(s)` | 空串 AHK 记 true（script2.cpp:2196-2205，注释 2197）；Locale 分支走 IsCharAlpha（2200）；纯数字参数 AHK 抛 Type 错 | `script2.cpp:2086` |
| `IsDigit` | CB | `/^[0-9]*$/.test(s)` | 仅 ASCII 数字（script2.cpp:2166-2174）；空串记 true；Locale 不影响本项 | `script2.cpp:2086` |
| `IsFloat` | CB | `typeof v === "number" ? !Number.isInteger(v) : (/\./.test(v) && !Number.isNaN(Number(v)))` | AHK 按 token 区分：浮点 token 即 true、1.0 这类整数值浮点为 Integer 判 false（script2.cpp:2109-2119）；JS 单一 number 无此区分；字符串走 IsNumeric==PURE_FLOAT（2157-2158，需含小数点），表达式同为“含小数点且可解析”，`1e3` 这类纯指数串两端判定可能不一致 | `script2.cpp:2086` |
| `IsInteger` | CB | `typeof v === "number" ? Number.isInteger(v) : /^[+-]?\d+$/.test(v)` | AHK 对浮点 token 恒判 false，含 1.0（script2.cpp:2115-2119）而 Number.isInteger(1.0) 为 true；字符串走 IsNumeric(allowFloat=false)（2154-2155，接受 0x 十六进制），JS 正则不含 0x | `script2.cpp:2086` |
| `IsLower` | CB | `/^[a-z]*$/.test(s)` | 空串记 true（script2.cpp:2215-2223）；Locale 分支走 IsCharLower（2218）；纯数字参数 AHK 抛 Type 错 | `script2.cpp:2086` |
| `IsNumber` | CB | `typeof v === "number" ? true : (String(v).trim() !== "" && Number.isFinite(Number(v)))` | AHK 字符串走 IsNumeric（script2.cpp:2151-2152 → util.cpp:326-372）：允许前导空白、接受 0x 十六进制、拒绝 0b/0o 与中缀空格；JS Number() 更宽松（0b11、尾随空白也可）。空串 AHK 记 false，表达式已排除 | `script2.cpp:2086` |
| `IsObject` | CB | `v !== null && ["object", "function"].includes(typeof v)` | AHK 把 ComObject/Func 等 IObject 全判 true（script_object_bif.cpp:42-45）；表达式含 function（class 同为 function）；null/undefined 两端均判 false | `script_object_bif.cpp:42` |
| `IsSetRef` | CB | `typeof x !== "undefined"` | AHK 判 VarRef 指向的变量是否赋值（script2.cpp:2246-2254），未赋值引用返回 false；JS 无该语言状态，同名判定只能用 typeof 守卫（let TDZ 访问会抛 ReferenceError） | `script2.cpp:2246` |
| `IsSpace` | CB | `/^\s*$/.test(s)` | AHK 用 _istspace（script2.cpp:2224-2232），与 JS \s 字符集在 U+0085/U+2028/U+FEFF 等边缘码点不一致；空串记 true | `script2.cpp:2086` |
| `IsTime` | CB | `s => { const m = /^(\d{4})(\d{2})(\d{2})(\d{2})?(\d{2})?(\d{2})?$/.exec(s); if (!m) return false; const d = new Date(+m[1], +m[2] - 1, +m[3], +(m[4] ?? 0), +(m[5] ?? 0), +(m[6] ?? 0)); return d.getFullYear() === +m[1] && d.getMonth() === +m[2] - 1 && d.getDate() === +m[3]; }` | AHK 走 YYYYMMDDToSystemTime 校验（script2.cpp:2160-2164），接受 YYYYMMDD 与 YYYYMMDDHH24MISS；ECMAScript 无日期字符串格式校验，表达式为自写校验（Date 回环比对以修正月份/日期溢出）；空串/非法日期记 false | `script2.cpp:2086` |
| `IsUpper` | CB | `/^[A-Z]*$/.test(s)` | 空串记 true（script2.cpp:2206-2214）；Locale 分支走 IsCharUpper（2209）；非字母字符判 false | `script2.cpp:2086` |
| `IsXDigit` | CB | `/^(?:0[xX])?[0-9a-fA-F]*$/.test(s)` | 允许 0x 前缀（script2.cpp:2175-2178）、仅 ASCII 十六进制（2181）；空串记 true | `script2.cpp:2086` |
| `Ln` | CB | `Math.log(v)` | v<0 AHK 抛参数错（math.cpp:292-294），JS 得 NaN；非数值 AHK 按 0 得 -Infinity，JS 得 NaN | `lib/math.cpp:288` |
| `Log` | CB | `Math.log10(v)` | 同 Ln | `lib/math.cpp:288` |
| `LTrim` | CB | `s.replace(/^[\t ]+/, "")` | 不能用 trim()：AHK 默认 cutset 是空格+Tab（string.cpp:1551）而 trim() 裁全部 Unicode 空白；自定义 omit 是字符集合（1551），需按 [.] 转义 | `lib/string.cpp:1540` |
| `Max` | CB | `Math.max(...vs)` | 非数值/空串 AHK 抛 Type 参数错（math.cpp:184-186），JS 得 NaN；整数/浮点分桶比较后返回原 token（166-192），值一致；JS 无参得 Infinity，AHK 强制至少一参 | `lib/math.cpp:152` |
| `Min` | CB | `Math.min(...vs)` | 同 Max | `lib/math.cpp:152` |
| `ObjBindMethod` | CB | `o[name].bind(o, ...args)` | AHK 允许省略 name，绑定对象自身作为可调用目标（script_object_bif.cpp:90-102），JS 需对象自身可调用；其余等价 | `script_object_bif.cpp:84` |
| `ObjGetBase` | CB | `Object.getPrototypeOf(Object(o))` | AHK 对原始值返回值原型、不可设基的对象返回 unset（script_object_bif.cpp:141-170）；表达式用 Object() 包装，null 原型两侧均可表达 | `script_object_bif.cpp:141` |
| `ObjHasOwnProp` | CB | `Object.hasOwn(o, name)` | 无：AHK FindField 只查自有字段（script_object.cpp:2054-2057），与 Object.hasOwn 等价（AHK 无 symbol 键） | `script_object.cpp:2054` |
| `ObjOwnPropCount` | CB | `Reflect.ownKeys(o).length` | AHK 计全部自有字段（含不可枚举，script_object.cpp:1924-1927），故不能用 Object.keys；Reflect.ownKeys 额外含 symbol 键（AHK 无） | `script_object.cpp:1924` |
| `ObjOwnProps` | CB | `Reflect.ownKeys(o).map(k => [k, o[k]])` | AHK 枚举全部自有字段并给出 name+value（script_object.cpp:2042-2046、3256-3265），JS Object.keys 只给 enumerable；表达式含 symbol 键是额外项 | `script_object.cpp:2042` |
| `ObjSetBase` | CB | `Object.setPrototypeOf(o, base)` | AHK 拒绝非对象 base（script_object_bif.cpp:149-151）且不触发 meta-function；JS 额外允许 null（设为无原型） | `script_object_bif.cpp:141` |
| `Ord` | CB | `s.codePointAt(0) ?? 0` | 代理对合并为码点（string.cpp:1329-1330）与 JS 一致；空串 AHK 记 0，表达式已用 ?? 0 吸收 | `lib/string.cpp:1319` |
| `Props` | CB | `function* (o) { for (const k in o) yield [k, o[k]]; }` | AHK 沿基类链逐层枚举并按名排序、跳过 NoEnumGet/无 getter 的属性（script_object.cpp:3311-3364），且可对原始值枚举其值原型（script_object_bif.cpp:226-249）；JS for-in 按插入序、只给 enumerable 名 | `script_object_bif.cpp:226` |
| `RTrim` | CB | `s.replace(/[\t ]+$/, "")` | 同 LTrim | `lib/string.cpp:1540` |
| `Sin` | CB | `Math.sin(v)` | 同 Cos | `lib/math.cpp:215` |
| `Sqrt` | CB | `Math.sqrt(v)` | v<0 AHK 抛参数错（math.cpp:292-294），JS 得 NaN；非数值 AHK 按 0 得 0，JS 得 NaN | `lib/math.cpp:288` |
| `StrCompare` | CB | `a.toLowerCase() < b.toLowerCase() ? -1 : a.toLowerCase() > b.toLowerCase() ? 1 : 0` | 默认大小写不敏感 = _tcsicmp（string.cpp:1162/1180）；CaseSense=Locale/Logical 走 lstrcmpi/StrCmpLogicalW（1181-1182）无 JS 等价；非 ASCII 折叠规则与 _tcsicmp 有边缘差异 | `lib/string.cpp:1159` |
| `StrLen` | CB | `s.length` | 无：AHK 返回 TCHAR（UTF-16 code unit）个数（string.cpp:1194-1196），与 .length 等价 | `lib/string.cpp:1191` |
| `StrLower` | CB | `s.toLowerCase()` | AHK 走 Win32 CharLower（string.cpp:348-349，按用户区域设置），JS 与 locale 无关；ASCII 输入无差异，土耳其 İ/i 等区域规则有差异 | `lib/string.cpp:338` |
| `StrUpper` | CB | `s.toUpperCase()` | AHK 走 Win32 CharUpper（string.cpp:350-351）；差异同 StrLower | `lib/string.cpp:338` |
| `Tan` | CB | `Math.tan(v)` | 同 Cos | `lib/math.cpp:235` |
| `Throw` | CB | `throw e` | AHK Throw 是可作表达式调用的 BIF（error.cpp:154），异常原型是 AHK Error 家族；JS throw 是语句、可抛任意值——模型差异，无专属适配代码 | `error.cpp:154` |
| `Trim` | CB | `s.replace(/^[\t ]+/, "").replace(/[\t ]+$/, "")` | 同 LTrim：默认 cutset 空格+Tab（string.cpp:1551），不可直接用 String.trim()；对象参数 AHK 抛参数错（1542-1543） | `lib/string.cpp:1540` |


### 2.2 `contract-only`（62）

过渡态，不计终态（stdlib.md §3）。62 = 44 项常规能力 + 18 项 L4 还原保留候选（stdlib.md §6），分列两个小节：常规能力按 L2 模块归属排期，还原保留候选统一落 `@rime/runtime-language` 但实现顺序不同（先命名提案、后实现与对照测试），故单独标记。

#### 2.2.1 常规能力（44）

目标模块按 stdlib.md §1 L2 命名（`fs`/`registry` 由台账 `@rime/storage` 改名），计划阶段取自 `AHK99-IMPLEMENTATION-PLAN.md`。

| 函数 | 台账 | 目标模块 | 计划阶段 | 为何不是 js-native 或 L4 还原 | 证据 |
|---|---|---|---|---|---|
| `ComObjActive` | CB | `@rime/native-interop` | M8（audit-gaps：COM/VARIANT 边界文档化） | COM 需 Apartment 归属与跨线程封送，非纯函数；stdlib.md §5.1 禁裸 COM 指针，只能经隔离插件面暴露；计划 §0.3 现把它记作 unsupported-by-policy，本判定取 contract-only（见 §5 冲突注记） | `script_com.cpp:153` |
| `ComObjConnect` | CB | `@rime/native-interop` | M8 | 同 ComObjActive：事件 sink 回调必须是可取消订阅并回 JS 线程调度 | `script_com.cpp:272` |
| `ComObjFlags` | CB | `@rime/native-interop` | M8 | 同 ComObjActive：COM marshalling 标志属于隔离面配置，不是脚本可移植能力 | `script_com.cpp:458` |
| `ComObjGet` | CB | `@rime/native-interop` | M8 | 同 ComObjActive：按 ProgID/CLSID 获取对象需要跨进程 COM，须 capability 门禁 | `script_com.cpp:64` |
| `ComObjQuery` | CB | `@rime/native-interop` | M8 | 同 ComObjActive：QI 结果仍是隔离面对象，不能返回裸指针 | `script_com.cpp:523` |
| `ComObjType` | CB | `@rime/native-interop` | M8 | 同 ComObjActive：可改为对隔离面包装对象的只读查询，等 M8 COM 边界定档 | `script_com.cpp:383` |
| `ComObjValue` | CB | `@rime/native-interop` | M8 | 同 ComObjActive：VARIANT 解包涉及类型归一化，属隔离面能力 | `script_com.cpp:374` |
| `CoordMode` | COV | `@rime/input` | M3（计划 line 97 显式 API） | 坐标空间必须是 mouse/screen 调用的参数而非进程全局态（stdlib.md §2 设置原则），当前 input/automation 尚无 coords 参数（sdk/src/input.ts 无该选项） | `lib/functions.h:52` |
| `Exit` | COV | `rime:runtime` | M3（HostLifecycle） | 退出必须由宿主执行：engine/js/src/host.cpp:211-220 只有 ping/delay/取消/inspect 桥，无 JS exit 面；不能在模块内自起脚本泵（AGENTS 线程与生命周期规则） | `script.cpp:1220` |
| `ExitApp` | COV | `rime:runtime` | M3（HostLifecycle） | 同 Exit（script.cpp:1240 → Script::ExitApp），需 OnExit 订阅可排空并诊断 | `script.cpp:1240` |
| `FileOpen` | CB | `@rime/fs` | M4（fs service + File 对象 31 成员） | 返回有所有权的 File 对象（编码、缓冲、GC/显式/shutdown 三路径），不是纯函数；台账原值 @rime/storage 按 stdlib.md §1 L2 命名改为 @rime/fs | `TextIO.cpp:1038` |
| `GetKeyName` | COV | `@rime/input` | M3（计划 line 97 输入映射） | 键名↔SC/VK 反查依赖当前键盘布局与扩展键标志（script2.cpp:2312-2317），非纯 L0 映射；sdk/src/send.ts 的键表（:141-362）尚未导出为公共 API | `script2.cpp:2312` |
| `GetKeySC` | COV | `@rime/input` | M3 | 同 GetKeyName（scan code 随布局变化，script2.cpp:2303-2311） | `script2.cpp:2303` |
| `GetKeyVK` | COV | `@rime/input` | M3 | 同 GetKeyName（script2.cpp:2294-2302） | `script2.cpp:2294` |
| `IsLabel` | COV | `rime:runtime` | M3 后（宿主可检查面） | 脚本内没有 label 概念（计划 §0.1：不实现 AHK 脚本语言），但“命名 handler 是否存在”属宿主可检查接口，应由 inspect/CLI 读出；无现成注册表 | `lib/functions.h:160` |
| `ListHotkeys` | COV | `@rime/input` | M3 | 需读出已注册 hotkey 与当前 suspend/policy 快照（input.ts:534-595 已有 setTimer/suspend/policy，但无列表读出面） | `script2.cpp:863` |
| `ListLines` | COV | `rime:runtime` | M3（诊断/Trace） | AHK 的脚本行日志对应我们的 Action Trace 与 runtime.inspect()（sdk/src/index.ts:34），尚无脚本可读的日志面（engine 无 console） | `script2.cpp:828` |
| `ListVars` | COV | `rime:runtime` | M3（计划 line 97：SchedulerPolicy 显式 API） | 需读出调度策略与运行期状态的可检查面（stdlib.md §1 L3），当前只有 runtime.inspect() 的有限快照 | `script2.cpp:856` |
| `OutputDebug` | COV | `rime:runtime` | M3（计划 line 97） | engine/hosts 无 JS console，也没有 OutputDebugString 出面（Win32 侧只在 error.cpp 内部使用）；诊断必须走宿主可检查接口，不能是散落全局函数（stdlib.md §5.5） | `script2.cpp:2630` |
| `Pause` | COV | `rime:runtime` | M3（HostLifecycle） | input.suspend()（sdk/src/input.ts:588）只覆盖 hotkey 分发，Pause 还要停 timer/线程派发——需 lifecycle 级挂起状态与 Trace | `script.cpp:12468` |
| `Persistent` | COV | `rime:runtime` | M3（HostLifecycle） | 驻留与否是宿主生命周期策略（脚本空闲后的退出条件），非脚本可变全局态 | `lib/functions.h:219` |
| `PostMessage` | COV | `@rime/window` | M7（Win32 消息层） | 跨进程消息注入需 HWND 稳定 ID 封装与 capability 门禁；input.md:3 记 SendMessage 未实现，window 域无 postMessage 面 | `lib/functions.h:225` |
| `RegCreateKey` | CB | `@rime/registry` | M4（registry service） | 注册表写入是跨进程系统状态，需 capability 门禁与 Action Trace，非 L0/L4 可覆盖；台账原值 @rime/storage 按 stdlib.md §1 改为 @rime/registry | `script_registry.cpp:632` |
| `RegDelete` | CB | `@rime/registry` | M4 | 同 RegCreateKey（删除需审计与失败原因） | `script_registry.cpp:632` |
| `RegDeleteKey` | CB | `@rime/registry` | M4 | 同 RegCreateKey（32/64 视图依赖 SetRegView） | `script_registry.cpp:632` |
| `RegRead` | CB | `@rime/registry` | M4 | 同 RegCreateKey（读也走 windows.registry.read 能力与不可变快照） | `script_registry.cpp:632` |
| `RegWrite` | CB | `@rime/registry` | M4 | 同 RegCreateKey | `script_registry.cpp:632` |
| `Reload` | COV | `rime:runtime` | M3（HostLifecycle） | 重载 = 卸载并重新装载脚本，须走版本化 Host ABI 的 load/unload 契约（AGENTS：有活动 Hook/回调时 unload 必须失败并说明原因） | `script.cpp:1188` |
| `SetCapsLockState` | COV | `@rime/input` | M3（计划 line 97） | 改键状态经 SendInput 注入，属 input 能力；input.md:3 记 Set*KeyState 未实现 | `lib/functions.h:257` |
| `SetControlDelay` | COV | `@rime/automation` | M3 | 延迟是每次调用的 options / 显式 settings，不是隐式全局（stdlib.md §2；settings.window 先例） | `lib/functions.h:258` |
| `SetDefaultMouseSpeed` | COV | `@rime/input` | M3 | 同 SetControlDelay（移动速度应为 mouse.move 的参数） | `lib/functions.h:259` |
| `SetKeyDelay` | COV | `@rime/input` | M3 | 同 SetControlDelay（应映射为 settings.input 显式对象或 send 选项） | `lib/functions.h:260` |
| `SetMouseDelay` | COV | `@rime/input` | M3 | 同 SetControlDelay | `lib/functions.h:261` |
| `SetNumLockState` | COV | `@rime/input` | M3 | 同 SetCapsLockState | `lib/functions.h:262` |
| `SetRegView` | COV | `@rime/registry` | M4（计划 line 105 明确点名） | 注册表 32/64 视图是 registry 服务配置，需并入 registry service 而非全局开关 | `lib/functions.h:263` |
| `SetScrollLockState` | COV | `@rime/input` | M3 | 同 SetCapsLockState | `lib/functions.h:264` |
| `SetStoreCapsLockMode` | COV | `@rime/input` | M3 | 属 send 的策略开关（发送后是否恢复 CapsLock），应进 DispatchPolicy/选项而非隐式全局 | `lib/functions.h:265` |
| `SetWinDelay` | COV | `@rime/window` | M3 | 同 SetControlDelay（窗口操作延迟应是调用选项） | `lib/functions.h:270` |
| `SetWorkingDir` | COV | `rime:runtime` | M3（计划 line 97） | 进程 cwd 是 process 域状态，需 capability 与 Trace；当前 SDK 无 set cwd 面 | `lib/functions.h:271` |
| `SoundGetMute` | CB | `@rime/sound` | M5（winmm） | 音频端点状态需 winmm 调用与能力门禁，非纯函数；stdlib.md §1 未列 sound 模块，按计划 M5 新建，台账原值 @rime/gui-menu 为跨域暂挂 | `lib/sound.cpp:292` |
| `SoundGetName` | CB | `@rime/sound` | M5（winmm） | 同 SoundGetMute | `lib/sound.cpp:292` |
| `SoundGetVolume` | CB | `@rime/sound` | M5（winmm） | 同 SoundGetMute | `lib/sound.cpp:292` |
| `SoundSetMute` | CB | `@rime/sound` | M5（winmm） | 同 SoundGetMute（写入需 Trace 与结果） | `lib/sound.cpp:292` |
| `SoundSetVolume` | CB | `@rime/sound` | M5（winmm） | 同 SoundGetMute | `lib/sound.cpp:292` |


#### 2.2.2 L4 还原保留候选（18）

原 `compat-shim` 桶按新口径并入 `contract-only`（计划 §0.1 禁止 AHK 名称兼容层，stdlib.md §6 把这类工作定义为 L4 还原保留层）。差异研究保留在“还原保留理由”列（JSON `note` 以 `还原保留：` 前缀）；实现以自有命名与参数形状落地，AHK 只作语义测试 oracle；全部 `contractTest: missing`，落点 `sdk/src/runtime-language.ts`。

| 函数 | 台账 | 还原保留理由（可观察差异） | 规模 | 证据 |
|---|---|---|---|---|
| `DateAdd` | COV | AHK 用 YYYYMMDDHH24MISS 字符串 + d/h/m/s/ms/n/y 单位做日期算术（math.cpp:381-…，functions.h:55），JS Date 是毫秒时间戳且没有该字符串类型；需显式解析与单位表 | ~45 行 | `lib/math.cpp:381` |
| `DateDiff` | COV | 同 DateAdd 的字符串与单位方言（math.cpp:426-…，functions.h:56），且 AHK 按单位截断差值；(b-a)/86400000 只覆盖天粒度 | ~40 行 | `lib/math.cpp:426` |
| `Format` | CB | AHK Format 用 {} 顺序占位、{:fmt} printf 风格、U/L/T 大小写选项与 * 宽度取参（string.cpp:1369-1536），ECMAScript 只有 %s 风格的二手 sprintf 与 Intl，无同构语法 | ~90 行 | `lib/string.cpp:1369` |
| `FormatTime` | CB | AHK 自带 token 方言（yyyy/MM/dd hh:mm:ss t…）并回落 Win32 GetDateFormat/GetTimeFormat（string.cpp:25-144），Intl.DateTimeFormat 的组件与本地化默认值都不同，需按 token 表逐项还原 | ~70 行 | `lib/string.cpp:25` |
| `InStr` | CB | 1-based 起始位置、负 StartPos 从右数、Haystack 与 Needles 的空串规则与 CaseSense 参数（string.cpp:1254-…），indexOf/startWith 是 0-based 且无该参数集 | ~35 行 | `lib/string.cpp:1254` |
| `Mod` | CB | 值语义与 JS % 一致（截断取余），但 AHK 除数为 0 抛 ZeroDivision（math.cpp:132-133/141-142）而 JS 得 NaN；非数值参数 AHK 抛 Type 错（147），JS 隐式 Coerce；浮点走 qmathFmod（143） | ~12 行 | `lib/math.cpp:123` |
| `Random` | CB | AHK Random(min, max) 为闭区间 [min,max] 且整数/浮点按参数类型分流（math.cpp:312-377），Math.random() 只有 [0,1)；AHK 无参返回浮点、单参返回 [1,n] | ~20 行 | `lib/math.cpp:312` |
| `RegExMatch` | COV | AHK 返回 Match 对象（Pos/Len/Name/Value/Mark 与 O) 输出变量族）且位置为 1-based（functions.h:236），RegExp.exec 为 0-based 数组；PCRE 构造无 JS 等价时显式抛错（stdlib.md §6） | ~60 行 | `lib/functions.h:236` |
| `RegExReplace` | COV | AHK 的 Limit/Position/O) 输出与 1-based 位置、以及 PCRE→JS 差异需还原（functions.h:237，stdlib.md §6）；replace/replaceAll 只覆盖子集 | ~55 行 | `lib/functions.h:237` |
| `Round` | CB | 半值远离零（-2.5 → -3，math.cpp:48-49），Math.round 半值朝 +∞（-2.5 → -2）；第 2 参 >0 时返回定长十进制字符串（"3.50"，79-81）而非 number；非数值/NaN 抛参数错（37/46）；非整数输入在 n≤0 时带 0.2 修正取整（90） | ~30 行 | `lib/math.cpp:26` |
| `Sort` | CB | AHK 接受 options 字符串（大小写/数值/方言/反转）与分隔符（默认 \n，string.cpp:777-874）返回重组字符串；Array.prototype.sort 需自写比较器且不处理 AHK 的 F/R/N/L 等选项 | ~60 行 | `lib/string.cpp:777` |
| `SplitPath` | COV | AHK 按 Windows 盘符/UNC/扩展名规则输出 name/dir/ext/nameNoExt/drive 五个分量（string.cpp:534-537、648-651，functions.h:280），ECMAScript 无内建路径模块（URL 不等价） | ~35 行 | `lib/string.cpp:648` |
| `StrReplace` | COV | AHK 第 4 参 CaseSense 默认不敏感（string.cpp:358-…），String.prototype.replaceAll 恒为大小写敏感；另有限制次数 Limit 与输出计数 | ~20 行 | `lib/string.cpp:358` |
| `StrSplit` | COV | AHK 接受分隔符集合、OmitChars、MaxParts（string.cpp:401-402，functions.h:286），split 只支持分隔符/正则，需组合实现 OmitChars 与 MaxParts 语义 | ~30 行 | `lib/string.cpp:402` |
| `StrTitle` | CB | AHK 用 Win32 StrToTitleCase 逐词首字母大写（string.cpp:352-353），ECMAScript 无内建 title-case（toLowerCase 只做全小写）；词边界/缩写/撇号规则需按原版还原 | ~15 行 | `lib/string.cpp:338` |
| `SubStr` | CB | 1-based、Start 为负从尾部计、Length 省略到尾与越界规则（string.cpp:1201-…），slice/substring 语义不同（负数方向、0-based） | ~30 行 | `lib/string.cpp:1201` |
| `Type` | CB | AHK 返回 Integer/Float/String/unset/对象类名（script2.cpp:3099-3121），typeof 只有 number/string/object/function 且不分整数浮点；需 typeof + Number.isInteger + 构造器名三段映射 | ~12 行 | `script2.cpp:3099` |
| `VerCompare` | CB | AHK 版本串比较规则（数字段 + 字母段 + 内部 rc/beta 语义）经 VersionSatisfies 落地（string.cpp:1567-1572），JS 无版本比较原语，Intl.Collator 也不等价 | ~40 行 | `lib/string.cpp:1567` |


### 2.3 `implemented`（5）

| AHK 函数 | 台账 | 服务表面（模块） | 改名/语义注记 | contract test |
|---|---|---|---|---|
| `Click` | CB | `@rime/input` | 服务表面 mouse.click(options)（sdk/src/input.ts:760，down/up 对与 count 校验）；AHK Click 的复合字符串语法（如 "x100 y200 Left"）由移植层解析——按 core-builtins.md:18 与 MouseClick 合并审计 | `tests/sdk/send.test.ts,tests/js/input_slice.cpp` |
| `GetKeyState` | COV | `@rime/input` | 服务表面 input.getKeyState(keyName, mode?)（sdk/src/input.ts:419，同步读、capability windows.input.read）；AHK 返回数值/双态，TS 返回 boolean，mode 仅保留 "P" 物理态差异 | `tests/native/input_tests.cpp,tests/js/input_slice.cpp` |
| `Sleep` | COV | `rime:runtime` | 服务表面 runtime.delay(ms, value?, cancellationId?)（sdk/src/index.ts:22，异步可取消；engine/js/src/host.cpp 的 delay 桥）；AHK Sleep 同步阻塞脚本线程，TS 必须 await——按 stdlib.md §4 禁止阻塞 JS 线程，保留改名注记 | `tests/js/runtime_smoke.cpp` |
| `WinActive` | CB | `@rime/window` | 服务表面 window.isActive(query)（sdk/src/window.ts:675，前台窗口探针）；AHK 返回 HWND 数值，TS 返回 boolean——改名 isActive 并用 WindowQuery 替代 WinTitle 字符串 | `tests/native/win32_tests.cpp,tests/js/vertical_slice.cpp` |
| `WinExist` | CB | `@rime/window` | 服务表面 window.exists(query)（sdk/src/window.ts:665，枚举到首个匹配即停）；同 WinActive 的 HWND→boolean 与 WinTitle→WindowQuery 改名 | `tests/native/win32_tests.cpp,tests/js/vertical_slice.cpp` |


### 2.4 `unsupported-by-policy`（20）

| 函数 | 台账 | 理由 | 等价物 / 替代路径 | 证据 |
|---|---|---|---|---|
| `ComCall` | CB | stdlib.md §5.4：vtable 裸调用与裸内存互操作，与 DllCall 同族裁剪 | 进程外插件 / 隔离 COM 面（计划 §0.3 已拍板） | `lib/DllCall.cpp:497` |
| `ComObjFromPtr` | CB | stdlib.md §5.1/§5.4：把裸指针包装成可调用对象（script_com.cpp:78） | 不提供；COM 对象一律经隔离插件面获取 | `script_com.cpp:78` |
| `Critical` | COV | stdlib.md §5.3：AHK 隐式伪线程/抢占模型（Critical 的线程中断语义）明令不暴露 | 协程/临界区排队与合并策略（Action Kernel 的可中断区间，进 Trace） | `lib/functions.h:53` |
| `DllCall` | CB | stdlib.md §5.4：DLL 装载与裸内存互操作 | 能力化的 native 插件（版本化 Host ABI） | `lib/DllCall.cpp:497` |
| `NumGet` | CB | stdlib.md §5.4：按地址读内存 | 以结构化二进制/TypedArray 解析代替 | `lib/interop.cpp:107` |
| `NumPut` | CB | stdlib.md §5.4：按地址写内存 | 以 TypedArray/序列化写入代替 | `lib/interop.cpp:172` |
| `ObjAddRef` | CB | stdlib.md §5.1：裸引用计数，暴露即泄露对象所有权模型 | JS GC 自动管理，无需显式增引用 | `script_object_bif.cpp:68` |
| `ObjFromPtr` | CB | stdlib.md §5.1：由裸地址构造对象（script_object_bif.cpp:114-133） | 不提供；跨边界只传稳定 ID 与快照 | `script_object_bif.cpp:114` |
| `ObjFromPtrAddRef` | CB | stdlib.md §5.1：裸地址构造 + 增引用 | 不提供 | `script_object_bif.cpp:114` |
| `ObjGetCapacity` | CB | stdlib.md §5 第 4 条（裸内存与引擎布局互操作）：返回 AHK 对象字段数组的物理容量（script_object.cpp:1967-1970），是引擎内部布局而非可移植语言概念，JS/QuickJS 无对应物 | new Array(n) 预分配；容量交给引擎 | `script_object.cpp:1967` |
| `ObjPtr` | CB | stdlib.md §5.1：向脚本暴露对象地址 | 不提供；调试用 inspect/Trace 读结构而非地址 | `script_object_bif.cpp:114` |
| `ObjPtrAddRef` | CB | stdlib.md §5.1：地址 + 引用计数 | 不提供 | `script_object_bif.cpp:114` |
| `ObjRelease` | CB | stdlib.md §5.1：显式释放引用 | JS GC 自动管理 | `script_object_bif.cpp:68` |
| `ObjSetCapacity` | CB | stdlib.md §5 第 4 条（裸内存与引擎布局互操作）：重新分配对象字段数组容量（script_object.cpp:1972-1995），暴露引擎内部布局 | 同 ObjGetCapacity | `script_object.cpp:1972` |
| `SoundGetInterface` | CB | stdlib.md §5.1：返回裸 COM 接口指针（sound.cpp:292 的 GetInterface 族） | 按能力暴露的 SoundGet/Set 高层读写（M5） | `lib/sound.cpp:292` |
| `StrGet` | CB | stdlib.md §5.4：按地址读字符串缓冲 | TypedArray/TextDecoder 解码 | `lib/interop.cpp:242` |
| `StrPtr` | CB | stdlib.md §5.4：暴露字符串缓冲地址 | 不提供 | `lib/interop.cpp:577` |
| `StrPut` | CB | stdlib.md §5.4：按地址写字符串缓冲 | TextEncoder 编码进二进制缓冲 | `lib/interop.cpp:242` |
| `Thread` | COV | stdlib.md §5.3：线程中断/优先级语义属隐式伪线程模型 | SchedulerPolicy 集中定义的优先级与并发（AGENTS 调度规则） | `lib/functions.h:293` |
| `VarSetStrCapacity` | CB | stdlib.md §5.4：预留变量缓冲容量（script2.cpp:2327-2331），属内存布局细节 | JS 字符串不可变，容量无概念 | `script2.cpp:2327` |


## 3. L4 还原保留候选清单与政策对照（18 项）

- **stdlib.md §6 逐点名的 14 项**：`SubStr`、`InStr`、`Mod`、`FormatTime`、`RegExMatch`、`RegExReplace`、`DateAdd`、`DateDiff`、`SplitPath`、`StrSplit`、`Format`、`Sort`、`Random`、`VerCompare` —— 全部进 §2.2.2 还原保留候选，与政策一致。
- **§6 以“等”字兜底、按同一标准（差异真实存在且有用户价值）纳入的 4 项**：`Round`（半值远离零 + n>0 返回定长十进制字符串）、`StrReplace`（第 4 参默认大小写不敏感，与 `replaceAll` 恒敏感相反）、`StrTitle`（L0 无 title-case 操作）、`Type`（Integer/Float/类名三段映射，`typeof` 无法表达）。
- **明确不进 L4 的（判 `js-native`）**：`Trim`/`LTrim`/`RTrim`、`Abs`、`StrLen`、`Is*` 11 项（`IsAlnum` 至 `IsXDigit`）、`StrLower`/`StrUpper`/`StrCompare`、`Chr`/`Ord` —— 与 §6“JS 已覆盖的不写代码”一致；`Trim` 类的默认 cutset 差异在 §2.1 差异列显式记录。
- **落点与形状**：`sdk/src/runtime-language.ts`，实现为纯函数、不反向依赖 L2（stdlib.md §6）；命名以 future-runtime.md 定义优先，无定义者实现阶段先出命名提案再落码；不新造 AHK 约定包装。18 项 `contractTest` 均为 `missing`，实现时补对照原版的语义用例。

## 4. PCRE → JS 政策

- `RegExMatch`/`RegExReplace` 判 `contract-only`（§2.2.2 L4 还原保留候选），目标 `@rime/runtime-language`；台账原值 `@rime/native-interop`（capability `native.unsafe`）属误归——纯字符串匹配不碰 native 面，本页更正并记入分类报告。
- 还原内容：AHK 返回形状（`Match` 对象的 `Pos`/`Len`/`Name`/`Value`/`Mark`、`O)` 输出变量族、1-based 位置）与 PCRE→JS 差异；实现走 JS `RegExp` + 语义还原壳（计划 M3 明确点名）。
- **PCRE 构造无 JS 等价时显式抛错，不静默错配**（stdlib.md §6）：不支持的分支/递归/回溯控制等构造必须同步抛 `Unsupported` 并给出构造名，禁止退化成“看起来能跑”的正则。

## 5. 分批与计划对齐注记

- **M3（纯 JS 快铺）**：51 项 `js-native`（等价式与差异入档，零专属代码）与 18 项 L4 还原保留候选（§2.2.2，`sdk/src/runtime-language.ts`，计划 §7.3 先命名提案、后实现与对照测试）排在 M3；`contract-only` 中的 `Set*Delay`/`CoordMode`/`SetWorkingDir`/`OutputDebug`/`ListVars`（计划 line 97）与 `Exit`/`ExitApp`/`Reload`/`Pause`/`Persistent`（计划 line 98）同属 M3 的显式 API/HostLifecycle 映射；`GetKeySC`/`GetKeyVK`/`GetKeyName`/`ListHotkeys`/`Set*LockState` 归 `@rime/input` 的 M3 输入映射。
- **M4**：`FileOpen`（fs service + `File` 对象）、`Reg*` 5 项 + `SetRegView`（registry service，计划 line 105 点名）。
- **M5**：`Sound*` 5 项（winmm，计划 line 112）。
- **M7**：`PostMessage`（Control 三层执行的 Win32 消息层）。
- **M8**：`ComObj*` 7 项（audit-gaps 的 COM/VARIANT 边界）+ 20 项 `unsupported-by-policy` 的拒绝行为测试与替代路径文档（计划 line 131）。
- **判定已定案（stdlib.md 修订后口径，无遗留冲突）**：
  1. `Critical`/`Thread` = `unsupported-by-policy`，引用 stdlib.md §5 第 3 条（隐式伪线程/抢占模型）；替代物为集中式 SchedulerPolicy 与 Action 临界区。计划 §M3 的措辞由 orchestrator 同步修订。
  2. `ComObj*` 7 项 = `contract-only`（M8）：stdlib.md §5 尾注明确其不在黑名单，去向是计划 §0.3 与 audit-gaps 的 COM 边界定档，按隔离插件信任模型处理。
  3. `ObjGetCapacity`/`ObjSetCapacity` = `unsupported-by-policy`，引用 stdlib.md §5 第 4 条（裸内存与引擎布局互操作，条目已点名这两项）。

## 6. 跨域注记

- `SetTitleMatchMode`/`DetectHiddenWindows`/`DetectHiddenText` 不在本页 138 项内：它们属于窗口域，实现落在 `settings.window` 同步读写面（见 [`window.md`](./window.md)），语义与状态以 window.md 为准。
- `Click`/`GetKeyState`/`Set*LockState` 等输入域条目即使在本页分类，实现仍由 `@rime/input` 提供；`WinActive`/`WinExist` 由 `@rime/window` 提供；本页只登记判定、服务表面与改名注记。
