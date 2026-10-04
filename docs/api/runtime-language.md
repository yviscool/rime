# Runtime and Language Compatibility

状态：138 项已分类——64 `js-native`、30 `contract-only`、24 `implemented`（5 项既有 + §2.2.2 的 10 项 L4 还原保留 + 3 项批 2 绑定 `OutputDebug`/`SetWorkingDir`/`GetKeySC` + 6 项批 3（输入映射 `GetKeyVK`/`GetKeyName`/`ListHotkeys`、锁键状态 `SetCapsLockState`/`SetNumLockState`/`SetScrollLockState`））、20 `unsupported-by-policy`。判定真值见 §2 映射表；状态词汇的权威是 [`stdlib.md`](./stdlib.md)（§3 四状态口径、§5 黑名单、§6 L4 还原保留层）。

命名与参数形状不在本页裁定：公共 API 命名遵循 [`AHK-TS-WINDOWS-API-DESIGN.md`](../AHK-TS-WINDOWS-API-DESIGN.md) §0 的六个核心设计问题与 [`future-runtime.md`](./future-runtime.md)（stdlib.md 开篇）。本页只登记**判定、证据与语义差异**，不给出 API 形状；AHK 名称仅作能力研究样本与语义核对测试的 oracle（计划 §0.1：不存在 AHK 名称兼容层）。

范围：`core-builtins.json` 全部 101 项 + `coverage.json` 中 `domain=runtime-language` 的 35 项 + `RegExMatch`/`RegExReplace` 2 项，合计 138 项。台账原值 `sdk-owned`/`contract-only` 是过渡态（stdlib.md §3），在本页收敛为四状态判定；本页只做判定与证据登记，不改台账 JSON。

源码证据前缀 `rime-research/AutoHotkey-alpha/source/`（表中短路径即相对该前缀；仓库内文件给完整路径）。纯字符串、日期和正则优先使用 TS/ECMAScript 标准库并配对照原版的语义还原测试；影响 Runtime 生命周期、调度和全局状态的函数一律映射到显式 Runtime API，不复制 AHK 的隐式伪线程与全局可变设置。

## 1. 四状态术语

| 状态 | 本页判定规则 | 计为终态 |
|---|---|---|
| `implemented` | 已有 TS 服务表面 + contract 测试（测试 ID 入台账）；本页给出服务模块与改名注记 | 是 |
| `js-native` | L0 已覆盖：本页给等价 TS 表达式与可观察差异（无差异写“无”），零专属代码，映射记录替代测试 ID | 是 |
| `contract-only` | 过渡态：目标模块与计划阶段已定、待实现；§2.2.2 的 10 项 L4 还原保留已于本轮实现并流转 `implemented`（`contractTest` = `tests/sdk/runtime-language.test.ts`），该小节保留分类研究原貌 | 否 |
| `unsupported-by-policy` | stdlib.md §5 黑名单或计划 §0.3 已拍板裁剪：本页给理由与等价物/替代 | 是（需理由与替代） |

终态公式（stdlib.md §3）：`终态 = implemented | js-native | unsupported-by-policy`。台账收敛规则：`sdk-owned` 收敛到 `contract-only`（差异有用户价值的进 §2.2.2 还原保留候选），`contract-only` 服务表面落地后收敛为 `implemented`，`unsupported-by-policy` 必须能指到 stdlib.md §5 的条目（三条已定案见 §5）。

## 2. 映射表（138 项）

台账标记：**CB** = `core-builtins.json`，**COV** = `coverage.json`。

### 2.1 `js-native`（64）

