# Screen API

状态：`implemented`（Monitor 族，含接线）：`screen.monitorCount()`、`screen.monitor(index?)` 已具备 Native service、`rime:screen` 模块、SDK 门面与三层 contract（`tests/native/screen_tests.cpp`、`tests/js/screen_slice.cpp`）；`contracts/registry/actions.json` 的 `screen.capture` capability 行与 `readSurfaces` 两行、`Bootstrap` 接线与 `production_capabilities()`、`sdk/src/modules.d.ts` 的 `declare module "rime:screen"` 均已落地（逐项见"实现状态"）。

仍为 `contract-only`：`CaretGetPos`、`ImageSearch`、`PixelGetColor`、`PixelSearch`、`SysGet`、`SysGetIPAddresses`（逐行状态见 `docs/api/coverage.json`）——它们需要截图/像素/caret seam，属本域后续提交。

源码证据：`functions.h` 的 `Monitor*`、`SysGet`、`Pixel*`、`ImageSearch`、`CaretGetPos`；`source/lib/env.cpp`（Monitor 族实现）、`source/lib/pixel.cpp`、`source/lib/win.cpp`；`source/window.h:193-199` 的 `MonitorInfoPackage` 与 `COUNT_ALL_MONITORS INT_MIN`。

监视器枚举返回不可变快照；像素读取、截图、图像搜索和 Caret 查询走 UI/worker lane 并支持取消。坐标空间、DPI、虚拟屏幕原点和颜色格式必须在 TS 类型中明确。

## API 清单

