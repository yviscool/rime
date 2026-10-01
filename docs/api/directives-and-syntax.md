# 指令、标签与语法级能力

这些能力不出现在 `functions.h`，但会改变脚本加载、Hook 安装、线程调度和全局状态，必须单独建模。

## 已从源码确认的指令/语法族

源码命中：`#HotIf`、`#Hotstring`、`#InputLevel`、`#InstallKeybdHook`、`#InstallMouseHook`、`#MaxThreads`、`#MaxThreadsPerHotkey`、`#MaxThreadsBuffer`、`#MenuMaskKey`、`#SingleInstance`、`#SuspendExempt`，以及热键标签、热字符串标签、自动执行段和包含文件。

## TS 设计结论

- `#SingleInstance` 变为 Host 实例策略，不允许脚本在运行时偷偷覆盖另一个 Runtime。
- `#HotIf` 变为带 Context 快照的条件订阅；条件求值不能直接在 Hook 线程执行 JS。
- `#Install*Hook` 变为显式 `hooks.keyboard()`/`hooks.mouse()` capability；安装状态可检查且可取消。
- `#MaxThreads*`、`#InputLevel`、优先级和缓冲策略进入 SchedulerPolicy，不散落在回调实现。
- `#Hotstring` 变为 `hotstrings.register()`，保留大小写、单词边界、即时触发、替换方式和发送模式。
- 自动执行段变为模块加载后的显式 `main()`；退出、错误和回调通过版本化 Host 生命周期处理。
- `Sleep`、模态对话框和 `Critical` 不得创建第二个脚本消息泵；统一交给 JS scheduler 和可观察 Action。

## 验收

必须测试条件热键在活动窗口切换、Hook 安装/卸载、队列满载、回调重入、暂停/恢复、Runtime shutdown 和 unload busy 情况下的顺序与结果。
