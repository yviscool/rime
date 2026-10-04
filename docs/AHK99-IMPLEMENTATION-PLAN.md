# AHK 99% 能力对齐实施计划

> 状态：已定稿待执行。本文件是长期战役的唯一计划真值；进度以 `docs/api/*.json` 的状态计数为准，不在 README 维护。

## 0. 验收口径（已拍板）

1. **能力对齐（还原保留）**：每个 AHK 函数、对象成员、内置变量、指令都有 TS 原生等价实现——能力不缺项、语义以对照原版的测试守住；命名、参数形状、同步性与暴露边界按 `docs/AHK-TS-WINDOWS-API-DESIGN.md` §0 的核心设计问题重新设计，**不实现 AHK 名称兼容层**。Rime 只执行 TypeScript，**不实现 AHK 脚本语言本身**（解析器/表达式编译器不在范围内）。AHK 名称只用于覆盖矩阵追踪，公共命名以 `docs/api/future-runtime.md` 与各领域规范为准。
2. **GUI 底座 = Win32 通用控件**：`GuiService` 在 UI Thread 拥有真实 HWND + common controls（Button/Edit/ListView/TreeView/…），行为与 AHK 对齐、可逐控件落到 Win32 消息。Rime UI Runtime 继续服务 Rim 自身 UI，两者不冲突；本计划不等待 UI Runtime。
3. **维持策略裁剪**：DllCall、ComCall、CallbackCreate/Free、ObjPtr/AddRef/Release 系、NumGet/NumPut、StrPtr、ComObj*、Obj*PtrData 等裸互操作保持 `unsupported-by-policy`，补文档与"拒绝行为"测试，计为已决策项（分母中为终态）。调试器、AHK 脚本引擎同样排除。

**99% 公式**：`终态条目 / 范围内条目 ≥ 99%`，终态 = `implemented | js-native | unsupported-by-policy`（`implemented` 须有测试 ID；`js-native` 以等价表达式与差异记录替代测试 ID）；`contract-only`/`sdk-owned` 为过渡态，逐步收敛到终态（状态口径见 `docs/api/stdlib.md` §3）；`excluded`（AHK v1 别名等版本差异条目）不计入范围内分母。

## 1. 事实基线

### 1.1 分母现状（M0 后冻结：只允许状态流转，不允许静默缺项）

| 清单 | 源码实际（=已追踪） | M0 补齐动作 |
|---|---:|---|
| `coverage.json`（functions.h `md_func`） | 253 | 立项时已对齐 |
| `core-builtins.json`（script.cpp `g_BIF`） | **101（BIF1 41 + BIFn 47 + BIFi 13）** | 补齐 60 个未追踪条目：Reg* 5、WinExist/WinActive、SoundGet/Set 系 6、Trim/RTrim/LTrim/StrLower/StrUpper/StrTitle/StrGet/StrPut、ACos/Ceil/Floor/Max/Min/Sqrt/Ln/Log、Is* 谓词 12、Obj* 裸互操作系、DllCall/ComCall/ComObjFromPtr、GetMethod/HasMethod/IsSetRef |
| `objects.json`（对象成员） | **243（20 对象）** | 提取控件专用对象（ListView 11 /TreeView 12 /StatusBar 3 /Edit/Date/Tab/ComboBox）等 30 成员 |
| `builtins.json`（`A_*` 变量） | **145 = `g_BIV_A` 134 + 11 个 v1 别名** | 134 逐个分类（快照字段 / EventContext / 动态 async）；11 别名标 `excluded`（不计分母，理由在各自 `source`） |
| 指令（`IsDirective`） | **22** | 逐项分类：TS 等价 API 4、配置映射 13、排除 5（`directives-and-syntax.md`） |
| 合计 | **764（有效 753 = 764 − 11 别名）** | 分母冻结；`matrix:check` 漂移检查守住（coverage ≡ `md_func`、core-builtins ≡ `g_BIF`、builtins ⊇ `g_BIV_A`、指令 ≡ `IS_DIRECTIVE_MATCH`） |

### 1.2 终态现状

