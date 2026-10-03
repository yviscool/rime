# Keyboard and Mouse API

状态：`Send` 字符串语言（`Send`/`SendInput`/`SendEvent`/`SendPlay`/`SendText`，`SendMode` 经每调用 `mode` 选项承载，均映射到 `keyboard.send*` 族）、结构化注入 `input.send`、鼠标族 `mouse.move/click/drag/getPos`、修饰键快照 `input.modifiers()` 均已实现并通过 contract。`KeyWait`、`GetKeyState`、`BlockInput`、`KeyHistory` 已实现为 `input.keyWait`/`input.getKeyState`/`input.blockInput`/`input.keyHistory`（见"键状态与输入控制"）。`SendMessage`、`SendLevel`、`GetKeyName`、`Set*KeyState` 仍未实现。全局 Hook 之上的声明式事件（`Hotkey`/`Hotstring`/`HotIf*`/`Install*Hook`/`SetTimer`/`OnMessage`/`OnClipboardChange`/`OnError`/`OnExit`）与捕获/调度控制（`input.createInputHook` 的 `InputHook` 23 成员、`input.suspend`、`input.policy`）已实现，契约与偏差见 [`hotkey-events.md`](./hotkey-events.md)。

源码证据：`functions.h` 的 `Send*`/`Mouse*`/`KeyWait`；`rime-research/AutoHotkey-alpha/source/keyboard_mouse.cpp`（SendKeys ~460-830、SendKey 1035-1265、MouseClickDrag 2035-2106、MouseClick 2116、MouseMove 2355、BlockInput 4512/4520）；`script2.cpp:1308`（MouseGetPos）、`script2.cpp:2264`（GetKeyState 模式首字符）、`script2.cpp:870`（KeyHistory）、`lib/wait.cpp:111`（KeyWait 默认等释放/physical）、`hook.cpp:263-266`（hook 吞噬 return 1 先例）、`hook.h:255`+`globaldata.cpp:97`（`KeyHistoryItem` 与 `g_MaxHistoryKeys=40`）；`source/window.cpp:1136`（GetNonChildParent）；`lib/win.cpp:762`（ControlGetClassNN）。

TS 面：`keyboard`/`mouse` 两个门面（`sdk/src/input.ts`），编译器为纯函数 `compileSend`（`sdk/src/send.ts`）。经 Action 管道执行：`input.send`、`input.mouse` 共用一个 executor，提交单次 `SendInput` 批次，`dwExtraInfo` 写入进程私有标记，Hook 据此标记 `selfInjected`；chord 匹配跳过 `selfInjected` 防止注入回灌热键。

## keyboard — Send 字符串语言

```js
import { keyboard, mouse, input } from "rime:input";

await keyboard.send("!^s");                  // Ctrl+Alt+s
await keyboard.send("{Ctrl down}x{Ctrl up}"); // 按住态跨批次保持
await keyboard.send("hello{Enter 3}world");
await keyboard.sendText("中文ABC");           // 每字符 KEYEVENTF_UNICODE
```

### 语法

- 修饰前缀：`!`(Alt) `^`(Ctrl) `+`(Shift) `<>`(Win)，作用于随后单键；成对符号把前缀键一并按下/抬起。
- `{Key}`：命名键（`Enter`、`F24`、`NumpadEnter`→VK_RETURN 等）、`{vkXX}`、`{scXXX}`、`{Key down}`/`{Key up}`（含 `DownR`/`DownTemp`：不跨批次保持）。
- 重复：`{Enter 3}`（名字与次数以第一个空格/Tab 分隔；`{Enter3}` 属未知项，抛错）；`repeat < 1` 发送空批次（AHK 同义）。
- 修饰键生命周期：`{Ctrl down}` 写入 persistent 状态，后续发送以该状态为起点（lazy release：编译器只补发当前批次真正需要释放的前缀键；`up` 时从 persistent 清除）。
- `{Blind}`：仅允许字符串开头；`{Raw}`：其后原样发送；`{Text}`：其后进入文本模式（见下）。`{}` 空项抛错；未配对 `{` 抛错；裸 `}` 按字面发送（与 AHK 一致）。
- 换行：字符串中的 `\r\n` 展开为 Enter；单 `\n` 同样处理（AHK `SendRaw` 语义）。
- 默认模式（`mode: "input"`）下字符先查静态 US 布局 VK 表（命中注入 VK，必要时补 Shift）；未命中字符编译为 `unicode` 步骤经 `KEYEVENTF_UNICODE` 注入——与 AHK `CharToVK` 失败后落 `SendKeySpecial` 同义（`keyboard_mouse.cpp:790` 的 `default: vk = 0` 分支），因此非 ASCII 在默认模式同样可用；本阶段固定 US 表、不随活动键盘布局变化（偏离清单见 `sdk/src/send.ts` 头注释）。

