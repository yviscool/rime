# Registry API

状态：`implemented`（Rime 原生表面，含接线）：`registry.read`、`registry.write`（`set`/`createKey`/`delete`/`deleteKey` 四 op）、`registry.view()`/`registry.setView()` 均已具备 Native service、Action executor、`rime:registry` 模块、SDK 门面与三层 contract（`tests/native/registry_tests.cpp`、`tests/js/registry_slice.cpp`、`tests/sdk/registry.test.ts`）；`contracts/registry/actions.json` 的 action/capability 行、`Bootstrap` 接线与 `production_capabilities()`、`sdk/src/modules.d.ts` 的 `declare module "rime:registry"`、`docs/api/README.md` 的领域表行均已落地（逐项见"实现状态"）。

## API 清单

| 调用 | 形态 | 执行 lane | capability | AHK 对应 |
| --- | --- | --- | --- | --- |
| `registry.read(key, name?, options?)` | Promise，直读服务（不入队） | worker lane | `registry.read` | `RegRead` |
| `registry.write(payload, options?)` | Promise，Action | Dispatcher → Kernel → executor | `registry.write` | `RegWrite` / `RegCreateKey` / `RegDelete` / `RegDeleteKey` |
| `registry.view()` | 同步读状态（SDK 门面 async 直通） | 调用线程 | 无 | 读 `SetRegView` 状态 |
| `registry.setView(view)` | 同步写状态（SDK 门面 async 直通） | 调用线程 | 无 | `SetRegView` |

模块导出为 `registry`（注册名 `rime:registry`），四函数 `length` 分别是 `1`/`1`/`0`/`1`。`read` 的第二个参数按类型分流：字符串是 `name`，`undefined`/`null`/对象是 `options`（因此 `read(key, options)` 也合法）。

```ts
type RegistryView = "default" | "64" | "32";
type RegistryValueType = "sz" | "expand_sz" | "dword" | "qword" | "multi_sz" | "binary";
type RegistryValue =
  | { type: "sz" | "expand_sz"; value: string }
  | { type: "dword" | "qword"; value: number }
  | { type: "multi_sz"; value: string[] }
  | { type: "binary"; value: number[] };
type RegistryWritePayload =
  | ({ op: "set"; key: string; name?: string } & RegistryValuePayload)
  | { op: "createKey"; key: string }
  | { op: "delete"; key: string; name?: string }
  | { op: "deleteKey"; key: string };
interface RegistryWriteResult { key: string; op: "set" | "createKey" | "delete" | "deleteKey"; }
```

`name` 省略或 `""` 指默认值；`key` 接受短别名（`HKLM`/`HKCU`/`HKCR`/`HKU`/`HKCC`）与长形式（`HKEY_CURRENT_USER`…），大小写不敏感，规范输出为短别名。`payload.key` 与 Action target id 必须逐字相等：target 是 `{"kind": "registry", "id": <payload key>}`，排队中的 Action 不必重新解析 payload 即可被检查。

## 源码证据

- `rime-research/AutoHotkey-alpha/source/lib/functions.h:263` 只注册了 `md_func(SetRegView, ...)`；`RegRead`/`RegWrite`/`RegCreateKey`/`RegDelete`/`RegDeleteKey` 是 `source/script.cpp:108-112` 的 `BIF*` 注册，实现位于 `source/script_registry.cpp`（`RegRead` 186、`RegWrite` 365、`RegCreateKeyEx` 396、`RegDeleteKey` 538/574/576，`BIF_Reg` 分发在 632）。逐函数矩阵行见 `docs/api/coverage.json`、`docs/api/core-builtins.json`（`RegRead` 等）与 `docs/api/compatibility-matrix.md`。
- Rime 实现：`engine/win32/src/registry.cpp`（service 与 `parse_registry_key`）、`engine/win32/src/registry_executor.cpp`（`registry.write`）、`engine/win32/js/src/registry_module.cpp`（`rime:registry`）、`sdk/src/registry.ts`（SDK 门面）。
- Win32 表面全部来自 `advapi32`：`RegOpenKeyExW`、`RegQueryValueExW`、`RegSetValueExW`、`RegCreateKeyExW`、`RegDeleteValueW`、`RegDeleteKeyW`、`RegEnumKeyExW`、`RegDeleteTreeW`；链接在 `engine/win32/CMakeLists.txt` 显式 `target_link_libraries(rime_win32 PRIVATE advapi32)`。

