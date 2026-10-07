# Native Interop and Isolation

状态：默认 `unsupported-by-policy`；需要受信任插件或显式宿主能力才能实现。

源码证据：`functions.h` 的 `DllCall`、`ComCall`、`CallbackCreate`、`CallbackFree`、`ObjGetDataPtr`、`ObjGetDataSize`、`ObjSetDataPtr`，以及 `source/lib/DllCall.cpp`、`source/lib/CCallback.cpp`、`source/script_com.cpp`。

标准 TS API 不暴露裸 `HWND`、`HANDLE`、COM 指针、任意函数指针、对象内存地址或任意 DLL 调用。受控替代必须声明 ABI、参数布局、线程/apartment、权限、取消和卸载条件；存在活动回调或外部引用时 unload 必须失败并给出诊断。

## 1. M8 COM/VARIANT 边界定档（2026-10-07）

`ComObjActive` / `ComObjConnect` / `ComObjFlags` / `ComObjGet` / `ComObjQuery` / `ComObjType` / `ComObjValue` 7 项由 `contract-only` 改判 `unsupported-by-policy`，与既有 `ComCall`/`ComObjFromPtr`/`DllCall` 等同族；`core-builtins.json` 因此 101/101 全终态。逐项理由、替代路径与 `script_com.cpp` 证据列在 [`runtime-language.md`](./runtime-language.md) §2.4，[`stdlib.md`](./stdlib.md) §5 的尾注已同步销注，audit-gaps 的「COM/VARIANT 边界文档化」同条销项。

台账字段（7 项一致，与 `ComCall`/`ComObjFromPtr` 同族对齐）：

| 字段 | 值 |
|---|---|
| `status` | `unsupported-by-policy` |
| `lane` | `plugin-isolated` |
| `async` | `explicit-capability` |
| `capability` | `native.unsafe` |
| `cancellation` | `plugin-defined` |
| `contractTest` | `tests/js/fixtures/policy-refusal.mjs` |
| `compatibilityTest` | `docs/api/runtime-language.md` |

拒绝面：`policy-refusal.mjs` 现守 36 个名字（本台账 25 + coverage 11，无同名重叠），在 `globalThis` 与 11 个 `rime:*` 模块的导出绑定及方法名上一个都解析不到；其中点名的替代必须真的存在——`ComObjGet`/`ComObjActive` → `automation.find`、`ComObjConnect` → `input.subscribe`。

## 2. 六个核心设计问题（`AHK-TS-WINDOWS-API-DESIGN.md` §0）逐条答案

1. **TS 如何表达这一族** —— 不表达。没有任何 `ComObj*` 签名进 `.d.ts`，`VARIANT` 不是类型系统的一部分，脚本里也没有「COM 对象」这个值形态。脚本看到的只有稳定 id 与已归一的值：要「找到并操作一个对象」是 `automation.find/read/invoke`，要「控件身份」是 `control.resolve` → `ControlSnapshot.id`。
2. **底层怎么实现，走哪条 lane** —— `plugin-isolated`：受信任进程内插件（版本化 Host ABI + 能力声明）或不可信进程外插件（序列化参数、无共享指针），COM 只在这两处出现。runtime 自持的 UIA COM 对象活在 Automation MTA 线程（AGENTS 线程与 Apartment 规则），不跨线程封送给 JS；QuickJS 所在的 JS 线程只收稳定 ID 与不可变快照，不接触 `IUnknown`/`IDispatch`。
3. **哪些 API 异步** —— 脚本级这 7 项没有签名，台账 `async: explicit-capability` 的含义是：入口若由插件提供，插件必须自己给 deadline 与取消语义（`cancellation: plugin-defined`），而不是继承 AHK 的同步阻塞。脚本侧可走的路都是 stdlib.md §4 的既有异步面：`automation.find/read/invoke`（`await` + `deadlineMs`/`AbortSignal`）、`input.subscribe`（返回 `Subscription` 而非 Promise，事件回 JS 线程分发）。
4. **什么不暴露给上层** —— 裸 `IUnknown`/`IDispatch` 与任何可调用的 COM 包装；`VARIANT` 本体、类型标签与 `ComObjType`/`ComObjValue` 的解包结果；`ComObjQuery` 的 QI 结果；`ComObjGet`/`ComObjActive` 的 ProgID/CLSID → 对象句柄；`ComObjFlags` 的 `EOAC_*` 封送标志与 Apartment 选择；`ComObjConnect` 的事件 sink 函数指针；ByRef VARIANT 出参。本文件原有的 DLL/地址/函数指针族不变。
5. **若重造 AHK 的 TS 标准库，这一族在哪** —— AHK 把「脚本即 COM 客户端」当语言能力（一行 `ComObjActive("Word.Application")` 就拿到应用对象，事件用 ByRef VARIANT 回调）。TS 标准库不复刻这个语法，复刻它的**意图**：脚本可移植的只有两条动词——「找并操作对象」（automation）与「订阅事件」（`input.subscribe`，可取消 `Subscription`）；需要 Office/Shell 私有协议的地方走带权限声明的插件清单，能力由宿主授予，而不是脚本里多一个全局函数。
6. **若 TS 是 Windows automation language，Windows 怎么建模** —— 主模型是 UIA 元素树（稳定 id、可重新验证身份），Win32 是本进程的快速路径；COM 不是操作系统对象模型，而是少数应用的私有协议。所以模型里没有「COM 对象」这一层：只有 `find/read/invoke` 到达的元素，和插件声明的私有能力。COM 要进入模型，必须先在插件内把它翻译成元素或值——不把指针翻译进 JS。

## 3. 卸载与生命周期

按 AGENTS 的 Host ABI 规则：插件登记的 COM sink、未释放的外部引用、未解决 Promise 与活动 JS 回调都必须让 `unload` 失败并逐项说明原因。`ComObjConnect` 对应的形态在本模型里就是可取消订阅 + 回调计数（`input.subscribe` 同款），不是「脚本注册一个裸回调对象」。

## 4. 测试

- 拒绝契约：`tests/js/fixtures/policy-refusal.mjs`（ctest `quickjs_policy_refusal`，`rime_js_bundle --production`）。已反向验证非恒真：往名单注入真实存在的 `resolve` → 失败 `resolve <- control.resolve`；把替代断言改成 `control.resolveX` → 失败「documented alternative control.resolve is missing」；还原后通过。
- 生产探针（2026-10-07，`rime_js_bundle --production`）：`typeof automation.find === "function"`、`typeof input.subscribe === "function"`、`typeof control.resolve === "function"`、`globalThis.windows.find === undefined`（替代路径确实由生产装配提供，不是文档里的名字）。