| 调用 | 形态 | 执行 lane | capability | AHK 对应 |
| --- | --- | --- | --- | --- |
| `screen.monitorCount(options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `MonitorGetCount` |
| `screen.monitor(index?, options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `MonitorGet` / `MonitorGetWorkArea` / `MonitorGetName` / `MonitorGetPrimary` |

模块导出为 `screen`（注册名 `rime:screen`），两函数原生 `length` 均为 `0`（可选参数不能用 arity 表达）。`monitor` 的首位参数按类型分流：数字是 `index`，`undefined`/`null`/对象是 `options`（因此 `screen.monitor(options)` 也合法），其余同步抛 `TypeError`。

```ts
interface ScreenRect { left: number; top: number; right: number; bottom: number; }
interface ScreenMonitor {
  index: number;    // 1-based, EnumDisplayMonitors 顺序
  primary: boolean;
  name: string;     // 设备名，如 \\.\DISPLAY1
  bounds: ScreenRect; // 整屏，含被任务栏遮住的部分
  work: ScreenRect;   // 去掉任务栏与自动隐藏栏之后的可用区
}
screen.monitorCount(options?): Promise<number>;
screen.monitor(index?, options?): Promise<ScreenMonitor>;
```

一个 `ScreenMonitor` 记录同时覆盖四条 AHK 命令：`MonitorGet` → `bounds`，`MonitorGetWorkArea` → `work`，`MonitorGetName` → `name`，`MonitorGetPrimary` → 省略 index 时选中的那一条（`primary` 标志同时告知它是不是主屏）。AHK 每条命令各自开一次 `EnumMonitorProc`，本实现一次枚举给出全部字段，但**字段全部逐一对齐 `MONITORINFOEX` 的 `rcMonitor`/`rcWork`/`szDevice`/`MONITORINFOF_PRIMARY`，不做任何换算**。

## 源码证据

- `rime-research/AutoHotkey-alpha/source/lib/env.cpp:92-207`：`EnumMonitorProc`（回调填 `MonitorInfoPackage`）、`EnumForMonitorGet`（`COUNT_ALL_MONITORS INT_MIN` 时计数、否则取指定 monitor）、`MonitorGetCount`、`MonitorGetPrimary`、`MonitorGet(MonitorGetWorkArea)`、`MonitorGetName`。
- `rime-research/AutoHotkey-alpha/source/window.h:193-199`：`MonitorInfoPackage{left, top, right, bottom, work_left, work_top, work_right, work_bottom}` 与 `COUNT_ALL_MONITORS`。
- Rime 实现：`engine/win32/src/screen.cpp`（service 与 `collect_monitor`）、`engine/win32/js/src/screen_module.cpp`（`rime:screen`）、`sdk/src/screen.ts`（SDK 门面）。
- Win32 表面只有 `EnumDisplayMonitors` 与 `GetMonitorInfoW`（`user32`，由现有链接提供）；不使用 `SM_CMONITORS`（见"设计取舍"）。

## TS 类型

`sdk/src/screen.ts` 导出 `ScreenRect`、`ScreenMonitor`、`ScreenBridge` 与 `screen` 门面。门面动态 `await import("rime:screen")`（同 `clipboard.ts`/`registry.ts`），因此两个方法都返回 Promise：

```ts
screen.monitorCount(options?): Promise<number>;          // 解包 native 的 { count }
screen.monitor(index?, options?): Promise<ScreenMonitor>;
```

`ScreenBridge` 是模块契约（`monitor` 两个重载把"省略 index"与"给定 index"分开表达，因为 native 侧 `argv[0]` 为 `undefined` 时按 options 处理）；模块声明在 `sdk/src/modules.d.ts`：

```ts
declare module "rime:screen" {
  const screen: import("./screen").ScreenBridge;
  export { screen };
}
```

JS 边界的坐标与计数都是**虚拟桌面像素**（`rcMonitor`/`rcWork` 原样，可能为负——副屏在主屏左侧时 `left < 0`）。DPI 缩放不做隐式换算：返回的就是 `GetMonitorInfoW` 给的设备像素，调用方需要逻辑像素时自己除以 `GetDpiForWindow` 类的比例。颜色格式与 caret 属于后续提交，本阶段类型里没有。

## 底层实现

- `ScreenService`（`engine/win32/include/rime/win32/screen.hpp`）：`monitor_count(int&)`、`monitor_at(int, Monitor&)`，均 `[[nodiscard]]` 返回 `rime::core::Error`；私有 `collect(std::vector<Monitor>&)` 是唯一的枚举入口。
- `collect_monitor`（`screen.cpp:17`）是 `EnumDisplayMonitors` 的回调：`MONITORINFOEXW` 取 `dwFlags & MONITORINFOF_PRIMARY`、`rcMonitor`、`rcWork`、`szDevice`，`to_utf8(std::wstring(...))` 转设备名。**单个显示器 `GetMonitorInfo` 失败只跳过该显示器**（返回 `TRUE` 继续），不中止枚举——丢一块屏不该把其它屏一起藏起来。
- 索引在回调内按 `out->size() + 1` 生成，因此 **index 与 `EnumDisplayMonitors` 回调顺序一一对应**，也与 AHK `MonitorGet` 的参数一致（AHK 同样以 1-based 枚举序取 monitor）。
- `monitor_at(0)` 不按位置取，而是**在结果里找 `primary` 标志**；万一 OS 没给任何主屏标志，回退到第一条（`screen.cpp:72-74`），不会返回空答案。
- 服务无状态、每次调用重新枚举：显示器热插拔后下一次调用即反映新拓扑，不缓存过期快照。

## 线程和资源所有权

- `HMONITOR`/`HDC`/`MONITORINFOEXW` 全部是每次调用内的栈上值，**没有任何句柄或指针离开 service**；JS 只收到 JSON 化的整数与设备名字符串。
- 服务实例由 Bootstrap 持有，模块 binding 只拿裸引用，生命周期与 `Bootstrap::stop()` 同进退。
- 枚举不触碰 HWND/COM/DirectComposition，因此不占用 UI Thread，也不需要 COM Apartment；工作体经 `start_async` 落在 worker lane 上执行。
- 结果是**不可变快照**：`monitor_value` 在入 JS 前把 `Monitor` 拷成 `json::Object`，JS 拿到的记录与后续枚举互不影响。

## 异步/取消

- 两个调用都经 `start_async(context, work, cancellationId)` 在 worker lane 执行一次服务调用，绑定 `cancellationId`/`AbortSignal` 可取消（拒绝 `cancelled`）。
- **`deadlineMs` 会被解析但不施加**——与 `registry.read`、`windows.active()` 一致：读不排队、不重试，只有一个不可中断的 OS 调用，给出 deadline 只会是假承诺。
- 无队列、无节流、无合并：并发调用各自枚举一次，互不共享可变状态。

## 权限

- capability 名字面量 `kScreenCaptureCapability = "screen.capture"` 集中在 `screen_module.cpp:26`，两处工作体各 `kernel->allows(...)` 一次（`screen_module.cpp:91`、`:124`），拒绝为 `capability_denied`，消息 `required capability was not granted: screen.capture`。
- 名字取 `screen.capture` 而不是 `screen.read`：读屏信息、像素、截图、caret 在 AHK 侧都由 `screen` 这一条 `#Requires AutoHotkey v2` 能力门槛统一放行，而本 runtime 的 capability 面向"能不能观察用户屏幕内容"这一类信任，Monitor 枚举只是这一类里最低风险的一项。
- `screen.capture` 已进 `production_capabilities()`（共 21 项），但**不在 `demo_capabilities()`**：示例应用不需要读屏。
- 台账：`contracts/registry/actions.json` 的 `capabilities[]` 含 `screen.capture`（`declared`/`checked` 两行指针），`readSurfaces[]` 含 `screen.monitorCount()` 与 `screen.monitor(index?)` 两行。

## 错误

`rime::core::Error::Code` → JS `ActionError.code`。稳定文案（测试逐字断言，改动即契约变更）：

| Code | 文案 |
| --- | --- |
| `invalid_contract` | `monitor index must be 0 (primary) or a positive number`、`monitor index <n> does not exist, <m> monitor(s) connected` |
| `target_gone` | `no monitor is connected` |
| `execution_failed` | `cannot enumerate monitors` |

同步抛出（不入队，native 模块直接抛）：`RangeError`（`monitor(index): index must be 0 or a positive number`，index 为负或非 32 位可表示）、`TypeError`（`monitor(index?, options?): index must be a number`，既不是数字也不是对象；`monitor(index?, options?): expected monitor(index?, options?)`；`monitorCount(options?)`，argc > 1）。

注意同步 `RangeError`/`TypeError` 与异步 `invalid_contract` 是**两条不同的道**：负 index 在解析期就被拒绝（连 worker 都不进），而"正数但不存在"必须先枚举才知道，因此只能是拒绝的 Promise。测试分别用 `try/catch` 与 `e.code` 各钉一遍。

## Trace

两个调用都是查询，**不 dispatch Action、不产生任何 Trace 条目**（`tests/js/screen_slice.cpp` 在完成全部调用后断言 `trace->snapshot().empty()`）。这是文档化豁免而不是遗漏：Trace 记录的是"可检查、可取消、有副作用"的 Action，枚举显示器没有副作用可回放。后续 `PixelGetColor`/`PixelSearch`/`ImageSearch` 同为只读，预计同样不进 Trace；若将来出现写入性 screen 操作，必须新建 action type 而不是复用本调用。

## 设计取舍

- **用 `EnumDisplayMonitors` 而不是 `SM_CMONITORS`**：`GetSystemMetrics(SM_CMONITORS)` 计数与 `EnumDisplayMonitors(NULL,NULL,…)` 在有 pseudo-monitor 时不保证一致（`engine/win32/js/src/env.cpp:143` 的既有注释记录了这个陷阱）。AHK 自己也在 `EnumForMonitorGet` 里同时提供"计数"与"取第 n 个"两种模式，本实现统一走枚举，计数与取值天然自洽。
- **一次枚举覆盖四条 AHK 命令**：AHK 每条命令各开一次枚举，所以 `MonitorGetCount` 与 `MonitorGet(1)` 之间可能看到不同的热插拔结果。本实现的 `monitor()` 返回整条记录，一次调用拿到 bounds/work/name/primary，四个字段必然同源；代价是"我只想要名字"也会拿回全套矩形——这是无害的多余信息，不值得为此拆成四个 native 函数。
- **省略 index = 主屏，而不是 = index 1**：与 AHK 的 `MonitorGet`（省略参数时取 primary）保持一致，`MonitorGetPrimary` 因此不需要独立 API。显式 `index === 0` 与省略等价，都是"按 primary 标志找"。
- **index 是 1-based 且按枚举序**：对齐 AHK 的 `MonitorGet MonitorNum`，而不是换成 0-based 自创约定；0 保留给"primary"这个特殊语义，与 AHK 的 `MonitorGetPrimary` 语义一致。
- **不施加 deadline**：见"异步/取消"。只读、单次、不可中断的 OS 调用给出 deadline 只会是假承诺，与 `registry.read` 的既有豁免同源。
- **不进 Trace**：见"Trace"。本 runtime 把 Trace 绑定在可检查的副作用上，读屏信息没有副作用可回放。

## contract / native / stress 测试

- `tests/native/screen_tests.cpp`（L5）：真实 `EnumDisplayMonitors` 下断言 `count >= 1`；`1..count` 全部可取且 `index` 自洽、名字非空、`work ⊆ bounds`、`right > left`；`count+1` 拒 `invalid_contract` 且消息含 `does not exist` 与该序号；负数拒 `invalid_contract`；恰有一个 `primary` 且 `monitor_at(0)` 就是那一条；`monitor_at(0)` 与按号码取它的记录在 bounds/work/name 上逐字相等。**计数的正确性由"`1..count` 成功且 `count+1` 失败"这对断言双向夹住**，不在测试里重写一遍枚举逻辑（否则会与被测实现同错同绿）。
- `tests/js/screen_slice.cpp`（L5）：生产接线（`rime:screen` + 真实 `ScreenService`）。段 1 用**运行时启动前先做的 native 读**作独立观察，逐字段比对 JS 返回的 count 与 primary 记录；段 2 钉住 `monitor(1)`/`monitor(count)`/`count+1 → invalid_contract`/`-1 → RangeError`/`'one' → TypeError`；随后断言 trace 为空；最后用空 capability 的第二个 runtime（同 `registry_slice` 的写法，JS lane 进程级，必须先停第一个）证明两处门禁都按 `screen.capture` 拒绝。
- stress：本域无队列、无 Hook、无重入路径，当前没有单独 stress 测试；像素/caret seam 落地后再补（它们才有取消竞态与时间窗）。

## 实现状态

| 部件 | 状态 | 位置 |
| --- | --- | --- |
| Native service | done | `engine/win32/include/rime/win32/screen.hpp`、`engine/win32/src/screen.cpp` |
| `rime:screen` 模块 | done | `engine/win32/js/include/rime/win32/js_screen.hpp`、`engine/win32/js/src/screen_module.cpp` |
| SDK 门面 | done | `sdk/src/screen.ts` |
| TS 模块声明 / 导出 | done | `sdk/src/modules.d.ts`、`sdk/src/index.ts` |
| capability 台账 | done | `contracts/registry/actions.json`（`capabilities` + `readSurfaces`） |
| Bootstrap 接线与生产能力 | done | `engine/win32/js/src/bootstrap.{hpp,cpp}`（`production_capabilities()` 含 `screen.capture`） |
| coverage 行 | done（Monitor 5 行 → `implemented`） | `docs/api/coverage.json` |
| contract 测试 | done | `tests/native/screen_tests.cpp`、`tests/js/screen_slice.cpp`（CMake 已登记） |
| `CaretGetPos` / `ImageSearch` / `PixelGetColor` / `PixelSearch` | contract-only | 待截图与像素 seam |
| `SysGet` / `SysGetIPAddresses` | contract-only | 待本域后续提交 |
