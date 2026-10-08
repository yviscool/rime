# GUI, Menu, Tray and Dialog API

状态：`SoundGetVolume` / `SoundSetVolume` / `SoundGetMute` / `SoundSetMute` / `SoundGetName` 已实现（实现归属 `@rime/sound`，见 [`sound.md`](./sound.md)），`SoundGetInterface` 为 `unsupported-by-policy`；本批 `MsgBox` / `InputBox` / `ToolTip` / `TraySetIcon` / `TrayTip` 已实现（实现归属 `@rime/ui`，capability `ui.create`，native 测试 `tests/native/gui_tests.cpp` + JS 切片 `tests/js/ui_slice.cpp` + 生产授予 `tests/js/fixtures/ui-grant.mjs`）；其余 8 项（`GuiCtrlFromHwnd` / `GuiFromHwnd` / `MenuSelect` / `MenuFromHandle` / `LoadPicture` / `IL_Create` / `IL_Add` / `IL_Destroy`）为 `contract-only`：目标模块 `@rime/ui`、计划阶段 M6（计划 §M6「GUI / Menu / 对话框」）已定，实现未落。

源码证据：`functions.h` 的 `Gui*`、`Menu*`、`Tray*`、`ToolTip`、`MsgBox`、`InputBox`、`Sound*`、`LoadPicture`、`IL_*`；`source/script_gui.cpp`、`source/script_menu.cpp`、`source/lib/sound.cpp`。

GUI、菜单、托盘和模态对话框对象必须由 UI Thread 所有，以稳定 ID 和事件订阅暴露。模态对话框不能启动第二个脚本消息泵；结果通过同一个 JS 调度器返回。图片和 ImageList 句柄不得直接暴露。

## 1. 六个核心设计问题（`AHK-TS-WINDOWS-API-DESIGN.md` §0）逐条答案（M6 定档前的设计承诺）

1. **TS 如何表达** —— 对象模型，不是全局单例：`Gui`（55 成员，含 28 种 `Add*` 构造器）/ `GuiControl`（43 成员）/ `Menu`·`MenuBar`（17 成员）是持有稳定 `GuiId`/`ControlId` 的对象，事件走 `Subscription`（回调进 Runtime task，同一 JS 调度器）；对话框 `MsgBox`/`InputBox` 是返回 Promise 的异步调用，不是阻塞语句；托盘与 `ToolTip` 是同域的变更动词。具体命名与参数形状在 M6 设计文档落定，本表只承诺这些形状约束，不承诺 AHK 同名函数。
2. **底层怎么实现，走哪条 lane** —— `lane: ui`，Win32 common controls 由 UI Thread 所有（AGENTS：HWND/窗口对象只在 UI Thread 使用），不跨线程传句柄、不在模块内自起消息泵；Win32 Message Pump 是唯一事件入口。`GuiCtrlFromHwnd`/`GuiFromHwnd`/`MenuFromHandle` 这三个「句柄反查」API 在本模型里没有对应的裸句柄可返回——反查结果是稳定 id（与窗口/控件同一 id 空间），不是 `HWND`/`HMENU`。
3. **哪些 API 异步** —— 台账 13 项全部 `async: async`：对话框等用户输入（可阻塞任意久）、托盘与 `ToolTip` 改的是 UI Thread 状态、`LoadPicture`/`IL_*` 可能做磁盘与解码 IO、`MenuSelect` 要等目标项可见。按 stdlib.md §4，凡可能 >1ms、可能等用户或等他进程的一律异步并带 `deadlineMs`/`AbortSignal`；同步面只保留纯查询与已取得快照的字段读。
4. **什么不暴露给上层** —— 裸 `HWND`/`HMENU`/`HICON`/`HIMAGELIST`（`LoadPicture` 返回的位图句柄、`IL_*` 的 ImageList 句柄、`MenuFromHandle` 的菜单句柄、`GuiFromHwnd` 的窗口句柄一律改为稳定 id 或不提供）；模态对话框的同步阻塞形态与第二套脚本消息泵；全局 `A_Icon*`/`Tray` 隐式可变态；`Gui` 的事件标签与隐式全局回调。
5. **若重造 AHK 的 TS 标准库，这一族在哪** —— AHK 的 GUI 是「隐式单例 + 事件标签 + 全局变量回传结果」；TS 侧复刻其能力而非其形态：对象 + `Subscription` + 显式 dispose，对象生命周期遵守 GC / 关闭 / shutdown 三路径且无泄漏（M6 验收含队列满载、嵌套泵、取消竞态、重入）。`MsgBox`/`InputBox` 的「等用户」在 TS 里只能是 `await`。
6. **若 TS 是 Windows automation language，Windows 怎么建模** —— GUI 是**本进程拥有的窗口树**（`GuiId`/`ControlId` 与窗口 id 同一 id 空间，`control.md` §1），托盘与菜单是 shell 资源，同样以稳定 id 引用；它们与「别人的窗口」（`windows.*` 查询/变更族）的区别是所有权与权限，不是 API 形状。句柄是实现细节，不进模型。

