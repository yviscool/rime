# Hotkey, Hook and Event API

状态：`specified`，尚未实现。

源码证据：`functions.h` 的 `Hotkey`、`Hotstring`、`InstallKeybdHook`、`InstallMouseHook`、`SetTimer`、`OnMessage`、`OnExit`、`OnError`、`OnClipboardChange`；`source/hotkey.cpp`、`source/hotkey.h`、`source/hook.cpp`、`source/input_object.cpp`。

每个 Hook、Timer、消息回调和 JS function reference 都是可取消 `Subscription`。回调先进入受监管的 Win32 message pump，再排队到 JS Thread；Hook 线程不得调用 QuickJS。关闭时拒绝新事件、取消并等待回调计数归零。
