# Sound API

状态：`SoundBeep` / `SoundPlay` 与 `SoundGetVolume` / `SoundSetVolume` / `SoundGetMute` / `SoundSetMute` / `SoundGetName` 共 7 项已实现（真设备与 AHK 默认值见 `tests/native/sound_tests.cpp`，JS contract、拒绝门与端点往返见 `tests/js/sound_slice.cpp`，facade 的 wait 拆分与错误透传见 `tests/sdk/sound.test.ts`，拓扑解析见 `tests/native/sound_topology_tests.cpp`）。只有 `SoundGetInterface` 判 `unsupported-by-policy`。

源码证据：`functions.h` 的 `SoundBeep` / `SoundPlay` / `SoundGet*` / `SoundSet*`；`source/lib/sound.cpp` 的 `SoundPlay`（540-600）、设备选择（60-126）、component 解析（146-160）、找不到 component（254-286）与 `BIF_Sound`（292-536）；`source/util.cpp:326-436` 的 `IsNumeric` 与 `source/util.h:484-494` 的 `ParseInteger`；`source/script.h:2489` 的 `AHK_PlayMe` 单 alias 规则与 `source/script.h:216-218` 的三条错误原文。本仓库侧对应实现：`engine/win32/src/sound_endpoint.cpp`（Core Audio 端点与拓扑 walk）、`engine/win32/include/rime/win32/sound.hpp`（`SoundService` 端点 API）与 `engine/win32/js/src/sound_module.cpp`（`rime:sound` 入口）。

7 个入口都走 worker lane（`async_task.hpp` 的骨架）：capability `media.sound` 在 body 的第一行读，缺权时连 winmm / Core Audio 都不碰；`deadlineMs` + `cancellationId`/`signal` 每个切片重新求值。**这些调用都不构建 Action，所以 Action Trace 恒为空**（`sound_slice.cpp` 断言调用前后 trace 条目数不变）。JS 只收到 `null`、数字、布尔或字符串，不接收 MCI 句柄、设备 ID 或 COM 指针。

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
await sound.getVolume();            // 端点控制见下一节
```

模块入口也可以直接用运行时模块：`import { sound } from "rime:sound";`。

## 5 个端点控制：`sound.getVolume` / `setVolume` / `getMute` / `setMute` / `getName`

AHK `SoundGetVolume` / `SoundSetVolume` / `SoundGetMute` / `SoundSetMute` / `SoundGetName`（`lib/sound.cpp:292-536`）。实现是 Core Audio：`IMMDeviceEnumerator` 选设备、`IAudioEndpointVolume` 读写 master、设备拓扑（`IDeviceTopology` → `IPart` → `IAudioVolumeLevel` / `IAudioMute`）读写命名 component。每次调用自带一个 COM apartment，接口只活在本次调用内，不跨线程封送，**JS 拿不到任何 COM 指针**；调用在 worker lane 完成，不占 UI Thread。

| AHK | 本仓库 | 返回 |
|---|---|---|
| `SoundGetVolume(component?, device?)` | `sound.getVolume(options?)` | `number`：百分比 `0..100` |
| `SoundSetVolume(value, component?, device?)` | `sound.setVolume(value, options?)` | `void` |
| `SoundGetMute(component?, device?)` | `sound.getMute(options?)` | `boolean` |
| `SoundSetMute(muted, component?, device?)` | `sound.setMute(muted, options?)` | `void` |
| `SoundGetName(component?, device?)` | `sound.getName(options?)` | `string` |

AHK 的两个位置参数在本仓库合成一个 options 对象（和 `play` 把 `wait` 挂在 options 上是同一条规则）：`{ component?, device?, ...actionOptions }`，actionOptions 仍是 `deadlineMs` / `cancellationId` / `signal` 那套。

| 项 | 值 |
|---|---|
| `options.component` | AHK 的 component 文法：省略或 `""` = master；`"Wave"`、`"Wave:2"`（末个冒号切分）、纯数字实例号（`lib/sound.cpp:146-160`） |
| `options.device` | AHK 的 device 文法：省略或 `""` = 默认渲染端点；`"名称"`、`"名称:N"`、1 起始索引（含未插入设备，`lib/sound.cpp:56-126`） |
| capability | `media.sound`（worker body 内读，缺权 → `capability_denied`，连 COM 都不初始化） |
| Trace | 无（直接服务调用，不构建 Action） |

### `setVolume(value)` 的取值

`value` 可以是数字或字符串，两者都交给 **AHK 同一个解析器**（`SoundService::parse_volume_setting`），因此 JS 不可能接受脚本侧会拒绝的写法：

- 首尾空格/制表符裁剪；`0x` 十六进制、指数形式合法；必须整串消费（`IsNumeric` + `ATOF`，`util.cpp:326-436`）。
- 除以 100 后 clamp 到 `[-1, 1]`：`1e999` 落到 100%，**不是错误**。
- 带 `+` / `-` 开头是相对调节（`lib/sound.cpp:331-342`）；**数字的负号同样算相对**——AHK 判断的是字符串化后的首字符，`setVolume(-5)` 是"降 5"，不存在绝对负音量。
- 不可解析（`"abc"`、`Infinity`、空串）→ `TypeError`，在任何 worker 启动之前抛出。

### 失败映射

| 原文（AHK `script.h:216-218`） | 本仓库 |
|---|---|
| `Device not found` | `target_gone` |
| `Component not found` | `target_gone` |
| `Component doesn't support this control type` | `unsupported` |
| 其他 HRESULT | `execution_failed`，消息 `sound <op> failed (HRESULT 0x%08X)` |

