# AHK → TypeScript Windows Automation API 设计

状态：设计基线（2026-10-01）

本文以 `rime-research/AutoHotkey-alpha/source/lib/functions.h` 以及 `window.cpp`、`keyboard_mouse.cpp`、`clipboard.cpp`、`hotkey.cpp` 为能力与语义参考，定义 Runtime 的 TypeScript 标准库。研究目录只用于还原核对（能力清单 + 对照原版的语义测试），不作为 Runtime 依赖。目标是把 AHK 的每一个 API **还原保留**为 TS 原生等价物——能力不缺项、语义可核对，同时把资源所有权、线程、取消、权限和可诊断性变成显式契约。

## 0. 核心设计问题（2026-10-02 修订）

设计权威是 TypeScript 与 Windows 本身的约束，不是 AHK 的形状。每个 API 落地前必须回答：

1. **TS 应该如何表达它**——模块导出、类型、同步还是 `Promise`、结构化 `RuntimeError` 还是预览/批处理用的 `ActionPlan`；
2. **底层实际上怎么实现**——走哪条 lane（UI / Worker / Automation MTA）、调用哪些 Win32/UIA/COM 原语、资源所有权归谁；
3. **哪些 API 应该异步**——只有跨 lane 或等待外部状态才返回 `Promise`；纯计算、纯快照与解析保持同步（详见 §8）；
4. **哪些东西不暴露给上层**——句柄、指针、线程 ID、消息泵一律不出 native，上层只见稳定 ID、快照、订阅与 Action 结果（详见 §9）；
5. **如果重新发明 AHK，TS 标准库应该怎么设计**——一致性、可组合性、可检查性、生命周期安全优先，见 [`api/future-runtime.md`](./api/future-runtime.md)；
6. **如果 TS 就是 Windows automation language，Windows 怎么建模**——sessions/desktops/monitors/processes/windows/controls 对象图；引用表示可重新验证的身份，快照是不可变观察（见 `future-runtime.md` §2）。

**还原保留原则**：覆盖矩阵中的每个 AHK API 都要力求还原保留——能力不能缺项，语义用对照原版的测试守住；但命名、参数形状、模块归属、同步性与暴露边界按上述六个问题重新设计。不保留 AHK 的全局状态、伪线程与隐式行为，也**不存在 AHK 名称兼容层**（见 [`api/design-review.md`](./api/design-review.md) 与实施计划 §0）。"兼容"一词在本仓库只指与 AHK 原版语义的核对测试，不指公共 API 形状。

## 1. 总体模型

```text
TS intent → Action IR → Action Kernel → Host adapter
                         ├─ UI lane (HWND, window manager, SendInput)
                         ├─ Automation MTA (UIA, COM, process)
                         └─ Worker/IO lane (file, regex, image, wait)
```

JS 线程只持有不可变快照、稳定 ID 和取消令牌。`HWND`、`HANDLE`、`HMONITOR`、COM interface、Hook pointer 不进入 JS；所有对象以带生命周期的 opaque ID 表达。Action 必须可追踪、可取消、可设 deadline；**失败一律抛出**（2026-10-02 修正，以现有实现与 future-runtime 的 throw 模型为准）：同步契约错误抛 `TypeError`，异步失败经 Promise reject 抛结构化错误（设计名 `RuntimeError`，携带 code/message/traceId 等；当前 SDK 实现为 `ActionError { code }`，见 `sdk/src/action.ts`）——不采用 `Result<T>` 返回包装。

## 2. TS 运行时原语

```ts
export type WindowId = string & { readonly __brand: "WindowId" };
export type ProcessId = number & { readonly __brand: "ProcessId" };
export type SubscriptionId = string & { readonly __brand: "SubscriptionId" };
export type CancellationToken = { readonly id: string; cancel(): void; readonly signal: AbortSignal };
export interface ActionOptions { signal?: AbortSignal; deadlineMs?: number; idempotencyKey?: string; parent?: string }
export interface Rect { left: number; top: number; right: number; bottom: number }
// 失败抛出而非返回（2026-10-02 修正）：同步契约错误 → TypeError；
// 异步失败 → reject 结构化错误（设计目标 RuntimeError；当前实现为
// sdk/src/action.ts 的 ActionError { code }）。不定义 Result<T> 包装。
```

