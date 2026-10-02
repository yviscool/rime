# Window API

状态：`implemented`（Rime 原生表面）。`list`（WinTitle 查询）、`active`、`info`、`controls`（WinGetControls/WinGetControlsHwnd）、`text`（WinGetText）、`exists`（WinExist）、`isActive`（WinActive）、`wait`（WinWait 家族条件等待）、`move`、`focus`、`close`、`hide`、`show`、`minimize`、`maximize`、`restore`，以及 `groups`（GroupAdd/GroupActivate/GroupDeactivate/GroupClose，`rime:window` 的 `groups` 导出 + SDK `groups()` 门面）均有 TS binding、`windows.window.read`/`windows.window.write` capability 校验、`window-v1` contract（`contracts/schema/window-v1.schema.json`）与 native/slice 测试。读写接受 `deadlineMs`/`cancellationId`（`AbortSignal`）选项，deadline 过期与排队超时返回 `timeout` 错误码。全局窗口设置 `settings.window`（`SetTitleMatchMode`/`DetectHiddenWindows`/`DetectHiddenText`）同步读写，不经过 Action 队列。

快照即 WinGet 读族的承载面：`rect`（WinGetPos）、`clientRect`（WinGetClientPos）、`style`/`exStyle`（WinGetStyle/WinGetExStyle，无符号 32 位）、`enabled`（WinGetEnabled）、`alwaysOnTop`（WinGetAlwaysOnTop）、`minMax`（WinGetMinMax，-1/0/1）、`transparent`/`transColor`（WinGetTransparent/WinGetTransColor，未设置为 `-1`/`""`）、`processPath`（WinGetProcessPath，读不到为 `""`）；`list` 的返回长度与首/末元素分别承载 WinGetCount、WinGetID、WinGetIDLast；`controls` 承载 WinGetControls（ClassNN）与 WinGetControlsHwnd（`id`，稳定 ID 而非裸 HWND），`text` 承载 WinGetText（`
` 分隔，隐藏控件是否计入由 `settings.window.detectHiddenText` 决定，默认跳过；`controls` 则始终包含隐藏控件）。`exists`/`isActive` 是存在性探针：前者枚举到首个匹配即停（不构建快照），后者只检查前台窗口。

源码证据：`rime-research/AutoHotkey-alpha/source/lib/functions.h` 的 `Win*`、`WinGroup*`；`source/window.cpp`、`source/window.h`；Rime 的 `engine/win32/src/window.cpp` 和 `engine/win32/src/ui_thread.cpp`。

核心类型使用稳定 `WindowId`、不可变 `WindowSnapshot` 和 `WindowQuery`。JS 不接收 `HWND`。查询、快照和等待走 Promise；纯字段访问同步。写操作生成 Action 并记录 Trace。目标消失时返回 `target_gone`（窗口已销毁，或 ID 属于上一个服务实例）；ID 形如 `[generation:32][sequence:32]`，进程内跨服务单调不复用（约束见 `docs/api/future-runtime.md`）。

## 测试覆盖（如实）

- `tests/js/vertical_slice.cpp`：`windows` 的 16/16 个 JS 调用全覆盖（含查询、扩展快照字段、`exists`/`isActive` 探针、`wait` 条件等待、`controls`/`text` 控件读、状态机与关闭）+ `groups` 的 4/4（add 去重计数、activate 缺组创建即 `null`、deactivate/close 缺组拒绝 `invalid_contract`、`close('all')` 精确计数 2 且不激活，victim 经查询确认销毁）；段 5d 覆盖设置默认值、大小写敏感匹配、四模式 `matchMode`、坏正则/未知选项的同步 `TypeError`、属性 setter 与 `set*` 返回前值、`detectHiddenWindows`/`detectHiddenText` 开关往返（含 `StateChanged` Trace 恰好 8 条、无变化 set 零 Trace），deny 运行时验证设置读写分别同步抛出 `windows.window.read`/`windows.window.write`。wait 段覆盖 `exists`/`closed`/`notActive` 首轮即决（快照或 `null`）、`deadlineMs:150` 拒绝 `timeout` 且不早于 deadline、取消绑定拒绝 `cancelled`。`focus` 允许 `SetForegroundWindow` 被前台锁拒绝（合法失败分支）；`move('active')` 仅在我方窗口持有前台时执行，否则整段 SKIP——测试不注入全局输入、不移动任何外来窗口。同文件另覆盖 `deadlineMs:0` 的 `timeout` 拒绝与非法 `deadlineMs` 的同步 `TypeError`。
- 12/12 个写类型经 JS → Dispatcher 队列 → Kernel → Executor → UI lane 端到端验证；`tests/native/win32_tests.cpp` 另覆盖 12/12 写类型的原生分派、空 policy 下逐类型的 capability 拒绝（拒绝后状态不变）与排队超时，组服务层另测规格去重、缺组语义与 `close('all')` 精确计数。
- `tests/sdk/windows.test.ts` 覆盖 `settings()` 加载与返回前值语义、`groups()` 门面的参数透传/信号剥离/取消 id 绑定（mock 桥）。
- `docs/api/coverage.json` 与 `compatibility-matrix.md` 的行是**逐 AHK 函数**状态（已测函数为 `implemented` 并带 contractTest 路径，未测扩展项仍为 `contract-only|missing`），与本页 Rime 原生 API 的状态不构成矛盾：两者粒度不同。
- cancel（`cancellationId`）：写入口由 slice-premature 覆盖（入队前取消，kernel pre-dispatch 拒绝 `cancelled`），读入口由 slice-read-cancel 覆盖（`list({cancellationId})` 经 host async 绑定拒绝 `cancelled`）；kernel 通用路径由 contract 测试覆盖。