等价式为可直接使用的 TS/ECMAScript 表达式；差异列记录可观察行为不一致处（按 stdlib.md §3：差异集中在错误路径、区域设置或低频参数形态）。本轮按 §6“JS 覆盖不了的语义能力”严格复判，把原 18 项还原保留候选中的 8 项——`Mod`、`Random`、`InStr`、`SubStr`、`StrTitle`、`Type`、`StrReplace`、`StrSplit`——改判入本表：等价式已覆盖其核心语义与已核验的边界情形，残余差异逐项记在差异列（复判过程见 §3）。再经 M3 显式 API 盘点（源码核验 `DoKeyDelay`/`DoMouseDelay` 只在 SendEvent 模式生效等），`SetKeyDelay`/`SetMouseDelay`/`SetDefaultMouseSpeed`/`SetStoreCapsLockMode`/`ListVars` 5 项从“待实现显式 API”改判本表——现状即等价行为，零代码。

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
| `InStr` | CB | `h.toLowerCase().indexOf(n.toLowerCase(), from - 1) + 1` | +1 对齐 AHK 的 1-based 返回值与 0=未找到（string.cpp:1311/1277）；AHK 省略 CaseSense 即 SCS_INSENSITIVE（script_func_impl.h:15），与 toLowerCase 一致；残余差异：Occurrence>1 与负 StartingPos 的 RTL 组合（string.cpp:1300-1314 tcsrstr）表达式不覆盖，需自行循环 indexOf；空 Needles AHK 抛参数错（1262-1263）而 indexOf("") 返回起点；大小写折衷不同（AHK 逻辑折衷 vs JS Unicode 折衷，İ/ß 可能改变串长导致索引偏移） | `lib/string.cpp:1254` |
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
| `ListVars` | COV | `Object.getOwnPropertyNames(globalThis)` | AHK 列当前函数局部变量、静态变量与全部全局并开调试窗口（script.cpp:12778-12809）；JS 不可得函数局部（无仪器不可能），全局快照已覆盖“脚本可见变量”主体，`runtime.inspect()` 另给模块/函数/订阅/任务/错误 | `script.cpp:12778` |
| `Ln` | CB | `Math.log(v)` | v<0 AHK 抛参数错（math.cpp:292-294），JS 得 NaN；非数值 AHK 按 0 得 -Infinity，JS 得 NaN | `lib/math.cpp:288` |
| `Log` | CB | `Math.log10(v)` | 同 Ln | `lib/math.cpp:288` |
| `LTrim` | CB | `s.replace(/^[\t ]+/, "")` | 不能用 trim()：AHK 默认 cutset 是空格+Tab（string.cpp:1551）而 trim() 裁全部 Unicode 空白；自定义 omit 是字符集合（1551），需按 [.] 转义 | `lib/string.cpp:1540` |
| `Max` | CB | `Math.max(...vs)` | 非数值/空串 AHK 抛 Type 参数错（math.cpp:184-186），JS 得 NaN；整数/浮点分桶比较后返回原 token（166-192），值一致；JS 无参得 Infinity，AHK 强制至少一参 | `lib/math.cpp:152` |
| `Min` | CB | `Math.min(...vs)` | 同 Max | `lib/math.cpp:152` |
| `Mod` | CB | `a % b` | 整型走 C %、浮点走 qmathFmod（math.cpp:137/143），余数都取被除数符号，与 JS % 相同——值语义一致；残余差异：除数为 0 时 AHK 抛 ZeroDivision（math.cpp:132/141）而 JS 得 NaN，非数值参数 AHK 抛 Type 错（147-148）而 JS 走 ToNumber 隐式转换 | `lib/math.cpp:123` |
| `ObjBindMethod` | CB | `o[name].bind(o, ...args)` | AHK 允许省略 name，绑定对象自身作为可调用目标（script_object_bif.cpp:90-102），JS 需对象自身可调用；其余等价 | `script_object_bif.cpp:84` |
| `ObjGetBase` | CB | `Object.getPrototypeOf(Object(o))` | AHK 对原始值返回值原型、不可设基的对象返回 unset（script_object_bif.cpp:141-170）；表达式用 Object() 包装，null 原型两侧均可表达 | `script_object_bif.cpp:141` |
| `ObjHasOwnProp` | CB | `Object.hasOwn(o, name)` | 无：AHK FindField 只查自有字段（script_object.cpp:2054-2057），与 Object.hasOwn 等价（AHK 无 symbol 键） | `script_object.cpp:2054` |
| `ObjOwnPropCount` | CB | `Reflect.ownKeys(o).length` | AHK 计全部自有字段（含不可枚举，script_object.cpp:1924-1927），故不能用 Object.keys；Reflect.ownKeys 额外含 symbol 键（AHK 无） | `script_object.cpp:1924` |
| `ObjOwnProps` | CB | `Reflect.ownKeys(o).map(k => [k, o[k]])` | AHK 枚举全部自有字段并给出 name+value（script_object.cpp:2042-2046、3256-3265），JS Object.keys 只给 enumerable；表达式含 symbol 键是额外项 | `script_object.cpp:2042` |
| `ObjSetBase` | CB | `Object.setPrototypeOf(o, base)` | AHK 拒绝非对象 base（script_object_bif.cpp:149-151）且不触发 meta-function；JS 额外允许 null（设为无原型） | `script_object_bif.cpp:141` |
| `Ord` | CB | `s.codePointAt(0) ?? 0` | 代理对合并为码点（string.cpp:1329-1330）与 JS 一致；空串 AHK 记 0，表达式已用 ?? 0 吸收 | `lib/string.cpp:1319` |
| `Props` | CB | `function* (o) { for (const k in o) yield [k, o[k]]; }` | AHK 沿基类链逐层枚举并按名排序、跳过 NoEnumGet/无 getter 的属性（script_object.cpp:3311-3364），且可对原始值枚举其值原型（script_object_bif.cpp:226-249）；JS for-in 按插入序、只给 enumerable 名 | `script_object_bif.cpp:226` |
| `Random` | CB | `Math.min(a,b) + Math.floor(Math.random() * (Math.abs(a-b) + 1))（整型闭区间）；Math.min(a,b) + Math.random() * Math.abs(a-b)（浮点）；Math.random()（无参）` | 闭区间与参数宽容度可由参数调整覆盖：min>max 自动交换、单参等价 (0,n)、无参返回 [0,1) 浮点（math.cpp:322-330/346-349），浮点式与 AHK 的 53-bit 构造 (rand>>11)/2^53*range+min（math.cpp:338）同构；残余差异：AHK 用 OS CSPRNG 并做拒绝采样保证均匀（math.cpp:326-370），Math.random 是非加密 PRNG 且有 ≤1/2^53 量级模偏差；范围超过 2^53 时表达式精度不足；整型/浮点需按参数选式（AHK 按是否含小数点自动分流，math.cpp:317-320） | `lib/math.cpp:312` |
| `RTrim` | CB | `s.replace(/[\t ]+$/, "")` | 同 LTrim | `lib/string.cpp:1540` |
| `SetDefaultMouseSpeed` | COV | `mouse.move(x, y, { speed })` 按调用传参 | AHK 是进程级默认（vars.cpp:68-75），我们的设计禁隐式全局（stdlib.md §2，speed 本就是每调用参数）；speed 与 AHK SendInput 一样注入时忽略（keyboard_mouse.cpp:2476-2478 ↔ input.ts:84-91），默认值 2 恒等 | `lib/vars.cpp:68` |
| `SetKeyDelay` | COV | 无代码：`keyboard.send` 恒为一个 SendInput 批（input.ts:682） | AHK DoKeyDelay 在非 SendEvent 模式直接返回（keyboard_mouse.cpp:2865-2878），本设置对 SendInput 本就是 no-op；差异：Rime 尚无 SendEvent 执行器，若未来实现需回到调度策略统一设计（计划 §M3） | `lib/keyboard_mouse.cpp:2865` |
| `SetMouseDelay` | COV | 同 `SetKeyDelay` | DoMouseDelay 同样只在 SendEvent 模式 sleep（keyboard_mouse.cpp:2882-2896） | `lib/keyboard_mouse.cpp:2882` |
| `SetStoreCapsLockMode` | COV | 无代码：Rime 固定“从不预切换 CapsLock”偏差已入档（send.ts:36） | AHK 默认 store-on：Send 前压低 CapsLock、结束后恢复（keyboard_mouse.cpp:413-416, 985-986）；Rime 从不切换，store-on/off 两模式下可观测行为一致，恢复分支必然真空 | `sdk/src/send.ts:36` |
| `Sin` | CB | `Math.sin(v)` | 同 Cos | `lib/math.cpp:215` |
| `Sqrt` | CB | `Math.sqrt(v)` | v<0 AHK 抛参数错（math.cpp:292-294），JS 得 NaN；非数值 AHK 按 0 得 0，JS 得 NaN | `lib/math.cpp:288` |
| `StrCompare` | CB | `a.toLowerCase() < b.toLowerCase() ? -1 : a.toLowerCase() > b.toLowerCase() ? 1 : 0` | 默认大小写不敏感 = _tcsicmp（string.cpp:1162/1180）；CaseSense=Locale/Logical 走 lstrcmpi/StrCmpLogicalW（1181-1182）无 JS 等价；非 ASCII 折叠规则与 _tcsicmp 有边缘差异 | `lib/string.cpp:1159` |
| `StrLen` | CB | `s.length` | 无：AHK 返回 TCHAR（UTF-16 code unit）个数（string.cpp:1194-1196），与 .length 等价 | `lib/string.cpp:1191` |
| `StrLower` | CB | `s.toLowerCase()` | AHK 走 Win32 CharLower（string.cpp:348-349，按用户区域设置），JS 与 locale 无关；ASCII 输入无差异，土耳其 İ/i 等区域规则有差异 | `lib/string.cpp:338` |
| `StrReplace` | COV | `q === '' ? h : h.replace(new RegExp(q.replace(/[.*+?^${}()\|[\]\\]/g, '\\$&'), 'gi'), () => r)` | 默认大小写不敏感与 AHK 一致（string.cpp:366：省略 CaseSense 即 SCS_INSENSITIVE）；空 SearchText 返回原串、计数 0（util.cpp:1130-1140）由三元覆盖；函数替换体规避 JS 替换串的 $ 特殊序列（AHK 替换串是字面量）；残余差异：Limit 次数上限与计数输出不覆盖，需自行计数；大小写折衷不同（AHK 逻辑折衷 vs JS /i/ 的 Unicode 折衷）；模式串转义依赖左侧转义式，需对照测试守住 | `lib/string.cpp:358` |
| `StrSplit` | COV | `s === '' ? [] : d == null \|\| d === '' ? [s] : s.split(d)` | 空输入返回 []（string.cpp:459-462）、分隔符省略或空串返回单元素整串（string.cpp:447-455 按非空计数）由三元覆盖；残余差异：OmitChars 需逐段 trim（p.replace(/^[chars]+\|[chars]+$/g, '')）；MaxParts 达上限时 AHK 把余项并入最后一段（string.cpp:477-481）而 JS split(n) 丢弃余项、语义相反，需 p.length>n ? [...p.slice(0,n-1), p.slice(n-1).join(d)] : p；MaxParts=0 返回 []；数组分隔符需 regex 联合 | `lib/string.cpp:402` |
| `StrTitle` | CB | `s.toLowerCase().replace(/\S+/g, w => w.replace(/\p{L}/, c => c.toUpperCase()))` | 与 StrToTitleCase（util.h:97-119）逐条对应：词边界只认空白（_istspace）、数字与标点不改写、每词只有首个字母大写其余小写（先整体小写再提升首字母，撤号与缩写按“非空白段”处理）；残余差异：AHK 走 Win32 风格逐字符 CharLower/CharUpper（string.cpp:353 调用），JS 用 Unicode 简单大小写映射，非 ASCII（ß、İ 等）与组合字符表现可能不同 | `util.h:97` |
| `StrUpper` | CB | `s.toUpperCase()` | AHK 走 Win32 CharUpper（string.cpp:350-351）；差异同 StrLower | `lib/string.cpp:338` |
| `SubStr` | CB | `(i => s.slice(i, len === undefined ? undefined : len < 0 ? Math.max(s.length + len, i) : i + len))(start === 0 ? s.length : start > 0 ? start - 1 : start)` | 逐条对应 string.cpp:1211-1247：start==0 返回空串（映射为 s.length 使 slice 为空）、start>0 转 0-based、start<0 为尾部偏移（slice 负索引同语义且同样钳到 0）、len 省略到尾、len<0 为尾部去掉 len 个字符（Math.max(s.length+len, i) 在余项不足时给出空串）、越界与零长度返回空串；残余差异：非整数 Start/Length 被 AHK 经 ParamIndexToInt64 向零截断（1212/1230），表达式按整数参数使用 | `lib/string.cpp:1201` |
| `Tan` | CB | `Math.tan(v)` | 同 Cos | `lib/math.cpp:235` |
| `Throw` | CB | `throw e` | AHK Throw 是可作表达式调用的 BIF（error.cpp:154），异常原型是 AHK Error 家族；JS throw 是语句、可抛任意值——模型差异，无专属适配代码 | `error.cpp:154` |
| `Trim` | CB | `s.replace(/^[\t ]+/, "").replace(/[\t ]+$/, "")` | 同 LTrim：默认 cutset 空格+Tab（string.cpp:1551），不可直接用 String.trim()；对象参数 AHK 抛参数错（1542-1543） | `lib/string.cpp:1540` |
| `Type` | CB | `v === undefined ? 'unset' : typeof v === 'string' ? 'String' : typeof v === 'number' ? (Number.isInteger(v) ? 'Integer' : 'Float') : typeof v === 'bigint' ? 'Integer' : (v?.constructor?.name ?? 'Object')` | 三段映射与 TokenTypeString（script2.cpp:3099-3121）一致：String/Integer/Float/对象类名/unset（无参调用即 undefined→unset）；残余差异：JS 只有 IEEE double，整数值的 Float（如 3.0）报 Integer 而 AHK 报 Float——token 类型在 L0 已丢失，专属函数同样无法恢复；函数报 Function 而 AHK 报 Func/Closure/BoundFunc；BigInt 报 Integer（AHK 对应 int64）；null 与 symbol 无 AHK 对应物 | `script2.cpp:3099` |


