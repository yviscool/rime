# 状态、错误与可重放语义

第二轮源码审计发现，AHK 的行为还依赖大量隐式全局状态，不能只按函数签名建模。`globaldata.h/.cpp` 至少包含：

- 主窗口、编辑窗口、Hook 句柄、播放 Hook、托盘/Tooltip/菜单状态；
- 逻辑/物理修饰键、物理键状态、鼠标按钮、输入级别、MenuMaskKey；
- 热键节流、最大线程、线程缓冲、暂停/挂起、Persistent、OnExit 状态；
- Timer 层、InputHook 超时、消息监视器、模态消息框、文件/文件夹对话框计数；
- GUI 链表、Sort 状态、Hotstring 缓冲和大小写/回退/结束字符策略；
- 工作目录、编码、剪贴板超时、屏幕 DPI、单实例策略和警告模式。

这些状态在 Rime 中必须拆成有所有权的服务状态：SchedulerPolicy、InputState、HotkeyRegistry、WindowService、GuiService、ClipboardService、DialogService 和 HostLifecycle。不得把它们重新做成跨模块可写的全局变量。

## 错误来源

`error.cpp`、`defines.h` 和 `script.cpp` 中的 `ErrorPrototype`、`ERR_*`、`FError`、`RuntimeError`、`OnError`、警告和退出路径构成独立错误契约。需要区分：参数错误、类型/未设置错误、目标不存在、Win32/OLE 错误、超时、取消、权限拒绝、队列拒绝、脚本异常、OnError 处理后继续、OnExit 阻止退出和不可恢复的 critical exit。

Rime 的 `RuntimeError` 至少应包含 `code`、`message`、`operation`、`target`、`nativeCode`、`traceId`、`cause` 和 `recoverable`。不能只把所有 AHK 失败压成布尔值或普通字符串。

## 可重放要求

Trace 必须记录输入快照、解析后的 query/key sequence、命中的 target ID、执行层（Win32/UIA/visual/COM）、状态变更、取消点、错误原型和最终结果。Hotkey/Timer/GUI/COM 回调必须记录父 task、队列策略和回调计数，才能重放嵌套消息泵与关闭竞态。

### TraceEntry 字段现状

结构定义在 `engine/core/include/rime/core/trace.hpp`，下表是**实际已实现**的字段（不是愿景清单）：

| 字段 | 含义 | 状态 |
| --- | --- | --- |
| `sequence` | 全局单调序号，由 `TraceSink` 加锁分配，跨 producer 共享一个全序 | 已实现 |
| `kind` | `TraceKind`（EventAccepted / EventDispatchStarted / EventDispatchFinished / ActionStarted / ActionFinished / StateChanged / ActionAccepted / ActionRefused） | 已实现 |
| `subject` | 事件名 / action type / 组件名（runtime、dispatcher） | 已实现 |
| `detail` | 人类可读细节（既有字符串语义保持不变，golden 与调度测试断言它们） | 已实现 |
| `action_id` | Action 身份；事件 / 状态条目恒为 0 | 已实现 |
| `capability` | Action 所需权限；仅 Action* 条目 | 已实现 |
| `result_code` | 契约错误码名（成功为 `none`）；Started / Accepted 为空 | 已实现 |
| `duration_ms` | Action 条目：executor 段耗时（steady，失败未进 executor 时为 0）；`EventDispatchFinished`：handler 分段耗时（dispatch 开始 → handler 返回，同一 Clock 域）；`EventDispatchStarted` 恒为 0 | 已实现（事件分段为本次新增） |
| `unix_ms` | 写入时刻：`system_clock` Unix epoch 毫秒，由 `InMemoryTrace` 在加锁写入时盖戳（与 `sequence` 同锁，故与序号同单调） | 已实现（本次新增） |
| `queue_wait_ms` | 队列等待：`ActionAccepted → ActionStarted` 的间隔，**仅在 `ActionStarted` 填充**（accepted 时刻未知时为 0）；其余条目恒为 0 | 已实现（本次新增） |

三段耗时的链路：`Dispatcher::submit` 在接受时给 `Action.accepted_unix_ms` 盖戳（该字段为进程内诊断字段，不进 wire：`codec.cpp` 的 `encode_action` 写显式字段表、`decode_action` 用 `has_only_keys` 拒绝多余键）→ Kernel 在 `ActionStarted` 计算 `queue_wait_ms` → `ActionFinished.duration_ms` 记 executor 段 → `EventDispatchFinished.duration_ms` 记 handler 段。

读取面：`hosts/desktop` 在 `RIME_TRACE=1`（任意非空且非 `0` 的值）时，退出前把 trace snapshot 以 JSON Lines 打到 stderr，每行一个条目，字段为 `sequence`、`kind`、`subject`、`detail`、`actionId`、`capability`、`resultCode`、`durationMs`、`unixMs`、`queueWaitMs`；未设置该变量时保持静默，dump 失败不影响退出码。

### 未实现（不要当作已有能力）

- 父 task：Hotkey/Timer/GUI/COM 回调的 parent task id 未记录。
- 回调计数：回调在飞 / 重入计数未记录。
- 队列策略快照（capacity / overflow / dedupe 窗口）未随条目落盘。
- 输入快照、解析后的 query / key sequence、命中的 target ID、执行层（Win32/UIA/visual/COM）、错误原型字段未进入 TraceEntry；目前只有 action 信封（capability / result_code）与耗时字段。