## 2. M6 第一批（已实现五项）的实现状态

- **入口**：`rime:ui` 模块导出 `ui` 命名空间（`ui.msgBox` / `ui.inputBox` / `ui.toolTip` / `ui.traySetIcon` / `ui.trayTip`），SDK 侧 `sdk/src/ui.ts`（`@rime/ui`）。五项全部 `async`、返回 Promise，无一构建 Action（capability 读取即审计面，Action Trace 保持为空），capability `ui.create` 在 worker body 内读取（`engine/win32/js/src/ui_module.cpp`），授予名单 `production_capabilities()`。
- **线程与所有权**：对话框、`TOOLTIPS_CLASS` 窗口与托盘图标全部由 UiThread（窗口服务的唯一消息泵，不启动第二泵）所有；`GuiService` 公开方法经 `UiThread::call` 编组，模态对话框以 `DialogBoxIndirectParamW` + 内存容器模板 + `WM_TIMER` 在泵上运行。
- **取消与超时**：`deadlineMs`/`signal` 只约束排队阶段与窗口出现前的阶段；**已打开的模态不被外部取消打断**，由自身的 AHK T 选项（`timeout` 秒）关闭并返回 `"Timeout"`——与 AHK 的阻塞语义一一对应，区别只是阻塞变成了 `await`。
- **不暴露**：裸 `HWND`（`toolTip` 不返回句柄）、第二套脚本消息泵、`A_Icon*` 隐式可变态；`freeze` 的"是否给出"以属性存在性区分（`freeze: false` 是显式解冻）。
- **平台注记**：内嵌 Runtime 的进程（`rime_host`、验证切片 `rime_gui_object_slice` 与 `rime_ui_slice`）携带 ComCtl32 v6 manifest（`engine/win32/resources/visual-styles.manifest`，资源 id 1）——`EM_SETCUEBANNER`（`GuiControl.SetCue`）按 MSDN 只在声明了 v6 的进程里被接受，没有它 Windows 直接拒绝、cue 静默丢失；未携带 manifest 的进程仍加载 v5.82，`TOOLINFO.cbSize` 因此固定给 v2 尺寸（64 字节，`gui.cpp` `TTM_ADDTOOL`）——v5.82 拒绝现代 SDK 的 72 字节，而 v6 接受更小的结构版本，两个加载形态都成立。
- **测试分级**：`gui_tests.cpp`（L5，服务层：按钮词、X/ESC、点击、InputBox 往返、tooltip 枚举、托盘 shell 探针、stop 可重复）；`ui_slice.cpp`（L5，JS 承诺管线： watcher 证明对话框真的出现、Timeout 词与 `value` 回传、tooltip/tray 的 OS 独立观测、五项 capability 拒绝、Trace 为空）；`ui.test.ts`（L3，SDK 门面的选项拆分）；`ui-grant.mjs`（L6，生产装配授予 `ui.create`）。

## 3. 测试承诺（M6 落地时补齐）

8 项 `contract-only` 现在 `contractTest: "missing"` 是过渡态（过渡态不计终态，`matrix:check` 只把终态行的 `missing` 判为证据缺口）；已实现 5 项的 `contractTest` 指向 `tests/native/gui_tests.cpp,tests/js/ui_slice.cpp`。M6 完成定义（计划 §M6 DoD）要求剩余条目转终态并填 `contractTest`/`compatibilityTest` 路径，验收含队列满载/嵌套泵/取消竞态/重入与对象三路径无泄漏（AGENTS：UI、Hook、Timer、COM 回调与 JS 引用可取消、可观察），领域文档「实现状态」同步。在此之前，未实现项**没有任何**「已实现」的暗示——实现状态以 `coverage.json` 为准。