| 清单 | implemented | js-native | unsupported | 终态合计 | 剩余（`contract-only`） |
|---|---:|---:|---:|---:|---:|
| coverage 253 | 102 | 7 | 7 | 116 | 137 |
| core-builtins 101 | 8 | 57 | 18 | 83 | 18 |
| objects 243（成员） | 0 | 0 | 0 | 0 | 243 |
| builtins 134（145 − 11 `excluded`） | 5 | 0 | 0 | 5 | 129 |

注：已落地能力已回填（M0-M2）：`process.*` 6 项、`Send`/`SendInput`（`input.send`）、`A_Clipboard`（`clipboard.read`/`clipboard.write`）、`Click/WinActive/WinExist`、`Sleep`（`runtime.delay`）、`GetKeyState` 等；15 个 action 的真值源在 `contracts/registry/actions.json`（`automation.*` 等无独立 AHK 函数条目，其状态记录在该注册表，`matrix:check` 校验 type 集 == executor 注册集）。138 项纯语言分类已入 `core-builtins`/`coverage`（映射表 `docs/api/runtime-language.md`）。当前终态 204 / 731 ≈ 27.9%（四个 JSON，不含指令）。

### 1.3 代码现状（结构事实，M0 之后）

- **已有**：15 个 action type、16 个 capability（12 `implemented` + 1 `test-only` + 3 `planned`）、5 个 service（window/input/process/clipboard/automation）+ UiThread + Dispatcher/Kernel + TimerService + 完整垂直切片模板（七段式 slice 测试）；`Lane::Worker` + `WorkerService`（异步读/泵与 5 个 executor 的 lane 检查）；中央真值源 `contracts/registry/actions.json` + `matrix:check` 漂移检查；共享 `rime::win32::Bootstrap` 生产接线（`tests/js/js_bundle.cpp` 与 `hosts/desktop` 同源，`rime_host <script>` 走同一生命周期）。
- **缺失**：registry/screen/fs/通用 COM/GUI/declarative Hotkey/OnMessage 零实现；`Send` 字符串语言（M2 语法层，底层 `input.send` 已具备）。
- **原版规模参考**（`rime-research/AutoHotkey-alpha`，≈103,700 行）：语言核心 ≈30k（排除）、GUI+Menu ≈15.5k、Hook/Hotkey/Send ≈13.3k、BIF 实现面 ≈12.5k、调度内核 ≈3.5k。我们的对应面：GUI/Menu 与 Hook/事件中枢是两个最大战役，BIF 面广而浅。

## 2. 阶段总览

```text
M0 地基与分母 ──► M1 Window 收官 ──► M2 输入/事件中枢（最难）
       │                │                  │
       ├────────────► M3 纯 JS 快铺（可穿插任意时点）
       │                                    │
       └─► M4 Worker 簇（fs/registry/process）─► M5 clipboard/screen
                                                  │
                       M6 GUI/Menu/Dialogs ◄──────┘（依赖 M1/M2 的回调与窗口）
                                  │
                       M7 Control 42 + 控件对象（依赖 M1+M6+automation）
                                  │
                       M8 完备性收尾与 99% 核算
```

每阶段完成定义（DoD）：`bun run test` + `bun run test:asan` 全绿；`matrix:gen && matrix:check` 通过；coverage/core-builtins/objects/builtins 对应条目转终态并填 `contractTest`/`compatibilityTest` 路径；领域文档"实现状态"同步；主题提交并推送。

## 3. 各阶段明细

### M0 地基与分母修正（先行，不可并行绕过）

1. **分母补全**
   - 提取 `g_BIF` 全 101 项 → 扩展 `core-builtins.json`（60 新条目逐项：domain/tsModule/lane/capability/status/测试）；Reg* 5 项归 storage/registry 域、WinExist/WinActive 归 window、Sound* 归 gui-menu、裸互操作归 unsupported。
   - 提取控件专用对象成员（ListView/TreeView/StatusBar/Edit/Date/Tab/ComboBox）→ `objects.json`。
   - `builtins.json`：134 个 `A_*` 逐个分类（快照字段 / EventContext / 动态 async / 内部排除并记录理由）。
   - 指令 22 项逐个分类（TS 等价 API / 配置映射 / 排除），补进 `directives-and-syntax.md`。
