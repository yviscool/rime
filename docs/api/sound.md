# Sound API

状态：`SoundBeep` / `SoundPlay` 已实现（真设备与 AHK 默认值见 `tests/native/sound_tests.cpp`，JS contract 与拒绝门见 `tests/js/sound_slice.cpp`，facade 的 wait 拆分与错误透传见 `tests/sdk/sound.test.ts`）。`SoundGetVolume` / `SoundSetVolume` / `SoundGetMute` / `SoundSetMute` / `SoundGetName` 仍为 `contract-only`（见文末"未实现"），`SoundGetInterface` 判 `unsupported-by-policy`。

源码证据：`functions.h` 的 `SoundBeep` / `SoundPlay` / `SoundGet*` / `SoundSet*`；`source/lib/sound.cpp` 的 `SoundPlay`（540-600）、设备选择（60-126）、component 解析（146-160）与 `BIF_Sound`（292-536）；`source/script.h:2489` 的 `AHK_PlayMe` 单 alias 规则与 `source/script.h:216-218` 的三条错误原文。

两个入口都走 worker lane（`async_task.hpp` 的骨架）：capability `media.sound` 在 body 的第一行读，缺权时连 winmm 都不碰；`deadlineMs` + `cancellationId`/`signal` 每个切片重新求值。**两个调用都不构建 Action，所以 Action Trace 恒为空**（`sound_slice.cpp` 断言调用前后 trace 条目数不变）。JS 只收到 `null` 或字符串，不接收 MCI 句柄、设备 ID 或 COM 指针。

## `sound.beep([frequency][, duration][, options])`（AHK `SoundBeep`）

kernel32 `Beep`。参数缺省时用 AHK 的默认值，且默认值在 native 侧解析（`SoundService::resolve_beep`），JS 不能把小数混进来。

| 项 | 值 |
|---|---|
| 默认 | `523` Hz / `150` ms（AHK `lib/sound.cpp:594-600`，native 测试用手写字面量钉死） |
| 负时长 | 回落 `150` ms——AHK 用它保证"负数=默认"而不是跑很久 |
| `0` | 保持 `0`（与 AHK 相同，不是回落） |
| 非整数 / 越界 | `TypeError`（本仓库"数值参数不做静默截断"的统一规则） |
| capability | `media.sound`（worker body 内读，缺权 → `capability_denied`） |
| 取消/超时 | `cancelled` / `timeout`（`options.cancellationId`、`options.signal`、`options.deadlineMs`） |
| Trace | 无 |

## `sound.play(file[, options])`（AHK `SoundPlay`）

在本进程**唯一的 MCI alias** 上播放；alias 上已经打开的声音先被关闭再开新的（AHK 自己的 `AHK_PlayMe` 单 alias 规则，`script.h:2489`），所以两个 `play` 不会抢设备。打开失败不会把 alias 留成"已占用"状态。

| 项 | 值 |
|---|---|
| `file` 以 `*` 开头 | AHK 的 `MessageBeep` 分支：`*` 后面的数字是 MB 类型，`*` 单独 = 类型 `0`，非数字即止、溢出饱和（`lib/sound.cpp:547-555`）。**这条路径永不等待**，与 AHK 一致 |
| `options.wait` | 见下节"等待播放结束" |
| capability | `media.sound`（worker body 内读，缺权 → `capability_denied`） |
| 失败 | `execution_failed`（MCI 原文，例如 `MCI open failed: ...`）、`timeout` / `cancelled` |
| Trace | 无 |

**MCI 按文件扩展名选设备**：字节完全相同的文件，`.tmp` 会被 `mciSendString` 以"指定的设备未打开，或不被 MCI 所识别"（263）拒绝，`.wav` 才能播——这是 Windows 的行为，AHK 依赖它，本实现不做后缀伪装。要播放没有已知后缀的字节流，先落成 `.wav` 临时文件（测试夹具 `tests/sound_wav_fixture.hpp` 就是这么做的）。

### 等待播放结束（`options.wait`）

与 `windows.wait`、`clipboard.wait`、process 的 wait 族同构：第一个切片打开并启动播放，此后按 20ms 轮询 alias 的 MCI mode（AHK 的等待循环同样每 20ms 查一次，`lib/sound.cpp:576-584`），直到 `stopped`。绝不为整段播放占住一个线程。

| 项 | 值 |
|---|---|
| capability | 每个切片重读 `media.sound`，中途被收回则在下一切片以 `capability_denied` 结束 |
| 终态 | **唯一出口**（成功、超时、取消、缺权）都会关闭 alias，MCI 句柄不会活过 promise |
| `timeout` | `deadlineMs` 到期，默认 `5000` ms |
| `cancelled` | `options.cancellationId` / `options.signal` |
| 不带 `wait` | promise 在 MCI 接受播放后立即 resolve |

**与 AHK 的偏差**：AHK 的 `SoundPlay(file, 1)` 无限等，本实现受仓库统一的有界等待约束，默认 5000ms，需要更久就显式传 `deadlineMs`（同 `input.keyWait`、`clipboard.wait`）。

```js
import { sound } from "@rime/sdk";
await sound.beep();                 // 523 Hz / 150 ms
await sound.beep(880, 200, { deadlineMs: 1000 });
await sound.play("C:\\tmp\\chime.wav");
await sound.play("C:\\tmp\\chime.wav", { wait: true, deadlineMs: 15000 });
await sound.play("*0");             // MessageBeep(MB_OK)，不等待
```

模块入口也可以直接用运行时模块：`import { sound } from "rime:sound";`。

## 未实现

| AHK | 状态 | 说明 |
|---|---|---|
| `SoundGetVolume` / `SoundSetVolume` | `contract-only` | Core Audio 端点 + 拓扑 component，M5 尾段交付 |
| `SoundGetMute` / `SoundSetMute` | `contract-only` | 同上 |
| `SoundGetName` | `contract-only` | 同上 |
| `SoundGetInterface` | `unsupported-by-policy` | 返回原始 COM 指针违反 AGENTS"不得向 JS 暴露裸 `HANDLE` / `HWND` / COM 指针"；不提供替代路径 |

`Sound*` 5 项的台账行在 `docs/api/core-builtins.json`，域为 `gui-menu`（源自 AHK 把 `BIF_Sound` 与 GUI 函数放在同一批源文件），实现归属 `@rime/sound`，能力同为 `media.sound`。