## TS 类型

`sdk/src/registry.ts` 导出 `RegistryView`、`RegistryValueType`、`RegistryValue`、`RegistryValuePayload`、`RegistryWritePayload`、`RegistryWriteResult`、`RegistryBridge` 与 `registry` 门面。门面动态 `await import("rime:registry")`（同 `clipboard.ts`），因此四个方法都返回 Promise：

```ts
registry.read(key, name?, options?): Promise<RegistryValue>;
registry.write(payload, options?): Promise<RegistryWriteResult>;
registry.view(): Promise<RegistryView>;     // native 侧同步，仅动态 import 使其异步
registry.setView(view): Promise<RegistryView>;
```

`RegistryBridge` 是模块契约：`read`/`write` 接收 `NativeActionOptions`，`view()`/`setView()` 在原生侧**同步**返回 `{view}`（`tests/sdk/registry.test.ts` 用 `RegistryBridge` 的赋值断言把它钉住）。模块声明在 `sdk/src/modules.d.ts`：

```ts
declare module "rime:registry" {
  const registry: import("./registry").RegistryBridge;
  export { registry };
}
```

## 底层实现

- `RegistryService`（`engine/win32/include/rime/win32/registry.hpp:51`）：`read`、`write_value`、`create_key`、`delete_value`、`delete_key`、`view`、`set_view`，全部 `[[nodiscard]]` 返回 `rime::core::Error`。
- `parse_registry_key`（`registry.hpp:39`、实现 `registry.cpp:328`）是纯函数：无 OS 调用、无 locale、ASCII fold；产出 `RegKeyParts{hive, subkey}`，**不暴露 `HKEY`**。
- 类型映射：`sz`→`REG_SZ`、`expand_sz`→`REG_EXPAND_SZ`、`dword`→`REG_DWORD`、`qword`→`REG_QWORD`、`multi_sz`→`REG_MULTI_SZ`（UTF-16 双 NUL）、`binary`→`REG_BINARY`。`multi_sz`/`binary` 走字节/元素逐个校验（空元素、非 0..255 字节均在写之前拒绝）。Win32 返回的其他类型（如 `REG_LINK`）不被本 runtime 承载，读回 `Unsupported`。
- executor（`engine/win32/src/registry_executor.cpp:109`）在 `execute` 内重新取 `action.payload` 并逐字段复校（不信任模块层的校验）；所有 payload 错误带 `registry.write payload ` 前缀（`registry_executor.cpp:22`），保证与 service 文案可区分。
- 视图：`view_` 默认 `"default"`（OS 原生字长），`"64"`/`"32"` 对应 `KEY_WOW64_64KEY`/`KEY_WOW64_32KEY` 标志，每次调用取一次快照（`view_snapshot()`），单次操作不会跨视图。

## 线程和资源所有权

- `RegistryService` 每次调用自开自关：`HKEY` 由 `.cpp` 内的 RAII guard 持有，**没有 `HKEY`、缓冲区或指针离开 service**；JS 只收到 JSON 化的 `type`/`value`。
- 服务实例由 Bootstrap 持有，模块 binding 与 executor 只拿裸引用；生命周期与 `Bootstrap::stop()` 同进退（先停模块与 executor，再销毁 service）。
- `view_` 由 `mutable std::mutex view_mutex_` 保护，锁只在取/换视图时持有，**不跨 Win32 调用持锁**；并发 `set_view()` 与 `read()` 不会产生撕裂视图。
- 注册表调用不触碰 HWND/COM/DirectComposition，因此不占用 UI Thread，也不需要 COM Apartment；executor 在 dispatcher 的 worker lane 上执行（`tests/native/registry_tests.cpp:135` 注明 harness 例外：它在主线程执行）。
- 文本统一 UTF-8 在 JS 边界，服务内转 UTF-16；含嵌入 NUL 的键、名、值在编码阶段就拒绝，不靠 Win32 截断。