公共 API 以模块导出，不挂全局函数。`Promise` 只表示跨 lane 或等待外部状态；纯快照/解析函数保持同步。每个异步方法均接受 `AbortSignal`，底层在可中断点检查取消。

## 3. Window API（AHK Win* 对等层）

```ts
export interface WindowSnapshot {
  id: WindowId; title: string; className: string; rect: Rect; clientRect: Rect;
  visible: boolean; minimized: boolean; maximized: boolean; enabled: boolean;
  processId: ProcessId; processName?: string; processPath?: string; style: number; exStyle: number;
}
export interface WindowQuery { title?: string | RegExp; text?: string | RegExp; className?: string;
  process?: string | ProcessId; excludeTitle?: string | RegExp; excludeText?: string | RegExp;
  includeHidden?: boolean; lastMatch?: boolean }
export type WindowPlacement = "left" | "right" | "top" | "bottom" | "full";
export interface WindowMoveRect { x?: number; y?: number; w?: number; h?: number }
export const windows: {
  list(query?: WindowQuery, o?: ActionOptions): Promise<WindowSnapshot[]>;
  active(o?: ActionOptions): Promise<WindowSnapshot | null>;
  find(query: WindowQuery, o?: ActionOptions): Promise<WindowSnapshot | null>;
  wait(query: WindowQuery, o?: ActionOptions & { timeoutMs?: number }): Promise<WindowSnapshot>;
  waitActive(query: WindowQuery, o?: ActionOptions & { timeoutMs?: number }): Promise<WindowSnapshot>;
  waitClosed(target: WindowRef | WindowQuery, o?: ActionOptions & { timeoutMs?: number }): Promise<boolean>;
  close(target: WindowRef | WindowQuery, o?: ActionOptions & { waitMs?: number }): Promise<void>;
  activate(target: WindowRef | WindowQuery, o?: ActionOptions): Promise<void>;
  move(target: WindowRef | WindowQuery, position: WindowPlacement | WindowMoveRect, o?: ActionOptions): Promise<void>;
  minimize(target: WindowRef | WindowQuery, o?: ActionOptions): Promise<void>;
  maximize(target: WindowRef | WindowQuery, o?: ActionOptions): Promise<void>;
  restore(target: WindowRef | WindowQuery, o?: ActionOptions): Promise<void>;
  show(target: WindowRef | WindowQuery, o?: ActionOptions): Promise<void>;
  hide(target: WindowRef | WindowQuery, o?: ActionOptions): Promise<void>;
  setTitle(target: WindowRef | WindowQuery, title: string, o?: ActionOptions): Promise<void>;
  setEnabled(target: WindowRef | WindowQuery, value: boolean|-1|0|1, o?: ActionOptions): Promise<void>;
  setAlwaysOnTop(target: WindowRef | WindowQuery, value: boolean, o?: ActionOptions): Promise<void>;
  setStyle(target: WindowRef | WindowQuery, change: string, o?: ActionOptions): Promise<void>;
  setExStyle(target: WindowRef | WindowQuery, change: string, o?: ActionOptions): Promise<void>;
  setTransparent(target: WindowRef | WindowQuery, alpha: number, o?: ActionOptions): Promise<void>;
  setTransColor(target: WindowRef | WindowQuery, color: string, o?: ActionOptions): Promise<void>;
  setRegion(target: WindowRef | WindowQuery, value?: string, o?: ActionOptions): Promise<void>;
  controls(target: WindowRef | WindowQuery, o?: ActionOptions): Promise<ControlSnapshot[]>;
  on(event: "created"|"destroyed"|"activated"|"moved"|"titleChanged", cb: (e: WindowEvent)=>void): Subscription;
};
export interface WindowRef { readonly id: WindowId; snapshot(o?: ActionOptions): Promise<WindowSnapshot>; }
```

