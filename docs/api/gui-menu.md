# GUI, Menu, Tray and Dialog API

状态：`SoundGetVolume` / `SoundSetVolume` / `SoundGetMute` / `SoundSetMute` / `SoundGetName` 已实现（实现归属 `@rime/sound`，见 [`sound.md`](./sound.md)），`SoundGetInterface` 为 `unsupported-by-policy`；本批 `MsgBox` / `InputBox` / `ToolTip` / `TraySetIcon` / `TrayTip` 已实现（实现归属 `@rime/ui`，capability `ui.create`，native 测试 `tests/native/gui_tests.cpp` + JS 切片 `tests/js/ui_slice.cpp` + 生产授予 `tests/js/fixtures/ui-grant.mjs`）；其余 8 项（`GuiCtrlFromHwnd` / `GuiFromHwnd` / `MenuSelect` / `MenuFromHandle` / `LoadPicture` / `IL_Create` / `IL_Add` / `IL_Destroy`）为 `contract-only`：目标模块 `@rime/ui`、计划阶段 M6（计划 §M6「GUI / Menu / 对话框」）已定，实现未落。

源码证据：`functions.h` 的 `Gui*`、`Menu*`、`Tray*`、`ToolTip`、`MsgBox`、`InputBox`、`Sound*`、`LoadPicture`、`IL_*`；`source/script_gui.cpp`、`source/script_menu.cpp`、`source/lib/sound.cpp`。

GUI、菜单、托盘和模态对话框对象必须由 UI Thread 所有，以稳定 ID 和事件订阅暴露。模态对话框不能启动第二个脚本消息泵；结果通过同一个 JS 调度器返回。图片和 ImageList 句柄不得直接暴露。

## 1. 六个核心设计问题（`AHK-TS-WINDOWS-API-DESIGN.md` §0）逐条答案（M6 定档前的设计承诺）

1. **TS 如何表达** —— 对象模型，不是全局单例：`Gui`（55 成员，含 28 种 `Add*` 构造器）/ `GuiControl`（42 成员）/ `Menu`·`MenuBar`（17 成员）是持有稳定 `GuiId`/`ControlId` 的对象，事件走 `Subscription`（回调进 Runtime task，同一 JS 调度器）；对话框 `MsgBox`/`InputBox` 是返回 Promise 的异步调用，不是阻塞语句；托盘与 `ToolTip` 是同域的变更动词。具体命名与参数形状在 M6 设计文档落定，本表只承诺这些形状约束，不承诺 AHK 同名函数。
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
- **平台注记**：宿主没有 comctl32 v6 manifest，进程加载 v5.82；`TOOLINFO.cbSize` 必须给 v2 尺寸（64 字节），现代 SDK 的 `sizeof(TOOLINFO)`（72 字节）会被 `TTM_ADDTOOL` 拒绝。
- **测试分级**：`gui_tests.cpp`（L5，服务层：按钮词、X/ESC、点击、InputBox 往返、tooltip 枚举、托盘 shell 探针、stop 可重复）；`ui_slice.cpp`（L5，JS 承诺管线： watcher 证明对话框真的出现、Timeout 词与 `value` 回传、tooltip/tray 的 OS 独立观测、五项 capability 拒绝、Trace 为空）；`ui.test.ts`（L3，SDK 门面的选项拆分）；`ui-grant.mjs`（L6，生产装配授予 `ui.create`）。

## 3. 测试承诺（M6 落地时补齐）

8 项 `contract-only` 现在 `contractTest: "missing"` 是过渡态（过渡态不计终态，`matrix:check` 只把终态行的 `missing` 判为证据缺口）；已实现 5 项的 `contractTest` 指向 `tests/native/gui_tests.cpp,tests/js/ui_slice.cpp`。M6 完成定义（计划 §M6 DoD）要求剩余条目转终态并填 `contractTest`/`compatibilityTest` 路径，验收含队列满载/嵌套泵/取消竞态/重入与对象三路径无泄漏（AGENTS：UI、Hook、Timer、COM 回调与 JS 引用可取消、可观察），领域文档「实现状态」同步。在此之前，未实现项**没有任何**「已实现」的暗示——实现状态以 `coverage.json` 为准。