## 异步/取消

- **读**（`registry.read`）：经 `start_async` 在 worker lane 执行一次服务调用，绑定 `cancellationId`/`AbortSignal` 可取消（拒绝 `cancelled`）；**`deadlineMs` 会被解析但不施加**——读不排队、不重试，只有一个不可中断的 OS 调用，故没有 deadline 语义（与窗口读路径的文档化豁免一致）。
- **写**（`registry.write`）：`make_action` 把 `deadlineMs` 折算为绝对 `deadline_unix_ms`（默认 5000ms，饱和运算防溢出），经 Dispatcher 有界队列 → Kernel（契约校验 → 取消检查 → 入口 deadline → capability → executor → 执行后复查）→ 结果；入队前/执行前/执行后的取消与过期分别拒绝 `cancelled` / `timeout`（副作用可能已发生时消息会声明）。
- **视图**：同步状态，无取消、无 deadline。
- 重入：同一 `rime:registry` 调用之间不存在共享可变缓冲；队列满载、重复事件与合并策略由集中调度层承担，registry 不自设节流。

## 权限

- `registry.read`：`registry_read` 在 worker lane 的工作体里先 `kernel->allows("registry.read")`（`registry_module.cpp:125`），拒绝为 `capability_denied`，消息 `required capability was not granted: registry.read`。
- `registry.write`：capability 由 Kernel 在 executor 前统一校验（`registry.write` 既是 action type 也是 capability 名），拒绝为 `capability_denied` 且**不进入 executor**（`tests/native/registry_tests.cpp` 断言拒绝后值保持被接受 Action 写入的内容）。
- `view()`/`setView()`：**不设 capability**——它只改变本服务实例的读写视图，不读也不写注册表内容。
- 两个能力名的字面量集中在 `registry_module.cpp:21-22`，供台账 `capabilities[].declared` 指向；`checked` 点为 `registry_module.cpp:125`（读，在 worker lane 的工作体内）与 `engine/action/src/kernel.cpp:128`（写，executor 之前；既有能力行填的是 `:97`，那是 envelope 校验行，属于历史填法）。

## 错误

`rime::core::Error::Code` → JS `ActionError.code`：`invalid_contract`、`target_gone`、`unsupported`、`execution_failed`、`capability_denied`、`cancelled`、`timeout`。稳定文案（测试逐字断言，改动即契约变更）：

| Code | 文案 |
| --- | --- |
| `invalid_contract` | `registry key must not be empty`、`registry key must start with HKLM, HKCU, HKCR, HKU or HKCC`、`registry key must not contain an empty path segment`、`registry <key\|name\|value> is not valid UTF-8: <text>`、`registry view must be "default", "64" or "32"`、`registry hive root cannot be deleted: <key>`、`registry dword value is out of range: 0..4294967295`、`registry value must not contain an embedded NUL`、`registry multi_sz element must not be empty` |
| `target_gone` | `registry key does not exist: <key>`、`registry value does not exist: <name\|(default)>` |
| `unsupported` | `registry value type is not supported: <名字\|Win32 数字>` |
| `execution_failed` | `registry key still has subkeys: <key>`、`registry read returned a malformed value`、`registry <open\|read\|write\|create\|delete> failed (Win32 error <n>)` |

