# 指令、标签与语法级能力

这些能力不出现在 `functions.h` 的 253 项 `md_func` 中，但会改变脚本加载、Hook 安装、线程调度和全局状态，必须单独建模。

## 真值源

指令全集来自 `rime-research/AutoHotkey-alpha/source/script.cpp` 的 `IS_DIRECTIVE_MATCH` 链，共 22 项，此后只允许状态流转不允许静默增删。

v1 的 `#InstallKeybdHook`、`#InstallMouseHook`、`#MenuMaskKey` 在 v2 中已不是指令：前两者变为 `InstallKeybdHook()`/`InstallMouseHook()` 函数（已在 `coverage.json` 253 项内），`#MenuMaskKey` 变为可写内置变量 `A_MenuMaskKey`（已在 `builtins.json` 内）。

## 22 项指令分类

分类口径：**TS 等价 API**（Runtime 公开 API 直接承接）、**配置映射**（宿主/调度策略/构建期配置）、**排除**（不进入 Runtime，且有明确等价物或理由）。

| 指令 | 分类 | TS 等价 / 去向 | 说明 |
|---|---|---|---|
| `#HotIf` | TS 等价 API | `hotkeys.register({ criterion })` | 条件热键订阅；条件基于 Context 快照求值，不在 Hook 线程执行 JS |
| `#Hotstring` | TS 等价 API | `hotstrings.register()` | 大小写、单词边界、即时触发、替换方式、发送模式作为全局默认 |
| `#Include` | TS 等价 API | ES module import + 构建期 bundler | 构建期展开，运行时不存在文件包含 |
| `#SuspendExempt` | TS 等价 API | `hotkeys.register({ suspendExempt })` | 订阅级属性，`suspend()` 时按属性豁免 |
| `#MaxThreads` | 配置映射 | `SchedulerPolicy.maxConcurrency` | 调度策略集中定义，不散落在回调实现 |
| `#MaxThreadsBuffer` | 配置映射 | `SchedulerPolicy` 满载策略 | 队列满载时的合并/丢弃/失败结果 |
| `#MaxThreadsPerHotkey` | 配置映射 | `SchedulerPolicy` 每订阅并发上限 | 同一订阅的重复事件策略 |
| `#InputLevel` | 配置映射 | input 注入层级策略 | 注入事件的级别与免疫规则进入 SchedulerPolicy |
| `#HotIfTimeout` | 配置映射 | `SchedulerPolicy.hotIfTimeout` | 条件求值超时与失败策略 |
| `#SingleInstance` | 配置映射 | Host 实例策略 | 只允许 Host 决定单实例行为，脚本不能覆盖另一个 Runtime |
| `#Requires` | 配置映射 | manifest 版本门槛 | 与 `runtime.info().ahkVersion` 对照，在加载期失败 |
| `#ErrorStdOut` | 配置映射 | Host 错误输出策略 | CLI/宿主把 error 事件定向到 stderr，由宿主配置决定 |
| `#NoTrayIcon` | 配置映射 | Host tray 策略 | manifest/host config 决定托盘图标，脚本无权隐藏 |
| `#Import` | 配置映射 | 构建期模块别名解析 | bundler/module resolver 的输入 |
| `#Module` | 配置映射 | 构建期模块声明 | `ParseModuleDirective` 对应构建期模块解析 |
| `#UseHook` | 配置映射 | hooks policy | 查询类能力显式走 `hooks.*` capability，安装状态可检查可取消 |
| `#WinActivateForce` | 配置映射 | `windows.activate({ force })` 默认值 | 激活失败时的降级策略是行为选项，不是脚本特权 |
| `#ClipboardTimeout` | 排除 | Action `deadline` + `AbortSignal` | 剪贴板等待的超时由调用方 deadline 承担，不设全局隐式超时 |
| `#DllLoad` | 排除 | 无（unsupported-by-policy） | DLL 装载只服务裸互操作，而 `DllCall` 不在 Runtime 暴露面 |
| `#StructPack` | 排除 | 无（unsupported-by-policy） | 结构体布局属裸内存布局互操作，无安全等价物 |
| `#Warn` | 排除 | 构建期 typecheck + lint | 脚本运行期警告由编译期检查取代 |
| `#IncludeAgain` | 排除 | 模块缓存语义唯一 | ESM 每模块只执行一次；重复包含需求由构建期展开覆盖 |

统计：TS 等价 API 4、配置映射 13、排除 5。

## 语法族（非指令）

- 热键标签 → `hotkeys.register()`，回调是普通 JS 函数；
- 热字符串标签 → `hotstrings.register()`；
- 自动执行段 → 模块加载后的显式 `main()`；退出、错误和回调通过版本化 Host 生命周期处理；
- 包含文件 → 模块系统（见 `#Include` 行）。

## TS 设计结论

- `Sleep`、模态对话框和 `Critical` 不得创建第二个脚本消息泵；统一交给 JS scheduler 和可观察 Action。
- 所有"配置映射"类必须集中进 SchedulerPolicy、Host manifest 或构建期配置三个位置，禁止散落在 Hook、Timer 和模块实现中。

## 验收

- 22 项分类表与 `script.cpp` 的 `IS_DIRECTIVE_MATCH` 集合一致（CI 漂移检查）。
- 必须测试条件热键在活动窗口切换、Hook 安装/卸载、队列满载、回调重入、暂停/恢复、Runtime shutdown 和 unload busy 情况下的顺序与结果。
