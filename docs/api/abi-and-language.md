# Host ABI、内置变量与语言边界

## Host ABI

研究依据：`source/ahklib.idl`、`source/abi.h`、`source/abi.cpp`、`README-LIB.md`。Rime 的 ABI 必须版本化为 `load/execute/error/inspect/exit/unload`，并使用序列化值和稳定订阅 ID。存在活动外部引用时 `unload` 返回可诊断的 busy 结果。

## 内置变量

`A_*` 变量不是普通函数，不能遗漏。应按以下域建立 `builtins.json`：脚本路径/命令行、工作目录、时间/日期、窗口和控件、键鼠状态、循环/线程、用户和系统、临时目录、语言/编码、退出状态、托盘/脚本实例。每个变量要标记：动态读取还是快照、来源 API、更新时机、线程、权限和 TS 替代。

## 语法与运行时

热键标签、热字符串、自动执行段、函数对象、异常/警告、伪线程、`Critical`、`Thread`、`OnError` 和消息泵顺序不能通过普通函数清单表达。Rime 不复制 AHK 的伪线程栈；用 JS task、Action parent、优先级、取消和 trace 表达同一类可观察行为，并记录无法兼容的细节。