**状态注（2026-10-02）**：本节签名是设计基线；其中 `find`、`on`、`WindowRef`、
`WindowQuery` 的 `text`/`RegExp`/`excludeTitle`/`excludeText`/`lastMatch` 与 `process`/
`className` 命名属**设计领先项**（未实现或命名不同，见 §12）——当前实现的 `title` 正则
走 AHK 字符串正则语义（`matchMode: "regex"`），进程/类字段名为 `ahkExe`/`ahkClass`。

AHK 的 `WinTitle` 参数统一转换成 `WindowQuery`；`"A"` 映射 `windows.active()`，`ahk_id` 映射 `WindowId`，`ahk_exe` 映射 `process`。`WinMove` 经 `windows.move` 承载：命名 placement（工作区对半，Rime 扩展）或 `{x?, y?, w?, h?}` 坐标 rect（AHK 的 X/Y/Width/Height，**省略字段保持当前值**的空参数规则）。`WinGetPos/ClientPos/List/Count/PID/ProcessName/ProcessPath/Class/Style/ExStyle/Text/Title/MinMax/Enabled/AlwaysOnTop/Transparent/TransColor` 均是 `snapshot` 或 `list` 的字段/派生方法。`WinActivateBottom` 经 `list`（保持 z 序）+ `focus` 最后一个匹配组合承载，`WinMoveTop/WinMoveBottom` 经 `windows.zorder(target, "top"|"bottom")` 承载，`WinKill/WinRedraw` 经 `windows.kill`/`windows.redraw` 承载，`WinMinimizeAll/WinMinimizeAllUndo` 经 `windows.minimizeAll()`/`windows.minimizeAllUndo()` 承载，`WinSetTitle/WinSetEnabled/WinSetAlwaysOnTop` 经 `windows.setTitle`/`windows.setEnabled`/`windows.setAlwaysOnTop` 承载，`WinSetStyle/WinSetExStyle/WinSetTransparent/WinSetTransColor` 经 `windows.setStyle`/`windows.setExStyle`/`windows.setTransparent`/`windows.setTransColor` 承载，`WinSetRegion` 经 `windows.setRegion` 承载（附带 `region` 快照读回字段——AHK 无对应读取器）。

实现：UI lane 调用 `EnumWindows`、`GetForegroundWindow`、`GetWindowTextW`、`GetWindowRect`、`GetClientRect`、`GetWindowThreadProcessId`、`IsWindowVisible/IsIconic/IsZoomed/IsWindowEnabled`、`SetWindowPos`、`ShowWindow`、`SetForegroundWindow`、`SetWindowLongPtr`、`SetLayeredWindowAttributes`。稳定 ID 由 UI lane registry 产生且不复用；窗口销毁后所有操作返回 `InvalidState`。激活沿用 AHK 的 restore → `SetForegroundWindow` → 必要时 `AttachThreadInput`/Alt-up 的策略，但必须在 Trace 记录每个尝试。

## 4. Control / UI Automation

```ts
export interface ControlSnapshot { id: string; automationId?: string; className: string; role: string; name: string; rect: Rect; enabled: boolean; visible: boolean }
export const controls: { find(parent: WindowRef|WindowQuery, q: ControlQuery, o?: ActionOptions): Promise<ControlRef[]>;
  getText(c: ControlRef, o?: ActionOptions): Promise<string>; setText(c: ControlRef, text: string, o?: ActionOptions): Promise<void>;
  click(c: ControlRef, o?: ActionOptions): Promise<void>; focus(c: ControlRef, o?: ActionOptions): Promise<void>;
  send(c: ControlRef, keys: KeySequence, o?: ActionOptions): Promise<void>;
}
```