2. **中央真值源**：新建机器可读的 action/capability 注册表（建议 `contracts/registry/actions.json`：type、capability、target.kind、payload schema 引用、executor 所在、module 所在），`matrix:check` 增加"代码注册的 type 集 == 真值源"漂移检查；15 个现有 type 先回填。
3. **Worker lane**：`Lane::Worker` 枚举 + 专用 worker 线程 claim；`async_task.hpp` 的异步读/泵逐步改投 worker（TimerService 保留 delay/timer 职责）；`require_lane` 进 executor。
4. **生产接线**：抽出共享 bootstrap（service + kernel 授权 + executor 注册 + module 注册），`tests/js/js_bundle.cpp` 与 `hosts/desktop` 共用，消除"测试即生产"的结构缺口。
5. **状态同步**：修正 README `specified` vs JSON `contract-only` 术语漂移；把已落地能力（input.send、clipboard.write、process.*、automation.*）回填为 `implemented` + contractTest 路径。

**产出**：分母固定（此后只减不增）；真值源 + 漂移检查上线；worker lane 可用；生产接线与测试同源。

### M1 Window 收官（52 + 2 新追踪，S~M）

- 补齐 39 个 contract-only：`WinGetText/WinGetControls/WinGetClientPos/WinGetPos/WinGetMinMax/WinGetStyle系/WinSetAlwaysOnTop/WinSetRegion/WinSetTitle/WinSetEnabled/…`；
- `WinWait/WinWaitActive/WinWaitClose/WinWaitNotActive` → **条件等待服务**：`windows.wait({until})`，25ms 调度器轮询 + worker lane 求值（`evaluate_wait`），绝不阻塞线程；deadlineMs 整预算 + 取消——**已完成**（WinEvent 窗口事件唤醒为后续优化）；
- `WinExist/WinActive`（M0 追踪的 g_BIF，已由 `exists`/`isActive` 覆盖）与 `GroupAdd/GroupActivate/GroupClose/GroupDeactivate`（WinGroup → `rime:window` 的 `groups` 导出与 `window.group.*` 动作，SDK `groups()` 门面）——**已完成**；
- 全局窗口设置：`SetTitleMatchMode/DetectHiddenWindows/DetectHiddenText` → `settings.window` 同步面（`WindowService` 原子状态，get=read/set=write，实际变化记 `StateChanged` Trace；四模式 `matchMode` 含 `regex`）——**已完成**；
- 补 window JS 入口取消测试（audit 指出的缺口）——**已完成**（写入口 slice-premature、读入口 slice-read-cancel）。

### M2 输入与事件中枢（15 + 14 + 指令 11 + InputHook 23，XL，最高难度）

- **Send 语言**：`Send/SendInput/SendEvent/SendPlay/SendText` 的字符串语法解析器（`{Blind}{Text}{Key}{Down}{Up}!^+#<>`、SendLevel、SendMode），编译到现有 `input.send` 批次；
- **鼠标与键状态**：`MouseClick/MouseClickDrag/MouseMove/MouseGetPos`、`KeyWait/BlockInput/KeyHistory/GetKeyState`（键状态存储挂在 Hook 线程，经快照读）；
- **声明式事件**：`Hotkey()/Hotstring()` 注册表、`HotIf*` 条件（Context 快照、JS 线程求值，禁 Hook 线程跑 JS）、`Install*Hook`、`SetTimer/OnMessage/OnClipboardChange/OnError/OnExit`——全部走统一 `Subscription`（关闭态+回调计数）；
- **InputHook 对象** 23 成员（采集状态机复用 Hook 线程）；
- **SchedulerPolicy 落地**：`#MaxThreads*/#InputLevel/#MaxThreadsBuffer/#SuspendExempt/#SingleInstance/#MenuMaskKey/#UseHook/#Warn/#Requires` 等 22 指令逐项映射到集中策略或 TS API；
- **压力验收**：队列满载、嵌套泵、取消竞态、重入、暂停/恢复、shutdown 卸载（audit-gaps #4 强制项）。