## 4. M6 批2（Gui/GuiControl 对象族）设计与交付形态

批2 范围：`Gui` 35 行中 **33 行实现**（`MenuBar` 因依赖 Menu 留批3；`Hwnd`/`FontHandle` 见 §4.7 政策翻转）、`GuiControl` 21 行中 **20 行实现**（`Hwnd` 政策翻转）、另 3 行翻 `unsupported-by-policy`（`Gui.Hwnd`、`Gui.FontHandle`、`GuiControl.Hwnd`——raw `HWND`/`HFONT` 暴露违反 §0.4 与 TS 边界）。批2 实现 `Add`/`AddButton`/`AddCheckBox`/`AddEdit`/`AddGroupBox`/`AddPicture`/`AddProgress`/`AddRadio`/`AddText` 九个构造器（Button/Edit 族）；其余 `Add*` 19 个、`GuiControl` 控件族成员（ListView/TreeView/StatusBar/ListBox/ComboBox 专属方法）、`Menu` 17 成员与 8 项 coverage 尾巴留批3，**在批2 落地后仍为 `contract-only`，没有任何"已实现"的暗示**。

### 4.1 对象形态与线程

- `new Gui(options?, title?, eventObj?)` **同步**返回原生对象（`InputHook` 先例：构造器返回 native object cast，`sdk/src/input/hooks.ts:46`）。`__New` 只做参数校验（options/title 类型、eventObj 非对象抛 TypeError——`objects.json` 错误栏）与 JS 侧状态分配，**不跨线程**：AHK 在 `__New` 内 `Create(title)`（`script_gui.cpp:383`）建 HWND，本实现把 HWND **推迟到第一个异步成员调用**在泵上隐式物化（隐藏创建、未 Show 前不可见）——契约可观察差异：`new Gui()` 本身永不阻塞，之后任何 `await g.X` 的行为与"窗口已存在"一致。`objects.json` 的 `async: "async"` 注记 AHK 源阻塞（`docs/api/clipboard.md:33` 先例）；本节逐成员写明 JS 交付形态。
- **JS 侧状态**（仅 JS Thread 触碰，无锁）：`GuiJsState` 条目 = `{id, title, opts, name, margins, backColor, eventObj, channel(回调 id), events(按目标+事件名的回调表), controls(有序 GuiControl 列表), destroyed}`；`GuiControlJsState` = `{id, guiId, type, vName, text/value 镜像, destroyed}`。状态持有事件回调 JSValue 与控件对象 JSValue——它们由 Gui 生命周期闭合（Destroy/停止/GC 三路径释放，见 §4.5），不是 GC 不可见的泄漏源。
- **泵侧记录**（仅 UI Thread 触碰）：`GuiRecord{hwnd, childMap(HWND→ctrlId), channel, hasCloseHandler, registeredClass...}`，按稳定 `GuiId` 存于 `GuiService::Impl`。**每个异步服务调用携带 `GuiSpec{title, styleFlags, channel}` 快照**：泵侧 `ensure_record` 缺记录时按快照建窗（幂等创建，消除跨 worker 的先序竞态）；控件 HWND 在 `Add` 的泵调用内创建，SDK 保证 `await g.Add(...)` 结束后才存在控件对象，故控件操作天然后序。
- 所有 HWND 操作经 `UiThread::call`（唯一泵），从 worker lane 进入（`start_async`）；**无第二脚本消息泵**。

### 4.2 交付形态表（async 注记 = AHK 源；JS 形态以此为准）