### 2.2 `contract-only`（30；另 19 项已实现，见 §2.2.2 与本节注记）

过渡态，不计终态（stdlib.md §3）。本节现为 30 项常规能力，按 L2 模块归属排期；原 44 + 10 的 54 项中，10 项 L4 还原保留已实现并流转 `implemented`（§2.2.2 注记），5 项（`ListVars`/`SetKeyDelay`/`SetMouseDelay`/`SetDefaultMouseSpeed`/`SetStoreCapsLockMode`）经 M3 盘点改判 §2.1 `js-native`，3 项（`OutputDebug`/`SetWorkingDir`/`GetKeySC`）经批 2 绑定实现流转 `implemented`（`runtime.debug`/`runtime.cwd`+`runtime.setCwd`/`input.getKeySC`，测试 `tests/js/runtime_smoke.cpp`+`tests/js/input_slice.cpp`），3 项（`GetKeyVK`/`GetKeyName`/`ListHotkeys`）经批 3 输入映射实现流转 `implemented`（`input.getKeyVK`/`input.getKeyName` 含共享语法 `scNNN` 扩展、`input.listHotkeys` 注册表读出，测试 `tests/js/input_slice.cpp`+`tests/js/events_slice.cpp`），3 项（`SetCapsLockState`/`SetNumLockState`/`SetScrollLockState`）经批 3 锁键状态实现流转 `implemented`（`keyboard.setLockState` 编排 + 原生 `input.setLockForce` force-toggle 抑制，测试 `tests/js/input_slice.cpp`+`tests/sdk/keyboard-lock.test.ts`），表列均留原处以保持分类研究原貌。

