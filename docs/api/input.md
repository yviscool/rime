# Keyboard and Mouse API

状态：`Send` 字符串语言（`Send`/`SendInput`/`SendEvent`/`SendPlay`/`SendText`，`SendMode` 经每调用 `mode` 选项承载，均映射到 `keyboard.send*` 族）、结构化注入 `input.send`、鼠标族 `mouse.move/click/drag/getPos`、修饰键快照 `input.modifiers()` 均已实现并通过 contract。`KeyWait`、`KeyHistory`、`BlockInput`、`SendMessage`、`SendLevel`、`GetKey*`/`Set*KeyState` 仍未实现。

源码证据：`functions.h` 的 `Send*`/`Mouse*`/`KeyWait`；`rime-research/AutoHotkey-alpha/source/keyboard_mouse.cpp`（SendKeys ~460-830、SendKey 1035-1265、MouseClickDrag 2035-2106、MouseClick 2116、MouseMove 2355）；`script2.cpp:1308`（MouseGetPos）；`source/window.cpp:1136`（GetNonChildParent）；`lib/win.cpp:762`（ControlGetClassNN）。

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

## 执行与错误契约总表

| 入口 | 校验时机 | capability | 失败形态 |
| --- | --- | --- | --- |
| `keyboard.send*` / `keyboard.modifiers` | 同步 | `windows.input.inject` | `TypeError`（文法）/ `Error`（capability）/ `ActionError`（异步） |
| `mouse.move/click/drag` | 同步参数、异步执行 | `windows.input.inject` | `TypeError` / `ActionError` |
| `mouse.getPos` | 异步 | `windows.input.read` | `ActionError { code: "capability_denied" }` |
| `input.send` / `input.mouse`（executor 复读） | payload 二次校验 | 声明于 executor 元数据 | `invalid_contract` → `ActionError` |

自注入观测：订阅回调中 `selfInjected === true` 表示本进程批次（键与鼠标均标记；WH_MOUSE_LL 实测只回报 `dwExtraInfo` 低 32 位，native 层以低半字比较，注释见 `input.cpp`）。chord 匹配跳过这类事件，外来注入照常参与。

## 偏差与不实现项（报告口径）

- SendEvent/SendPlay：编译语法与批次同 SendInput，交付模式不同（AHK event/play 走消息注入/回放）。
- 未知 `{..}` 项抛错而非静默跳过（见上，有意偏离）。
- `{Text}` 外的非 ASCII 依赖 US 布局 VK 表，不可映射即抛错；不实现布局探测。
- `SendLevel`、CapsLock 预翻转（`{CapsLock}` 按普通键处理）、SendEvent/SendPlay 的批间光标预测、标题栏点击补偿、`{Click}`/`{ASC}`/`{U+}`/鼠标键注入、相对移动 `R` 标志、X1/X2/滚轮点击：不实现。
- 绝对坐标按主屏 `SM_CXSCREEN/SM_CYSCREEN` 归一化（AHK 同为主屏-only，不带 `MOUSEEVENTF_VIRTUALDESK`）。
- coverage 台账中 `Send`/`SendInput` 行的 `lane: ui` 与实际 worker-lane 执行不一致（orchestrator 持有台账，不在本阶段修改）。

## 测试与契约

- `tests/sdk/send.test.ts`：文法单元 + 门面行为（mock `rime:input`）。
- `tests/native/input_tests.cpp`：`input.send`/`read_modifier_state`/`send_mouse` 契约与真实注入（含 Unicode 包、自标记、绝对/相对移动、down/up）。
- `tests/js/input_slice.cpp`：bridge→executor 全链（Send 文本/Unicode、`input.mouse`、`input.modifiers`、`input.mouseGetPos`、denied 门禁）。
- `contracts/registry/actions.json`：`input.send`/`input.mouse` 条目与 `windows.input.inject`/`windows.input.read` 门禁声明。

## 订阅与 chord

见 [`hotkey-events.md`](./hotkey-events.md)：`input.subscribe`/`input.bind`/`input.unbind` 与全局 Hook 共用同一订阅与取消契约。