| 形态 | 成员 | 说明 |
|---|---|---|
| 同步构造 | `__New` | 见 §4.1 |
| `Promise` 方法 | `Add`(→`Promise<GuiControl>`), `Destroy`, `Flash`, `GetPos`/`GetClientPos`(→`Promise<{x,y,width,height}>`，AHK `&x` ByRef 出参改返回对象), `Hide`, `Maximize`, `Minimize`, `Move`, `OnEvent`, `OnMessage`, `Opt`, `Restore`, `SetFont`, `Show`, `Submit`(→`Promise<Record<string,unknown>>`), `GuiControl` 全部方法 | worker lane + 泵；`Add` 的参数校验（控件类型名、options 解析）在 JS 线程同步完成，同步抛 `ValueError`/`TypeError`，泵只执行已验证的创建 |
| `Promise` 读属性 | `g.Title`/`g.BackColor`/`g.MarginX`/`g.MarginY`/`g.FocusedCtrl`, `ctrl.Enabled`/`ctrl.Visible`/`ctrl.Text`/`ctrl.Value`/`ctrl.Focused` | 读=泵上实读（真值在 HWND）；HWND 未物化前 `Title`/`BackColor`/`MarginX/Y` 返回 JS 侧镜像（AHK 同为脚本可写状态） |
| 同步读写属性 | `g.Name`（纯脚本侧身份）、`ctrl.Name`/`ctrl.Type`/`ctrl.ClassNN`/`ctrl.Gui`（JS 侧可算：ClassNN=类型名+序号，`script_gui.cpp:GuiControlType::get_ClassNN` 同为查表） | 无 OS 状态，`InputHook` 同步属性先例 |
| set 属性 | `g.Title`/`BackColor`/`MarginX/Y`/`Name`、`ctrl.Text`/`Value`/`Enabled`/`Visible`/`Name` | **setter 同步校验 + 立即抛错**（类型 TypeError、`destroyed` 抛 `InvalidState`——`objects.json` 错误栏即同步可观察项），**窗口写入为 fire-and-forget Promise**，拒绝走 host `record()` 错误观察者（`window.ts:225` minimizeAll fire-and-forget 先例），读回（同名 getter `await`）可观察写入结果。`Value` 类型不符（如给 Button 赋非字符串）同步 `ValueError`（`script_gui.cpp:GuiControlType::put_Value` 校验同位） |
| 同步查找 | `g.__Item`（`g["name"]` 按 vName 查、数字键 = 0-based 位置，未命中读 `undefined`——现代语义，无 `UnsetItemError`）、`g.__Enum`（`Symbol.iterator` 依序产出控件，JS 侧列表） | 数字键语义偏离 AHK（AHK 把数字当 HWND 查，`script_gui.cpp:433-437`；裸句柄政策拒绝该入口，数字键改位置索引并在此声明） |

### 4.3 事件管道（GUI 回调进 Subscription）

- **通道**：`__New` 在 JS 线程 `host->add_callback(dispatch, channel)` 注册**每 Gui 一条派发通道**；泵侧 `GuiRecord.channel` 由后续异步调用携带的 `GuiSpec` 幂等镜像。`WndProc` 只做 `event_queue()->push(channel, json)`（任意线程生产、JS 线程 drain 派发——`host.hpp:31-41` 与 `events_module.cpp` 的 channel 先例）。
- **JS 侧回调表**：`OnEvent(name, fn, addRemove?)` 同步段校验事件名（批2 集合见下）与 `addRemove ∈ {-1,0,1}`（越界 `ValueError`，`script_gui.cpp:2541-2543` 同判）并登记 `{fn|eventName字符串, addRemove}`；异步段把"该目标存在处理器"镜像到泵侧（`Close` 默认行为需要，见下）并返回 Promise。`addRemove=0` 移除首个匹配实例（`script_gui.cpp:2561-2568`），负值"call it last"、正值/缺省先调（`script_gui.cpp:2579-2581` 注记，按 AHK 文档 1=first/-1=last 对齐并测试锁定）。
- **批2 事件集合**（`ValueError` 于集合外，集合在本节封口即契约）：Gui 级 `Close`（WM_CLOSE/X，无处理器时窗口销毁——`script_gui.cpp` Close 默认即销毁；有处理器则由处理器决定）、`Resize`（WM_SIZE，参数 `(gui, minMax, width, height)`）；控件级 `Click`（WM_COMMAND/BN_CLICKED 与 SS_NOTIFY，`(ctrl, info)`，`script_gui.cpp` Click param_count=2）、`Change`（EN_CHANGE，`(ctrl, info)`）。`OnMessage(number, cb)` 透传任意已注册消息 `(wParam, lParam, msg)`——**不含 hwnd**（§0.4 政策偏差：AHK 第 4 参是句柄）；`OnCommand(notifyCode, cb)`/`OnNotify(notifyCode, cb)` 按码路由 `(ctrl, code, ...)`。投递走单一 FIFO 队列串行（AHK `max_instances` 线程上限由队列串行替代——可观察差异记于本节）。
- **Subscription 归属**：每 Gui 的通道回调以 `allocate_subscription_id()` 登记进 `subscriptions()`（CLI/调试器可查「模块/订阅/当前任务」——AGENTS 可检查接口），`Destroy` 与运行时停止时释放；`eventObj` 上的字符串回调名在派发时解析为 `eventObj[name]`（`__New` 第 3 参，`script_gui.cpp:346-351`）。
- **派发载荷**：`{gui:<id>, target:'gui'|<ctrlId>, event:'Close'|..., args:[...]}`；JS 侧派发闭包按表调用 `fn(targetObj, ...args)`，`targetObj` 为 JS 侧持有的 Gui/GuiControl 对象。异常进 host `record()`（错误观察者），不中断后续事件。

