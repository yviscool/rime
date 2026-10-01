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