## 执行模型

**写路径（Action）**：JS 构造 Action → Dispatcher 有界队列（入队决策 `ActionAccepted`/`ActionRefused` 进 Trace；同键共存的连续变更由 idempotency-key/同参数 key 合并，被合并方以 `cancelled:superseded by newer action` 拒绝，队列满为 `queue_full`）→ pump 执行 → Kernel（契约校验 → 取消检查 → 入口 deadline → capability → 执行器 → 执行后 deadline/取消复检）→ Executor（每次 UI 往返前重算剩余预算，含 `active` 目标解析；组动作以 `{kind:"group", id:"<组名>"}` 为目标，不解析窗口身份）→ UI lane → 快照结果。

- 执行后过期：结果改写为 `timeout`，消息声明 `side effects may have occurred`（副作用不可撤销，调用方据此判断）。
- 执行后取消：同理改写为 `cancelled` 并声明副作用可能已发生。
- 取消边界：仅在「执行前 / 往返之间 / 执行后」检查；已进入的 Win32 调用（如 `SendMessageTimeoutW`）只能等其超时，不被强行打断。

**读路径（观察，豁免于 Action Kernel）**：`list`/`active`/`info`/`controls`/`text`/`exists`/`isActive`/`wait` 经同一 capability 策略校验后直调 `WindowService`，**不产生 `ActionStarted/ActionFinished` Trace，也没有 Kernel 级 deadline**；`deadlineMs` 在请求构建时折算为该次 UI 阶段的等待预算——对 `wait` 则是**整个等待预算**（到期拒绝 `timeout`）。这是观察（Snapshot/Query）与变更（Action）的语义分界，属文档化豁免，不是遗漏。

**设置路径（同步状态，豁免于 Action Kernel）**：`settings.window` 的四个属性与三个 `set*` 方法在 JS 线程同步读写 `WindowService` 原子状态——**不入队、不产生 Action Trace**；`get` 校验 `windows.window.read`、`set` 校验 `windows.window.write`（先解析/校验值、再查能力，均同步抛出 `TypeError`）。每次**实际变化**记录一条 `kind=StateChanged`、`subject=window.settings`、`detail="key before -> after"` 的 Trace，无变化的 set 返回当前值且零 Trace。

## 语义细则