### `{Text}` / `sendText` 的 Unicode 路径

文本模式（`mode: "text"`，含 `{Text}` 切换与 `sendText()`）不依赖布局：每个 UTF-16 码元编译为一个 `unicode` 步骤，注入时发 `KEYEVENTF_UNICODE` 键包（`wScan = 码元`，`wVk = 0`），代理对自然拆成两次注入（与 AHK `SendUnicodeChar` 一致）。Hook 侧该包报告为 `vk 231 (0xE7)` + `scan = 码元`（本机探针实测：ASCII、Latin-1、CJK、代理项一致），因此 chord 永不匹配它，订阅侧以 `vk===231 && scan` 断言自注入文本事件。`sendText("中文ABC")` 因此完整可用——US 布局回退只属于非文本 VK 路径。

### 模式

| 调用 | `mode` | 本阶段语义 |
| --- | --- | --- |
| `keyboard.send(keys, {mode})` | `input`（默认）/`event`/`play`/`text`/`raw` | 同一编译器、同一 SendInput 批次 |
| `keyboard.sendInput(keys)` | `input` | AHK `SendInput` 等价 |
| `keyboard.sendEvent(keys)` | `event` | SendInput 近似（交付模式偏差，见"偏差"） |
| `keyboard.sendPlay(keys)` | `play` | SendInput 近似 |
| `keyboard.sendText(keys)` | `text` | Unicode 文本模式 |

### 错误契约（不对称，属有意设计）

- 同步 `TypeError`：文法/模式错误——`keys` 非字符串、未知 `{..}` 项、未配对 `{`、空 `{}`、`{Blind}` 非首、未知扫描码、`mode` 不在五值内。
- 同步 `Error`（消息含 `windows.input.inject`）：capability 缺失——在 `keyboard.send` 进入编译前由 `input.modifiers()` 同步抛出（见下）。
- 异步拒绝 `ActionError`：`capability_denied`/`timeout`/`cancelled`/`invalid_state`（服务未运行为 `invalid_state:input service is not running`）。

### 未知 `{..}` 项：抛错（偏离 AHK，有意）

AHK 对未知 `{..}` 项静默跳过；本仓库按显式契约风格改为 `TypeError`（与 M1 空分组查询被拒绝同源）。明确不支持并会抛错的完整列表：`{Click}`、`{ASC n}`、`{U+...}`、鼠标键 `{LButton}` 等、字符串中段的 `{Blind}`、未知命名键。错误消息列出上述拒绝项。测试：`tests/sdk/send.test.ts`（unsupported/brace 用例）。

### `input.modifiers()` — 同步读，随注入同门禁

```js
const mods = keyboard.modifiers();
// => { lcontrol, rcontrol, lshift, rshift, lalt, ralt, lwin, rwin, capsLock }
```

`keyboard.modifiers()` 是 `GetAsyncKeyState` 的实时投影（CapsLock 为 `GetKeyState(VK_CAPITAL)` 的 toggle 位）。它是注入管线的前置状态源——编译器每次 `send` 都要先读起始修饰键状态——因此与消费者 `input.send`/`input.mouse` 共用同一个 `windows.input.inject` 门禁：缺 capability 时同步抛 `Error`，消息含 `windows.input.inject`。把非内容性的、易失的输入管线状态放进只读门禁是本阶段的明示选择；该函数保持公开导出。测试：`tests/js/input_slice.cpp`（九字段形状 + denied 用例）。

## mouse — 鼠标族

```js
await mouse.move(456, 789);                    // 绝对移动（一次批次）
await mouse.move(456, 789, { speed: 50 });     // speed 校验但不生效，见下
await mouse.click({ button: 1, count: 2 });    // 双击
await mouse.click({ button: 1, x: 100, y: 200 }); // 移到点上再按
await mouse.drag({ from: {x,y}, to: {x,y}, button: 1 });
const pos = await mouse.getPos();              // { x, y, window, control }
```

