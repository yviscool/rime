# Monitor, Screen and Visual API

状态：`contract-only`，尚未实现。

源码证据：`functions.h` 的 `Monitor*`、`SysGet`、`Pixel*`、`ImageSearch`、`CaretGetPos`；`source/lib/pixel.cpp`、`source/lib/win.cpp`。

监视器枚举返回不可变快照；像素读取、截图、图像搜索和 Caret 查询走 UI/worker lane 并支持取消。坐标空间、DPI、虚拟屏幕原点和颜色格式必须在 TS 类型中明确。
