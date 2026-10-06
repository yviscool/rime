# Clipboard API

状态：`clipboard.read` / `clipboard.write` / `ClipWait` 已实现（JS contract 见 `tests/js/breadth_slice.cpp`，服务层谓词与空写语义见 `tests/native/clipboard_tests.cpp`）。`ClipboardAll` 二进制快照与变化订阅尚未实现。

源码证据：`functions.h` 的 `ClipWait`、`OnClipboardChange`；`source/clipboard.cpp`、`source/clipboard.h`、`source/lib/wait.cpp`（`ClipWait` 谓词）。

剪贴板交换属于 UI/COM lane，读写和等待均为可取消 Promise；JS 只收到 UTF-8 文本或序列化格式快照，不接收 `HGLOBAL`、`IDataObject` 或 COM 指针。交换期间新事件排队并写入 Trace。

## `clipboard.write(text[, options])`

把文本写进 `CF_UNICODETEXT`，走 `clipboard.write` Action（capability `windows.clipboard.write`，进 Trace）。**`text` 为空串时剪贴板被清成真正空**（`CF_UNICODETEXT` 不再存在），与 AHK `Clipboard := ""` 一致——原实现仍写入单个 NUL，格式永远存在，`ClipWait` 就无法观察到空剪贴板。读回结果不受影响（无格式 → `""`）。

## `clipboard.wait(options?)`（AHK `ClipWait`）

等待剪贴板出现内容。**不产生 Action Trace**：它和 `windows.wait`、`input.keyWait`、process 的 wait 族同构，按 `async_task.hpp` 的 `slice_wait_step` 在 worker lane 上以 25ms 切片求值条件，绝不为整个 deadline 占住一个线程。

| 项 | 值 |
|---|---|
| capability | `windows.clipboard.read`（每次求值先读策略，缺权在首个切片以 `capability_denied` 拒绝） |
| lane | worker（切片求值；`OpenClipboard` 不参与，谓词只读） |
| 返回 | `{ ready: true }` |
| 谓词（默认） | `CF_UNICODETEXT` 或 `CF_HDROP` 可用（照抄 AHK `wait.cpp:78`：文件拖放算有内容，因为 CF_HDROP→文本的隐式转换可用） |
| 谓词（`anyData: true`） | 任一格式存在（`CountClipboardFormats() != 0`，与 AHK 相同，同为不打开剪贴板） |
| `timeout` | `deadlineMs` 到期（默认 5000ms——**与 AHK 的「默认无限等」不同**，有界等待是本仓库的统一规则，同 `input.keyWait`） |
| `cancelled` | `options.cancellationId` / `options.signal` 触发（本 loop 不持有原生资源，因此像 `windows.wait` 一样把取消 id 绑进 `begin_async`） |
| Trace | 无（wait 族不 dispatch Action；`tests/js/breadth_slice.cpp` 断言等待期间 trace 条目数不变） |

```js
import { clipboard } from "rime:clipboard";
const r = await clipboard.wait({ deadlineMs: 1500 });      // 文本或文件出现
await clipboard.wait({ anyData: true, deadlineMs: 1500 }); // 任意格式出现
```
