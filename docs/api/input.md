# Keyboard and Mouse API

状态：`specified`；已实现输入订阅、chord 绑定与 `input.send` 结构化键盘注入（contract 见 `tests/native/input_tests.cpp`、`tests/js/input_slice.cpp`）。AHK `Send` 字符串语法、`Mouse*`、`KeyWait`、`BlockInput` 和键状态查询尚未实现。

源码证据：`functions.h` 的 `Send*`、`Mouse*`、`KeyWait`、`GetKey*`、`Set*KeyState`、`BlockInput`；`source/keyboard_mouse.cpp`、`source/keyboard_mouse.h`、`source/input_object.cpp`、`source/hook.cpp`。

TS 使用 `KeySequence`、`MouseButton`、`Point`，保留 AHK `{Blind}`、`{Text}`、左右修饰键和 down/up 语义。SendInput 批次是 UI/input lane 的临界区；取消只在输入事件边界生效，不能把半个按键序列报告为成功。

## input.send

已实现的注入走 Action 管道，capability 为 `windows.input.inject`：

```js
import { input } from "rime:input";

// action type: input.send，target: { kind: "input", id: "keyboard" }
await input.send([{ vk: 135, down: true }, { vk: 135, down: false }]);
// => { sent: 2 }
```

- 步骤在 JS 入口校验：非空数组、每步为对象、`vk` 是 1..254 整数、`down` 是布尔，违例抛 `TypeError`；缺 capability 抛 `Error`，消息带 `windows.input.inject`。Action executor 复读同一契约（contract enforcer）。
- 批次经 `SendInput` 单次提交，`dwExtraInfo` 写入进程私有标记；Hook 侧据此把事件标为 `selfInjected`，订阅回调即可区分本进程注入与外来注入。
- chord 匹配跳过 `selfInjected` 事件，防止脚本注入回灌自己的热键造成死循环；外来注入输入仍参与匹配。
- 服务未运行时拒绝为 `invalid_state:input service is not running`；空批次为 `invalid_contract`。

## 订阅与 chord

见 [`hotkey-events.md`](./hotkey-events.md)：`input.subscribe`/`input.bind`/`input.unbind` 与全局 Hook 共用同一订阅与取消契约。