### 4.4 Options 语言（批2 子集，集合外抛 `ValueError`）

- **Gui options**（`__New`/`Opt`，`script_gui.cpp:4974-5350` 的子集）：`+Resize +AlwaysOnTop +ToolWindow +MinimizeBox +MaximizeBox +Caption -Caption +Border +OwnDialogs +LastFound?`（批2 不含 `Label=`/`+HwndID` 等）；未识别字母**抛 `ValueError` 而非静默忽略**。
- **Show options**（`script_gui.cpp:7196+`）：`w<n> h<n> x<n> y<n> Center AutoSize Hide Min Max NoActivate`。
- **控件 options**（`ControlParseOptions` 子集，`script_gui.cpp:5354+`）：`x y w h`（绝对坐标）、`v<Name>`（Submit 键名）、`+Wrap`?（不做）、缺省坐标走默认流式布局（首控件落在 MarginX/Y，其后按 AHK 默认定位——实现时以 `mPrevX/mPrevY` 源为准）。`g<Label>` 标签回调**批2 不支持**（抛 `ValueError`，用 `OnEvent` 替代——§0.4 已弃事件标签）。
- **SetFont(options?, fontName?)**：`s<n>` 字号 + 可选字体名（`script_gui.cpp:8132+` 子集）。

### 4.5 生命周期三路径（M6 验收）

1. **关闭路径** `await g.Destroy()`：同步段置 `destroyed`、释放 JS 侧回调表与控件表（`remove_callback(channel)` + `subscriptions()` 注销，JS 线程）；异步段泵上销毁 HWND/子窗口。之后任何成员同步或异步抛 `InvalidState`。外部销毁（X 关闭/服务侧关闭）走同一幂等释放：`WM_DESTROY` 推送的 `__closed` 在派发时释放 JS 侧（不进脚本回调），之后成员同步抛 `InvalidState`，unload 门禁不再看见该 Gui。
2. **GC 路径**：`new Gui()`（`ui.createGui` → `build_gui_object`）同步经 `FinalizationRegistry`（quickjs-ng 内建，每 `GuiJsState` **一个**实例，由 `ensure_registry` 懒建）以 `{id}` 为 token 注册；对象不可达时清理回调触发 `uiGuiGC(id)`，该回调是 host job 排到 JS 线程执行，**不内联进 GC**（`gui_object.cpp` `ui_gui_gc` 注释）。弱引用未命中而 `gui_object_for` 重建对象壳时对新壳重新注册（旧目标已不可达，registry 侧不持有它）。显式 `Destroy` 与 GC、shutdown 都汇入 `release_gui`——入口 `if (gui.destroyed) return` 使其幂等，其中仅在对象仍可达时 `unregister`（GC 路径上 deref 已空、无对象可注销）。
3. **shutdown 路径**：`GuiService::stop()` 在泵上销毁全部残留 Gui 窗口（批1 stop 同形），JS 侧状态在宿主关闭序列释放通道；**存在未 `Destroy` 且未被外部销毁的 Gui 时 unload 门禁因残留回调失败**（AGENTS「仍有订阅/回调」失败路径），测试覆盖该失败与成功关闭两条。`stop()` 在有模态对话框打开时直接拒绝并点名（`ExecutionFailed: cannot stop: modal dialog '<title>' is open`），不排队等泵——开着无超时对话框时关不掉是显式失败，不是静默 Timeout。
4. **验收电池**（计划 §M6 DoD）：队列满载（事件通道压满不丢不重、按 FIFO）、嵌套泵（Show 中派发 Close 不重入）、取消竞态（`deadlineMs`/`signal` 只约束排队与物化前阶段，已建窗口操作不受取消打断）、重入（同 Gui 连续 OnEvent/Destroy 竞态）、三路径无泄漏（`callback_count`/`subscription_count` 归零、泵侧记录清空）。

