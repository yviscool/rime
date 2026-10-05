# Process and Shell API

状态：`contract-only`；`Run` 与 `ProcessClose/ProcessExist/ProcessGetName/ProcessGetParent/ProcessGetPath` 已实现（contract 见 `tests/native/process_tests.cpp`、`tests/js/breadth_slice.cpp`）。`RunWait`/`ProcessWait*`/`RunAs`/`Shutdown`/`ProcessSetPriority` 尚未实现。

命令解析（`Run` → `process.launch` → `ProcessService::launch`）：裸命令（不含盘符与路径分隔符 `\`、`/`）先经 `SearchPathW` 按 Win32 搜索顺序（应用目录、当前目录、系统目录、Windows 目录、`PATH`）解析为绝对路径，再以该绝对路径作 `lpApplicationName` 交给 `CreateProcessW`；无扩展名的裸名先按 `.exe` 查找，未命中再按原名查找一次，已带扩展名的裸名只按原名查找。带路径的命令不参与搜索、原样传入，全路径行为与命令行拼接（`"command" + args`）、`workingDir`、异步语义均不变。搜索不到且 `CreateProcessW` 也失败时返回 `execution_failed`，消息写明 PATH 搜索失败并附 Win32 错误码。历史缺陷：裸命令曾直接作为 `lpApplicationName` 传入，而 Win32 只按当前目录补全、不做路径搜索，导致 `notepad.exe` 之类命令必须靠切换当前目录才能启动；该绕行已由本次解析修复取代。验证：`tests/native/process_tests.cpp`（裸名与全路径两条输入都走 `ProcessService::launch`）、`tests/native/golden_exec_tests.cpp`（golden 裸 `notepad.exe` 从测试自身 cwd 启动，不再切目录）。

源码证据：`functions.h` 的 `Run`、`RunWait`、`RunAs`、`Process*`、`Shutdown`；`source/lib/process.cpp`、`source/application.cpp`；Runtime 侧 `engine/win32/src/process.cpp` 的 `ProcessService::launch`。

进程使用稳定 `ProcessId` 和快照；启动返回受 Runtime 所有的 `ProcessRef`。`RunWait`、`ProcessWait`、`ProcessWaitClose` 通过可等待句柄和取消令牌实现，不能在 JS Thread 轮询或阻塞。凭据和终止操作需要独立 capability。