#### 2.2.1 常规能力（30）

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
| `SetNumLockState` | COV | `@rime/input` | M3 | 同 SetCapsLockState | `lib/functions.h:262` |
| `SetRegView` | COV | `@rime/registry` | M4（计划 line 105 明确点名） | 注册表 32/64 视图是 registry 服务配置，需并入 registry service 而非全局开关 | `lib/functions.h:263` |
| `SetScrollLockState` | COV | `@rime/input` | M3 | 同 SetCapsLockState | `lib/functions.h:264` |
| `SetWinDelay` | COV | `@rime/window` | M3 | 同 SetControlDelay（窗口操作延迟应是调用选项） | `lib/functions.h:270` |
| `SetWorkingDir` | COV | `rime:runtime` | M3（计划 line 97） | 进程 cwd 是 process 域状态，需 capability 与 Trace；当前 SDK 无 set cwd 面 | `lib/functions.h:271` |
| `SoundGetMute` | CB | `@rime/sound` | M5（winmm） | 音频端点状态需 winmm 调用与能力门禁，非纯函数；stdlib.md §1 未列 sound 模块，按计划 M5 新建，台账原值 @rime/gui-menu 为跨域暂挂 | `lib/sound.cpp:292` |
| `SoundGetName` | CB | `@rime/sound` | M5（winmm） | 同 SoundGetMute | `lib/sound.cpp:292` |
| `SoundGetVolume` | CB | `@rime/sound` | M5（winmm） | 同 SoundGetMute | `lib/sound.cpp:292` |
| `SoundSetMute` | CB | `@rime/sound` | M5（winmm） | 同 SoundGetMute（写入需 Trace 与结果） | `lib/sound.cpp:292` |
| `SoundSetVolume` | CB | `@rime/sound` | M5（winmm） | 同 SoundGetMute | `lib/sound.cpp:292` |


