# Keyboard and Mouse API

状态：`specified`；当前只有输入订阅基础设施，注入尚未实现。

源码证据：`functions.h` 的 `Send*`、`Mouse*`、`KeyWait`、`GetKey*`、`Set*KeyState`、`BlockInput`；`source/keyboard_mouse.cpp`、`source/keyboard_mouse.h`、`source/input_object.cpp`、`source/hook.cpp`。

TS 使用 `KeySequence`、`MouseButton`、`Point`，保留 AHK `{Blind}`、`{Text}`、左右修饰键和 down/up 语义。SendInput 批次是 UI/input lane 的临界区；取消只在输入事件边界生效，不能把半个按键序列报告为成功。全局输入注入和 Hook 分别需要 capability。