- `move(x, y, {speed})`：坐标必须为 int32 有限值（浮点 → `TypeError`；AHK 静默截断，此处显式拒绝）；只提供 x 或 y（partial point）→ `TypeError`（AHK 静默返回）。
- `speed`：整数 `0..100` 校验（`TypeError`），**对注入结果无影响**——本阶段只走 `SendInput`，而 AHK `SendInput` 模式本身忽略速度：`keyboard_mouse.cpp:2477` 的 `if (aSpeed == 0 || sSendMode == SM_INPUT)` 直接走瞬移（分步动画只存在于 `SM_EVENT` 的 `MouseMove` 路径，`MAX_MOUSE_SPEED=100` 见 `defines.h:1050`）。因此 `speed: 0` 与 `speed: 50` 同为单次移动，测试 pin 了这一点（非近似，而是与 AHK SendInput 源码同义）。
- `click({button, count, x, y, speed})`：`button ∈ {1,2,3}`（X1/X2/滚轮拒绝，对应 AHK `keyboard_mouse.cpp:2193-2291` 的扩展分支不实现）；`count < 1` → `{sent: 0}` 且不触桥；`x/y` 成对校验后先移动再按序 down/up（`SM_SWAPBUTTON` 逻辑翻转在 native 层复制，`keyboard_mouse.cpp:2190`）。
- `drag({from, to, button, speed})`：`from` 可省（从当前光标起）；`to` 必填；批次顺序 from→down→to→up（`keyboard_mouse.cpp:2082-2106` 同序）。
- 所有步骤合成一个 `input.mouse` 批次 → 单次 `SendInput`，自注入标记同键盘路径；相对移动（`RelMove`）仅作为 native 契约能力暴露给测试，门面不提供 AHK 的 `R` 标志。
- capability：`windows.input.inject`；同步参数校验 `TypeError`，capability/action 失败为异步 `ActionError`。

## mouse.getPos — 只读，独立 capability

`mouse.getPos()` → `Promise<{ x, y, window, control }>`。执行链：`GetCursorPos` + `WindowService::window_at(x, y)`（UI lane，`WindowFromPoint` → `GetNonChildParent` 的 WS_CHILD 回溯），`window` 为快照或 `null`，`control` 为 `{ id, className, classNN }`（ClassNN 编号沿用 AHK `EnumChildFindPoint` 序号规则，`script2.cpp:1380-1410`）。

capability 为**新增的 `windows.input.read`**（读/写分离，与 `windows.window.read`/`clipboard` 读门禁同构；注入门禁不覆盖只读观察）。缺 capability 时异步拒绝 `{ code: "capability_denied" }`，消息含 `windows.input.read`。coverage 台账原记 `windows.input.inject` 属登记期猜测，由 orchestrator 在集成时修正 `coverage.json`。测试：`tests/js/input_slice.cpp`（getPos 形状 + denied 用例）。

## 键状态与输入控制 — getKeyState / keyWait / blockInput / keyHistory

```js
import { input } from "rime:input";

input.getKeyState("capslock", "t");              // boolean，同步（T=toggle 首字符模式）
await input.keyWait("F24");                      // true：默认等释放 + physical + 5s 预算
await input.keyWait("F24", { down: true, deadlineMs: 200 }); // 超时 reject { code: "timeout" }
input.blockInput("on", { cancellationId });      // boolean：当前是否处于 block
input.keyHistory({ maxEvents: 40 });             // { capacity, count, events[] }，同步
```

