# Future TS Windows Runtime API

这份文档定义面向未来的标准库方向。AutoHotkey 只作为能力研究样本，不作为命名、语法或行为兼容目标。标准库优先保证一致性、可组合性、可检查性和生命周期安全。

## 1. 统一心智模型

```text
Context snapshot
  -> Intent
  -> Action plan
  -> capability check
  -> lane executor
  -> trace + result
```

脚本看到的是模块、引用、快照、计划和事件。Native 看到的是 lane、资源所有权和 Win32/UIA/COM 调用。所有改变外部世界的操作都是 Action；所有跨 lane 的操作都是 Promise。

```ts
type Id<K extends string> = string & { readonly __kind: K };
type WindowId = Id<"window">;
type ProcessId = Id<"process">;
type ControlId = Id<"control">;
type SubscriptionId = Id<"subscription">;

interface CallOptions {
  signal?: AbortSignal;
  deadlineMs?: number;
  parent?: ActionId;
}

interface Subscription {
  readonly id: SubscriptionId;
  close(): Promise<void>;
}
```

普通调用抛出结构化 `RuntimeError`；需要预览、批处理、审计或重放时使用 `ActionPlan`。

## 2. Windows 对象图

```text
Runtime
 ├─ sessions
 │   └─ desktops
 │       ├─ monitors
 │       ├─ processes
 │       │   └─ windows
 │       │       └─ controls
 │       └─ input / clipboard
 ├─ filesystem
 ├─ shell
 └─ automation
```

对象引用只表示可重新验证的身份，不保证对象永久存在。快照是不可变观察结果；引用是执行操作的入口。

## 3. Window 与 Control

```ts
export interface WindowSnapshot {
  id: WindowId;
  title: string;
  className: string;
  bounds: Rect;
  clientBounds: Rect;
  state: "normal" | "minimized" | "maximized" | "hidden";
  processId: ProcessId;
  sessionId: Id<"session">;
  desktopId: Id<"desktop">;
  monitorId?: Id<"monitor">;
  dpi: number;
}

export const windows = {
  list(query?: WindowQuery, options?: CallOptions): Promise<WindowSnapshot[]>;
  active(options?: CallOptions): Promise<WindowSnapshot | null>;
  ref(id: WindowId): WindowRef;
  observe(event: WindowEventName, handler: (event: WindowEvent) => void): Subscription;
};

export interface WindowRef {
  readonly id: WindowId;
  snapshot(options?: CallOptions): Promise<WindowSnapshot>;
  activate(options?: CallOptions): Promise<void>;
  close(options?: CallOptions): Promise<void>;
  show(options?: CallOptions): Promise<void>;
  hide(options?: CallOptions): Promise<void>;
  move(bounds: Rect, options?: CallOptions): Promise<void>;
  setState(state: WindowState, options?: CallOptions): Promise<void>;
  controls(options?: CallOptions): Promise<ControlSnapshot[]>;
}

export interface ControlRef {
  readonly id: ControlId;
  snapshot(options?: CallOptions): Promise<ControlSnapshot>;
  invoke(options?: CallOptions): Promise<void>;
  focus(options?: CallOptions): Promise<void>;
  value(options?: CallOptions): Promise<unknown>;
  setValue(value: unknown, options?: CallOptions): Promise<void>;
}
```

Native 路径固定为 `WindowRef -> request -> JS scheduler -> UI lane -> WindowRegistry -> HWND -> Win32/UIA -> snapshot/result`。`HWND` 只存在 UI lane；ID 必须包含 Runtime service generation，防止销毁后复用。

## 4. Input、Hook 与调度

```ts
export const keyboard = {
  send(sequence: KeySequence, options?: CallOptions): Promise<void>;
  press(key: Key, options?: CallOptions): Promise<void>;
  release(key: Key, options?: CallOptions): Promise<void>;
  state(key?: Key, options?: CallOptions): Promise<KeyState | KeyStateMap>;
  observe(handler: (event: KeyboardEvent) => void, options?: HookOptions): Subscription;
};

export const mouse = {
  move(point: Point, options?: CallOptions): Promise<void>;
  click(button: MouseButton, options?: CallOptions): Promise<void>;
  drag(path: readonly Point[], options?: CallOptions): Promise<void>;
  position(options?: CallOptions): Promise<Point>;
  observe(handler: (event: MouseEvent) => void, options?: HookOptions): Subscription;
};

export const scheduler = {
  after(delayMs: number, task: Task, options?: ScheduleOptions): Subscription;
  every(periodMs: number, task: Task, options?: ScheduleOptions): Subscription;
  state(): Promise<SchedulerSnapshot>;
};
```

