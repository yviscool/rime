# Clipboard API

状态：`specified`，尚未实现。

源码证据：`functions.h` 的 `ClipWait`、`OnClipboardChange`；`source/clipboard.cpp`、`source/clipboard.h`。

剪贴板交换属于 UI/COM lane，读写和等待均为可取消 Promise；JS 只收到 UTF-8 文本或序列化格式快照，不接收 `HGLOBAL`、`IDataObject` 或 COM 指针。交换期间新事件排队并写入 Trace。
