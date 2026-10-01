# Clipboard API

状态：`contract-only`；剪贴板文本读写已实现（`clipboard.read`/`clipboard.write`，contract 见 `tests/js/breadth_slice.cpp`）。`ClipWait`、`ClipboardAll` 二进制快照与变化订阅尚未实现。

源码证据：`functions.h` 的 `ClipWait`、`OnClipboardChange`；`source/clipboard.cpp`、`source/clipboard.h`。

剪贴板交换属于 UI/COM lane，读写和等待均为可取消 Promise；JS 只收到 UTF-8 文本或序列化格式快照，不接收 `HGLOBAL`、`IDataObject` 或 COM 指针。交换期间新事件排队并写入 Trace。
