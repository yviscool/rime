# TS Windows Automation 设计审查

审查日期：2026-10-01

设计取向：不实现 AHK 兼容层。AHK 源码只用于发现 Windows automation 所需的能力和边界；公共 API 以未来的 TypeScript 语义重新设计。

结论：当前文档已经覆盖 AHK 源码来源、253 个 `functions.h` 函数、41 个核心 BIF、对象/GUI/Menu/InputHook/File/COM 来源、内置变量、指令、状态和错误。但它还没有完全满足“可直接实现的标准库规范”：TS 公共类型、对象成员的逐项映射、内置变量的读取模型、Window Native binding wire contract 和每个 API 的测试 ID 仍需继续收敛。

## 五个核心问题的审查

| 问题 | 当前文档 | 结论 |
|---|---|---|
| TS 应该如何表达 | 有 branded ID、snapshot、query、ActionOptions、Subscription 草案 | 原则正确；需要统一 `Result`/异常策略、对象生命周期和模块命名 |
| Window 底层怎么实现 | 已有 `WindowService`、UI lane、稳定 ID 和 Win32 调用说明 | 基本正确；还缺 JS binding 的序列化契约、失效 ID 和事件来源 |
| 哪些 API 异步 | 已按 lane 给出大方向 | 需要从“领域级”细化到每个函数，并明确同步 API 只限纯 JS/快照操作 |
| 哪些不暴露 | 已禁止 HWND/HANDLE/COM 指针和任意 DllCall | 正确；需要列出受控插件替代接口和 unload 条件 |
| 重新发明 AHK/TS 建模 Windows | 已有模块划分和 Action 模型 | 方向正确；还需明确 Windows 对象图、Context、Intent、Action 和 capability 的层次 |

## 对象成员如何处理

AHK 对象不能整体搬到 TS，也不能把每个对象成员都变成散落的全局函数。采用四种表达：

### 1. 纯语言对象使用原生 TS

`Object`、`Array`、`Map`、`Func`、正则结果和字符串/日期/数学能力属于 JS/TS 层。AHK 的原型链、ByRef 和动态属性只作为研究依据，不进入 Runtime 内核。

```ts
export type AhkCompat = {
  defineProp<T extends object>(object: T, name: string, descriptor: PropertyDescriptor): T;
  hasProp(object: object, name: PropertyKey): boolean;
};
```

### 2. Windows 资源使用 opaque ref + snapshot

`Gui`、`GuiControl`、`Menu`、`InputHook`、`File`、`ComObject` 不把 C++ 对象直接暴露给 QuickJS。TS 只持有：

```ts
interface ResourceRef<K extends string> {
  readonly kind: K;
  readonly id: string;
  close(options?: ActionOptions): Promise<void>;
}

interface SnapshotResourceRef<K extends string, S> extends ResourceRef<K> {
  snapshot(options?: ActionOptions): Promise<Readonly<S>>;
}
```

方法分组保持对象语义，但每次调用都进入拥有资源的 lane：

```ts
interface WindowRef extends SnapshotResourceRef<"window", WindowSnapshot> {
  activate(options?: ActionOptions): Promise<void>;
  move(rect: Rect, options?: ActionOptions): Promise<void>;
}

interface FileRef extends ResourceRef<"file"> {
  read(options?: ReadOptions): Promise<Uint8Array | string>;
  write(value: Uint8Array | string, options?: ActionOptions): Promise<number>;
}
```

对象成员矩阵仍然保留 AHK 名称、参数和返回语义，但映射到稳定的 TS object facade。`Handle`、`Ptr`、`Hwnd` 等 AHK 属性在标准 API 中返回 `undefined`/诊断信息或被替换为对应的 branded ref；只有受信任插件 ABI 才能申请受控 native token。

### 3. 回调成员使用 Subscription

`Gui.OnEvent`、`Gui.OnMessage`、`InputHook.OnKeyDown`、`Menu` 回调和 COM 事件不把 JS 函数存进 Native 对象而不受管理。统一返回：

```ts
interface Subscription {
  readonly id: SubscriptionId;
  readonly closed: boolean;
  close(): Promise<void>;
}
```

关闭先禁止新事件，再等待回调计数归零；同一 JS Context 内不允许隐式重入。

## 内置变量如何处理

不复制 AHK 的全局可写变量。内置变量分成三类：