### M3 纯 JS 快铺（可穿插，M）

- 138 项纯语言归档已定（`docs/api/runtime-language.md` 映射表）：**64 项 `js-native`**（零专属代码，等价表达式与差异入档）、**33 项 `contract-only`**（L2/L3 目标待实现）、**21 项 `implemented`**（5 项既有 + **10 项 L4 还原保留已实现** + 3 项批 2 绑定 `OutputDebug`/`SetWorkingDir`/`GetKeySC` + 3 项批 3 输入映射 `GetKeyVK`/`GetKeyName`/`ListHotkeys`）、20 项 `unsupported-by-policy`；
- **L4 还原保留（10 项，已实现）**：`Round/Format/FormatTime/Sort/SplitPath/VerCompare/DateAdd/DateDiff/RegExMatch/RegExReplace`——已落 `sdk/src/runtime-language.ts`（自有命名 `round/format/formatTime/sortLines/splitPath/compareVersions/addTime/diffTime/regexMatch/regexReplace`，不建 AHK 名称兼容层，计划 §0.1）与 `tests/sdk/runtime-language.test.ts`（41 例对照原版语义测试，逐条附 AHK `file:line` 引证），台账流转 `implemented`；其余 8 项原候选（`SubStr/InStr/Mod/StrReplace/StrTitle/Type/StrSplit/Random`）差异可用等价表达式消解，判 `js-native` 入档不写代码；
- `SetControlDelay/SetWinDelay/CoordMode/Exit/ExitApp/Reload/Pause/Persistent` → Runtime/HostLifecycle/SchedulerPolicy 显式 API（`Sleep`/`Suspend` 已分别由 `runtime.delay`/`input.suspend` 承载并流转 `implemented`；`SetKeyDelay`/`SetMouseDelay`/`SetDefaultMouseSpeed`/`SetStoreCapsLockMode`/`ListVars` 经 M3 源码盘点判 `js-native` 零代码入档；`OutputDebug`→`runtime.debug`、`SetWorkingDir`→`runtime.cwd`/`runtime.setCwd` 已实现；`Critical`/`Thread` 按 `stdlib.md` §5.3 判 `unsupported-by-policy`）；
- `GetKeyVK/GetKeyName/ListHotkeys/Set*LockState` → `@rime/input` M3 输入映射（`GetKeySC`→`input.getKeySC`、`GetKeyVK`→`input.getKeyVK`、`GetKeyName`→`input.getKeyName`、`ListHotkeys`→`input.listHotkeys` 已实现）；
- `js-native` 行只做映射入档与差异核对，不写代码。

### M4 Worker 簇：storage + registry + process（45 + 5 + 31 + 12，L）

- **fs service**（worker lane）：`File* 20 / Dir* 6 / Drive* 14 / Env* 2 / Ini* 3 / Download`（WinHTTP）；路径 capability 校验（`filesystem.read/write`，评估读/写/删细分）；`FileSelect/DirSelect` 对话框（UI Thread，无第二泵）；
- **`File` 对象 31 成员**：FileRef 生命周期、编码、确定性 close（GC/显式/shutdown 三路径）；
- **registry service**：`RegRead/RegWrite/RegDelete/RegCreateKey/RegDeleteKey + SetRegView`（Reg* 5 为 M0 追踪项）；
- **process 补齐**：`ProcessWait/ProcessWaitClose/RunWait/RunAs/Shutdown/ProcessSetPriority/ProcessGetParent/ProcessGetPath`——可等待句柄 + 取消令牌（禁 JS 线程轮询）；`RunAs/Shutdown` 独立高权限 capability。

### M5 clipboard + screen（≈16，M）

- clipboard：`ClipWait`（变化订阅 + 可取消等待）、`ClipboardAll`（不透明二进制快照）、交换临界区 + Trace；与 M2 的 `OnClipboardChange` 共用监听器；
- screen：`MonitorGet* 5/SysGet/SysGetIPAddresses`（不可变快照 + DPI/坐标语义入 TS 类型）；`PixelGetColor/PixelSearch/ImageSearch`（GDI 捕获起步、worker 上做像素/模板匹配；AGENTS 要求后续可换 DXGI/D3D11，接口先隔离）；`CaretGetPos`（UIA/Win32）；
- `SoundBeep/SoundPlay + SoundGet/Set 系 6`（winmm）。