`ControlGet*`、`ControlClick`、`ControlFocus`、`ControlMove`、`ControlShow/Hide`、`ControlSet*`、`ControlSend/Text`、ListView/TreeView helpers 全部保留为语义操作；优先 UIA pattern（Invoke、Value、Selection、Toggle、RangeValue），无 provider 时回退 Win32 message（`WM_SETTEXT`、`BM_CLICK` 等），最后才允许坐标/视觉层。COM 接口只存在 Automation MTA，JS 收到不可变 `ControlSnapshot`。

## 5. Input API

```ts
export const keyboard: { send(keys: KeySequence, o?: ActionOptions & { mode?: "input"|"event"|"play"|"text"|"raw" }): Promise<void>;
  press(key: Key, o?: ActionOptions): Promise<void>; release(key: Key, o?: ActionOptions): Promise<void>;
  hotkey(keys: Key[], cb: (e: HotkeyEvent)=>void, o?: HotkeyOptions): Subscription; }
export const mouse: { move(p: Point, o?: ActionOptions): Promise<void>; click(button?: MouseButton, o?: ActionOptions): Promise<void>;
  drag(from: Point, to: Point, o?: ActionOptions): Promise<void>; position(o?: ActionOptions): Promise<Point>; }
```

`Send/SendEvent/SendInput/SendPlay/SendText` 保留为 mode 别名；AHK `{Blind}`、`{Text}`、修饰键左右侧和 down/up 语法在 `KeySequence` parser 中保持。实现使用 `SendInput`/`KEYBDINPUT`、`MOUSEINPUT`，必要时 `PostMessage`/低级 hook；输入临界区不可隐式重入，取消只能在事件边界生效。

## 6. Clipboard、process、shell、pixel/image

```ts
export const clipboard: { readText(o?: ActionOptions): Promise<string>; writeText(s: string, o?: ActionOptions): Promise<void>;
  onChange(cb: (e: ClipboardEvent)=>void): Subscription }
export const process: { list(q?: ProcessQuery, o?: ActionOptions): Promise<ProcessSnapshot[]>; run(target: string, o?: RunOptions): Promise<ProcessRef>;
  runWait(target: string, o?: RunOptions & { timeoutMs?: number }): Promise<ExitStatus>; close(p: ProcessRef|ProcessQuery, o?: ActionOptions): Promise<void>;
  wait(q: ProcessQuery, o?: ActionOptions & { timeoutMs?: number }): Promise<ProcessRef>; waitClosed(q: ProcessQuery, o?: ActionOptions & { timeoutMs?: number }): Promise<boolean>; }
export const screen: { pixelColor(p: Point, o?: ActionOptions): Promise<Color>; pixelSearch(r: Rect, c: Color, o?: PixelOptions): Promise<Point|null>; imageSearch(r: Rect, image: ImageSource, o?: ImageOptions): Promise<Point|null>; }
```

对应 `ClipWait/OnClipboardChange`、`Run/RunWait/RunAs`、`ProcessClose/Exist/GetName/GetPath/GetParent/SetPriority/Wait/WaitClose`、`PixelGetColor/PixelSearch/ImageSearch`。进程枚举使用 Toolhelp/PSAPI，启动使用 `CreateProcessW`，等待使用 waitable handle；凭据、DLL 注入、任意 `DllCall/ComCall` 默认不暴露，仅通过受权限约束的插件能力提供。

## 7. Timer、Hook、消息和调度

```ts
export const timers: { every(periodMs: number, cb: () => void, o?: ScheduleOptions): Subscription; after(ms: number, cb: () => void, o?: ScheduleOptions): Subscription }
export const hooks: { keyboard(cb: (e: KeyboardEvent)=>void, o?: HookOptions): Subscription; mouse(cb: (e: MouseEvent)=>void, o?: HookOptions): Subscription; message(msg: number, cb: (e: MessageEvent)=>void, o?: HookOptions): Subscription }
```

