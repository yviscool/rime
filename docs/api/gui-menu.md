# GUI, Menu, Tray and Dialog API

状态：`SoundGetVolume` / `SoundSetVolume` / `SoundGetMute` / `SoundSetMute` / `SoundGetName` 已实现（实现归属 `@rime/sound`，见 [`sound.md`](./sound.md)），`SoundGetInterface` 为 `unsupported-by-policy`；本域其余 13 项（`GuiCtrlFromHwnd` / `GuiFromHwnd` / `MsgBox` / `InputBox` / `ToolTip` / `TraySetIcon` / `TrayTip` / `MenuSelect` / `MenuFromHandle` / `LoadPicture` / `IL_Create` / `IL_Add` / `IL_Destroy`）为 `contract-only`：目标模块 `@rime/gui-menu`、计划阶段 M6（计划 §M6「GUI / Menu / 对话框」）已定，实现未落。

源码证据：`functions.h` 的 `Gui*`、`Menu*`、`Tray*`、`ToolTip`、`MsgBox`、`InputBox`、`Sound*`、`LoadPicture`、`IL_*`；`source/script_gui.cpp`、`source/script_menu.cpp`、`source/lib/sound.cpp`。

GUI、菜单、托盘和模态对话框对象必须由 UI Thread 所有，以稳定 ID 和事件订阅暴露。模态对话框不能启动第二个脚本消息泵；结果通过同一个 JS 调度器返回。图片和 ImageList 句柄不得直接暴露。

## 1. 六个核心设计问题（`AHK-TS-WINDOWS-API-DESIGN.md` §0）逐条答案（M6 定档前的设计承诺）

1. **TS 如何表达** —— 对象模型，不是全局单例：`Gui`（55 成员，含 28 种 `Add*` 构造器）/ `GuiControl`（42 成员）/ `Menu`·`MenuBar`（17 成员）是持有稳定 `GuiId`/`ControlId` 的对象，事件走 `Subscription`（回调进 Runtime task，同一 JS 调度器）；对话框 `MsgBox`/`InputBox` 是返回 Promise 的异步调用，不是阻塞语句；托盘与 `ToolTip` 是同域的变更动词。具体命名与参数形状在 M6 设计文档落定，本表只承诺这些形状约束，不承诺 AHK 同名函数。
2. **底层怎么实现，走哪条 lane** —— `lane: ui`，Win32 common controls 由 UI Thread 所有（AGENTS：HWND/窗口对象只在 UI Thread 使用），不跨线程传句柄、不在模块内自起消息泵；Win32 Message Pump 是唯一事件入口。`GuiCtrlFromHwnd`/`GuiFromHwnd`/`MenuFromHandle` 这三个「句柄反查」API 在本模型里没有对应的裸句柄可返回——反查结果是稳定 id（与窗口/控件同一 id 空间），不是 `HWND`/`HMENU`。
3. **哪些 API 异步** —— 台账 13 项全部 `async: async`：对话框等用户输入（可阻塞任意久）、托盘与 `ToolTip` 改的是 UI Thread 状态、`LoadPicture`/`IL_*` 可能做磁盘与解码 IO、`MenuSelect` 要等目标项可见。按 stdlib.md §4，凡可能 >1ms、可能等用户或等他进程的一律异步并带 `deadlineMs`/`AbortSignal`；同步面只保留纯查询与已取得快照的字段读。
4. **什么不暴露给上层** —— 裸 `HWND`/`HMENU`/`HICON`/`HIMAGELIST`（`LoadPicture` 返回的位图句柄、`IL_*` 的 ImageList 句柄、`MenuFromHandle` 的菜单句柄、`GuiFromHwnd` 的窗口句柄一律改为稳定 id 或不提供）；模态对话框的同步阻塞形态与第二套脚本消息泵；全局 `A_Icon*`/`Tray` 隐式可变态；`Gui` 的事件标签与隐式全局回调。
5. **若重造 AHK 的 TS 标准库，这一族在哪** —— AHK 的 GUI 是「隐式单例 + 事件标签 + 全局变量回传结果」；TS 侧复刻其能力而非其形态：对象 + `Subscription` + 显式 dispose，对象生命周期遵守 GC / 关闭 / shutdown 三路径且无泄漏（M6 验收含队列满载、嵌套泵、取消竞态、重入）。`MsgBox`/`InputBox` 的「等用户」在 TS 里只能是 `await`。
6. **若 TS 是 Windows automation language，Windows 怎么建模** —— GUI 是**本进程拥有的窗口树**（`GuiId`/`ControlId` 与窗口 id 同一 id 空间，`control.md` §1），托盘与菜单是 shell 资源，同样以稳定 id 引用；它们与「别人的窗口」（`windows.*` 查询/变更族）的区别是所有权与权限，不是 API 形状。句柄是实现细节，不进模型。

## 2. 测试承诺（M6 落地时补齐）

13 项现在 `contractTest: "missing"` 是 `contract-only` 的正常状态（过渡态不计终态，`matrix:check` 只把终态行的 `missing` 判为证据缺口）。M6 完成定义（计划 §M6 DoD）要求：对应条目转终态并填 `contractTest`/`compatibilityTest` 路径，验收含队列满载/嵌套泵/取消竞态/重入与对象三路径无泄漏（AGENTS：UI、Hook、Timer、COM 回调与 JS 引用可取消、可观察），领域文档「实现状态」同步。在此之前，本域**没有任何**「已实现」的暗示——实现状态以 `coverage.json` 为准。