#### 2.2.2 L4 还原保留（10，已实现）

**状态：10 项已全部实现**——落点 `sdk/src/runtime-language.ts`（`round`/`format`/`formatTime`/`sortLines`/`splitPath`/`compareVersions`/`addTime`/`diffTime`/`regexMatch`/`regexReplace`，命名提案见计划 §M3），对照语义测试 `tests/sdk/runtime-language.test.ts`（41 例，逐条附 AHK `file:line` 引证），台账已流转 `implemented`、`contractTest` 指向该测试。

计划 §0.1 禁止 AHK 名称兼容层，stdlib.md §6 把“JS 覆盖不了的语义能力”定义为 L4 还原保留层。原 18 项候选经本轮严格复判：8 项改判 §2.1 `js-native`（等价式可覆盖），其余 10 项保留——“还原保留理由”列逐项写明**单表达式在何处失效**（JSON `note` 以 `还原保留：` 前缀）。实现以自有命名与参数形状落地，AHK 只作语义测试 oracle。

| 函数 | 台账 | 还原保留理由（可观察差异） | 规模 | 证据 |
|---|---|---|---|---|
| `DateAdd` | COV | 表达式不可靠（注：本版本只有 S/M/H/D 四个单位、按 FILETIME 直接加秒，math.cpp:393-416，无日历/DST 运算——保留理由是输入校验与截断语义）：YYYYMMDDHH24MISS 必须按 AHK 同样校验（util.cpp:86-90 SystemTimeToFileTime 拒绝非法分量与 year<1601），而 Date.UTC 对 2024-02-31 静默进位、对 year<1601 静默通过；秒数按 (__int64) 向零截断（math.cpp:416）再格式化为 14 位串（420），朴素表达式会接受 AHK 拒绝的输入 | ~45 行 | `lib/math.cpp:381` |
| `DateDiff` | COV | 表达式不可靠：除同 DateAdd 的输入校验外，空参默认取当前本地墙钟（util.cpp:283-303 GetSystemTimeAsFileTime+FileTimeToLocalFileTime）需与朴素字符串时间在同一表达式内混算，差值先按整型秒（util.cpp:307-318）再向零截断除以单位（math.cpp:435-438），三者无法单式可靠表达 | ~40 行 | `lib/math.cpp:426` |
| `Format` | CB | 表达式不可靠：AHK Format 是小型格式语言（{} 顺序占位、{:fmt} printf 风格、U/L/T 大小写选项、* 宽度从参数取值，string.cpp:1369-1536），ECMAScript 只有 %s 风格 sprintf 与模板串，无同构语法，占位解析与类型分派必须专用实现 | ~90 行 | `lib/string.cpp:1369` |
| `FormatTime` | CB | 表达式不可靠：AHK 自带 token 方言（yyyy/MM/dd hh:mm:ss t…，string.cpp:25-144）并回落 Win32 GetDateFormat/GetTimeFormat 取本地化默认（AM/PM 文案、月份名），Intl.DateTimeFormat 的组件、默认值与本地化输出都不同，逐 token 对齐必须是专用引擎 | ~70 行 | `lib/string.cpp:25` |
| `RegExMatch` | COV | 表达式不可靠：PCRE 构造（分支、递归、回溯控制、命名组）无 JS 等价时须显式抛错（stdlib.md §6），返回的 Match 对象（Pos/Len/Name/Value/Mark 与 O) 输出变量族）与 1-based 位置（functions.h:236）不是 RegExp.exec 数组能表达的形状 | ~60 行 | `lib/functions.h:236` |
| `RegExReplace` | COV | 表达式不可靠：同 RegExMatch 的 PCRE→JS 显式抛错要求；Limit、Position、O) 输出与 1-based 位置（functions.h:237）需要还原壳，replace/replaceAll 只覆盖子集 | ~55 行 | `lib/functions.h:237` |
| `Round` | CB | 表达式不可靠：Math.round 半值朝 +∞（-2.5→-2）而 AHK 半值远离零（math.cpp:48-49）；n>0 时 AHK 用 %0.*f 输出定长十进制字符串（“3.50”，79-81），toFixed 对恰好一半的负数取更大 n（-0.125.toFixed(2)=“-0.12”）与 AHK（“-0.13”）相反；n≤0 走 0.2 修正后按 int64 返回（90-91），非数值抛参数错而 JS 得 NaN | ~30 行 | `lib/math.cpp:26` |
| `Sort` | CB | 表达式不可靠：options 是旗标方言（C/COn/COff/CL/CLocale/CLogical、D+分隔符、N、P+列偏移、R/Random、U、Z、反斜杠按裸文件名，string.cpp:801-870）+ 自定义比较函数 + 尾分隔符空项规则（string.cpp:777-874），单表达式无法表达；Array.prototype.sort 需自写比较器，且 AHK 默认不区分大小写、按换行分行并重组 | ~60 行 | `lib/string.cpp:777` |
| `SplitPath` | COV | 表达式不可靠：四类路径规则耦合——URL 把 :// 之后的服务器名当 drive（string.cpp:542-553）、UNC 双反斜杠探测（600-602）、仅对 drive 去前导空白（541）、C:file.txt 的 dir 含冒号（621、670-673），扩展名取 name 内最右点（635），单条正则或 split 组合会在 URL/UNC/相对路径上静默错分 | ~35 行 | `lib/string.cpp:648` |
| `VerCompare` | CB | 表达式不可靠：AHK 版本比较是分段算法（数字段与字母段切分、rc/beta 顺序经 VersionSatisfies 落地，string.cpp:1567-1572），单表达式无法同时表达分段、进制解析与大小写规则，Intl.Collator 也不等价 | ~40 行 | `lib/string.cpp:1567` |


