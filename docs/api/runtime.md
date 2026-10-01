# Runtime API（rime:runtime）

状态：`implemented`；由 Host 内建模块提供，contract 覆盖见 `tests/js/runtime_smoke.cpp`、`tests/js/js_smoke.cpp`。

源码证据：`engine/js/src/host.cpp` 的 `runtime_*` 函数；TypeScript 声明为 `sdk/src/index.ts` 的 `RuntimeBridge`。

```js
import { runtime } from "rime:runtime";
```

每个 Host 一份 `runtime` 对象。所有 API 只能在该 Host 的 JS Thread 上调用；模块已随 Host 卸载时调用返回 `InternalError: runtime host is gone`。

## API

### `ping(): string`

同步返回 `"pong"`，用于验证模块绑定已装载。

### `delay<T>(milliseconds: number, value: T, cancellationId?: number): Promise<T>`

- `milliseconds` 经 `ToUint32` 归一；少于两个参数抛 `TypeError`。
- `value` 必须可 `JSON.stringify`，resolve 的是 JSON 往返后的拷贝，不是原引用。
- `cancellationId` 非整数、NaN、Infinity 或超出 2^53-1 抛 `TypeError`；非正数视为不绑定取消。
- timer 由 Host 的 `TimerService` 线程触发，结果在下一次 `drain` 作为 completion 投递并 settle。
- 调用同步产生两项所有权：一个未决 promise（`tasks.async`）和一个 armed timer（`tasks.timers`）。两者都会让 `HostAbi::unload` 失败。
- 绑定的取消 id 被 `cancel` 后：promise 立即以 code `cancelled` reject，armed timer 同步解除，timer 不会再触发。
- timer service 正在停止时：promise 以 code `invalid_state` reject。

### `cancellation(): number`

创建取消 id（从 1 单调递增，不复用）。id 只是记账句柄：它本身不阻塞卸载，也不自动过期；调用方应以 `cancel` 或 `releaseCancellation` 收尾，`context().cancellations` 会如实反映未收尾的数量。

### `cancel(cancellationId: number): boolean`

同步翻转该 id 的 `CancellationSource`，随后在 drain 上 reject 所有绑定该 id 的未决 promise 并解除它们的 armed timer。未知 id 或 `0` 返回 `false`；已 settle 的 promise 不受影响。幂等：重复取消返回 `true` 且无副作用。

### `releaseCancellation(cancellationId: number): boolean`

删除 id 记账。未知 id 返回 `false`。释放**不产生取消效果**：它只让计数归零，进行中的工作继续按原样完成；应在工作结束（无论成功或已取消）后调用。

### `subscribe(callback: (event: unknown) => void): number`

注册一个 runtime-owned callback，返回订阅 id。回调在 Host 事件队列于 `drain` 时把 `(callback_id, JSON payload)` 投递给该 id 时执行，以解析后的 payload 为唯一参数（输入事件当前经 `input.subscribe` 与 chord 注册各自的回调投递）。未关闭的订阅使 `HostAbi::unload` 失败，因此订阅是需要显式释放的所有权。

### `unsubscribe(subscriptionId: number): boolean`

移除订阅。未知 id 返回 `false`；移除后回调不再投递，卸载条件随之解除。

### `inspect(): string`

返回完整 host 状态 JSON，读取不执行任何未知脚本：

- `modules`：已注册 native 模块、已加载文件模块和 file root。
- `functions`：JS 全局对象上的函数名。
- `subscriptions`：活动订阅的标签列表。
- `tasks`：`async`（未决 promise）、`queued`（待投递 completion）、`timers`（armed timer）、`callbacks`（持有的 JS 回调）计数。
- `errors`：`Host::record` 与异常记录的 `{where, message}` 列表。

C++ 侧 `Host::inspect(request)` 支持 `{"kind": "modules" | "functions" | "subscriptions" | "tasks" | "errors"}` 过滤；JS 侧无参，固定返回全部段。

### `context(): RuntimeContext`

每次调用返回一份新建的只读快照（`schemaVersion: 1`），修改返回值不影响后续读取：

```ts
interface RuntimeContext {
  schemaVersion: 1;
  modules: string[];
  tasks: { async: number; queued: number; timers: number; callbacks: number };
  subscriptions: number;
  cancellations: number;
}
```

用于 CLI/调试器轮询所有权，与 `inspect` 的区别是：`context` 面向稳定的 schema，`inspect` 面向完整调试信息。

## 错误格式

所有异步拒绝都是同时携带 `code` 与 `message` 的 `Error`；`code` 取值来自 `rime::core::error_code_name`：

`none`、`invalid_state`、`queue_closed`、`queue_full`、`cancelled`、`timeout`、`capability_denied`、`invalid_contract`、`unsupported`、`execution_failed`、`target_gone`。

原生失败以 `code:message` 成帧投递；前缀必须是精确的 code 名才会被解析为 code，否则整串按 message 处理，因此含 `:` 的消息不能伪造 code。同步参数错误抛 `TypeError`，能力缺失抛带 capability 名的 `Error`。

## 生命周期与 unload

`HostAbi::unload` 在下列任一项非空时以 `InvalidState` 失败，错误消息逐项列出原因：

- outstanding subscriptions
- JS callbacks still held
- unresolved promises
- armed timers
- pending host events

条件解除后 `unload` 成功且幂等；卸载后 `execute` 被拒绝。`exit(code)` 只记录退出码并触发 `on_exit`，不是 unload，资源保持到 `unload` 成功为止。

## 示例

```js
import { runtime } from "rime:runtime";

// 可取消的延时：取消后 timer 不再触发，promise 以 cancelled 拒绝。
const id = runtime.cancellation();
runtime.delay(500, "done", id).then(
  (value) => console.log(value),
  (error) => console.log(error.code, error.message), // "cancelled" ...
);
runtime.cancel(id);
runtime.releaseCancellation(id);

// 所有权轮询：确认没有遗留工作再决定退出。
const snapshot = runtime.context();
if (snapshot.tasks.async === 0 && snapshot.subscriptions === 0) {
  // 可以安全卸载
}
```
