# Window API

状态：`specified`；`list/active/info/move/focus` 有 Native 基础实现，尚未完成 TS binding 和完整 contract。

源码证据：`rime-research/AutoHotkey-alpha/source/lib/functions.h` 的 `Win*`、`WinGroup*`；`source/window.cpp`、`source/window.h`；Rime 的 `engine/win32/src/window.cpp` 和 `engine/win32/src/ui_thread.cpp`。

核心类型使用稳定 `WindowId`、不可变 `WindowSnapshot` 和 `WindowQuery`。JS 不接收 `HWND`。查询、快照和等待走 Promise；纯字段访问同步。写操作进入 UI lane，生成 Action 并记录 Trace。窗口消失时返回 `InvalidState`，ID 不复用。

必须逐项覆盖：`WinActivate`、`WinActivateBottom`、`WinClose`、`WinGetAlwaysOnTop`、`WinGetClass`、`WinGetClientPos`、`WinGetControls`、`WinGetControlsHwnd`、`WinGetCount`、`WinGetEnabled`、`WinGetExStyle`、`WinGetID`、`WinGetIDLast`、`WinGetList`、`WinGetMinMax`、`WinGetPID`、`WinGetPos`、`WinGetProcessName`、`WinGetProcessPath`、`WinGetStyle`、`WinGetText`、`WinGetTitle`、`WinGetTransColor`、`WinGetTransparent`、`WinHide`、`WinKill`、`WinMaximize`、`WinMinimize`、`WinMinimizeAll`、`WinMinimizeAllUndo`、`WinMove`、`WinMoveBottom`、`WinMoveTop`、`WinRedraw`、`WinRestore`、`WinSetAlwaysOnTop`、`WinSetEnabled`、`WinSetExStyle`、`WinSetRegion`、`WinSetStyle`、`WinSetTitle`、`WinSetTransColor`、`WinSetTransparent`、`WinShow`、`WinWait`、`WinWaitActive`、`WinWaitClose`、`WinWaitNotActive`。

验收必须覆盖 WinTitle/A/`ahk_id`/`ahk_exe` 解析、隐藏窗口、前台激活拒绝、窗口销毁竞态、超时取消、队列关闭和稳定 ID。
