# Clipboard API

状态：`clipboard.read` / `clipboard.write` / `ClipWait` / `ClipboardAll`（`saveAll` + `restoreAll`）已实现（JS contract 见 `tests/js/breadth_slice.cpp`，服务层谓词、空写语义与快照往返见 `tests/native/clipboard_tests.cpp`，facade 与 `ClipboardAll.__New` 见 `tests/sdk/clipboard.test.ts`）。变化订阅尚未实现。

源码证据：`functions.h` 的 `ClipWait`、`OnClipboardChange`；`source/clipboard.cpp`、`source/clipboard.h`、`source/lib/wait.cpp`（`ClipWait` 谓词）；`source/var.cpp` 的 `GetClipboardAll` / `SetClipboardAll`（快照记录序列）与 `script_object.cpp` 的 `ClipboardAll::__New`。

剪贴板交换属于 UI/COM lane，读写、等待与快照均为可取消 Promise；JS 只收到 UTF-8 文本、序列化格式快照的 `number[]`，不接收 `HGLOBAL`、`IDataObject`、格式 ID 或 COM 指针。交换期间新事件排队并写入 Trace。

## `clipboard.read([options])`

读文本（capability `windows.clipboard.read`）。空串表示剪贴板当前没有 `CF_UNICODETEXT`。这是查询，**不进 Trace**。

## `clipboard.write(text[, options])`

把文本写进 `CF_UNICODETEXT`，走 `clipboard.write` Action（capability `windows.clipboard.write`，进 Trace）。**`text` 为空串时剪贴板被清成真正空**（`CF_UNICODETEXT` 不再存在），与 AHK `Clipboard := ""` 一致——原实现仍写入单个 NUL，格式永远存在，`ClipWait` 就无法观察到空剪贴板。读回结果不受影响（无格式 → `""`）。

## `clipboard.saveAll([options])` / `clipboard.restoreAll(snapshot[, options])`（AHK `ClipboardAll`）

不透明二进制快照：`saveAll` 把当前剪贴板的每个可序列化格式拷成字节，`restoreAll` 清空剪贴板后按同一批字节重建（AHK `Clipboard := clipallObj`）。

| 项 | `saveAll` | `restoreAll` |
|---|---|---|
| 语义 | 查询，**不进 Trace** | `clipboard.restore` Action（capability `windows.clipboard.write`，进 Trace，可取消） |
| 返回 | `ClipboardAll`（`.size` / `.bytes`） | `{ formats: <重建的格式数> }` |
| 失败 | `capability_denied` / `execution_failed`（剪贴板忙） | 同上，外加 `invalid_contract`（不是快照 / 字节不是 0..255 整数） |

**快照布局**：12 字节头（magic `RIMB`、版本、保留位）+ AHK 的记录序列 `[u32 format][u32 size][size 字节]`，以 `format == 0` 结束（照 `var.cpp` 的 `GetClipboardAll`，尺寸 32 位、x86/x64 互通）。记录里的格式 ID 只在字节内部存在，JS 拿不到也造不出；头校验在 `EmptyClipboard()` **之前**完成，所以外来或损坏的字节会被拒绝而剪贴板原封不动。

**跳过的格式**（照 AHK）：`CF_BITMAP` / `CF_ENHMETAFILE` / `CF_DSPENHMETAFILE`（句柄类型，`GlobalSize` 不可靠）、`CF_TEXT` / `CF_OEMTEXT`（合成必然重建）、`CF_DIB` 与 `CF_DIBV5` 二者取其一。单个格式读失败只跳过该格式，不做整体放弃——AHK 认为部分保存远好于全部失败。

**与 AHK 的两处偏差**：

1. AHK 的 `ClipboardAll()`（无参）在**构造器内**同步读剪贴板；JS 构造器不能 await 原生调用，所以捕获是 `await clipboard.saveAll()`，`new ClipboardAll(data?, size?)` 只包装已有字节（`objects.json` 的 `async: "async"` 描述的是 AHK 源）。
2. AHK 的 `ClipboardAll(data, size)` 信任调用方给的 `size`，缓冲区不够长会读到尾部之外；本实现先检查 `size <= data.length`，越界抛 `RangeError`。

```js
import { clipboard, ClipboardAll } from "@rime/sdk";
const snapshot = await clipboard.saveAll(); // ClipboardAll（.size / .bytes）
await clipboard.write("something else");
await clipboard.restoreAll(snapshot);       // { formats: 3 }
new ClipboardAll([1, 2, 3], 2);             // 只留前两字节
new ClipboardAll([1, 2], 4);                // RangeError: size 超过数据长度
```


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