`SetTimer`、`OnMessage`、`InstallKeybdHook/InstallMouseHook`、热键/热字串和 `ListHotkeys` 均保留。Hook 回调先进入 Win32 message pump，再排队到 JS task；禁止 Hook 线程直接调用 QuickJS。订阅为一次性可关闭对象，回调计数归零后才允许卸载。

## 8. 同步/异步判定

同步：纯 parser（KeySequence、颜色、查询构造）、枚举值转换、已取得快照的字段访问。

异步：所有窗口/控件/输入/剪贴板/进程/屏幕 API；原因是跨 UI、MTA、IO lane，可能阻塞、触发系统策略或需要等待消息泵。事件订阅不返回 Promise，而返回 `Subscription`；等待类 API 支持 timeout + cancellation。

## 9. 不暴露给上层的实现细节

不暴露裸句柄和指针、线程 ID、COM apartment、`AttachThreadInput`、窗口过程地址、Hook 模块句柄、临时 HWND、内部队列、SendInput 批次、UIA cache request、插件宿主 ABI 指针。上层只看到稳定 ID、快照、Action 结果、诊断 Trace 和显式权限错误。

## 10. 权限、错误与还原保留

模块按 `windows.window.read/write`、`windows.input.inject`、`windows.hook.global`、`process.launch/terminate`、`windows.clipboard.read/write`、`screen.capture` 声明权限；高风险能力默认拒绝。错误码至少包括 `PermissionDenied`、`InvalidState`、`Timeout`、`Cancelled`、`ForegroundDenied`、`HungWindow`、`UiaUnavailable`、`QueueFull`、`Unsupported`。

不实现 AHK 名称兼容层（见 §0 与 `api/design-review.md`）：AHK 的每个 API 在覆盖矩阵中逐项追踪，以 TS 原生等价实现**还原保留**，内部统一生成标准 Action IR；公共 API 使用 `windows`、`controls`、`keyboard`、`mouse`、`process`、`clipboard`、`screen` 模块导出。每个还原保留的 API 的测试必须覆盖输入解析、Win32/UIA 执行、取消竞态、资源卸载和 Trace。

## 11. 与当前实现的垂直切片

现有 `WindowService` 已验证 UI-thread registry、UTF-8 快照、`active/list/info/move/focus`，以及 WinTitle 查询（`title`/`matchMode`/`ahkClass`/`ahkExe`/`ahkId`/`includeHidden`/`active`）和状态操作（`close/hide/show/minimize/maximize/restore`）。`rime:window` 提供对应 Promise binding：读路径校验 `windows.window.read`，写路径经 Action Kernel 校验 `windows.window.write` 并写入 Trace；读写接受 `deadlineMs`/`cancellationId`（SDK 侧 `AbortSignal` 自动绑定并释放 cancellation id），deadline 过期与 UI 排队超时返回 `timeout`。`window-v1` contract 见 `contracts/schema/window-v1.schema.json`。剩余步骤：

1. 将 `WindowInfo` 映射为 `WindowSnapshot`，把 `uint64 id` 编码为 branded `WindowId`。
2. 补齐等待订阅的关闭排空（WinSet*/region 已全部落地，WinWait* 已由 `windows.wait` 承载，zOrder 已由 `windows.zorder` 承载，redraw 已由 `windows.redraw` 承载）。
3. 再扩展 Control/UIA、输入和进程模块；不把 AHK 全局状态复制到 Runtime。

## 12. 对齐审计（2026-10-01）

