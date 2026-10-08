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
- 嵌入式 Host ABI 必须提供版本化的 load/execute/error/exit/unload 契约；存在 Host 自己登记的活动资源（Hook 订阅、未投递的宿主事件、未解决 Promise、武装 Timer、JS 回调）时，unload 必须失败并逐项说明原因。窗口过程与 COM 元素引用由 Bootstrap 的服务生命周期持有，随 `Bootstrap::stop()` 释放，不进入 unload 判定；仅当它们以订阅/回调/事件队列条目被 Host 登记时才计入。

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
- `(await Window.active())?.move("left")` 能通过公开 API 执行。
- UIA、Win32 和 COM 资源没有泄漏或跨 Apartment 错误。
- UI Thread、JS Thread 和 Automation MTA 的边界可被测试。
- Action Trace 能记录输入、Context、执行器、结果和错误。
- 消息泵、Hook、Timer、COM 回调和 JS task 的顺序在压力、嵌套消息泵、队列满载和取消竞态下可重放。
- 卸载测试必须证明 Host 登记的 Hook 订阅、未投递宿主事件、Promise、Timer 和 JS 回调全部退出，并覆盖“仍有订阅/回调/未投递事件”的失败路径；窗口过程与 COM 元素引用的释放由 Win32/Automation 服务的 stop 测试独立证明。
- Host ABI 的 load/execute/error/exit/unload 以及脚本检查接口有版本化 contract 测试。
- Native 代码使用 ASan/WinDbg 验证句柄、内存和线程问题。
- 时间窗、deadline 与 timer 调度的原生单测注入 `ManualClock` 推进判定，不以 `sleep` 等待真实时间流逝；真时钟只留给必须真实投递的切片级测试。`ManualClock` 必须比注册了监听器的组件活得久。
- 公开 API 变化必须同步更新 TypeScript 声明和示例。

### 测试真实性分级（L1-L6）

每个测试按它实际证明的东西分级；新增测试在文件头注释声明最高等级（`Realism: Lx`），既有测试在下次改动时补注。

- L1 结构级：文件、schema、声明、矩阵、注册表的存在性与形状检查（schema-smoke、contract:check）。只证明“写对了”，不证明“做对了”。
- L2 纯逻辑级：无 OS、无时钟、无随机的纯函数与解析器（parse、format、match、golden 解码）。期望值由断言直接给定。
- L3 仿真组件级：真实组件 + 被替换的环境（ManualClock、内存队列、mock bridge、注入 policy）。被测代码真实运行，只有环境是假的。
- L4 真实组件级：进程内全链路真实接线并产生可验证结果（Kernel + 真实 Executor + 真实 service 的拒绝路径，golden failure 层）。只读或零副作用。
- L5 桌面系统级：真实 Win32/UIA/剪贴板/输入产生可观察副作用，且创建、断言、清理全部在测试内完成（fixture 窗口、进程树终止、剪贴板恢复）。
- L6 端到端对抗级：生产装配（`rime_js_bundle` / host 全 wiring、production capabilities）叠加强条件（压力、嵌套消息泵、取消竞态、卸载失败路径）。

分级义务：

- 行为契约至少 L3，关键执行路径必须有 L5，每个 Runtime 层至少一条 L6 竖切。
- 低于 L4 的测试不得作为行为契约的唯一证据；L1 永远不能给行为背书。
- 一条对外声明（registry、golden、公开 API）必须被两个独立消费者验证（R4 golden 的 TS/双 C++/QuickJS 消费者即是范例）。

### 反作弊九条

以下模式产生的“通过”不计为证据，评审必须拒绝：

1. 禁恒真断言：`expect(true)`、只断言“没抛异常”而不检查返回值、副作用或 Trace。
2. 禁测自己：期望值不得由测试内与被测对象同一份逻辑算出（测试里再实现一遍算法，会一起错一起绿）；期望必须来自 golden、OS 观察或协议对端。
3. 禁吞错：catch 后不失败、错误只打印不断言、`finally` 中的断言掩盖主错误。
4. 禁静默跳过：资源缺失即 skip 并算通过等于失败；只能显式失败，或按已记录的环境抖动协议隔离复跑后记录（协议与台账见 `docs/FLAKY.md`）。
5. 禁 sleep 当同步：等待必须是带超时的条件轮询、事件通知或 `ManualClock` 推进；纯 sleep 定时断言禁止（见时间策略条目）。
6. 禁重试洗绿：逻辑性失败不得循环重跑；只有已记录的环境抖动允许隔离复跑 ≤3 次并记录结论。
7. 禁 mock 被测者：桩只许替代环境（OS、时钟、桥），不许替代被测单元本身；对 mock 的断言不作契约证据。
8. 禁无断言执行：只调用 API 不验证结果、副作用或 Trace 的冒烟代码必须补断言，或改名为非测试脚本。
9. 禁孤证：单一低真实性测试不得承载对外契约；缺口未补时必须在报告中写明局限与补充计划。

## 修改原则

- 保持提交和改动范围小，避免无关重构。
- 优先复用现有层的接口，不为单个 Rim 功能创建 Runtime 特例。
- 新增抽象前先证明它解决了跨模块的真实重复或边界问题。
- 不以 README 中的愿景替代可运行的垂直切片；每个阶段都要留下可执行验证。
- AHK 永远是养料，不是目标：不做任何兼容（不追兼容数字、不复制 AHK 语义怪癖）。
  AHK 只允许以三种身份出现——需求雷达（查能力缺口）、跑分对手（同场景对照）、
  行为参照（重叠功能的边界定义）；引用时必须注明出处。新增 API 必须有独立于
  AHK 的现代 TS 理由，1-based 索引、魔法全局变量、字符串 DSL 等 AHK 主义不得
  进入新 API。
