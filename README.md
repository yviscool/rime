# Windows Runtime

[![CI](https://github.com/yviscool/rime/actions/workflows/ci.yml/badge.svg)](https://github.com/yviscool/rime/actions/workflows/ci.yml)

一个 Windows-first、JavaScript/TypeScript-native 的可编程桌面 Runtime。

它让：

- **Program Windows**：用 JavaScript/TypeScript 自动化 Windows；
- **Build Windows**：用现代组件模型构建 Windows 应用；
- **Operate Windows**：通过键盘、手势、Context、AI 和 Action 操作电脑。

Rim 是这个 Runtime 上的第一款完整应用。Runtime 本身不依赖 Rim；未来的 CLI、插件、自动化脚本和其他应用都可以直接使用同一套能力。

## 定位

项目不是 Electron、Tauri、Flutter 或新的 AutoHotkey 语法层。AutoHotkey 源码只作为研究 Windows 能力边界和调度约束的样本，不是公共命名、语法或行为兼容目标；公共 API 以现代 JavaScript/TypeScript 语义重新设计，并建立独立的 Windows Runtime Host。

```text
JavaScript / TypeScript
          ↓
        QuickJS
          ↓
    Windows Runtime
     ├── Automation
     ├── UI Runtime
     ├── Context
     └── Action Kernel
          ↓
Win32 / UIA / COM / Hooks / D3D / DirectComposition
          ↓
        Windows
```

## 仓库组成

- **QuickJS Host**：QuickJS-ng 宿主、模块加载、独立 JS Thread 的 `rime::js::Runtime`、`settle`/关闭语义和 `rime:*` 原生模块注册。
- **Action Kernel**：Action/Result 编解码（contract v1）、capability check、trace、scheduler/event queue 与 shutdown 阶段。
- **Win32 层**：Window、Input Hook、Process、Clipboard、UI Automation 五组服务，以及 `rime:window` / `rime:input` / `rime:process` / `rime:clipboard` / `rime:automation` JS 模块。
- **SDK 与 Rim**：`@rime/sdk` 公共 API；Rim bundle 在宿主内执行窗口、进程与剪贴板操作。
- **验证**：contract/typecheck/SDK 测试、MSVC CTest（/W4 /WX）、QuickJS 切片与 bundle 执行、MSVC AddressSanitizer，由 GitHub Actions 在 `windows-latest` 上执行。
- **API 规范**：`docs/api/` 是逐领域规范入口；`coverage.json` 登记 `functions.h` 的 253 个函数，`core-builtins.md`/`core-builtins.json` 登记 `script.cpp` 的 41 个核心内建，`objects.json`、`builtins.json`、`abi-and-language.md` 登记对象成员、内置变量和 Host ABI 来源。

设计原则、架构决策与阶段顺序见 `docs/`。逐项实现状态不在本文件维护，以 `docs/api/coverage.json` 与 `docs/api/compatibility-matrix.md` 为准。

## 技术栈

| 层 | 选择 | 责任 |
| --- | --- | --- |
| 系统 | Windows 10 22H2+ / Windows 11、Windows SDK | 系统目标和 ABI |
| Native Core | C++20、MSVC/clang-cl、CMake、Ninja | 宿主、资源、线程和生命周期 |
| JavaScript | 固定版本 QuickJS-ng | 脚本执行、模块和 GC |
| COM | WIL、标准 COM 接口 | UIA、Shell、Clipboard 和系统生态 |
| 窗口 | Win32 HWND、Message Loop、Raw Input、Hooks | 窗口和输入基础 |
| 图形 | D3D11、DXGI、DirectComposition、Direct2D、DirectWrite | GPU 合成、绘制和文字 |
| 自动化 | UI Automation、MSAA、Win32、SendInput | 控制其他 Windows 应用 |
| 存储 | SQLite WAL | 配置、索引、Recall 和插件状态 |
| 脚本工具链 | TypeScript、Bun build | TS/TSX 构建为可由 QuickJS 执行的 JavaScript |
| 验证 | CTest、Bun test、MSVC AddressSanitizer、WinDbg | 契约、切片与资源检查 |

应用与 SDK 以 TypeScript 为首选源码；Bun 只参与依赖管理、类型检查和构建。构建产物由 QuickJS-ng 执行，Runtime 不依赖 Node、Bun、Chromium、WebView2 或 Electron。

## 分层架构

```text
TypeScript Applications: Rim / CLI / Plugins
            ↓
       TypeScript SDK
            ↓
     Bun Build (build time)
            ↓
      JavaScript Bundle
            ↓
     QuickJS-ng Runtime
            ↓
Context / Intent / Action Kernel
            ↓
Engine UI Runtime
  Window / Visual / Layout / Style / Animation
            ↓
Automation Runtime
  Window / Process / Input / UIA / COM / Clipboard
            ↓
QuickJS Host
  Module Loader / Promise / Events / Bindings
            ↓
Native Core
  Thread / Handle / Error / Storage / Logging
            ↓
Windows
```

## 仓库结构

```text
contracts/  schema、spec 和 generated 公共契约
engine/     Native Core、Action Kernel、QuickJS Host 和 Win32 层
hosts/      OS/process adapter；desktop 是第一个宿主
sdk/        TypeScript API、类型和构建时工具
apps/       Rim、CLI 和示例应用
plugins/    插件 SDK、Manifest 和实现
tests/      contract、native、host、replay 和集成测试
tools/      Bun 构建、检查、测试、打包和诊断命令
docs/       架构与运维记录；docs/api 是逐领域 API 规范、覆盖矩阵和设计审查
site/       文档和 Playground 构建表面
```

核心依赖方向为 `contracts ← engine/core ← engine/action ← hosts/desktop`。平台适配器依赖核心接口，不反向依赖 Rim、SDK 或具体宿主。

当前验证入口：

```text
bun install
bun run doctor
bun run typecheck
bun run test
bun run test:all
```

`test` 依次执行 contract schema 校验、contract 生成检查、TypeScript 类型检查、SDK 测试、MSVC 原生 CTest 和 QuickJS 集成（含 Rim bundle 执行）；`test:all` 追加 MSVC AddressSanitizer 套件；`doctor` 检查 CMake/Ninja/MSVC/QuickJS-ng 工具链。GitHub Actions 使用同一组入口。

`ts:quickjs` 会构建 Rim 的 TypeScript 入口，再将 bundle 交给 C++ QuickJS Host 执行；宿主注册 `rime:window`、`rime:input`、`rime:process`、`rime:clipboard`、`rime:automation` 等原生模块，bundle 在其中执行 Rim 的窗口/进程/剪贴板查询并校验 settle 与确定性关闭。Native bridge smoke 同时验证宿主 API 可用。`test` 汇总契约、TypeScript、Native 和 QuickJS 验证。

Schema 的 JSON 是跨语言契约事实源；C++ 类型负责执行边界，TypeScript SDK 和插件协议必须从同一版本契约生成或校验，不能各自定义字段。

Automation 分为三类能力：

```text
Direct Control   Win32 / SendInput / PostMessage / Process
Semantic Control UI Automation / MSAA / Role / Name / Pattern
Visual Fallback  Capture / OCR / Template Matching
```

UI 使用保留式 Visual Tree：

```text
Component Tree → Layout Tree → Visual Tree
              → DComp Visual Tree
              → Direct2D / DirectWrite
              → D3D11 / GPU
```

## 统一执行模型

所有输入最终进入同一条执行管线：

```text
Keyboard / Gesture / Launcher / Plugin / AI
                  ↓
              Context Snapshot
                  ↓
                Intent
                  ↓
              Action IR
                  ↓
            Capability Check
                  ↓
             Action Executor
                  ↓
             Windows Runtime
```

Action 必须是可检查、可记录、可取消的中间表示，而不是只能直接调用的函数：

```ts
type Action =
  | { type: "window.move"; target: WindowRef; position: Position }
  | { type: "input.type"; target: ElementRef; text: string }
  | { type: "clipboard.write"; value: ClipboardValue }
  | { type: "process.launch"; command: LaunchSpec };
```

## 线程模型

```text
JS Thread       QuickJS、脚本回调、Action 编排
UI Thread       HWND、Direct2D、DirectWrite、DirectComposition
Automation MTA  UIA、COM 查询、WinEvent、进程观察
Worker Pool     文件、OCR、索引、压缩和 AI 请求
```

QuickJS、UI 对象和 COM 对象不得任意跨线程。跨线程只传递消息、ID、快照和序列化数据。耗时 Native 调用不能阻塞 JS Thread，所有异步操作都必须可取消。

事件调度还必须满足以下不变量：

```text
Win32 / Hook / Timer / COM completion
                  ↓
             Event Queue
                  ↓
       Scheduler + interrupt policy
                  ↓
             JS Task / Action
```

Win32 Message Pump 是 UI、Hook、Timer 和窗口回调的事件入口。消息泵可以被嵌套（例如模态对话框或系统拖放），但 Runtime 不得因此产生第二个未受监管的脚本调度器；所有嵌套泵都必须向同一个调度器报告，并遵守队列顺序、背压、取消和关闭状态。


MSAA 扩展、UI Runtime 和插件模块将在后续阶段以同样的 SDK 表面接入。未来标准库的模块划分与签名（`windows`、`keyboard`/`mouse`、`processes`、`fs`、`clipboard`、`displays`、`actions`，以及 branded `WindowId`/`WindowRef`、`CallOptions`、`Subscription`）以 `docs/api/future-runtime.md` 为准；AHK 函数名只用于覆盖矩阵追踪，不作为公共命名。

系统资源必须由 Native 层通过 RAII 管理。`HWND`、`HANDLE`、`IUnknown*` 等原始指针不能直接暴露给 JavaScript，只能通过 Runtime-owned opaque object 和稳定 ID 访问。

## 从 AutoHotkey 研究得到的 Windows 运行时经验

`AutoHotkey-alpha` 证明了桌面自动化最难的部分不是把 Win32 函数映射到脚本，而是让消息泵、输入 Hook、Timer、GUI 回调和脚本执行保持可预测的顺序。Runtime 保留这些边界经验，但不保留它的全局状态和隐式语义，也不以 AHK 命名、语法或行为兼容为目标：

- Win32 Message Pump 是 UI/输入事件的唯一入口；Hook、Timer、窗口消息和外部完成通知先进入事件队列，再由 JS Thread 的调度器交付。
- JS 回调不是新的操作系统线程。它们是带有来源、优先级、取消状态和父 Action 的 Runtime task；同一 JS Context 内禁止任意重入。
- 每个可能阻塞或改变活动窗口的操作都必须声明可中断区间。进入 SendInput、激活窗口、剪贴板交换等临界区后，新的脚本事件只能排队，不能隐式打断。
- 最大并发、事件节流和重复触发策略必须是显式的调度策略。队列满时必须有丢弃、合并或失败结果，并写入 Trace。
- Hook、Timer、GUI、COM callback 和 JS function reference 都属于 Runtime-owned subscription；关闭或卸载前必须先禁止新事件、取消订阅、等待回调退出，再释放其宿主对象。
- Runtime 需要版本化的 Host ABI：加载、执行、错误回调、检查（inspect）、退出码和卸载（`load/execute/error/inspect/exit/unload`）都必须是显式接口。若仍有 Hook、窗口过程、COM 引用或 JS 回调，卸载必须返回可诊断的 busy/failed 状态，而不是依赖进程退出清理。
- 宿主应能检查模块、函数、源文件、行号、订阅和当前 Action；AutoHotkey 宿主 ABI 的 `Funcs`、`Vars`、`Labels`、`OnProblem` 说明可检查的宿主比只有执行入口更适合长期工具链。

这些规则取代了 AutoHotkey 中依赖全局变量、伪线程栈和隐式 `MsgSleep()` 的兼容行为。我们只保留已经被 Windows 现实验证过的调度和生命周期约束。

## 插件模型

插件权限使用能力声明：

```json
{
  "permissions": [
    "windows.window.read",
    "windows.window.write",
    "windows.clipboard.read",
    "windows.clipboard.write",
    "process.launch"
  ]
}
```

权限名称与 `docs/api/coverage.json` 的 `capability` 字段同一命名空间（如 `windows.input.inject`、`windows.automation.control`、`windows.hook.global`、`filesystem.read/write`、`screen.capture`、`native.unsafe`），按能力授予，不按函数散落判断。

可信插件可以进程内运行；不可信插件通过独立进程、Named Pipe/RPC 和 capability token 运行。插件 API 必须版本化，不能依赖 Rim 的内部实现。

## 工程顺序

1. QuickJS + C++ Host + Module Loader
2. Native Core：线程、句柄、错误、日志、关闭流程
3. Win32 Window、Process、Input API
4. UIA、COM 和 UI Automation 对象生命周期
5. Action IR、Dispatcher、Capability Check 和 Trace
6. D3D11、DirectComposition、Direct2D、DirectWrite
7. Window、Visual、Layout、Style、Text 和 Animation
8. TypeScript SDK、TSX 编译和 CLI
9. Plugin Manifest、权限和进程隔离
10. Rim 第一条完整垂直切片


```text
Global Hotkey → Context → Action IR → Action Kernel → Windows
```

## 设计不变量

- 一个宿主、一个线程模型、一个资源生命周期模型。
- 一个错误模型、一个事件模型、一个 Context、一个 Action IR。
- Runtime 不依赖 Rim；Rim 只能依赖公开 Runtime API。
- UI Thread 负责 UI；JS Thread 负责脚本；跨线程使用消息和快照。
- 每个 Native 资源都有明确所有权和确定性关闭路径。
- 每个系统能力都能被权限检查、记录和取消。
- 公共 API 优先稳定，内部实现可以迭代。
- AutoHotkey 源码只是能力研究样本；公共 API 不承诺 AHK 命名或行为兼容。