前两条逐字沿用 AHK 的文案（本仓库自己的字符串，不受 Windows 语言影响）；第三条是**有意的偏差**：AHK 抛 `Target` 异常，本仓库按能力边界归到 `unsupported`，因为"这个部件没有该控制类型"是目标不支持该操作，而不是目标消失。

### 与 AHK 的偏差

1. **`SoundSetMute` 的相对形式不进 TS**：AHK 的 `SoundSetMute("+1")` 切换、`"-1"` 解除，是字符串约定，没有诚实的布尔拼写。本仓库只收绝对布尔，切换写 `await sound.setMute(!await sound.getMute())`。
2. **参数形态**：AHK 的 `(component?, device?)` 位置参数合并为一个 options 对象（上表）。
3. **读回精度**：百分比经 float32 往返，刚写入的值读回可能差最后几位（`0.01%` 级），AHK 同样如此；测试以 `0.01` 容差断言。
4. **没有渲染端点的机器**（例如 CI 的 headless runner）：5 个调用全部以 `target_gone` + `Device not found` 结束。这被测试的第二个分支**断言**（每条调用的 code 与文案），不是跳过。
5. **同一台机器上的 `beep` / `play`**：kernel32 `Beep` 与 MCI 播放也必须诚实失败——`execution_failed` + `Beep failed (win32 error ...)` / `MCI open|play failed ...`，失败后不留半个 alias，这两条同样由测试的第二分支断言。唯一例外是 `MessageBeep`（`play("*0")`）：user32 没有文档化"无波形设备"时的行为，没有可断言的预期就不编造一个，因此它只在存在端点的机器上执行。

```js
import { sound } from "@rime/sdk";

const volume = await sound.getVolume();          // 例如 14
await sound.setVolume(42);                       // 绝对
await sound.setVolume("+5");                     // 相对 +5
await sound.setVolume(-5);                       // 也是相对 -5
await sound.setMute(true);
const name = await sound.getName({ component: "Wave", device: "Speakers" });
await sound.setMute(!await sound.getMute());     // 切换（替代 AHK 的 SoundSetMute("+1")）
```

证据分层：L2 手写字面量钉解析规则（`tests/native/sound_tests.cpp` 的 `parse_volume_setting` / `parse_component` / `parse_device` 段）、L3 合成 `IPart` 树钉拓扑 walk（`tests/native/sound_topology_tests.cpp`）、L5 真设备写读回带 restore（`sound_tests.cpp` 端点段 + `tests/js/sound_slice.cpp`）、facade 的参数拆分与错误透传（`tests/sdk/sound.test.ts`）。

## 未实现

| AHK | 状态 | 说明 |
|---|---|---|
| `SoundGetInterface` | `unsupported-by-policy` | 返回原始 COM 指针违反 AGENTS"不得向 JS 暴露裸 `HANDLE` / `HWND` / COM 指针"；不提供替代路径 |

台账在 `docs/api/core-builtins.json`：`BIF_Sound` 那 6 行域为 `gui-menu`（源自 AHK 把 `Sound*` 与 GUI 函数放在同一批源文件），`tsModule` 为 `@rime/sound`（实现归属），能力同为 `media.sound`；5 行已翻 `implemented` 并指向 `tests/native/sound_tests.cpp,tests/js/sound_slice.cpp`，`SoundGetInterface` 保持 `unsupported-by-policy`。
