# Clipboard API

状态：`contract-only`；剪贴板文本读写已实现（`clipboard.read`/`clipboard.write`，contract 见 `tests/js/breadth_slice.cpp`）。`ClipWait`、`ClipboardAll` 二进制快照与变化订阅尚未实现。

`clipboard.write("")` 会把剪贴板清成**真正空**（`CF_UNICODETEXT` 不存在），与 AHK `Clipboard := ""` 一致——原实现写入单个 NUL，格式始终存在，会让 `IsClipboardFormatAvailable` 一直报告有文本、`ClipWait` 无法观察到空剪贴板。经 `clipboard.read` 读回仍是 `""`，对外读语义未变；`tests/native/clipboard_tests.cpp` 断言格式确实消失。

源码证据：`functions.h` 的 `ClipWait`、`OnClipboardChange`；`source/clipboard.cpp`、`source/clipboard.h`。

剪贴板交换属于 UI/COM lane，读写和等待均为可取消 Promise；JS 只收到 UTF-8 文本或序列化格式快照，不接收 `HGLOBAL`、`IDataObject` 或 COM 指针。交换期间新事件排队并写入 Trace。