- **前台选择**：只用 `active: true`。历史上的 `title == "A"` 魔法短路已删除——不存在无法按字面匹配的标题。
- **`focus`**：先 `SW_RESTORE` 再 `SetForegroundWindow`；被前台锁拒绝时返回 `ExecutionFailed`，消息含 `foreground lock`。
- **大小写**：`title` 在所有模式下**大小写敏感**（AHK 规则：WinTitle 全模式敏感，唯一例外是 RegEx 模式的 `i)` 选项前缀）；`ahkClass` 大小写不敏感（ahk_class 自 AHK v2.1 起如此）；`ahkExe` 为 ASCII fold（规则见 `window.cpp` 的 `fold_ascii` 注释），非 ASCII 部分按精确字节比较。
- **`matchMode`**：`startswith`（1）、`contains`（2，默认）、`exact`（3）、`regex`（4）；查询未给出 `matchMode` 时解析全局 `settings.window.titleMatchMode`。`regex` 模式将 `title`（或开启时的 `ahkClass`/`ahkExe`）按 AHK 正则编译：支持 `i)` 选项前缀；`m)`/`s)`（multiline/dotall）与未知字母同步抛 `Unsupported`，坏正则与空模式同步抛 `InvalidContract`——都不入队。`includeHidden` 缺省时解析全局 `settings.window.detectHiddenWindows`；无标题窗口始终被过滤。
- **全局设置**：`settings.window` 暴露 `titleMatchMode`（`"1"|"2"|"3"|"RegEx"`）、`titleMatchModeSpeed`（`"Fast"|"Slow"`）、`detectHiddenWindows`、`detectHiddenText` 四个属性与 `setTitleMatchMode`/`setDetectHiddenWindows`/`setDetectHiddenText` 三个方法；`set*` 返回前值（AHK return-previous 契约），`setTitleMatchMode` 接受模式串或 `"Fast"|"Slow"`（分派到速度旋钮）。默认：`"2"`、`"Fast"`、`false`、`false`。
- **`ahkId`**：正整数（number 或数字字符串），经 `from_chars` 严格解析，拒绝溢出与非数字。
- **`wait`（WinWait 家族）**：`until` ∈ `exists`（WinWait，成功返回目标快照）/ `active`（WinWaitActive，快照）/ `closed`（WinWaitClose，`null`）/ `notActive`（WinWaitNotActive，`null`；匹配不存在即视为满足，是 `active` 的逻辑否定）。`deadlineMs`（默认 5000）是整个等待预算，到期拒绝 `timeout`；`cancellationId` 触发拒绝 `cancelled`。实现为 25ms 调度器轮询 + worker lane 单次求值（`WindowService::evaluate_wait`）——**不阻塞任何线程**；窗口事件（WinEventHook）唤醒是后续优化，当前以轮询承担事件可见性。`until` 值非法时同步抛 `TypeError`（循环根本不启动）。
- **`groups`（WinGroup 家族）**：`rime:window` 的 `groups` 导出 + SDK `groups()` 门面，四者均为 `windows.window.write` 动作（目标 `{kind:"group"}`，接受 `deadlineMs`/`cancellationId`）。`add(name, query, options?)`（GroupAdd）按精确四字段去重、缺组即建、返回规格计数；`activate(name, options?)`（GroupActivate）循环激活成员（`reverse` 从最近成员开始；缺组**创建后**因无成员返回 `null`，AHK 的 create-if-missing）；`deactivate(name, options?)`（GroupDeactivate）激活合格的非成员（可见、非顶层置顶/无激活、非裸工具窗、无属主、非桌面），缺组拒绝 `invalid_contract`（AHK 参数错误）；`close(name, mode?, options?)`（GroupClose）`""` 关闭前台成员后激活下一成员（Windows 已自动提升下一成员时停在该处）、`"reverse"` 反向、`"all"` 关闭全部成员且不激活，缺组与未知 `mode` 拒绝 `invalid_contract`。组注册表与访问轮换状态（cap 500）活在 UI lane 的 `WindowService` 内，无锁、进程级；成员匹配复用查询语义（全局 `matchMode`/`detectHiddenWindows` 参与）；空 query 规格被同步拒绝（AHK 容忍占位空规格，Rime 视为契约错误）；结果无目标时为 `null`（对齐 `active()`/`wait()`），激活被前台锁拒绝时为 `execution_failed`，`close('all')` 的计数不受焦点影响。

必须逐项覆盖的 AHK 函数（研究清单；逐函数状态以 `compatibility-matrix.md` 为准）：`WinActivate`、`WinActivateBottom`、`WinClose`、`WinGetAlwaysOnTop`、`WinGetClass`、`WinGetClientPos`、`WinGetControls`、`WinGetControlsHwnd`、`WinGetCount`、`WinGetEnabled`、`WinGetExStyle`、`WinGetID`、`WinGetIDLast`、`WinGetList`、`WinGetMinMax`、`WinGetPID`、`WinGetPos`、`WinGetProcessName`、`WinGetProcessPath`、`WinGetStyle`、`WinGetText`、`WinGetTitle`、`WinGetTransColor`、`WinGetTransparent`、`WinHide`、`WinKill`、`WinMaximize`、`WinMinimize`、`WinMinimizeAll`、`WinMinimizeAllUndo`、`WinMove`、`WinMoveBottom`、`WinMoveTop`、`WinRedraw`、`WinRestore`、`WinSetAlwaysOnTop`、`WinSetEnabled`、`WinSetExStyle`、`WinSetRegion`、`WinSetStyle`、`WinSetTitle`、`WinSetTransColor`、`WinSetTransparent`、`WinShow`、`WinWait`、`WinWaitActive`、`WinWaitClose`、`WinWaitNotActive`、`GroupAdd`、`GroupActivate`、`GroupClose`、`GroupDeactivate`、`SetTitleMatchMode`、`DetectHiddenWindows`、`DetectHiddenText`。

验收必须覆盖 WinTitle 解析、隐藏窗口、前台激活拒绝、窗口销毁竞态、超时取消、队列关闭和稳定 ID。