- **键名解析**（三者共用 `state_key`）：chord 键名（字母/数字/`f1..f24`/导航与锁定名）、修饰键与鼠标键别名、AHK 的 `vkXX` 显式十六进制（1..FE，同 `TextToVK` 的 aAllowExplicitVK 拼写）；大小写不敏感（AHK 键名大小写不敏感）；未知名/空串同步 `TypeError`（"unknown key name: X"，不猜键）。
- **`getKeyState(keyName[, mode])`**：mode 按首字符选择（同 AHK `script2.cpp:2264` 只看第一位）：`L`/`l` logical（默认）、`P`/`p` physical、`T`/`t` toggle；空串或其他首字符 → `TypeError`。三条读路：logical = `GetAsyncKeyState`；toggle = `GetKeyState(VK_CAPITAL)` 低位；physical = **Hook 维护的 256 位原子快照**——安装时以 `GetAsyncKeyState` 播种、每条 hook 事件更新（含被 block 吞噬的与自注入的事件），服务未运行时回退 `GetAsyncKeyState`。同步读，capability `windows.input.read`：**先校验参数、后门禁**（缺 capability 同步抛 `Error`，消息含 `windows.input.read`，同 `input.modifiers()` 风格）。
- **`keyWait(keyName[, options])`**：默认与 AHK `wait.cpp:111` 一致——**等待释放 + physical**；`options.down: true` 改等按下，`options.mode: 'physical'|'logical'`，`deadlineMs`（默认 **5000**，见偏差），`cancellationId` 走既有取消总线。25ms 轮询链（`schedule_task` → `schedule_worker`，同 `windows.wait` WaitLoop 模式，不经 Action 管道故无 Action Trace）：命中 resolve `true`；预算耗尽 reject `{ code: "timeout", message: "key wait timed out after Nms" }`；绑定取消 → `cancelled`；首次轮询缺 capability → `capability_denied`。键名/`options` 类型/`mode` 拼写错误同步 `TypeError`。
- **`blockInput(mode[, options])`**：mode 仅 `'on'`/`'off'`（含 `1`/`0`、`Send`/`Mouse` 等 AHK 变体在内一律 `TypeError`，见偏差）；capability `windows.input.inject`（参数先于门禁，被拒调用不置位）。同步返回布尔——当前是否处于 block；服务未运行时 `'on'` 返回 `false`（hook 未安装，置位即谎言）。**吞咽语义**（AHK `keyboard_mouse.cpp:4512`、hook `return 1` 先例 `hook.cpp:263-266`）：非自注入事件在**入队之后**被 hook 吞掉——订阅与物理快照照常记录，OS 侧 `GetAsyncKeyState` 保持抬起（本机探针实测：下游 hook 收不到、`SendInput` 返回时判定已生效）；本进程 `dwExtraInfo` 标记的自注入照常放行，脚本自身注入管线不受影响。**释放保证**：`options.cancellationId` 布防 50ms timer 线程守卫（cancellation 一到即 `set_blocked(false)`）；显式 `'off'` 停止守卫；`InputService::stop()` **无条件清位**（关机序第一步）——任何关停/取消路径都不可能把桌面留在 block 状态。被吞输入仍更新 `GetLastInputInfo`（探针实测，该计时来源是独立可观察面）。
- **`keyHistory([options])`**：同步报告 `{ capacity, count, events }`（事件旧→新）。行结构 `{ vk, scan, down, injected, selfInjected, timestamp, elapsed }`（`timestamp` 为 GetTickCount 毫秒，`elapsed` 为与上一事件的间隔）。`options.maxEvents`（整数 **0..500**；越界、分数、非数 → `TypeError`）**调整记录环容量**——查询带副作用与 AHK 容量参数同源，属文档化行为；默认 40（AHK `globaldata.cpp:97 g_MaxHistoryKeys`），500 为本仓库上限。门禁 `windows.input.read`：参数校验先于门禁、**resize 只发生在门禁之后**（被拒调用不改变环）。键事件与鼠标按键入环（按钮映射 `VK_LBUTTON` 等），移动/滚轮不入环。

偏差与设计选择（键状态四件套）：

- `keyWait` 默认预算 5000ms（house rule：一切等待有界）；AHK `KeyWait` 无超时参数时无限等待。
- logical 模式读 `GetAsyncKeyState` 而非 AHK 的线程队列位：JS 线程没有消息泵（本机探针实测：非泵线程 down 位失真、toggle 位正确），AHK 的线程队列语义不可移植；toggle 位与 AHK 等价。
- `blockInput` 仅 `'on'`/`'off'`：AHK 的 `Send`/`Mouse`/`SendAndMouse`/`Default`/`MouseMove`/`MouseMoveOff` 及 `1`/`0` 拼写抛 `TypeError`（`functions.h:8` 家族暂不逐个等价实现）。
- 自注入输入绕过 block（见上）；AHK hook 模式下自身发送的交互未逐条等价验证，此处以"本进程注入管线可用"为显式选择。
- `keyHistory` 无 GUI 历史窗口（AHK 无参调用打开窗口）、无目标窗口列；`maxEvents` 上限 500 为本仓库扩展（AHK 无参数化容量）。
- 四个面都是**读/控制路径而非 Action**：`getKeyState`/`keyHistory`/`blockInput` 同步、`keyWait` 轮询异步，均不经 `Context → Intent → Action IR → Action Kernel`（与 `windows.wait`、`mouseGetPos` 同构）；能力门禁、取消、超时、诊断仍齐备，只是不进 Action Trace。
- TS 声明已随 M2-B 落地：`sdk/src/input.ts` 的 `InputBridge` 四方法与配套类型（`KeyStateMode`/`KeyWaitOptions`/`BlockInputOptions`/`KeyHistoryOptions`/`KeyHistoryRow`/`KeyHistoryReport`）。