### M6 GUI / Menu / 对话框（15 函数 + 114 对象成员 + 控件对象，XXL）

- `GuiService`（UI Thread 所有权 + 稳定 GuiId/ControlId + Win32 common controls）：`Gui` 55 成员（Add* 构造器 28 种控件）、`GuiControl` 42 成员；
- `Menu/MenuBar` 17 成员 + 托盘（`TraySetIcon/TrayTip/MenuSelect`）；
- 模态对话框 `MsgBox/InputBox`（结果经统一调度器，禁第二脚本泵）；`ToolTip/FileSelect 已在 M4/DirSelect`、`LoadPicture/IL_Create/IL_Add/IL_Destroy`（ImageList 不透明句柄）；
- 控件专用对象方法：ListView 11、TreeView 12、StatusBar 3、Edit/Date/Tab/ComboBox（M0 已提取的成员逐项）；
- GUI 回调进 Subscription 体系；验收含队列满载/嵌套泵/取消竞态/重入 + 对象三路径（GC/关闭/shutdown）无泄漏。

### M7 Control 42 +（依赖 M1+M6，L）

- `Control* 42`：三层执行（UIA pattern → Win32 消息 → visual fallback），执行层进 Trace；`ControlId` = UIA runtime id + generation（不复用 HWND 别名）；`Edit*` 系；读 `ControlGet*` 全部走快照缓存 + 失效 `target_gone`；
- 与 M6 控件对象共享注册表。

### M8 完备性收尾与 99% 核算（M）

- audit-gaps 12 条逐条销项（COM/VARIANT 边界文档化、ahklib.idl → 版本化 Host ABI contract、错误原型、globaldata 状态簇归属表、漂移检查 CI）；
- 对象 213+ 成员语义还原测试、builtins 全量测试补齐；
- `unsupported-by-policy` 每项：拒绝行为测试 + 替代路径文档；
- 生成**核算报告**（脚本输出终态计数/百分比）入 CI，`≥99%` 为硬门槛；
- 生产 hosts 端到端：Rim bundle 经共享 bootstrap 跑通全模块。

## 4. 质量与流程约束

- 每个功能族沿用**七段式垂直切片**（`docs/` 既有模板）：规范/coverage → schema（复杂 payload 的域才建，window 已有）→ executor → service → JS binding → 接线 → slice/native/SDK 三层测试；
- 遵守 AGENTS 线程铁律：JS/UI/Automation/Worker lane 边界、`require_lane`、无裸句柄出 JS、订阅可取消可观察、临界区排队合并；
- 每阶段结束：全量 test + ASan + matrix + 文档状态同步 + 主题 commit/push；
- 新域一律回答 AGENTS 八问（层/线程/JS 暴露/权限/取消/Trace/CLI 可查/测试）。

## 5. 主要风险

| 风险 | 对策 |
|---|---|
| M2 事件中枢语义错误导致全局故障 | 先建压力/重放测试夹具，再铺函数；SchedulerPolicy 集中，禁模块自造规则 |
| M6 GUI 工作量失控 | 按控件族分批（Button/Edit→LV/TV/SB→Menu/Tray→对话框），每批独立 DoD |
| 分母漂移（源码审计再次发现缺口） | M0 先固定分母 + CI 漂移检查；此后只允许状态流转不允许静默缺项 |
| coverage 与代码再次脱节 | 真值源 + `matrix:check` 强制；每阶段 DoD 含状态回填 |
| 单线程推进吞吐 | M3（纯 JS）与 M1/M4 并行；切片小步提交 |

## 6. 进度度量（机器可读）

- `bun run matrix:check` + 新增核算脚本：输出各 JSON 终态计数与总百分比；
- 目标曲线：M0 后分母固定（764，有效 753）；M1~M5 每阶段消化 15~25%；M6/M7 消化对象成员大头；M8 收口 ≥99%。
