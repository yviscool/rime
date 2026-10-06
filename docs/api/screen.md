# Screen API

状态：`implemented`（Monitor 族 + Pixel 族 + ImageSearch + CaretGetPos + SysGet 族，含接线）：`screen.monitorCount()`、`screen.monitor(index?)`、`screen.pixel(x, y)`、`screen.pixelSearch(area, color, options)`、`screen.imageSearch(area, imagePath, options)`、`screen.caret(options)`、`screen.sysGet(index, options)`、`screen.sysGetIPAddresses(options)` 已具备 Native service、纯扫描层、图像解码层、`rime:screen` 模块、SDK 门面与三层 contract（`tests/native/screen_tests.cpp`、`tests/js/screen_slice.cpp`）；`contracts/registry/actions.json` 的 `screen.capture` capability 行与 `readSurfaces` 八行、`Bootstrap` 接线与 `production_capabilities()`、`sdk/src/modules.d.ts` 的 `declare module "rime:screen"` 均已落地（逐项见"实现状态"）。本域已无 `contract-only` 条目。

源码证据：`functions.h` 的 `Monitor*`、`SysGet`、`SysGetIPAddresses`、`Pixel*`、`ImageSearch`、`CaretGetPos`；`source/lib/env.cpp`（Monitor 族实现、`SysGet` 实现）、`source/lib/pixel.cpp`、`source/lib/win.cpp`、`source/script_autoit.cpp`（`SysGetIPAddresses` 实现）；`source/window.h:193-199` 的 `MonitorInfoPackage` 与 `COUNT_ALL_MONITORS INT_MIN`。

监视器枚举返回不可变快照；像素读取与图像搜索走 worker lane 并支持取消。坐标空间、DPI、虚拟屏幕原点和颜色格式必须在 TS 类型中明确。

## API 清单