### 2.3 `implemented`（24：5 项既有 + §2.2.2 的 10 项 + 批 2 的 3 项 + 批 3 的 6 项（输入映射 3 + 锁键状态 3），行留原节）

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


## 3. L4 还原保留清单与政策对照（10 项，已实现）

本轮按 stdlib.md §6 的判据——只有“JS 覆盖不了的语义能力”才进 L4，**能被单条已记录表达式压平的差异归 `js-native`**——对原 18 项候选逐项复判，结果如下。
- **保留还原的 10 项（§2.2.2）**：§6 逐点名的 9 项 `FormatTime`、`RegExMatch`、`RegExReplace`、`DateAdd`、`DateDiff`、`SplitPath`、`Format`、`Sort`、`VerCompare`，加 §6“等”兜底的 `Round`。单表达式失效点逐项写在 §2.2.2 的“还原保留理由”列，概括为：`Round` 的半值远离零与定长十进制字符串、`Format`/`FormatTime` 的格式语言与 token 引擎、`Sort` 的旗标方言 + 自定义比较函数、`SplitPath` 的 URL/UNC/冒号规则耦合、`VerCompare` 的分段比较算法、`DateAdd`/`DateDiff` 的输入校验与秒级截断（本版本无日历/DST 运算，保留理由是校验与截断语义）、`RegEx*` 的 PCRE 缺口显式抛错与 Match 对象返回形状。
- **§6 点名但经表达式判定改判 `js-native` 的 5 项**：`Mod`（`a % b`，整型 C % 与浮点 qmathFmod 同为余数取被除数符号，差异只在除零/非数值错误路径）、`SubStr`（`slice` + 两处钳制覆盖 1-based、负 start、负 length、空起始与越界，已对照 string.cpp:1211-1247 核验）、`InStr`（`toLowerCase` + `indexOf` + 1 对齐 1-based 与默认 CaseSense=Off，Occurrence/负起点 RTL 记为低频参数差异）、`StrSplit`（`split` + 空输入/空分隔符三元，OmitChars/MaxParts 记为差异）、`Random`（闭区间 `floor` 式，模偏差、CSPRNG 源与 2^53 范围记为差异）。**建议 §6 的示例清单同步措辞**（其“等”字已可涵盖，或直接改为“示例见 runtime-language.md §3”）。
- **§6 以“等”字兜底但同样改判 `js-native` 的 3 项**：`StrReplace`（转义式 + `/gi/` + 函数替换体覆盖默认大小写不敏感、字面替换与空搜索，Limit/计数记为差异）、`StrTitle`（整体小写 + 非空白段首字母提升，逐条对应 util.h:97-119）、`Type`（`typeof` + `Number.isInteger` + 构造器名三段映射，整数值 Float 的 token 类型在 L0 已丢失，专属函数同样无法恢复）。
- **明确不进 L4 的（判 `js-native`）**：`Trim`/`LTrim`/`RTrim`、`Abs`、`StrLen`、`Is*` 11 项（`IsAlnum` 至 `IsXDigit`）、`StrLower`/`StrUpper`/`StrCompare`、`Chr`/`Ord` —— 与 §6“JS 已覆盖的不写代码”一致；`Trim` 类的默认 cutset 差异在 §2.1 差异列显式记录。
- **落点与形状**：`sdk/src/runtime-language.ts`，实现为纯函数、不反向依赖 L2（stdlib.md §6）；命名以 future-runtime.md 定义优先，无定义者实现阶段先出命名提案再落码；不新造 AHK 约定包装。10 项 `contractTest` 均为 `missing`，实现时补对照原版的语义用例；8 项改判行的 `contractTest` 指向本页（映射与差异记录替代测试 ID，stdlib.md §3）。