executor 自身的 payload 错误统一加前缀 `registry.write payload `：`must be a JSON object`、`requires a string op`、`requires a non-empty string key`、`target id must match the payload key`、`name must be a string`、`requires a string type`、`requires a value for op set`、`does not support op: <op>`、`op createKey does not accept name, value or type`、`op delete does not accept value or type`、`op deleteKey does not accept name, value or type`、`does not support type: <t>`、`value must be a string for type <t>`、`value must be a number for type <t>`、`value must be an integer in 0..9223372036854775807 for type <t>`、`value must be an array for type multi_sz`、`value elements must be strings for type multi_sz`、`value must be an array for type binary`、`value elements must be integers 0..255 for type binary`。非前缀的 executor 文案：`unsupported action type: <t>`、`registry.write requires target kind 'registry', got: <k>`；取消为 `action was cancelled before execution` / `after execution`；成功 `detail` 为 `registry updated`，`value` 为 `{key, op}`。

同步抛出（不入队）：`TypeError` 承载 arity/key 形状/视图取值（`read(key, name?, options?)`、`write(payload)`、`setView(view): view must be "default", "64" or "32"`）。

## Trace

只有 `registry.write` 进 Trace（`ActionStarted`/`ActionFinished`，含被 executor 拒绝的 Action——它们以 `invalid_contract` 结束但仍计入两次）。`registry.read`、`registry.view()`、`registry.setView()` 是观察/状态路径，**不产生任何 Trace 条目**，这是文档化豁免而不是遗漏。`tests/js/registry_slice.cpp:338-339` 断言一次完整往返恰有 5 条 `registry.write` `ActionStarted` 与 5 条 `ActionFinished`（`createKey`、`set`、被拒绝的坏 op、`delete`、`deleteKey`），并在随后的空 capability 运行时证明两道门禁都拒绝。

## 设计取舍

- **`set` 不隐式建键**：AHK `RegWrite` 会隐式创建键，Rime 要求先 `createKey`。理由：Action 的副作用必须由 payload 唯一预测（可检查、可重放），隐式建键会让"同 payload 不同结果"取决于世界状态。
- **`deleteKey` 拒绝非空键而不是部分删除**：Win32 `RegDeleteKey` 的子键存在行为依赖实现细节；本实现先 `RegEnumKeyExW` 检查并返回 `registry key still has subkeys`，结果不依赖"第一个子键是谁"。
- **hive root 不可删**：`HKCU` 单独成键时 `deleteKey` 返回 `invalid_contract`（`registry hive root cannot be deleted: HKCU`），不把根拒绝交给 OS。
- **视图不设 capability**：`compatibility-matrix` 中 `SetRegView` 的 capability 列是 `runtime`——这正是台账表达"无 OS capability、纯运行时服务状态"的既有取值（`Sleep`/`DateAdd`/`SplitPath` 等 42 项同列），与"注册表视图不设 capability"的实现一致，故该列无需修正。
- **dword/qword 的 JSON 范围**：payload 接受 `[0, 2^63)`（`json_uint64` 拒绝负数、非有限与 `>= 9223372036854775808.0`），宽度由 service 施加（dword 越界 → `registry dword value is out of range: 0..4294967295`）。`> 2^53` 的整数经过 JSON double 会丢精度——这是 JSON 载体的既有限制，已在 `binary`/`qword` 的取值建议中回避（大数据请用 `binary`）。
- **SDK `view()`/`setView()` 是 async**：原生同步，但门面通过动态 `import` 取模块，无法同步返回；`RegistryBridge` 层保持同步以便同步路径复用。
- **读不施加 deadline**：见"异步/取消"，单次 OS 调用没有可中断点，给出的 deadline 只会是假承诺。

## contract / native / stress 测试

