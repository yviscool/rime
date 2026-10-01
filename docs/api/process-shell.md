# Process and Shell API

状态：`specified`，尚未实现。

源码证据：`functions.h` 的 `Run`、`RunWait`、`RunAs`、`Process*`、`Shutdown`；`source/lib/process.cpp`、`source/application.cpp`。

进程使用稳定 `ProcessId` 和快照；启动返回受 Runtime 所有的 `ProcessRef`。`RunWait`、`ProcessWait`、`ProcessWaitClose` 通过可等待句柄和取消令牌实现，不能在 JS Thread 轮询或阻塞。凭据和终止操作需要独立 capability。
