# Runtime and Language Compatibility

状态：部分 `sdk-owned`，部分 `contract-only`。

源码证据：`functions.h` 的 `RegEx*`、`Str*`、`DateAdd`、`DateDiff`、`IsLabel`、`Sleep`、`Exit*`、`Reload`、`Suspend`、`Pause`、`Persistent`、`Critical`、`Thread`、`CoordMode`、`DetectHidden*`、`SetTitleMatchMode`、`SetWorkingDir`。

纯字符串、日期和正则优先使用 TS/ECMAScript 标准库，并提供兼容测试；影响 Runtime 生命周期、调度和全局状态的函数必须映射到显式 Runtime API，不能复制 AHK 的隐式伪线程和全局可变设置。