| 调用 | 形态 | 执行 lane | capability | AHK 对应 |
| --- | --- | --- | --- | --- |
| `screen.monitorCount(options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `MonitorGetCount` |
| `screen.monitor(index?, options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `MonitorGet` / `MonitorGetWorkArea` / `MonitorGetName` / `MonitorGetPrimary` |
| `screen.pixel(x, y, options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `PixelGetColor` |
| `screen.pixelSearch(area, color, options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `PixelSearch` |
| `screen.imageSearch(area, imagePath, options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `ImageSearch` |
| `screen.caret(options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `CaretGetPos` |
| `screen.sysGet(index, options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `SysGet` |
| `screen.sysGetIPAddresses(options?)` | Promise，直读服务（不入队） | worker lane | `screen.capture` | `SysGetIPAddresses` |

模块导出为 `screen`（注册名 `rime:screen`），八个函数里 `sysGet` 的原生 `length` 是 `1`（`index` 是唯一必填参数），其余七个是 `0`（可选参数不能用 arity 表达）。`monitor` 的首位参数按类型分流：数字是 `index`，`undefined`/`null`/对象是 `options`（因此 `screen.monitor(options)` 也合法），其余同步抛 `TypeError`。

```ts
interface ScreenRect { left: number; top: number; right: number; bottom: number; }
type ScreenPixelSearchResult = { found: false } | { found: true; x: number; y: number };
type ScreenCaretResult = { found: false } | { found: true; x: number; y: number };
interface ScreenPixelSearchOptions extends ActionOptions { variation?: number; }
interface ScreenMonitor {
  index: number;    // 1-based, EnumDisplayMonitors 顺序
  primary: boolean;
  name: string;     // 设备名，如 \\.\DISPLAY1
  bounds: ScreenRect; // 整屏，含被任务栏遮住的部分
  work: ScreenRect;   // 去掉任务栏与自动隐藏栏之后的可用区
}
screen.monitorCount(options?): Promise<number>;
screen.monitor(index?, options?): Promise<ScreenMonitor>;
screen.pixel(x, y, options?): Promise<number>;            // 0xRRGGBB
screen.pixelSearch(area, color, options?): Promise<ScreenPixelSearchResult>;
screen.imageSearch(area, imagePath, options?): Promise<ScreenPixelSearchResult>;
screen.caret(options?): Promise<ScreenCaretResult>;       // 屏幕坐标
screen.sysGet(index, options?): Promise<number>;          // 解包 native 的 { metric }
screen.sysGetIPAddresses(options?): Promise<string[]>;    // 解包 native 的 { addresses }
```

`imagePath` 必须是字符串（否则同步 `TypeError`），按 UTF-8 交给解码层；`area` 与 `pixelSearch` 是同一个 `ScreenRect`。

一个 `ScreenMonitor` 记录同时覆盖四条 AHK 命令：`MonitorGet` → `bounds`，`MonitorGetWorkArea` → `work`，`MonitorGetName` → `name`，`MonitorGetPrimary` → 省略 index 时选中的那一条（`primary` 标志同时告知它是不是主屏）。AHK 每条命令各自开一次 `EnumMonitorProc`，本实现一次枚举给出全部字段，但**字段全部逐一对齐 `MONITORINFOEX` 的 `rcMonitor`/`rcWork`/`szDevice`/`MONITORINFOF_PRIMARY`，不做任何换算**。

## 源码证据

- `rime-research/AutoHotkey-alpha/source/lib/env.cpp:92-207`：`EnumMonitorProc`（回调填 `MonitorInfoPackage`）、`EnumForMonitorGet`（`COUNT_ALL_MONITORS INT_MIN` 时计数、否则取指定 monitor）、`MonitorGetCount`、`MonitorGetPrimary`、`MonitorGet(MonitorGetWorkArea)`、`MonitorGetName`。
- `rime-research/AutoHotkey-alpha/source/window.h:193-199`：`MonitorInfoPackage{left, top, right, bottom, work_left, work_top, work_right, work_bottom}` 与 `COUNT_ALL_MONITORS`。
- `rime-research/AutoHotkey-alpha/source/lib/pixel.cpp:145-640`：`PixelSearch` 与 `ImageSearch` 的共同骨架（`SET_COLOR_RANGE` 的 variation 规则、`red_low`/`red_high` 的不回绕语义、first-pixel optimization、`*` 选项解析、方向感知扫描）。
- `rime-research/AutoHotkey-alpha/source/lib/vars.cpp:991-1021`：`CaretGetPos`——前台窗口线程的 `GetGUIThreadInfo`、`hwndCaret` 判定、`rcCaret` 经 `ClientToScreen` 转屏幕坐标；无前台窗口或无 caret 时把输出变量置空并返回 FALSE。
- `rime-research/AutoHotkey-alpha/source/lib/env.cpp:85-88`：`SysGet(Index)` 就是 `GetSystemMetrics(aIndex)` 的直通包装，没有任何错误分支。
- `rime-research/AutoHotkey-alpha/source/script_autoit.cpp:99-145`：`SysGetIPAddresses`——`WSAStartup` → `gethostname` → `gethostbyname` → `inet_ntoa` 逐个 `Append` 进返回数组，失败时返回已收集的部分（`WSAStartup` 失败直接返回空数组），全程 IPv4。
- Rime 实现：`engine/win32/src/screen.cpp`（service、`clip_area`、`caret`、`system_metric`、`ip_addresses` 与 `collect_monitor`）、`engine/win32/src/screen_pixels.cpp`（纯匹配层）、`engine/win32/src/screen_seam.cpp`（捕获 seam）、`engine/win32/src/image_loader.cpp`（GDI+ 解码）、`engine/win32/js/src/screen_module.cpp`（`rime:screen`）、`sdk/src/screen.ts`（SDK 门面）。
- Win32 表面只有 `EnumDisplayMonitors`、`GetMonitorInfoW`、`GetForegroundWindow`/`GetGUIThreadInfo`/`ClientToScreen`（`user32`）、`GetSystemMetrics`（`user32`）与 `GetAdaptersAddresses`（`iphlpapi`，`rime_win32` 为此链 `iphlpapi`）；不使用 `SM_CMONITORS`（见"设计取舍"）。

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

JS 边界的坐标与计数都是**虚拟桌面像素**（`rcMonitor`/`rcWork` 原样，可能为负——副屏在主屏左侧时 `left < 0`）。DPI 缩放不做隐式换算：返回的就是 `GetMonitorInfoW` 给的设备像素，调用方需要逻辑像素时自己除以 `GetDpiForWindow` 类的比例。颜色统一为 `0xRRGGBB` 数字，`pixel`/`pixelSearch`/`imageSearch` 共用同一种格式；`caret` 返回同一种坐标空间的 `x`/`y`（屏幕坐标，见下）。

## 底层实现

- `ScreenService`（`engine/win32/include/rime/win32/screen.hpp`）：`monitor_count(int&)`、`monitor_at(int, Monitor&)`、`pixel_color(int, int, std::uint32_t&)`、`pixel_search(...)`、`image_search(...)`、`ip_addresses(std::vector<std::string>&)` 返回 `rime::core::Error`；`caret()` 返回 `Caret`、`system_metric(int)` 返回 `int`，这两个没有错误通道（原因写在各自的头文件注释里）；私有 `collect(std::vector<Monitor>&)` 是唯一的枚举入口，`clip_area` 是两个搜索共用的唯一矩形裁剪入口。
- `system_metric(int)` 是 `GetSystemMetrics` 的一行直读。`ip_addresses(...)` 走 `GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, ...)`：第一趟要尺寸、第二趟填 `std::vector<unsigned char>`，再逐条取 `FirstUnicastAddress` 中 `sa_family == AF_INET` 且 `DadState == IpDadStatePreferred` 的地址，把 `in_addr` 的四个字节按 wire order 直接格式化成点分十进制——因此既不 `WSAStartup`，也不链接 `ws2_32`，只新增 `iphlpapi`。
- `collect_monitor`（`screen.cpp:17`）是 `EnumDisplayMonitors` 的回调：`MONITORINFOEXW` 取 `dwFlags & MONITORINFOF_PRIMARY`、`rcMonitor`、`rcWork`、`szDevice`，`to_utf8(std::wstring(...))` 转设备名。**单个显示器 `GetMonitorInfo` 失败只跳过该显示器**（返回 `TRUE` 继续），不中止枚举——丢一块屏不该把其它屏一起藏起来。
- 索引在回调内按 `out->size() + 1` 生成，因此 **index 与 `EnumDisplayMonitors` 回调顺序一一对应**，也与 AHK `MonitorGet` 的参数一致（AHK 同样以 1-based 枚举序取 monitor）。
- `monitor_at(0)` 不按位置取，而是**在结果里找 `primary` 标志**；万一 OS 没给任何主屏标志，回退到第一条（`screen.cpp:72-74`），不会返回空答案。
- 服务无状态、每次调用重新枚举：显示器热插拔后下一次调用即反映新拓扑，不缓存过期快照。

## 线程和资源所有权

- `HMONITOR`/`HDC`/`MONITORINFOEXW` 全部是每次调用内的栈上值，**没有任何句柄或指针离开 service**；JS 只收到 JSON 化的整数与设备名字符串。
- 服务实例由 Bootstrap 持有，模块 binding 只拿裸引用，生命周期与 `Bootstrap::stop()` 同进退。
- 枚举不触碰 HWND/COM/DirectComposition，因此不占用 UI Thread，也不需要 COM Apartment；工作体经 `start_async` 落在 worker lane 上执行。
- 结果是**不可变快照**：`monitor_value` 在入 JS 前把 `Monitor` 拷成 `json::Object`，JS 拿到的记录与后续枚举互不影响。
- `ImageBuffer`（`image_loader.hpp`）是 `std::vector` 拥有的普通字节；GDI+ 的 startup token 在 `load_image_file` 内 take/release（`GdiplusSession` 析构时 `GdiplusShutdown`），**解码结束不留下任何进程级状态**。
- `GetAdaptersAddresses` 的缓冲区是调用内的 `std::vector<unsigned char>`，`IP_ADAPTER_ADDRESSES*` 与 `sockaddr_in` 只在同一次调用里读，不离开 service；没有 socket 被创建、保存或关闭，也就没有句柄所有权问题。

## 异步/取消

- 七个调用都经 `start_async(context, work, cancellationId)` 在 worker lane 执行一次服务调用，绑定 `cancellationId`/`AbortSignal` 可取消（拒绝 `cancelled`）。
- **`deadlineMs` 会被解析但不施加**——与 `registry.read`、`windows.active()` 一致：读不排队、不重试，只有一次不可中断的系统调用（图像搜索外加一次文件解码），给出 deadline 只会是假承诺。
- 无队列、无节流、无合并：并发调用各自读一次屏幕，互不共享可变状态；`ScreenService` 本身无状态。

## 权限

- capability 名字面量 `kScreenCaptureCapability = "screen.capture"` 集中在 `screen_module.cpp:30`，八个工作体各 `kernel->allows(...)` 一次（`screen_module.cpp:96`、`:129`、`:189`、`:286`、`:351`、`:391`、`:426`、`:454`），拒绝为 `capability_denied`，消息 `required capability was not granted: screen.capture`。
- 名字取 `screen.capture` 而不是 `screen.read`：读屏信息、像素、截图、caret 在 AHK 侧都由 `screen` 这一条 `#Requires AutoHotkey v2` 能力门槛统一放行，而本 runtime 的 capability 面向"能不能观察用户屏幕内容"这一类信任，Monitor 枚举只是这一类里最低风险的一项。
- `screen.capture` 已进 `production_capabilities()`（共 21 项），但**不在 `demo_capabilities()`**：示例应用不需要读屏。
- 台账：`contracts/registry/actions.json` 的 `capabilities[]` 含 `screen.capture`（`declared` 一行 + `checked` 八行指针），`readSurfaces[]` 含 `screen.monitorCount()`、`screen.monitor(index?)`、`screen.pixel(x, y)`、`screen.pixelSearch(area, color, options)`、`screen.imageSearch(area, imagePath, options)`、`screen.caret(options?)`、`screen.sysGet(index, options?)`、`screen.sysGetIPAddresses(options?)` 八行。

## 错误

`rime::core::Error::Code` → JS `ActionError.code`。稳定文案（测试逐字断言，改动即契约变更）：

| Code | 文案 |
| --- | --- |
| `invalid_contract` | `monitor index must be 0 (primary) or a positive number`、`monitor index <n> does not exist, <m> monitor(s) connected`、`pixel (<x>, <y>) is outside the virtual screen`、`search area does not intersect the virtual screen`、`variation must be in 0..255`、`image path must not be empty`、`image file could not be loaded: <path>`、`image file has no pixels: <path>`、`image file is too large to search: <path>` |
| `target_gone` | `no monitor is connected` |
| `execution_failed` | `cannot enumerate monitors`、`cannot capture the screen at (<x>, <y>)`、`cannot capture the screen area`、`screen capture returned no pixel`、`cannot start the image decoder`、`cannot allocate the decoded image`、`cannot convert the image to 32bpp BGRA`、`cannot read the decoded image`、`cannot size the adapter address list`、`cannot read the adapter address list` |

同步抛出（不入队，native 模块直接抛）：`RangeError`（`monitor(index): index must be 0 or a positive number`，index 为负；`<where> must be an integer in -2147483648..2147483647`；`pixelSearch color must be in 0x000000..0xFFFFFF`；`variation must be in 0..255`；`<where> must be a finite integer`）、`TypeError`（`monitor(index?, options?): index must be a number`；`monitor(index?, options?): expected monitor(index?, options?)`；`monitorCount(options?)`/`pixel(x, y, options?)`/`pixelSearch(area, color, options?)`/`imageSearch(area, imagePath, options?)`/`sysGet(index, options?)`/`sysGetIPAddresses(options?)` 的 arity（`sysGet` 少给 `index` 或多给参数同列）；`<where> must be an integer`，含小数与非数字；`pixelSearch(area, ...)`/`imageSearch(area, ...)` 的 `area must be an object`；`options must be an object`；`imageSearch(area, imagePath, options?): imagePath must be a string`）。

注意同步 `RangeError`/`TypeError` 与异步 `invalid_contract` 是**两条不同的道**：负 index 在解析期就被拒绝（连 worker 都不进），而"正数但不存在"必须先枚举才知道，因此只能是拒绝的 Promise。测试分别用 `try/catch` 与 `e.code` 各钉一遍。

## Trace

七个调用都是查询，**不 dispatch Action、不产生任何 Trace 条目**（`tests/js/screen_slice.cpp` 在完成全部调用后断言 `trace->snapshot().empty()`）。这是文档化豁免而不是遗漏：Trace 记录的是"可检查、可取消、有副作用"的 Action，枚举显示器、读像素、在屏幕上找图、读系统指标与本机地址都没有副作用可回放。若将来出现写入性 screen 操作，必须新建 action type 而不是复用本调用。

## 设计取舍

- **用 `EnumDisplayMonitors` 而不是 `SM_CMONITORS`**：`GetSystemMetrics(SM_CMONITORS)` 计数与 `EnumDisplayMonitors(NULL,NULL,…)` 在有 pseudo-monitor 时不保证一致（`engine/win32/js/src/env.cpp:143` 的既有注释记录了这个陷阱）。AHK 自己也在 `EnumForMonitorGet` 里同时提供"计数"与"取第 n 个"两种模式，本实现统一走枚举，计数与取值天然自洽。
- **一次枚举覆盖四条 AHK 命令**：AHK 每条命令各开一次枚举，所以 `MonitorGetCount` 与 `MonitorGet(1)` 之间可能看到不同的热插拔结果。本实现的 `monitor()` 返回整条记录，一次调用拿到 bounds/work/name/primary，四个字段必然同源；代价是"我只想要名字"也会拿回全套矩形——这是无害的多余信息，不值得为此拆成四个 native 函数。
- **省略 index = 主屏，而不是 = index 1**：与 AHK 的 `MonitorGet`（省略参数时取 primary）保持一致，`MonitorGetPrimary` 因此不需要独立 API。显式 `index === 0` 与省略等价，都是"按 primary 标志找"。
- **index 是 1-based 且按枚举序**：对齐 AHK 的 `MonitorGet MonitorNum`，而不是换成 0-based 自创约定；0 保留给"primary"这个特殊语义，与 AHK 的 `MonitorGetPrimary` 语义一致。
- **不施加 deadline**：见"异步/取消"。只读、单次、不可中断的 OS 调用给出 deadline 只会是假承诺，与 `registry.read` 的既有豁免同源。
- **不进 Trace**：见"Trace"。本 runtime 把 Trace 绑定在可检查的副作用上，读屏信息没有副作用可回放。

## Pixel 族（`pixel` / `pixelSearch`）

三层，各层有各自的真相来源：

1. **纯扫描层**（`engine/win32/src/screen_pixels.cpp`，无 OS、无时钟）：`Framebuffer{width, height, bgra}` 上的 `color_at` / `color_matches` / `search`。`color_matches` 逐通道做 `abs` 比较并把 variation 夹在 0..255——**不做无符号回绕**，所以 target 的红分量为 1 时不会把红分量 255 的像素当成"接近"（AHK 在 `pixel.cpp` 用 `red_low`/`red_high` 表达同一条规则）。`search` 先归一化反向矩形，再按行主序（上→下、左→右）扫描。
2. **捕获 seam**（`engine/win32/include/rime/win32/screen_seam.hpp`）：`capture_rect(bounds, bgra, width, height)`。真实路径 `GetDC(nullptr)` → `CreateCompatibleBitmap` → `BitBlt` → `GetDIBits`（`biHeight` 取负得到 top-down 32bpp BGRA）；测试通过 `screen_seam::set_capture` 换掉**这一次屏幕读取**。seam 只替换环境，不替换被测单元——service、扫描层与模块全程真实运行（同 `input_seam` 的规则）。
3. **服务与模块**：`ScreenService::pixel_color` / `pixel_search` 先用 `GetSystemMetrics(SM_XVIRTUALSCREEN 等)` 求虚拟屏矩形，坐标或矩形落在屏外即 `invalid_contract`——绝不 BitBlt 屏外区域、把返回的一片黑色当作"没找到"。矩形部分越界时裁到屏内，报告的坐标仍是真实屏幕坐标。模块层 `screen.pixel` / `screen.pixelSearch` 走 `start_async`，capability 每次求值先读。

**与 AHK 的偏差**：

1. `PixelGetColor` 的 `Mode` 参数不暴露：本 runtime 只有一种颜色格式，返回 `0xRRGGBB` 数字（AHK 返回 `0xRRGGBB` 形式的字符串）。
2. `PixelSearch` 的扫描方向不跟随矩形四角顺序：反向矩形被归一化，答案永远是"行主序第一个命中"；AHK 会按 `right_to_left` / `bottom_to_top` 换扫描方向。
3. 未命中返回 `{found:false}`（与 AHK 的 `Found=false` + 空坐标同义），**不携带坐标键**，而不是塞 `-1`；未命中是结果，不是错误。

## ImageSearch（`imageSearch`）

在 Pixel 族的三层之上再加一层**图像解码**，四层各自负责一件事：

1. **解码层**（`engine/win32/src/image_loader.cpp`）：`load_image_file(path, ImageBuffer&)` 走 Windows 自带的 GDI+（不引入第三方依赖），结果统一转成与 seam 捕获帧同布局的 32bpp BGRA，`ImageBuffer::view()` 直接给出 `Framebuffer`，匹配层因此不需要知道任何文件格式。`GdiplusSession` 在函数内 take/release startup token，解码完不留进程级状态。失败按"谁的错"分两档：文件打不开/不是图/没有像素/超过 64M 像素上限 → `invalid_contract`（调用方参数错）；解码器起不来、转不出 32bpp、读不出像素 → `execution_failed`（系统不可用）。
2. **匹配层**：复用 `screen_pixels::image_search`——行主序枚举候选左上角，**先比针的第一枚像素**再逐像素验证（同 AHK 的 first-pixel optimization），variation 规则与单像素完全一致。
3. **服务**：`ScreenService::image_search` 按 **variation → 矩形裁剪 → 解码文件 → 捕获屏幕** 的顺序求值，坏路径在读屏之前就报出来（屏幕不会被无谓地读一次），矩形裁剪与 `pixel_search` 共用 `clip_area`。
4. **模块与门面**：`screen.imageSearch(area, imagePath, options)`；`area` 与 `pixelSearch` 共用 `parse_area`，`variation` 与 `ActionOptions` 共用 `parse_screen_options`。门面把 `variation` 重新并进 bridge 参数——`runAction` 只转发 action 字段，域名参数必须由门面合并（`tests/sdk/screen.test.ts` 钉住这条，回归即失败）。

**与 AHK 的偏差**：

1. 选项不编码在路径字符串里：AHK 的 `ImageSearch` 把 `*n`（variation）、`*wN hN`、`#RRGGBB` 透明色、`*IconN` 拼在 `ImageFile` 上；本实现放进类型化的 `options`，且只暴露 `variation`。
2. 不支持从 `.ico`/`.exe`/`.dll` 取资源图标，也不做透明色比较。
3. 只比较 RGB 三通道（`color_matches`），alpha 不参与：带 alpha 的素材按 GDI+ 画到黑底后的颜色参与匹配，截图这类不透明素材不受影响。
4. 扫描方向与 `PixelSearch` 同一偏差：矩形归一化后按行主序取第一个命中，不跟随四角顺序换方向。
5. 结果形状不同：AHK 用 `ErrorLevel` 1（屏幕上没找到）/2（文件问题）加输出变量；本实现"屏幕上没找到"是 `{found:false}`，文件问题是 `invalid_contract` 拒绝，没有第三种状态。

## CaretGetPos（`caret`）

第六个入口，**不走捕获 seam**：caret 不是屏幕像素，而是焦点控件的状态，Win32 只能通过前台线程的 `GetGUIThreadInfo` 读到。

- 语义对齐 `vars.cpp:991-1021`：`GetForegroundWindow()` → `GetWindowThreadProcessId` 取线程 → `GetGUIThreadInfo` → 要求 `hwndCaret` 非空 → `rcCaret`（caret 窗口的客户区坐标）经 `ClientToScreen` 转成屏幕坐标。
- **没有 caret 不是错误**：无前台窗口、GUI 线程信息读不到、没有 caret、或窗口在两次调用之间消失（转换失败）都是 `{found:false}`，与 `pixelSearch` 的 miss 同形状、同样不带坐标；AHK 用"输出变量置空 + 返回 FALSE"表达同一状态。
- 坐标固定是**屏幕坐标**，不跟随任何 CoordMode：AHK 会按 `CoordMode, Caret` 的设置用 `CoordToScreen(origin, COORD_MODE_CARET)` 换算成窗口/客户区相对坐标，本 runtime 尚无 CoordMode，因此不提供换算——这是该入口唯一一条坐标语义偏差，且方向是"少一个可变行为"。
- 只有前台窗口的 caret 读得到：caret 属于焦点控件，而焦点只在前台线程里。要断言一个已知 caret，必须先让目标窗口拿到前台（生产 `window.focus()` 的激活阶梯），测试就是这么安排的。
- 不用 UIA/`IAccessible`：`GetGUIThreadInfo` 是无 COM、无 Apartment 的 user32 调用，COM 路径会额外引入跨线程封送与完整性级别的失败面，而两者的输出一致。
- 这个入口没有 `invalid_contract`/`execution_failed`：调用方没有参数可错，系统侧没有"资源不可用"与"没有 caret"的区别，全部折叠进 `found:false`。

## SysGet / SysGetIPAddresses（`sysGet` / `sysGetIPAddresses`）

本域最后两个入口，都是机器级只读：一个读 Win32 系统指标，一个列本机 IPv4 地址。和 caret 一样不走捕获 seam——它们读的既不是屏幕像素，也不是焦点控件。

**`sysGet(index, options)`** 对齐 `env.cpp:85-88`：就是 `GetSystemMetrics(index)` 的直读。

- **未知下标返回 `0`，与 AHK 一样不报错**：`SM_SWAPBUTTON` 这类指标本来就可能为 0，"坏下标"与"真实 0"在 Win32 层就不可区分，凭空造一个错误码只会比 AHK 更不诚实；调用方要区分就自己校验下标。
- 下标只做整数校验（`parse_int32`）：非数字/小数/非有限值同步 `TypeError`，超出 int32 同步 `RangeError`，与 `pixel` 的坐标同一套规则；负下标**不**在同步期拒绝——`-1` 是一次合法查询，答案是 0。
- 不做 DPI 换算：`GetSystemMetrics` 返回什么就是什么，与本文件其它入口"不隐式换算 DPI"一致。

**`sysGetIPAddresses(options)`** 对齐 `script_autoit.cpp:99-145` 的**内容**而非**手段**：同样返回本机 IPv4 点分十进制字符串数组（含 `127.0.0.1`），但数据源是 `GetAdaptersAddresses` 的适配器地址表。

- **偏差（有意）**：AHK 走 Winsock，需要 `WSAStartup`/`WSACleanup` 这对**进程级**副作用，一个库没有理由替整个进程做这件事；`GetAdaptersAddresses` 不需要任何全局初始化，`rime_win32` 因此只多链 `iphlpapi`、不链 `ws2_32`。两端的集合可能不同（`gethostbyname` 只回主机名解析得到的地址），这是同一类问题的不同数据源，不是漏实现。
- **偏差（失败面）**：AHK 在 `WSAStartup`/`gethostbyname` 失败时把空数组或已收集的部分当成功返回；本实现只在适配器查询本身失败时整体 `execution_failed`（两条文案见"错误"），不返回半张表——半张地址表比失败更危险。
- **只列"可用"地址**：`DadState != IpDadStatePreferred` 的地址（DAD 进行中、重复、已废弃）不进列表——它们还不可达，Windows 的 `GetIpAddrTable` 同样不列它们。开发机上就出现过两条 169.254 链路本地地址只在适配器表、不在路由表的情况，正是这一步过滤让测试的两边按集合相等。
- **顺序不承诺**：本实现是适配器枚举顺序，AHK 是 `h_addr_list` 顺序；测试按集合比较，不按顺序比较。
- 空数组是结果不是错误（查不到地址的机器），与 AHK 返回空 `Array` 同义。

## contract / native / stress 测试

- `tests/native/screen_tests.cpp`（L5，一个文件内四层）：
  - 纯扫描层：手搭 4x3 framebuffer + 字面量颜色，覆盖 `color_at` 越界、variation 边界（0/1/5/63/64/255）、非回绕规则、行主序命中、反向矩形归一化、未命中时不动输出坐标。
  - 纯匹配层（`image_search`）：6x4 场里把针埋在 (4,2)、再放一枚"只匹配针首像素"的诱饵在 (1,0)——报出诱饵就等于把预过滤当成匹配；另覆盖针未出现（坐标不动）、针比场宽（不是沿左边缘裁剪搜）、variation 0x10 命中/0x0F 未命中。
  - seam 路径：注入"红=绝对 x、绿=绝对 y、蓝=0x40"的合成屏，`pixel_color(3,5)` 断言字面量 `0x00030540`，`pixel_search` 断言 `(2,3)`，未命中、variation 64/63 边界、三个 `invalid_contract` 文案各钉一遍。
  - 图像路径：BMP fixture（`tests/screen_image_fixture.hpp`，测试内写、测试内删）2x1 针必须命中 (2,3)；区域太窄 / 区域没有那两个像素各返回 miss；坏文件 → `image file could not be loaded`、空路径 → `image path must not be empty`；area 与 variation 的 `invalid_contract` 与 `pixel_search` 同文案。
  - 真实端到端：从真实捕获里裁 4x4 作针，在**同一区域**搜索必须命中 (0,0)（针与区域等大 → 只有一个候选位置，不依赖桌面显示什么）；翻转针里一个通道的位之后必须 miss——命中与未命中两条都在真实捕获上走完整链路。
  - 随后**还原 seam** 再测真实捕获：16x16 矩形的宽高与字节数、`pixel_color(0,0)` 成功。
  - Monitor 部分：真实 `EnumDisplayMonitors` 下断言 `count >= 1`；`1..count` 全部可取且 `index` 自洽、名字非空、`work ⊆ bounds`、`right > left`；`count+1` 拒 `invalid_contract` 且消息含 `does not exist` 与该序号；负数拒 `invalid_contract`；恰有一个 `primary` 且 `monitor_at(0)` 就是那一条；`monitor_at(0)` 与按号码取它的记录在 bounds/work/name 上逐字相等。**计数的正确性由"`1..count` 成功且 `count+1` 失败"这对断言双向夹住**，不在测试里重写一遍枚举逻辑（否则会与被测实现同错同绿）。
  - caret 部分：本测试自己搭一个**前台窗口 + caret**——无边框 popup（客户区 == 窗口矩形）由 `WindowService` 建在它自己的 UI 线程上（建在本线程就不会泵消息，桌面上会显示成点不动的黑窗，激活阶梯还会堵在它上面），caret 也由该线程创建，前台用生产 `focus()` 的阶梯争抢并在到手后 `GetForegroundWindow()` 复核；期望点 = `GetWindowRect` + 本测试写入的偏移，**不经过被测查询**。命中后断言精确坐标，`HideCaret`/`DestroyCaret` 之后（窗口仍是前台）必须 `found:false`。取不到前台是环境抢占，按 FLAKY 协议隔离复跑，不允许降级断言。
  - `SysGet` 部分：期望来自**另一条 Windows API**——`SM_CXSCREEN`/`SM_CYSCREEN` 必须等于先前 `EnumDisplayMonitors` 路径给出的主屏宽高，四条 `SM_*VIRTUALSCREEN` 必须等于把 `monitor_at(1..count)` 的记录求并集后的结果（两组都是 OS 对同一事实的两次陈述），`system_metric(-1)` 必须是 `0`。测试不重算一遍 `GetSystemMetrics`。
  - `SysGetIPAddresses` 部分：先断言返回非空（机器一定有回环）、每条都是测试自己手写的点分十进制校验器认可的四组 0..255、`127.0.0.1` 必须在列；再用 **`GetIpAddrTable`（IPv4 路由/地址表）**独立读一遍本机地址，两边按**集合**必须相等——服务读的是适配器表，测试读的是地址表，两个不同的 OS 数据源互为对方的观察。`MIB_IPADDRROW::dwAddr` 按 wire order 取四字节格式化，格式化逻辑在测试里单独写了一份。
- `tests/js/screen_slice.cpp`（L5）：生产接线（`rime:screen` + 真实 `ScreenService`），pixel 与 image 段都注入合成屏以拿到字面量期望，BMP 针由测试自己写。caret 段**排在最前**（它是唯一依赖桌面的读）：同样自建前台窗口 + caret，先在 native 侧断言期望点，再钉 JS 侧的 `{found:true,x,y}` 与 `Object.keys` 恰好三个键、销毁 caret 后的 `{found:false}` 且只有 `found` 一个键。sysget 段紧随其后：native 侧先钉住 `system_metric(0)` 与地址表，JS 侧再钉 `sysGet(0)` 等于那次 native 读、`sysGet(-1)` 为 `0`、`sysGet('not a number')` 同步 `TypeError`、`sysGetIPAddresses()` 与 native 地址**集合**相等且每条都过测试自己写的四组 0..255 校验、`127.0.0.1` 必须在列。段 1 用**运行时启动前先做的 native 读**作独立观察，逐字段比对 JS 返回的 count 与 primary 记录；段 2 钉住 `monitor(1)`/`monitor(count)`/`count+1 → invalid_contract`/`-1 → RangeError`/`'one' → TypeError`；段 3 钉住 `pixel(3,5)` 的字面量色、`pixelSearch` 命中 `(2,3)`、`{found:false}` 不带坐标、越界矩形 → `invalid_contract`、`1.5 → TypeError`、`9999999999 → RangeError`、`0x1000000 → RangeError`；段 4 钉住 `imageSearch` 命中 `(2,3)`、两种"针放不下/没有像素"的 miss、坏文件与越界区域的错误码、路径非字符串与 options 非对象的 `TypeError`、`variation: 300` 的 `RangeError`；随后断言 trace 为空；最后用空 capability 的第二个 runtime（同 `registry_slice` 的写法，JS lane 进程级，必须先停第一个）证明八个入口都按 `screen.capture` 拒绝。
- `tests/sdk/screen.test.ts`（L3，`bun test --isolate tests/sdk`）：真 facade + 仪表化 `rime:screen` 桥（桥是环境，不是被测单元）。回归钉点是 `pixelSearch`/`imageSearch` 必须把 `variation` 与 action options **一起**交给 bridge——`runAction` 按字段重建 bridge 参数，域名参数由门面合并，丢掉它就等于静默改掉搜索语义；`caret` 另钉 action options 转发与 miss 不带坐标；`sysGet` 钉住 `index` 原样转发与 `{metric}` 解包（含 `0` 这个必须原样透出的读数），`sysGetIPAddresses` 钉住 action options 转发与 `{addresses}` 解包；其余是解包与 `ActionError` code 映射。
- stress：本域无队列、无 Hook、无重入路径，caret 与 SysGet 族落地后也没有改变这点（前台窗口由 OS 决定、系统指标与地址表由 OS 决定，都不由本域排队），当前没有单独 stress 测试。

## 实现状态

| 部件 | 状态 | 位置 |
| --- | --- | --- |
| Native service | done | `engine/win32/include/rime/win32/screen.hpp`、`engine/win32/src/screen.cpp` |
| 纯扫描层 + 捕获 seam | done | `engine/win32/{include/rime/win32/screen_pixels.hpp,src/screen_pixels.cpp}`、`engine/win32/{include/rime/win32/screen_seam.hpp,src/screen_seam.cpp}` |
| 图像解码层 | done | `engine/win32/{include/rime/win32/image_loader.hpp,src/image_loader.cpp}`（GDI+，`rime_win32` 链 `gdiplus`） |
| `rime:screen` 模块 | done | `engine/win32/js/include/rime/win32/js_screen.hpp`、`engine/win32/js/src/screen_module.cpp` |
| SDK 门面 | done | `sdk/src/screen.ts` |
| TS 模块声明 / 导出 | done | `sdk/src/modules.d.ts`、`sdk/src/index.ts` |
| capability 台账 | done | `contracts/registry/actions.json`（`capabilities` + `readSurfaces` 八行） |
| Bootstrap 接线与生产能力 | done | `engine/win32/js/src/bootstrap.{hpp,cpp}`（`production_capabilities()` 含 `screen.capture`） |
| coverage 行 | done（Monitor 5 行 + `PixelGetColor`/`PixelSearch`/`ImageSearch`/`CaretGetPos`/`SysGet`/`SysGetIPAddresses` → `implemented`，本域 11 行全部实现） | `docs/api/coverage.json` |
| contract 测试 | done | `tests/native/screen_tests.cpp`、`tests/js/screen_slice.cpp`、`tests/sdk/screen.test.ts`、`tests/screen_image_fixture.hpp`（CMake 已登记） |
| `CaretGetPos` | done | `ScreenService::caret()`、`engine/win32/js/src/screen_module.cpp` 的 `screen_caret`、`sdk/src/screen.ts` |
| `SysGet` / `SysGetIPAddresses` | done | `ScreenService::system_metric()`/`ip_addresses()`、`screen_module.cpp` 的 `screen_sys_get`/`screen_sys_get_ip_addresses`、`sdk/src/screen.ts`（`rime_win32` 链 `iphlpapi`） |