| 类型 | 例子 | TS 表达 |
|---|---|---|
| 静态 Host 信息 | 版本、平台、脚本路径、用户目录 | `runtime.info()` 返回不可变快照 |
| 动态系统状态 | 活动窗口、光标、键状态、时间、剪贴板 | `context.snapshot()` 或专门模块查询，读取时明确可能异步 |
| 当前事件字段 | `A_ThisHotkey`、`A_EventInfo`、`A_GuiControl`、`A_EndChar` | 回调参数 `EventContext`，只在回调期间有效 |

```ts
export interface EventContext<E = unknown> {
  readonly eventId: string;
  readonly source: "hotkey" | "hotstring" | "timer" | "window" | "gui" | "clipboard";
  readonly timestamp: number;
  readonly activeWindow: WindowSnapshot | null;
  readonly payload: Readonly<E>;
  readonly parentActionId?: string;
}
```

事件上下文直接使用结构化字段；不设计 AHK 全局变量代理。每个研究变量在 `builtins.json` 中继续登记来源、动态性、更新点、权限和标准 TS 替代 API。

## Window 从 Win32 到 TS 的完整路径

```text
TS windows.list/query/ref
  -> QuickJS rime:window binding
  -> 序列化 request {operation,target/query,actionOptions}
  -> JS Thread 调度到 UI lane
  -> WindowService::UiThread::call
  -> WindowRegistry: WindowId <-> HWND（仅 UI thread）
  -> EnumWindows/GetForegroundWindow/GetWindowTextW/GetWindowRect/SetWindowPos/ShowWindow/SetForegroundWindow
  -> immutable WindowSnapshot / ActionResult
  -> completion queue
  -> QuickJS Promise settlement
```

规则：

1. `HWND` 只存在 UI lane；JS 只收到稳定 `WindowId` 和快照。
2. ID 形如 `[generation:32][sequence:32]`（generation 每个 WindowService 实例递增），跨服务也不复用；窗口销毁或 ID 属于旧服务实例时返回 `target_gone`。
3. `list/active/info` 在当前 Native 层是同步 C++ 调用，但 JS binding 必须返回 Promise，因为它跨 JS/UI lane。
4. `move/activate/close/show/hide/set*` 必须生成 Action，检查 capability、deadline、前置条件并写 Trace。
5. Window query 在 UI lane 解析，不能在 JS 线程保存 HWND 或依赖窗口标题的瞬时匹配结果。
6. 前台激活、SendInput、剪贴板交换属于临界区；取消只能在安全事件边界生效。
7. UI 线程关闭顺序是拒绝新任务、取消队列、卸载窗口回调/Hook、等待任务，再销毁窗口和 lane。

## 同步和异步边界

同步只允许：

- KeySequence、WinTitle、颜色、路径和选项的纯解析；
- 已取得快照的字段访问；
- TS/ECMAScript 的字符串、日期、正则、数学和集合操作；
- 不访问 Native 状态的 Action 构造和序列化。

必须异步：

- 所有 UI lane 操作，包括窗口/控件/GUI/Menu/SendInput；
- UIA、COM、剪贴板、进程、文件、驱动器、对话框、截图和图像搜索；
- 等待窗口/进程/按键/Timer/消息的操作；
- 任何可能触发消息泵、系统策略、用户交互或不确定耗时的 API。

异步 API 统一接受 `AbortSignal`/`deadlineMs`，返回 Promise；订阅返回 `Subscription`，等待操作同时可取消并区分 timeout、cancelled、closed 和 target-gone。

## 不暴露的内容

标准库不暴露：裸 `HWND/HANDLE/HHOOK/HMENU/HGLOBAL`、线程 ID、窗口过程地址、COM 指针、VARIANT 地址、对象内存地址、QuickJS 指针、内部队列、Hook 模块句柄、`AttachThreadInput`、任意 `DllCall/ComCall` 和未审计的 SendMessage。

替代方式是 branded ref、snapshot、typed automation pattern、受权限的 plugin token 和结构化 Action。插件 token 也必须有 ABI 版本、权限、所属 lane、过期/关闭状态和 unload 检查。

## 最终判断

现有文档符合架构方向，但还不符合“每个 API 可独立实现”的最终标准。要达到该标准，下一步必须：

1. 将 `objects.json` 的每个成员补齐 TS 签名、lane、错误、所有权和测试 ID；
2. 将 `builtins.json` 的每个变量补齐动态性、更新点和 `EventContext`/模块替代；
3. 为 `rime:window` 固化 request/response JSON contract，并实现 Promise binding；
4. 把 `coverage.json` 的 `async/capability/cancellation` 从领域推断改成逐函数审定；
5. 为上述条目增加 contract、native、lifecycle 和 replay 测试。
