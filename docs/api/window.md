# Window API

状态：`implemented`（Rime 原生表面）。`list`（WinTitle 查询）、`active`、`info`、`move`、`focus`、`close`、`hide`、`show`、`minimize`、`maximize`、`restore` 均有 TS binding、`windows.window.read`/`windows.window.write` capability 校验、`window-v1` contract（`contracts/schema/window-v1.schema.json`）与 native/slice 测试。读写接受 `deadlineMs`/`cancellationId`（`AbortSignal`）选项，deadline 过期与排队超时返回 `timeout` 错误码。

源码证据：`rime-research/AutoHotkey-alpha/source/lib/functions.h` 的 `Win*`、`WinGroup*`；`source/window.cpp`、`source/window.h`；Rime 的 `engine/win32/src/window.cpp` 和 `engine/win32/src/ui_thread.cpp`。

核心类型使用稳定 `WindowId`、不可变 `WindowSnapshot` 和 `WindowQuery`。JS 不接收 `HWND`。查询、快照和等待走 Promise；纯字段访问同步。写操作生成 Action 并记录 Trace。窗口消失时返回 `InvalidState`，ID 在服务生命周期内单调不复用（跨服务 generation 尚未实现，见 `docs/api/future-runtime.md`）。

## 测试覆盖（如实）

- `tests/js/vertical_slice.cpp`：11/11 个 JS 调用全覆盖（含查询、状态机与关闭）。`focus` 允许 `SetForegroundWindow` 被前台锁拒绝（合法失败分支）；`move('active')` 仅在我方窗口持有前台时执行，否则整段 SKIP——测试不注入全局输入、不移动任何外来窗口。同文件另覆盖 `deadlineMs:0` 的 `timeout` 拒绝与非法 `deadlineMs` 的同步 `TypeError`。
- 8/8 个写类型经 JS → Kernel → Executor → UI lane 端到端验证；`tests/native/win32_tests.cpp` 另覆盖 8/8 写类型的原生分派、空 policy 下逐类型的 capability 拒绝（拒绝后状态不变）与排队超时。
- `docs/api/coverage.json` 与 `compatibility-matrix.md` 的行是**逐 AHK 函数**状态（已测函数为 `implemented` 并带 contractTest 路径，未测扩展项仍为 `contract-only|missing`），与本页 Rime 原生 API 的状态不构成矛盾：两者粒度不同。
- cancel（`cancellationId`）：仅 kernel 通用路径有覆盖，window JS 入口尚无取消测试。

## 执行模型

**写路径（Action）**：JS 构造 Action → Kernel（契约校验 → 取消检查 → 入口 deadline → capability → 执行器 → 执行后 deadline/取消复检）→ Executor（每次 UI 往返前重算剩余预算，含 `active` 目标解析）→ UI lane → 快照结果。

- 执行后过期：结果改写为 `timeout`，消息声明 `side effects may have occurred`（副作用不可撤销，调用方据此判断）。
- 执行后取消：同理改写为 `cancelled` 并声明副作用可能已发生。
- 取消边界：仅在「执行前 / 往返之间 / 执行后」检查；已进入的 Win32 调用（如 `SendMessageTimeoutW`）只能等其超时，不被强行打断。

**读路径（观察，豁免于 Action Kernel）**：`list`/`active`/`info` 经同一 capability 策略校验后直调 `WindowService`，**不产生 `ActionStarted/ActionFinished` Trace，也没有 Kernel 级 deadline**；`deadlineMs` 在请求构建时折算为该次 UI 阶段的等待预算。这是观察（Snapshot/Query）与变更（Action）的语义分界，属文档化豁免，不是遗漏。

## 语义细则

- **前台选择**：只用 `active: true`。历史上的 `title == "A"` 魔法短路已删除——不存在无法按字面匹配的标题。
- **`focus`**：先 `SW_RESTORE` 再 `SetForegroundWindow`；被前台锁拒绝时返回 `ExecutionFailed`，消息含 `foreground lock`。
- **大小写**：`title` 全 Unicode 不敏感；`ahkClass`/`ahkExe` 为 ASCII fold（规则见 `window.cpp` 的 `fold_ascii` 注释），非 ASCII 部分按精确字节比较。
- **`matchMode`**：`contains`（默认）或 `exact`；`includeHidden` 决定是否包含隐藏窗口，无标题窗口始终被过滤。
- **`ahkId`**：正整数（number 或数字字符串），经 `from_chars` 严格解析，拒绝溢出与非数字。

必须逐项覆盖的 AHK 函数（研究清单，逐函数绑定未开始）：`WinActivate`、`WinActivateBottom`、`WinClose`、`WinGetAlwaysOnTop`、`WinGetClass`、`WinGetClientPos`、`WinGetControls`、`WinGetControlsHwnd`、`WinGetCount`、`WinGetEnabled`、`WinGetExStyle`、`WinGetID`、`WinGetIDLast`、`WinGetList`、`WinGetMinMax`、`WinGetPID`、`WinGetPos`、`WinGetProcessName`、`WinGetProcessPath`、`WinGetStyle`、`WinGetText`、`WinGetTitle`、`WinGetTransColor`、`WinGetTransparent`、`WinHide`、`WinKill`、`WinMaximize`、`WinMinimize`、`WinMinimizeAll`、`WinMinimizeAllUndo`、`WinMove`、`WinMoveBottom`、`WinMoveTop`、`WinRedraw`、`WinRestore`、`WinSetAlwaysOnTop`、`WinSetEnabled`、`WinSetExStyle`、`WinSetRegion`、`WinSetStyle`、`WinSetTitle`、`WinSetTransColor`、`WinSetTransparent`、`WinShow`、`WinWait`、`WinWaitActive`、`WinWaitClose`、`WinWaitNotActive`。

验收必须覆盖 WinTitle 解析、隐藏窗口、前台激活拒绝、窗口销毁竞态、超时取消、队列关闭和稳定 ID。
