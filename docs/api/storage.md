# File, Directory, Drive and Environment API

状态：`specified`，尚未实现。

源码证据：`functions.h` 的 `File*`、`Dir*`、`Drive*`、`Env*`、`Ini*`、`Download`、`SplitPath`；`source/lib/file.cpp`、`source/lib/drive.cpp`、`source/lib/env.cpp`。

纯路径解析和字符串转换可同步；文件、目录、驱动器、下载和选择器全部在 IO/UI lane 异步执行，接受取消和权限。路径必须经过 sandbox/capability 校验，不能把 AHK 的隐式工作目录和全局可变状态复制到 Runtime。