本设计文档是 API 目标蓝图，不代表当前 Runtime 已经实现全部 AHK 功能。对照
`rime-research/AutoHotkey-alpha/source/lib/functions.h`（253 个内建函数）逐项检查后，
当前 C++/TS 垂直切片已有 `rime:window`（含 WinTitle 查询的 `list`、`active/info/move`、
`focus/close/hide/show/minimize/maximize/restore`）、`rime:input`（`subscribe/unsubscribe`）、
`rime:process`（`list/info/launch/terminate`）与 `rime:clipboard`（`read/write`）binding，及
对应的 service、executor、capability 校验和 contract/slice 测试；但 `functions.h` 函数级条目
仍无一项 `implemented`，其余 API 尚未有 JS binding、Action executor 和 contract 测试。
因此不能宣称“所有功能已对齐”。

可执行的领域规范和逐项覆盖矩阵已拆到 [`docs/api/`](./api/README.md)。本文件只保留跨领域
原则、架构决策和垂直切片顺序；实现任务必须以领域规范和 `coverage.json` 为准。

第二轮审计进一步确认：`functions.h` 的 253 个函数甚至不是全部函数型 API；`script.cpp`
还注册了 101 个独立的核心内建函数（`BIF1` 41 + `BIFn` 47 + `BIFi` 13）。对象原型、GUI/Menu/InputHook/File/COM
对象、内置变量、Host ABI 和语法级事件分别记录在 `docs/api/source-inventory.md`、
`objects.json`、`builtins.json` 和 `audit-gaps.md`。在这些清单逐项提取完成前，设计仍不能
称为完备。

第三轮审计补充了来源闭包、全局状态、错误原型和可重放要求，见
[`docs/api/source-closure.md`](./api/source-closure.md) 与
[`docs/api/state-and-errors.md`](./api/state-and-errors.md)。这些内容决定了 API 是否真正可
实现和可维护，不能被单独的函数签名替代。

### 覆盖状态

| AHK 功能域 | functions.h 代表函数 | 文档状态 | Runtime 状态 |
|---|---|---|---|
| 窗口查询/操作 | WinActivate、WinClose、WinGet*、WinMove、WinSet*、WinWait*、WinGroup*、WinMoveTop/Bottom、WinActivateBottom、WinKill、WinRedraw、WinMinimizeAll/Undo（约 55） | 已定义目标接口及 WinTitle 映射；`region` 已实现（含快照读回扩展） | `list`（含 WinTitle 查询）/`active/info/move/focus/close/kill/redraw/hide/show/minimize/maximize/restore/zorder/minimizeAll/minimizeAllUndo`/`setTitle/setEnabled/setAlwaysOnTop/setStyle/setExStyle/setTransparent/setTransColor/setRegion`/`exists/isActive/wait`（WinWait 家族经 `until`）/`groups`（WinGroup 家族经 `window.group.*` 动作）已实现（WinActivateBottom 经 list+focus 组合承载）；WinSet* 全部实现 |
| 控件/UIA | Control* 全集、Edit*、ListViewGetContent、StatusBar*、Gui*（约 48） | 仅定义通用 `controls` 抽象，未逐函数列签名/返回值/失败语义 | 未实现 UIA/Win32 fallback |
| 键鼠/热键/Hook | MouseClick*、Send*、Hotkey、Hotstring、KeyWait、Install*Hook、BlockInput、GetKey*、Set*KeyState | 定义基础 `keyboard`/`mouse`/`hooks`；AHK 解析细节尚未形成语法规范 | 仅低级输入事件订阅；注入和热键未实现 |
| 剪贴板/消息 | ClipWait、OnClipboardChange、SendMessage、OnMessage | 仅概念提及 | `clipboard.read/write` binding 已实现（`windows.clipboard.read/write` capability）；`ClipWait`/`OnClipboardChange`/`OnMessage` 未实现 |
| 进程/启动 | Run、RunWait、RunAs、Process*、Shutdown | 定义 `process` 目标 API 和权限 | `process.list/info/launch/terminate` binding 已实现（`process.launch/terminate`、`process.inspect`）；`Run*`/`Shutdown` 未实现 |
| 屏幕/图像 | PixelGetColor、PixelSearch、ImageSearch、MonitorGet*、SysGet* | 定义 `screen.pixel*`；monitor/system 信息缺少接口 | 未实现 |
| 文件/目录/环境 | File*、Dir*、Env*、Ini*、Download、Drive*、SplitPath | 未定义 TS 模块或权限模型 | 未实现 |
| 定时/调度/运行时 | SetTimer、Sleep、Critical、Persistent、ExitApp、Reload、Suspend、Pause、OnExit/OnError | 仅 `timers.every/after` 草案；生命周期契约在核心文档 | 未实现 JS API |
| GUI/菜单/托盘/声音 | Gui*、Menu*、Tray*、ToolTip、MsgBox、InputBox、Sound*、LoadPicture、IL_* | 未覆盖 | 未实现 |
| 注册表/COM/原生扩展 | Reg*、DllCall、ComCall、Obj*DataPtr、Callback* | 明确禁止裸能力；没有受权限插件 API 的签名 | 未实现（按设计应保持隔离） |
| 纯语言/字符串 | RegEx*、StrSplit、StrReplace、DateAdd/DateDiff、IsLabel 等 | 应由 TS/标准库直接承担，语义差异以还原测试守住 | 不属于 Runtime binding |