## 4. PCRE → JS 政策

- `RegExMatch`/`RegExReplace` **已实现**（`implemented`，§2.2.2），落点 `@rime/runtime-language`，`contractTest` = `tests/sdk/runtime-language.test.ts`；台账原值 `@rime/native-interop`（capability `native.unsafe`、`lane=plugin-isolated`）属误归——纯字符串匹配不碰 native 面，本轮随流转一并矫正为 `sync`/`js`/`runtime` 并记入分类报告。
- 还原内容：AHK 返回形状（`Match` 对象的 `Pos`/`Len`/`Name`/`Value`/`Mark`、`O)` 输出变量族、1-based 位置）与 PCRE→JS 差异；实现走 JS `RegExp` + 语义还原壳（计划 M3 明确点名）。
- **PCRE 构造无 JS 等价时显式抛错，不静默错配**（stdlib.md §6）：不支持的分支/递归/回溯控制等构造必须同步抛 `Unsupported` 并给出构造名，禁止退化成“看起来能跑”的正则。

## 5. 分批与计划对齐注记

- **M3（纯 JS 快铺）**：64 项 `js-native`（等价式与差异入档，零专属代码）排在 M3；10 项 L4 还原保留**已实现**（`sdk/src/runtime-language.ts` + `tests/sdk/runtime-language.test.ts`，命名提案见计划 §M3，残留清单见本页 §3 报告注记）；`contract-only` 中的 `SetControlDelay`/`SetWinDelay`/`CoordMode`/`Exit`/`ExitApp`/`Reload`/`Pause`/`Persistent`（计划 §M3 第三条；`SetKeyDelay`/`SetMouseDelay`/`ListVars` 等 5 项已改判 §2.1 `js-native`，`OutputDebug`/`SetWorkingDir` 已由批 2 绑定实现）同属 M3 的显式 API/HostLifecycle 映射；`GetKeyVK`/`GetKeyName`/`ListHotkeys`/`Set*LockState` 归 `@rime/input` 的 M3 输入映射（`GetKeySC` 已由批 2、`GetKeyVK`/`GetKeyName`/`ListHotkeys` 已由批 3 实现）。
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