- `tests/native/registry_tests.cpp`（`add_test NAME rime_win32_registry_service`，L5）：`parse_registry_key` 6 项；6 种类型的服务写读 + **逐类型 raw advapi32 观察**（`REG_SZ` 的 UTF-16 含 NUL、CJK 尺寸、`REG_DWORD`=42、`REG_QWORD`=4294967296、`REG_MULTI_SZ` 双 NUL、`REG_BINARY` 字节）；dword 越界与空 multi 元素的稳定文案；缺键/缺值/重复删除的 `target_gone`；非空键拒绝 `registry key still has subkeys`；hive root 删除拒绝；视图合法/非法；OS 注入的 `REG_LINK`（类型 100）读回 `unsupported`；executor 往返 4 个成功 Action（`createKey`/`set`/`delete`/`deleteKey`，`detail == "registry updated"` 且逐次 OS 观察）、`set` 不建键的拒绝、8 项 payload 契约违规的逐字文案、空 capability Kernel 的拒绝且值保持不变。采用"捕获 → 清理 → 断言"，fixture 为 `HKCU\Software\Rime\RegistryTest_<pid>`，`RegDeleteTreeW` 只删该子树（`Software\Rime` 是共享目录，永不删除）。
- `tests/js/registry_slice.cpp`（`add_test NAME quickjs_registry_slice`，L5）：生产装配（`rime:registry` 模块 + `registry.write` executor + 真实 service）6 段：① arity/键形同步 `TypeError` 与坏 hive、缺键异步文案 ② `createKey` 落地（OS 观察）③ `set` 经 Action pipeline 写入并 `read` 回读 ④ 视图同步状态与未知视图 `TypeError` ⑤ executor payload 校验以 `invalid_contract` 返回且零副作用 ⑥ `delete` → `target_gone` → `deleteKey`；随后 fixture 先删再断言，5/5 Trace 计数，第二个空 capability 运行时独立验证两道门禁（JS lane 进程独占，必须在第一个 `runtime.stop()` 之后）。
- `tests/sdk/registry.test.ts`：mock `rime:registry`（环境替身，不被断言），验证门面的参数透传（key/name/options）、`deadlineMs` 折算、`ActionError` 携带原 `code`/`message`、已中止信号不触桥、`view`/`setView` 直通，以及两条 `@ts-expect-error` 负类型断言与 `RegistryBridge` 赋值相容性。
- 未覆盖：真实 WOW64 重定向差异（32 位视图在 64 位测试进程里读到同一份 HKCU）、Win32 错误 234/重试上限、registry 动作的排队满载压力路径（由通用调度压力测试承载，registry 不另设队列）。

## 实现状态

`implemented`（本域文档、三层代码/测试与接线全部落地）。逐项核对：

1. `contracts/registry/actions.json`：action `registry.write` 已登记（`validation` `engine/win32/src/registry_executor.cpp:128`、`executor` `:110`、`module` `engine/win32/js/src/registry_module.cpp`、`sdk` `sdk/src/registry.ts`、`tests` 两层），`capabilities` 中 `registry.read`/`registry.write` 均为 `implemented` 并带 `declared`/`checked`/`actions`。
2. `engine/win32/js/src/bootstrap.cpp`：成员 `registry_service_`/`registry_binding_`、`register_modules` 调 `register_registry_module`、`register_executors` 调 `kernel_.register_executor("registry.write", ...)`、`production_capabilities()` 含 `"registry.read"`/`"registry.write"`（20 项）。
3. `sdk/src/modules.d.ts`：`declare module "rime:registry"` 已声明，`bun run typecheck` 通过。
4. `docs/api/README.md` 领域表已加 `registry.md` 行；`docs/api/runtime-language.md` 的 `windows.registry.read`→`registry.read`、`@rime/registry` 台账命名两处漂移已修正。
5. 构建登记（已完成，供评审核对）：`engine/win32/CMakeLists.txt`（`src/registry.cpp`、`src/registry_executor.cpp`、`PRIVATE advapi32`）、`engine/win32/js/CMakeLists.txt`（`src/registry_module.cpp`）、`tests/native/CMakeLists.txt`（`rime_registry_tests` → `add_test NAME rime_win32_registry_service`）、`tests/js/CMakeLists.txt`（`rime_registry_slice` → `add_test NAME quickjs_registry_slice`）。

`bun run test` 全绿（`matrix:check` 报 `Production bootstrap registers all 38 implemented action types`），`quickjs_registry_slice` 与 `rime_win32_registry_service` 均通过。
