# Hotkey, Hook and Event API

状态：`contract-only`；全局输入 Hook、事件订阅与 chord 绑定（`input.bind`/`input.unbind`，含自注入抑制）已实现，contract 见 `tests/js/input_slice.cpp`。`Hotkey`/`Hotstring` 声明式函数、`OnMessage`、`OnExit`、`OnClipboardChange` 尚未实现。

源码证据：`functions.h` 的 `Hotkey`、`Hotstring`、`InstallKeybdHook`、`InstallMouseHook`、`SetTimer`、`OnMessage`、`OnExit`、`OnError`、`OnClipboardChange`；`source/hotkey.cpp`、`source/hotkey.h`、`source/hook.cpp`、`source/input_object.cpp`。

每个 Hook、Timer、消息回调的 JS function reference 都是可取消的 `Subscription`：回调先进受监管的 Win32 message pump，再排队到 JS Thread；Hook 线程不直接调用 QuickJS。关闭时拒绝新事件、取消并等待回调，直到回调计数归零才完成卸载。