### 4.6 控件族（批2 九类）与内容

`Add(type, options?, content?)` 的 `type` 集合 = `Button/CheckBox/Edit/GroupBox/Picture/Progress/Radio/Text`（大小写不敏感，AHK `ConvertTypeName` 同位——`script_gui.cpp:298-305`；集合外 `ValueError`）；`AddXxx` 糖等价 `Add("Xxx",...)`。Win32 类名：BUTTON（push/checkbox/radio/groupbox 用 style 区分）、STATIC（text/picture）、EDIT、`msctls_progress32`（common controls 版本见批1 平台注记：带 manifest 的进程是 v6，否则 v5.82）。内容：字符串（文本类）、路径（Picture → `LoadImageW` 私有拷贝，句柄不出泵）、数字（Progress → `PBM_SETPOS`）；`TYPE_HAS_ITEMS` 类（List/DDL/ComboBox）不在批2，内容收到数组时 `TypeError`。`Edit.SetCue(text)` 发 `EM_SETCUEBANNER`，前提就是批1 平台注记里的 ComCtl32 v6 manifest；`tests/js/gui_object_slice.cpp` 用同一份 manifest 在真实 Edit 控件上回读 `EM_GETCUEBANNER` 背书，而不是只看调用没抛。

### 4.7 政策翻转（3 行 → `unsupported-by-policy`）

`Gui.Hwnd`、`Gui.FontHandle`、`GuiControl.Hwnd` 的 `error` 栏已声明 `unsupported-by-policy (raw HWND/HBRUSH exposure)`（§0.4「图片和 ImageList 句柄不得直接暴露」同族）；批2 将其 `status` 翻为终态并保留拒绝理由与替代（稳定 id 模型：`GuiFromHwnd`/`GuiCtrlFromHwnd` 反查在批3 以 id 落地），测试经 `policy-refusal` 同名入口拒绝路径背书。

### 4.8 明确不做（批3+ 前提）

`MenuBar`、`Add*` 剩 16 个待实现构造器、控件族方法（LV 11/TV 12/SB 3/List 增删查）、`Menu` 17 成员、8 项 coverage 尾、事件集扩张（`Escape`/`ContextMenu`/`DropFiles`/`Focus`/`DblClk`/`ItemSelect`…）、g-label 与事件标签、DPI 重缩放（`mDefaultDPIResize`）。未实现项在 `objects.json`/`coverage.json` 维持 `contract-only`，`ValueError` 拒绝集合外事件名即是"未实现"的显式信号。

### 4.9 批3 政策 verdict（3 行 → `unsupported-by-policy`，文档先行零代码）

理由独立于 AHK，引用 capability 模型、安全边界或 OS 语义三处之一：

- `AddActiveX`：进程内嵌任意 COM 服务器（IE 内核级攻击面）；capability 模型无法限定“哪一个 COM 服务”，替代路径是带清单的进程外插件。
- `AddTab3`：唯一语义是按控件退订视觉样式；v6 manifest 下 comctl32 不提供该路径，无现代用例。
- `AddCustom`：注册自定义窗口类 = 脚本代码进入窗口过程，违反重入与生命周期铁律；未来如需，走声明式控件描述（新提案，批5 可重议）。

三行 `error` 栏已写清理由与替代，`compatibilityTest` 保持 `missing`（未实现的方法无拒绝入口可测；背书 = 台账状态 + `checkObjectsLedger`，沿 §4.7 的 16 条先例）。DoD 模板见 `m6-batch3-dod.md`。
