# Engineering Rules

本仓库实现的是 Windows-first、JavaScript/TypeScript-native 的 Programmable Desktop Runtime。Rim 是第一个应用，不是 Runtime 的定义。

## 目录边界

```text
engine/   平台无关 Native Core、QuickJS、Automation、UI、Action
hosts/    Windows/desktop 宿主与未来平台适配器
sdk/      TypeScript 类型、TSX 编译、公共 API
apps/     Rim、CLI 和示例应用
plugins/  插件 SDK、清单和插件实现
tests/    Contract、Native、Host、UI、Automation 和集成测试
tools/    Bun 构建、测试、诊断和开发工具
```

参考调研目录不是产品源码，不应被当作 Runtime 的依赖或架构模板。

## 技术约束

- Native 主体使用 C++20、MSVC/clang-cl、CMake 和 Ninja。
- JavaScript 引擎使用固定版本 QuickJS-ng。不要把 Node、Bun、Chromium 或 WebView2 作为 Runtime 前提。
- TypeScript/TSX 只在构建阶段由 Bun build（all-in-one：包管理、类型检查、构建）编译为 JavaScript。
- Windows API 优先使用 Win32、UIA、COM、D3D11、DXGI、DirectComposition、Direct2D 和 DirectWrite。
- Windows 资源使用 RAII 和明确所有权。不得向 JS 暴露裸 `HANDLE`、`HWND` 或 COM 指针。
- 公共 JS API 使用模块和稳定 ID，不使用散落的全局函数。

## 线程与生命周期

- QuickJS 运行在单独的 JS Thread，不跨线程访问 `JSRuntime` 或 `JSContext`。
- HWND、Direct2D、DirectWrite 和 DirectComposition 对象只在 UI Thread 使用。
- UI Automation 和 COM 对象遵守 Apartment 规则；不要把 COM 对象未经封送传给其他线程。
- 耗时 Native 调用必须异步化，并支持取消。
- 跨线程传递消息、不可变快照、稳定 ID 或序列化值，不传递拥有状态的裸指针。
- Win32 Message Pump 是 UI、Hook、Timer 和窗口回调的事件入口；不得在模块内部另起未受监管的脚本消息泵。
- JS 回调使用 Runtime task 和调度器，不把每个事件伪装成 OS 线程；同一 JS Context 内禁止隐式重入。
- 长操作必须声明可中断区间。窗口激活、SendInput、剪贴板交换等临界区内，新事件只能排队或按明确策略合并。
- Hook、Timer、窗口过程、COM callback 和 JS function reference 必须是可取消的订阅对象，拥有明确的关闭状态和回调计数。
- Runtime 关闭必须可重复、可观察：先拒绝新输入和新 Action，取消并等待任务/订阅，卸载 Hook 和窗口回调，停止插件与 Worker，再销毁 UI/COM 资源，最后关闭 JS；任何未释放引用都必须产生诊断。
- 嵌入式 Host ABI 必须提供版本化的 load/execute/error/exit/unload 契约；存在活动 Hook、窗口过程、COM 引用或 JS 回调时，unload 必须失败并说明原因。

## 架构规则

- 所有输入统一经过 `Context → Intent → Action IR → Action Kernel`。
- Action 必须可检查、可记录、可取消，并能返回明确的执行结果和失败原因。
- Automation 分为 Direct、Semantic、Visual 三层；不要把 UIA、坐标点击和 OCR 混成一个不可解释的调用。
- UI 使用 Component Tree、Layout Tree、Visual Tree 和渲染器分层。
- UI Runtime 不依赖 Rim；Rim 的功能必须通过公开 Runtime API 实现。
- 插件 API 必须版本化并声明权限。可信进程内插件和不可信进程外插件不能共享同一信任假设。
- 调度策略必须集中定义：最大并发、重复事件、节流、队列满载时的合并/丢弃/失败结果，不能散落在 Hook、Timer 和模块实现中。
- Action 的临界区、可中断点、重入策略和父子关系必须进入 Trace；禁止依赖隐式 `Sleep` 或全局可变标志协调模块。
- 宿主必须提供可检查接口：模块、函数、源位置、订阅、当前任务和错误都能被 CLI/调试器读取，且读取不会执行未知脚本。

## 实现顺序

先完成 Native Core、QuickJS Host、模块加载、Win32 基础能力和对象生命周期，再扩展 UI、Action、插件和 Rim。不要先在 Rim 中创建只能由 Rim 使用的底层抽象。

每个新能力都应回答：

1. 它属于哪个 Runtime 层？
2. 它的线程和资源所有权是什么？
3. 它如何通过 JS 模块暴露？
4. 它是否需要权限、取消、事件和诊断？
5. 它是否能被 CLI、插件和 Rim 共同使用？

## 验证要求

优先验证系统成立性，而不是只验证展示效果：

- QuickJS 能加载 Native 模块并确定性退出。
- `Window.active().move("left")` 能通过公开 API 执行。
- UIA、Win32 和 COM 资源没有泄漏或跨 Apartment 错误。
- UI Thread、JS Thread 和 Automation MTA 的边界可被测试。
- Action Trace 能记录输入、Context、执行器、结果和错误。
- 消息泵、Hook、Timer、COM 回调和 JS task 的顺序在压力、嵌套消息泵、队列满载和取消竞态下可重放。
- 卸载测试必须证明 Hook、窗口过程、COM 引用、订阅和 JS 回调全部退出，并覆盖“仍有外部引用/Hook 未卸载”的失败路径。
- Host ABI 的 load/execute/error/exit/unload 以及脚本检查接口有版本化 contract 测试。
- Native 代码使用 ASan/WinDbg 验证句柄、内存和线程问题。
- 公开 API 变化必须同步更新 TypeScript 声明和示例。

## 修改原则

- 保持提交和改动范围小，避免无关重构。
- 优先复用现有层的接口，不为单个 Rim 功能创建 Runtime 特例。
- 新增抽象前先证明它解决了跨模块的真实重复或边界问题。
- 不以 README 中的愿景替代可运行的垂直切片；每个阶段都要留下可执行验证。