## 捕获与调度控制 — createInputHook / suspend / policy（M2-D）

```js
import { input } from "rime:input";

const ih = input.createInputHook("C L8", "{Esc}", "ok,xy"); // InputHook 对象（23 成员）
ih.Start();                            // 门禁 windows.hook.global；捕获与 LL Hook 同路
input.suspend("on");                   // 只关 hotkey/hotstring 匹配，返回生效状态
input.policy({ maxConcurrency: 4 });   // 读/写中心调度策略，fail-closed
input.hotkey("f16", fn, { suspendExempt: true, inputLevel: 1, on: true }); // 注册选项
```

- `createInputHook(options?, endKeys?, matchList?)` 按 AHK `InputHook(...)` 三参构造：全部参数**先校验后提交**（未知选项/未知 EndKey/坏 MatchList → 同步 `TypeError`）；构造与属性读写免 capability，`Start()` 需要 `windows.hook.global`。成员表、`EndReason`、`KeyOpt`、`Wait`/`Timeout` 与 `On*` 契约见 [`hotkey-events.md`](./hotkey-events.md)。
- `suspend(on?)`：布尔或 `"on"|"off"|"toggle"`，无参（`undefined`/`null`）= toggle，其他类型 `TypeError`；返回**生效后的挂起状态**——没有只读入口，归一/恢复用 `input.suspend(false)`。只过滤 hotkey/hotstring 匹配（`suspendExempt` 豁免）；Timer、`onMessage`、InputHook 捕获、key history 不受影响，不产生丢弃计数。
- `policy(snapshot?)`：中心调度策略唯一读写面，默认 `{ maxConcurrency: 0, maxConcurrencyPerHotkey: 1, inputLevel: 0, hotIfTimeout: 1000, overflow: "coalesce" }`；传入快照则先整体校验后提交——未知字段、负数/非整数、`overflow` 越界、非对象均 `TypeError` 且不改任何现值；省略字段保持原值。实现落在 `engine/core/include/rime/core/scheduler_policy.hpp`，Hook/Timer/模块共享同一判定。
- 注册选项 `{ on, suspendExempt, inputLevel }`（`input.hotkey`/`input.hotstring` 第三参）：未知键、`inputLevel < 0`、`on` 非布尔 → 同步 `TypeError`，校验先于注册提交（被拒调用不产生注册）。

## 执行与错误契约总表

| 入口 | 校验时机 | capability | 失败形态 |
| --- | --- | --- | --- |
| `keyboard.send*` / `keyboard.modifiers` | 同步 | `windows.input.inject` | `TypeError`（文法）/ `Error`（capability）/ `ActionError`（异步） |
| `mouse.move/click/drag` | 同步参数、异步执行 | `windows.input.inject` | `TypeError` / `ActionError` |
| `mouse.getPos` | 异步 | `windows.input.read` | `ActionError { code: "capability_denied" }` |
| `input.send` / `input.mouse`（executor 复读） | payload 二次校验 | 声明于 executor 元数据 | `invalid_contract` → `ActionError` |
| `input.getKeyState` | 同步 | `windows.input.read` | `TypeError`（键名/模式）/ `Error`（capability） |
| `input.keyWait` | 同步校验、异步轮询 | `windows.input.read` | `TypeError` / 异步 `{ code: "timeout"\|"cancelled"\|"capability_denied" }` |
| `input.blockInput` | 同步 | `windows.input.inject` | `TypeError`（mode/options）/ `Error`（capability）；服务未运行返回 `false` |
| `input.keyHistory` | 同步（resize 在门禁后） | `windows.input.read` | `TypeError`（maxEvents/arity）/ `Error`（capability） |
| `input.createInputHook` | 同步（先校验后提交） | 构造免；`Start()` 需 `windows.hook.global` | `TypeError`（参数）/ `Error`（capability） |
| `input.suspend` | 同步 | 无 | `TypeError`（非布尔/非 `"on"`/`"off"`/`"toggle"`） |
| `input.policy` | 同步（整体校验后提交） | 无 | `TypeError`（未知字段/负数/非整数/`overflow` 越界/非对象） |

