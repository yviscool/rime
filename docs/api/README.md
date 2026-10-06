# Windows Automation API 规范

这里是可独立实现和验收的 API 规范入口。总览设计见 [`../AHK-TS-WINDOWS-API-DESIGN.md`](../AHK-TS-WINDOWS-API-DESIGN.md)；面向未来的统一标准库见 [`future-runtime.md`](./future-runtime.md)。AHK 只用于研究能力边界，不是公共命名或兼容目标。

## 覆盖矩阵

[`coverage.json`](./coverage.json) 是从 `rime-research/AutoHotkey-alpha/source/lib/functions.h` 提取的 AHK v2 内建函数清单。每一项都必须有源码证据、TS 模块、同步/异步判定、权限、取消点、错误语义和测试状态。

[`compatibility-matrix.md`](./compatibility-matrix.md) 是面向评审和实现分派的逐函数表；它由覆盖矩阵生成，显示每个函数命中的 AHK 源码文件、目标 lane、同步性、权限、取消策略和当前状态。新增函数或调整映射时，必须同时更新 JSON 和生成表。

已具备行为契约的能力另有一层硬证据：[`contracts/golden/`](../../contracts/golden/) 的 15 个 Action golden（四层 `shape`/`lifecycle`/`semantic`/`failure`）由 `bun tools/golden-smoke.ts`、`rime_golden_tests`、`rime_golden_exec_tests` 与 QuickJS fixture 四个独立消费者交叉验证，其中 `tests/native/golden_exec_tests.cpp` 把 `failure` 与 `semantic` 层经真实桌面 executor 执行。对应 AHK 行的 `contractTest` 会引用该测试（映射见 [`../AHK99-IMPLEMENTATION-PLAN.md`](../AHK99-IMPLEMENTATION-PLAN.md) §1.2 与 [`contracts/golden/README.md`](../../contracts/golden/README.md)）。

当前状态含义（口径见 [`stdlib.md`](./stdlib.md) §3）：

- `contract-only`：目标契约已在设计中，但没有完成 Native binding、Action executor 和 contract test（JSON 状态词，与旧文档中的 `specified` 同义）；不是终态。
- `js-native`：由 ECMAScript 标准能力直接承担，零专属代码；等价表达式与可观察差异记录在 [`runtime-language.md`](./runtime-language.md)（或本域文档），以该记录替代测试 ID。终态。
- `unsupported-by-policy`：保留能力边界说明，但默认不向脚本暴露危险的裸指针或任意进程内调用（理由入档，另有拒绝行为测试）。终态。
- `excluded`：AHK v1 别名或版本差异条目，不计入分母；理由记录在各自的 `source` 字段。
- `implemented`：Native binding、执行器和 contract test 均已完成，`contractTest` 指向真实存在的测试/契约路径。当前 Window 基础操作、窗口扩展读（WinGet 快照字段与 WinExist/WinActive 探针）、控件与文本读（WinGetControls/WinGetText）、窗口全局设置（SetTitleMatchMode/DetectHiddenWindows/DetectHiddenText）、`process.*` 查询与启动终止、`input.send`/`input.mouse` 注入族、键鼠读与键名解析（KeyWait/GetKeyState/BlockInput/KeyHistory/GetKeyName/GetKeyVK/GetKeySC/Set*LockState）、全局 Hook 之上的事件族（Hotkey/Hotstring/HotIf/Timer/OnMessage/OnClipboardChange/OnError/OnExit/InputHook/suspend/policy）、剪贴板读写和 `A_Clipboard`、`runtime-language` 还原族与生命周期 API 达到此标准（终态）。
- 旧词 `sdk-owned` 已按 [`stdlib.md`](./stdlib.md) §3 收敛：纯标准能力的条目改判 `js-native` 或 `contract-only`，台账中不再出现该状态。

当前矩阵的 `sourceFiles` 是基于函数名的源码命中结果，属于研究索引；在进入实现前，必须把它收敛到实际定义函数和关键 Win32 调用，并补充精确的错误和返回值语义。

## 领域规范

每个领域文件使用同一结构：API 清单、源码证据、TS 类型、底层实现、线程和资源所有权、异步/取消、权限、错误、Trace、设计取舍、contract test 和实现状态。

| 文件 | 范围 |
|---|---|
| [`window.md`](./window.md) | WinActivate、WinClose、WinGet*、WinSet*、WinWait*、WindowRef |
| [`control.md`](./control.md) | Control*、Edit*、ListView/TreeView/StatusBar、UIA |
| [`input.md`](./input.md) | Send*、Mouse*、KeyWait、BlockInput、键状态 |
| [`hotkey-events.md`](./hotkey-events.md) | Hotkey、Hotstring、Hook、SetTimer、OnMessage、生命周期事件 |
| [`clipboard.md`](./clipboard.md) | ClipWait、剪贴板读写和变化订阅 |
| [`process-shell.md`](./process-shell.md) | Run*、Process*、Shutdown、Shell |
| [`storage.md`](./storage.md) | File*、Dir*、Drive*、Env*、Ini*、Download |
| [`registry.md`](./registry.md) | RegRead/RegWrite/RegDelete/RegCreateKey/RegDeleteKey、SetRegView |
| [`screen.md`](./screen.md) | Monitor*、SysGet*、Pixel*、ImageSearch、Caret |
| [`gui-menu.md`](./gui-menu.md) | Gui、Menu、Tray、ToolTip、MsgBox、InputBox、Sound、ImageList |
| [`native-interop.md`](./native-interop.md) | DllCall、ComCall、Callback、Obj*DataPtr、注册表和隔离策略 |
| [`runtime-language.md`](./runtime-language.md) | Runtime 生命周期、字符串、日期、正则和 TS 标准能力 |
| [`runtime.md`](./runtime.md) | `rime:runtime` 模块：delay、cancellation、subscribe、inspect、context |
| [`source-inventory.md`](./source-inventory.md) | 函数之外的对象、ABI、内置变量和语法来源 |
| [`core-builtins.md`](./core-builtins.md) | `script.cpp` 独立注册的 101 个核心内建函数 |
| [`object-model.md`](./object-model.md) | Object/GUI/Menu/File/InputHook/COM 对象边界 |
| [`abi-and-language.md`](./abi-and-language.md) | Host ABI、内置变量和语言级行为 |
| [`audit-gaps.md`](./audit-gaps.md) | 未逐项提取的来源和完成标准 |
| [`directives-and-syntax.md`](./directives-and-syntax.md) | 指令、标签、热字符串和语法级事件 |
| [`state-and-errors.md`](./state-and-errors.md) | 隐式状态、错误原型、Trace 和可重放语义 |
| [`source-closure.md`](./source-closure.md) | 完整来源闭包与审计顺序 |
| [`design-review.md`](./design-review.md) | 五个核心问题的统一设计审查与最终决策 |

## 单项规范模板

新增或修改 API 时必须填写：

```text
AHK 名称与版本
源码证据（文件、函数、关键调用）
TS 模块与签名
同步/异步和所属 lane
Action type / capability
取消点、超时和重入策略
错误码和失败结果
资源所有权与关闭路径
Trace 字段
设计取舍与研究依据
contract / native / stress 测试
实现状态
```
| [`ts-windows-model.md`](./ts-windows-model.md) | TS types, Window identity, async boundaries and hidden native details |