### 完备性结论与验收门槛

在宣称 API 对齐前，必须为 `functions.h` 的每个函数建立还原对照表（名称、参数、返回值、
同步/异步、错误码、权限、取消点、Trace 字段和替代模块），并标记 `implemented /
contract-only / unsupported-by-policy`。窗口、控件、输入、进程、剪贴板、屏幕、文件、
注册表和 GUI 等域都需要至少一个可执行 contract 测试；测试应覆盖取消竞态、队列满载、
资源卸载和权限拒绝。当前仓库尚未满足这些门槛，文档仍属于设计基线，不能视为完备实现。

### 文档与现有 SDK 的已知偏差（2026-10-02 复查更新）

- **已对齐**：branded `WindowId`（实现为 `Brand<number, "WindowId">`——数值品牌承载
  `[generation:32][sequence:32]`，与 §2 草案的字符串形式不同，属形态差异而非缺失）、
  `ActionOptions`（`deadlineMs`/`cancellationId`/`signal`/`parentActionId`/`idempotencyKey`）、
  AbortSignal 取消（结构型 `CancellationSignal`，真实 `AbortSignal` 结构兼容）与错误模型
  （§1/§2 已改为 throw，不再使用 `Result<T>`）。
- **命名已统一（2026-10-02 改名）**：实现已采用设计稿名称 `WindowSnapshot`/`WindowQuery`
  （TS 与原生 `rime::win32::WindowQuery` 一致），旧名 `WindowHandle`/`WindowQueryFields`
  已从 SDK 与测试中移除；改名同步了 TS 声明与测试，contract fixtures 不受影响（schema
  本就叫 `snapshot`）。
- **仍属设计领先、未实现**：`WindowRef` 对象句柄（实现直接接收 `WindowId`）、`windows.find`/
  `windows.on` 事件订阅、查询字段 `excludeTitle`/`excludeText`/`lastMatch`/`text`、`title`
  的 RegExp 对象形式（实现只收字符串，正则按 AHK 字符串正则语义走 `matchMode: "regex"`）、
  `process`/`className` 命名（实现用 AHK 原生 `ahkExe`/`ahkClass`）、§4 控件定位/交互层
  （`controls.find/getText/click/focus/send`、`ControlRef`/`ControlQuery`——当前
  `controls()` 只承载 WinGetControls/WinGetControlsHwnd 读族）与 `ControlSnapshot.id`
  形态（设计 `string` ↔ 实现复用 `WindowId` brand）。
- **仍为已知偏差**：`sdk/src/input.ts` 的事件订阅返回裸 number 并使用同步回调，尚未提供
  Subscription 对象。

该偏差清单在实现每个垂直切片时复查；示例引用未实现项时必须先按设计稿标注状态，声明、
Native binding 与 contract fixtures 同步更新。