自注入观测：订阅回调中 `selfInjected === true` 表示本进程批次（键与鼠标均标记；WH_MOUSE_LL 实测只回报 `dwExtraInfo` 低 32 位，native 层以低半字比较，注释见 `input.cpp`）。chord 匹配跳过这类事件，外来注入照常参与。

## 偏差与不实现项（报告口径）

- SendEvent/SendPlay：编译语法与批次同 SendInput，交付模式不同（AHK event/play 走消息注入/回放）。
- 未知 `{..}` 项抛错而非静默跳过（见上，有意偏离）。
- `{Text}` 外的非 ASCII 依赖 US 布局 VK 表，不可映射即抛错；不实现布局探测。
- `SendLevel`、CapsLock 预翻转（`{CapsLock}` 按普通键处理）、SendEvent/SendPlay 的批间光标预测、标题栏点击补偿、`{Click}`/`{ASC}`/`{U+}`/鼠标键注入、相对移动 `R` 标志、X1/X2/滚轮点击：不实现。
- 绝对坐标按主屏 `SM_CXSCREEN/SM_CYSCREEN` 归一化（AHK 同为主屏-only，不带 `MOUSEEVENTF_VIRTUALDESK`）。
- M2-B 四件套（`KeyWait`/`GetKeyState`/`BlockInput`/`KeyHistory`）的逐条偏差见上文"键状态与输入控制"节；TS 声明已在 `sdk/src/input.ts` 的 `InputBridge` 落地（`getKeyState`/`keyWait`/`blockInput`/`keyHistory` 及配套类型）。

## 测试与契约

- `tests/sdk/send.test.ts`：文法单元 + 门面行为（mock `rime:input`）。
- `tests/native/input_tests.cpp`：`input.send`/`read_modifier_state`/`send_mouse` 契约与真实注入（含 Unicode 包、自标记、绝对/相对移动、down/up）；M2-B：`read_key_state` 三模式对照 Win32 源、`physical_key_down` 播种与自注入跟踪、`keyHistory` 环（resize/裁剪/0 关断/500 上限）、block 吞咽（订阅可见而 `GetAsyncKeyState` 不可见、自注入放行、off 恢复、`stop()` 清位）。
- `tests/js/input_slice.cpp`：bridge→executor 全链（Send 文本/Unicode、`input.mouse`、`input.modifiers`、`input.mouseGetPos`、denied 门禁）；M2-B：`getKeyState`（TypeError 矩阵、toggle≡`modifiers().capsLock`、按住/释放实读）、`keyWait`（即时 resolve、timeout、cancel、真实按放、TypeError、denied `capability_denied`）、`blockInput`（TypeError、被拒调用不置位、吞咽+订阅可见+自注入放行、cancellation 守卫释放、`off` 恢复投递）、`keyHistory`（TypeError 矩阵、capacity resize 后六事件剩四行、denied）。
- `contracts/registry/actions.json`：`input.send`/`input.mouse` 条目与 `windows.input.inject`/`windows.input.read` 门禁声明；M2-B 追加两 capability 的 `checked` 行（getKeyState/keyWait/keyHistory/blockInput 门禁点）与三条 `readSurfaces` 条目。

## 订阅与 chord

见 [`hotkey-events.md`](./hotkey-events.md)：`input.subscribe`/`input.bind`/`input.unbind` 与 `input.hotkey`/`input.hotstring`/`input.hotIf*`/`input.setTimer`/`input.onMessage`/`input.onClipboardChange`/`input.onError`/`input.onExit` 共用同一订阅与取消契约（`{ id, kind, close() }`、关闭态、回调计数、teardown 顺序）；M2-D 的 `input.createInputHook`/`input.suspend`/`input.policy` 与注册选项 `{ on, suspendExempt, inputLevel }` 同文承载。