Hook 线程只采集不可变事件并投递队列；JS 回调永远由 scheduler 调度。输入注入和全局 Hook 是不同 capability。

## 5. Automation

```ts
export const automation = {
  find(root: WindowRef | ControlRef, query: SemanticQuery, options?: CallOptions): Promise<ControlRef[]>;
  invoke(target: ControlRef, options?: CallOptions): Promise<void>;
  read(target: ControlRef, options?: CallOptions): Promise<unknown>;
  write(target: ControlRef, value: unknown, options?: CallOptions): Promise<void>;
  screenshot(target?: WindowRef, options?: CallOptions): Promise<ImageSnapshot>;
};
```

执行层按 `UIA semantic -> Win32 direct -> input -> visual` 选择，并在 Trace 中记录实际层级。用户调用的是语义能力，不是坐标和 HWND。

## 6. Processes、Shell 与存储

```ts
export const processes = {
  list(query?: ProcessQuery, options?: CallOptions): Promise<ProcessSnapshot[]>;
  launch(spec: LaunchSpec, options?: CallOptions): Promise<ProcessRef>;
  wait(ref: ProcessRef, options?: CallOptions): Promise<ExitStatus>;
  terminate(ref: ProcessRef, options?: CallOptions): Promise<void>;
};

export const fs = {
  read(path: PathLike, options?: ReadOptions): Promise<Uint8Array>;
  write(path: PathLike, data: Uint8Array | string, options?: WriteOptions): Promise<void>;
  list(path: PathLike, options?: CallOptions): Promise<readonly FileEntry[]>;
  watch(path: PathLike, handler: (event: FileEvent) => void): Subscription;
};

export const shell = {
  open(target: ShellTarget, options?: CallOptions): Promise<void>;
  selectFile(options?: FilePickerOptions): Promise<readonly Path[]>;
};
```

文件和进程资源由 Worker/IO lane 所有；JS 只持有引用、快照和结果。凭据、终止、驱动器和系统关机是独立高权限 capability。

## 7. Clipboard、Display 与媒体

```ts
export const clipboard = {
  read(format?: ClipboardFormat, options?: CallOptions): Promise<ClipboardValue>;
  write(value: ClipboardValue, options?: CallOptions): Promise<void>;
  observe(handler: (event: ClipboardEvent) => void): Subscription;
};

export const displays = {
  list(options?: CallOptions): Promise<readonly MonitorSnapshot[]>;
  primary(options?: CallOptions): Promise<MonitorSnapshot>;
  pixel(point: Point, options?: CallOptions): Promise<Color>;
  capture(area?: Rect, options?: CaptureOptions): Promise<ImageSnapshot>;
};

export const media = {
  beep(spec?: BeepSpec, options?: CallOptions): Promise<void>;
  play(source: MediaSource, options?: CallOptions): Promise<void>;
};
```

## 8. UI construction

Runtime UI 与自动化目标窗口分开。Runtime 自己创建的窗口使用 Component/Layout/Visual Tree；外部窗口只通过 Window/Control/Automation API 操作。

```ts
export const ui = {
  createWindow(spec: WindowSpec): Promise<AppWindow>;
  dialog(spec: DialogSpec, options?: CallOptions): Promise<DialogResult>;
  menu(spec: MenuSpec): Promise<MenuRef>;
};
```

## 9. Action 与权限

```ts
interface ActionPlan<T> {
  inspect(): Promise<ActionInspection>;
  execute(options?: CallOptions): Promise<T>;
  cancel(): void;
}

export const actions = {
  plan<T>(spec: ActionSpec<T>): ActionPlan<T>;
  execute<T>(spec: ActionSpec<T>, options?: CallOptions): Promise<T>;
};
```

每个 Action 声明 `capability`、target、preconditions、deadline、parent 和 idempotency key。权限按能力授予，不按函数散落判断。

## 10. 不进入标准库的内容

裸句柄、线程 ID、COM 指针、窗口过程、Hook 地址、任意 DLL 调用、任意内存读写、未审计消息发送、内部调度队列和 Native 对象指针不进入标准 TS API。需要这些能力时使用隔离插件和版本化 token。

## 11. 设计结果

标准库不是 AHK 函数的逐项翻译，而是以 Windows 对象图和 Action Kernel 为核心的组合式 TS 系统：

- snapshot 表示观察；ref 表示可验证资源；
- Promise 表示跨 lane 或外部等待；
- Subscription 表示事件所有权；
- Action 表示改变外部世界；
- Context 表示当前环境；
- capability 表示权限；
- Trace 表示可观察和可重放行为。
