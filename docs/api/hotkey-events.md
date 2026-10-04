# Hotkey, Hook and Event API

状态：`implemented`。全局输入 Hook、chord 绑定（`input.subscribe`/`input.bind`/`input.unbind`，含自注入抑制）与本轮 M2-C 的声明式事件族均已实现并通过 contract：`input.hotkey`、`input.hotstring`、`input.hotIf`/`input.hotIfWinActive`/`input.hotIfWinExist`/`input.hotIfWinNotActive`/`input.hotIfWinNotExist`、`input.installKeybdHook`/`input.installMouseHook`、`input.setTimer`、`input.onMessage`、`input.onClipboardChange`、`input.onError`、`input.onExit`；M2-D 追加 `input.createInputHook`（`InputHook` 对象 23 成员）、`input.suspend(on?)`、`input.policy(snapshot?)` 与注册选项 `{ on, suspendExempt, inputLevel }`。contract 见 `tests/js/input_slice.cpp`、`tests/js/events_slice.cpp`。

源码证据：`lib/functions.h` 的 `Hotkey`、`Hotstring`、`InstallKeybdHook`、`InstallMouseHook`、`SetTimer`、`OnMessage`、`OnExit`、`OnError`、`OnClipboardChange`、`HotIf*`；`source/hotkey.cpp`（`Hotkey` 匹配与 `Hotstring::ParseOptions` ~2500-2620）、`source/hook.cpp`（Hook 线程与 `g_MaxHistoryKeys`）、`source/script2.cpp`（`OnMessage`/`OnExit` 装载）、`source/input_object.cpp`。实现：`engine/win32/js/src/events_module.cpp`（全部事件导出与调度）、`engine/win32/js/src/input_module.cpp`（`subscribe`/`bind`/`unbind`）、`engine/win32/src/input.cpp`（LL Hook、自注入标记、key history）、`engine/win32/src/context_watcher.cpp`（HotIf 窗口快照）、`engine/js/src/host.cpp`（error observer、exit handler、teardown、ABI busy 检查）。

TS 面：`sdk/src/input/types.ts` 的 `InputBridge`（`EventSubscription`、`HotkeyEvent`、`MessageEvent`、`ClipboardChangeEvent`、`ErrorEvent`、`ExitEvent`、`HotIfDescriptor`、`RegistrationOptions`、`DispatchPolicy`、`createInputHook`/`suspend`/`policy` 扩展），以及 `InputHook` class + interface 合并（构造委托 `input.createInputHook`，`objects.json` 的 23 成员中除 `__New` 外全部由 interface 声明）。

## 派发模型（线程与所有权）

- **事件入口只有受监管的 Win32 message pump**。Hook 线程只做匹配前的记录与标记（自注入、拦截位），把不可变事件投递到 Host event queue；Timer、窗口消息、剪贴板通知同样先入 pump/queue。Hook 线程与 UI Thread 都不直接进入 QuickJS，JS 回调一律经 `Host::invoke_callback` 在 JS Thread 执行。
- **HotIf 分两层**：窗口条件（`HotIfWin*`）由 UI lane 的 ContextWatcher 预先采集快照，Function 条件（`hotIf(fn)`）在 JS Thread 求值，参数为 `{ active, seq }`；抛异常或返回非布尔一律 fail-closed，并经 `onError` 记录 `rime:input.hotIf`。
- **Hotstring 匹配流**由 Hook 侧事件驱动（字母/数字累积、退格回退、修饰键/鼠标复位），替换文本经 `input.send` 以 Action 提交，因此注入有 capability、有 Trace、可被取消；自注入事件不参与匹配，防止回灌。
- **临界区与合并**：同一 tick 到期的 Timer 按 priority 降序、同序按注册顺序执行；tick 在队列中或执行中时后续触发被合并（不排队列）。`onMessage` 达到 `maxInstances` 上限后丢弃新消息而不排队（AHK 同规则）。
- **调度策略集中定义（M2-D）**：容量半（`#MaxThreads` → `maxConcurrency`、`#MaxThreadsPerHotkey` → `maxConcurrencyPerHotkey`、`#MaxThreadsBuffer` → `overflow`）与匹配半（`inputLevel`、suspend、`#HotIfTimeout` → `hotIfTimeout`）统一实现在 `engine/core/include/rime/core/scheduler_policy.hpp` 的 `SchedulerPolicy`，由 `input.policy()` 暴露唯一可读快照；Hook、Timer、模块内部不得各自散落一套上限或合并逻辑。容量拒绝计入 `dropped`（可观察，不静默），匹配半只过滤不计数。

## 统一 Subscription

每个注册都返回同一形状的对象：

```js
const sub = input.hotkey("ctrl+shift+k", fn);  // { id, kind, close() }
sub.close();   // 幂等：首次释放返回 true，重复/已卸载返回 false
```

- `kind` ∈ `hotkey | hotstring | timer | message | clipboard | error | exit`；同一 id 在 `SubscriptionRegistry` 内唯一（注册 id 与 Host callback id 共用一个计数空间）。
- `hotkey`/`hotstring` 按（criterion, options, trigger/chord）去重：重复注册返回**同一 id**、就地替换 action；每次 `hotkey()` 返回的是**新的 JS 对象**，比较请用 `id` 而不是对象相等。
- 关闭语义：`close()` 先拒绝新事件，再取消并等待在途回调；`runtime.stop()` 之后 Host teardown 按"Hook/dispatch → hotkey/hotstring → timer → onMessage → clipboard → onError/onExit → HotIf 条件"顺序释放，`open_count()` 归零，未释放引用产生诊断。
- **`onExit` 不进 `SubscriptionRegistry`**：等待中的退出处理器不得阻塞卸载，`HostAbi::unload` 的 busy 检查把它排除在外，并把它作为卸载的第一个干净步骤执行。

## API 契约

### `input.hotkey(name, action, options?)`

```js
input.hotkey("ctrl+shift+k", () => trace("fired"));
input.hotkey("f24", { type: "input.send", capability: "windows.input.inject",
                      target: { kind: "input", id: "keyboard" },
                      payload: [{ vk: 65, down: true }, { vk: 65, down: false }] });
input.hotkey("f24", "off");            // 控制形式：注册表里没有这个名字即抛 TypeError
```

- chord 语法同 `bind`：Rime 文法（`ctrl+shift+k`、`f24`）与 AHK 单字符修饰前缀（`^ ! # +`）均可，修饰位精确匹配。
- 第一匹配生效（first-match-wins），`hotIf` 条件不满足时落到下一个候选；注册表快照后求值，条件回调内可增删注册。
- 能力门禁：`windows.hook.global`（注册、控制形式均在门禁后）。畸形 chord/action/control word → 同步 `TypeError`；能力缺失 → 同步 `Error`（消息含 capability 名）。
- observer 形式收到 `{ name }`；action 形式走 Action 管道（`source: rime:input`，随 Trace 可查）。

### `input.hotstring(spec, action?, onOff?)`

```js
input.hotstring(":*:btw::by the way");        // 省略 action：注入替换文本
input.hotstring("::btw2::replaced", fn);      // observer：只通知，不擦除、不注入
input.hotstring(":*:", { ...defaults });      // 选项-only：改写默认项，返回 null
input.hotstring("::btw::by the way", "off");  // 控制形式
input.hotstring("EndChars", " ");             // 全局设置：读/写，返回值
input.hotstring("MouseReset", false);
input.hotstring("Reset");                     // 只清匹配缓冲
```

- 规格形式：`:` + `options` + `:` + `trigger` + `::` + `replacement`。选项字母：`*`（通配，无结束字符也触发）、`?`（词内）、`B`（退格）、`C`/`C0`/`C1`（大小写/跟随输入）、`O`（省略结束字符）、`Z`（触发后复位）、`X`（见偏差）；`R`/`T` 接受为 no-op（始终 raw）；`K`/`P`/`S` 拒绝。选项-only 形式改写 `option_defaults`，只影响之后的注册。
- 默认项：`B`、`Z`、`C0`（conform）开，结束字符 `-{}[]()';:'"\`\\,.?!` + 空白。
- 能力门禁：注册需要 `windows.hook.global`；省略 action 的替换形式额外需要 `windows.input.inject`（注册时即拒绝，而不是失败在触发时）。设置/选项-only 形式无门禁（纯状态，AHK 同）。
- 去重键 = criterion + 大小写敏感 + 词内 + trigger；重复注册替换选项与 action，返回同一 id。

### `input.hotIf*` 条件

```js
const previous = input.hotIf(ev => ev.active?.exe === "notepad.exe");
input.hotIfWinActive("Rime - .*");       // title(正则) / className / processName
const back = input.hotIfWinExist("ahk_exe notepad.exe");
input.hotIf(null);                       // 回到无条件，返回之前生效的描述符
```

- 返回**之前生效**的描述符 `{ kind, id }`，`kind ∈ none | function | winActive | winExist | winNotActive | winNotExist | gone`。
- 相同函数引用（或相同窗口查询三元组）复用既有 criterion；criterion 只在没有注册引用时释放。

### `input.installKeybdHook(install?, force?)` / `installMouseHook`

- `install`（默认 `true`）与 `force`（默认 `false`，即使仍有订阅流也强制卸下）均为布尔；非布尔 → `TypeError`；安装需要 `windows.hook.global`。
- 返回**布尔：当前生效的安装状态**——这是对 AHK `void` 返回的有意偏离，使"强制卸下但流仍在"这类状态可观察。

### `input.createInputHook(options?, endKeys?, matchList?)` — InputHook 对象（M2-D）

```js
const ih = input.createInputHook("C L8", "{Esc}", "ok,xy"); // AHK InputHook(...) 同形三参
ih.KeyOpt("{Esc}", "+E");   // KeyOpt(keys, keyOptions)：选项字母 + - E I N Z，"{All}" 作用于全部键
ih.Start();                // 幂等；开始捕获，等待 OnChar/OnEnd
ih.OnChar = ev => { ... }; // { vk, scan, down, char }；终止键不产生 OnChar
ih.OnEnd   = ev => { ... };// { reason, input, endKey, endMods, match }
await ih.Wait();           // 等到结束，返回 EndReason；已结束立即返回
await ih.Wait(0.3);        // 秒级 deadline（同 AHK Timeout 语义）
ih.Timeout = 1.5;          // 秒：负数 RangeError，写入后原样读回
ih.MinSendLevel = 2;       // 负数夹到 0（同 AHK MinSendLevel）
ih.Stop();                 // 停止捕获并触发 OnEnd(reason "Stopped")
```

- **23 个可枚举成员**与 `objects.json` 对齐：`__New`、`KeyOpt`、`Start`、`Stop`、`Wait`、`Timeout`、`EndKey`、`EndMods`、`EndReason`、`InProgress`、`Input`、`Match`、`MinSendLevel`、`NotifyNonText`、`OnChar`、`OnEnd`、`OnKeyDown`、`OnKeyUp`、`BackspaceIsUndo`、`CaseSensitive`、`FindAnywhere`、`VisibleNonText`、`VisibleText`；另有非枚举内部句柄 `id`（`Object.keys` 不可见）。TS 侧 `InputHook` 是 class + interface 声明合并，构造委托 `createInputHook`，不承诺 `instanceof`/原型链。
- **构造参数按 AHK `InputHook(Options, EndKeys, MatchList)`**，先校验后提交：未知选项字母（`Q`）、非字符串 options、未知 EndKey（`f99`）、含空元素的 MatchList（`','`）都在任何状态提交前同步 `TypeError`。选项串字母：`B`（关退格撤销）、`C`（大小写敏感）、`I[n]`（MinSendLevel 0..101）、`L<n>`/`L-<n>`（缓冲上限，负数夹 0）、`T<秒>`（Timeout，负数不触发）、`V`（可见文本+非文本）、`*`（FindAnywhere）、`H`/`M`/`E` 接受为 no-op（偏差）。
- **门禁**：构造与属性读写不需 capability；`Start()` 需要 `windows.hook.global`（与 `input.hotkey` 注册一致，参数校验先于门禁）。
- **状态**：`EndKey`/`EndMods`/`EndReason`/`InProgress`/`Input`/`Match` 只读（赋值 `TypeError`）；`On*` 接受函数/`null`/`undefined`（清除），非函数赋值 `TypeError`，未设置时读回 `undefined`。新对象初始为 `EndReason === 'Stopped'`、`InProgress === false`、空 `Input`/`EndKey`/`EndMods`/`Match`、`Timeout === 0`、`MinSendLevel === 0`。
- **EndReason**：`EndKey`（EndKeys 命中，`EndKey` 为小写 chord 名、`EndMods` 为 `<^ <! <+ <#` 形式）、`Match`（MatchList 命中，`Match` 为命中文本）、`Timeout`、`Stopped`（`Stop()`）、`Full`（缓冲满）、`Language`、`MinSendLevel`。`Wait()` 对已结束的对象立即返回 `EndReason`。
- 捕获流与 hook 同路：`feed_input_hooks` 先于自注入过滤，因此 InputHook 能看到本进程注入的按键；suspend 与注册 inputLevel **不**过滤捕获（只过滤 hotkey/hotstring 匹配），捕获自身用 `MinSendLevel` 与事件级别比较。

### `input.suspend(on?)`（M2-D）

```js
input.suspend(false);          // 归一到 off，返回 false（没有只读接口，用它恢复）
input.suspend("on");           // true；亦可 "off" / "toggle"
input.suspend();               // 无参 = toggle（AHK Suspend 的开关语义）
input.hotkey("f16", fn, { suspendExempt: true }); // 挂起期间照常触发
```

- 参数为布尔或 `"on"|"off"|"toggle"`；数字/其他字符串同步 `TypeError`；返回**生效后的挂起状态**。无参（或 `undefined`/`null`）翻转——无法在不改变状态的前提下读取，恢复/归一请用 `input.suspend(false)`。
- 语义 = AHK `Suspend`：只关闭 **hotkey/hotstring 匹配**（经中心 `SchedulerPolicy::dispatch_allowed`，`suspendExempt` 注册豁免）；Timer、`onMessage`、InputHook 捕获、key history 不受影响，也不计丢弃（匹配规则，非负载拒绝）。无 capability 门禁。

### `input.policy(snapshot?)`（M2-D）

```js
input.policy();                                  // 读
// { maxConcurrency: 0, maxConcurrencyPerHotkey: 1, inputLevel: 0,
//   hotIfTimeout: 1000, overflow: "coalesce" }
input.policy({ maxConcurrency: 4, overflow: "reject" }); // 部分提交
```

- 五字段与 `directives-and-syntax.md` 的指令一一对应（`#MaxThreads`/`#MaxThreadsPerHotkey`/`#InputLevel`/`#HotIfTimeout`/`#MaxThreadsBuffer`）。无参返回当前快照；传入对象则**先整体校验后提交**：未知字段、负数/非整数、`overflow` 不在 `reject|dropOldest|coalesce`、非对象（数字等）均同步 `TypeError`，且不改变任何现值（fail-closed）；省略的字段保持原值。无 capability 门禁。

### `input.setTimer(fn, period?, priority?)`

- `period` 毫秒：`0` 删除该 timer 并返回 `null`；负数一次性执行；省略沿用既有 period（新 timer 默认 `250`）。`priority` 越大越先执行（同 tick 内），省略沿用既有值。
- 按函数引用去重：同 `fn` 再次注册是就地更新。回调收到 `{}`。非函数/非整数 → `TypeError`。

### `input.onMessage(msgNumber, fn, maxInstances?)`

- 回调收到 `{ msg, wParam, lParam, hwnd }`（pump 观察到消息后投递到 JS Thread）。`maxInstances` 默认 1、上限 255，超限丢弃；`maxInstances: 0` 删除该 `(msg, fn)` 组合并返回 `null`；同组再注册是就地更新上限。
- 需要运行中的 message pump；`msgNumber` 必须落在 uint32、`maxInstances` 不得为负，否则 `TypeError`。

### `input.onClipboardChange(fn)` / `input.onError(fn)` / `input.onExit(fn)`

- 剪贴板：`{ type: 1 }` 表示本进程写入，`{ type: 0 }` 表示外来变更；需要 `windows.clipboard.read`、clipboard service 与运行中的 message pump。每个 listener 只收到自己注册的那次变更（无 fan-out）。
- 错误：`{ where, message }`；observer 自身抛错会被再入，但 Host 在投递期间记录不再排队，因此失败的 observer 是有界的（contract 断言 `1 <= count <= 3`）。
- 退出：`{ reason }`（`runtime.stop()` → `"shutdown"`），在 JS Thread 上于 teardown 之前执行；见上文"统一 Subscription"的 busy 例外。

## 偏差与不实现项（报告口径）

- `install*Hook` 返回布尔有效状态（AHK `void`）。
- `hotIfWin*` 的三个位置参数是 `(title, className, processName)`，与 AHK `HotIfWinActive(WinTitle, WinText)` 的 (title, text) 不同；正则语法沿用 Rime 的窗口查询。
- `hotkey` 名字不支持 AHK 的 `~`（透传）、`*`（通配热键）、`$`（强制走 Hook）前缀与 `<^`/`>`/`<#` 这类左右修饰前缀，统一抛 `TypeError`；`SendLevel` 尚未实现，输入级别经注册选项 `inputLevel`（事件级别固定 `0`）与 `input.policy().inputLevel` 实现（M2-D）。
- Hotstring 的 `X`/`X0` 会被解析并接受，但本阶段**不做表达式执行**：函数/action 行为一律由第二个参数承载；省略 action 时按文本注入。`R`/`T` 接受为 no-op（始终 raw）。`K`/`P`/`S` 拒绝。
- `onMessage` 为同步注册 + 队列投递，不提供 AHK 的 `OnMessage(msg, fn, "L")` 低优先级变体；负数 `msgNumber` 抛 `TypeError`（AHK 亦要求合法消息号）。
- `Hotstring` 的结束字符只在默认集与 `EndChars` 覆盖之间选择，不实现 AHK 的 `?` 结束字符语义扩展；MouseReset 关闭后鼠标移动不再复位匹配流。
- 未实现：`Hotkey` 的 `$` 强制 Hook 与 `#UseHook` 的强制走 Hook 路径（`Suspend` 已实现为 `input.suspend`，见上）。`#MaxThreads*`/`#InputLevel`/`#HotIfTimeout` 不以脚本内指令形式接入（脚本指令解析层未落地），其调度语义由 M2-D 的 `input.policy` 与注册选项 `{ on, suspendExempt, inputLevel }` 承载，字段映射见 `directives-and-syntax.md`；计划项与 `audit-gaps.md` 口径同步。
- InputHook 偏差：构造选项串中 `H`/`M`/`E` 与 `KeyOpt` 中 `S`/`V` 接受为 no-op；AHK 的 `ValueError` 统一映射为 `TypeError`（quickjs-ng 无 ValueError，仓库既有口径）；`__New` 由工厂 `input.createInputHook` 承担（TS class 构造委托之）；无 `AbortSignal` 取消，停止一律 `Stop()`；EndKey 返回小写 chord 名（`"escape"`），AHK 混用大小写。

## 测试与契约

- `tests/js/events_slice.cpp`（`quickjs_events_slice`）：boot/导出矩阵 → hotkey 注册/控制形式/关闭 → HotIf（函数、窗口条件、fail-closed）→ Hook 安装与恢复 → Timer（周期、一次性、优先级、删除）→ `onMessage`（投递、`maxInstances` 丢弃、删除）→ hotstring 端到端（observer、替换为 Action IR、控制形式、通配、选项-only）→ `onClipboardChange`（自注入 1 / 外来 0）→ `onError`（promise rejection 的 `{where,message}`、抛错 observer 有界）→ M2-D：InputHook 形状（23 成员、`id` 非枚举、构造四路 TypeError）→ 捕获（Start 幂等、OnChar 载荷）→ EndKey/Match/Timeout/`Wait(0.3)` deadline → KeyOpt/属性契约/OnChar 内重入 `Stop()` → suspend+`suspendExempt`+`inputLevel` → `input.policy` fail-closed+部分提交+往返 → 队列负载 burst（8 次精确投递、`service.dropped_events()` 不变、恢复第 9 次）→ observer 内自关订阅的重入 → `runtime.stop()` 后 `open_count()==0` → capability denied 矩阵（`Start()` 拒于 `windows.hook.global`；`createInputHook`/`suspend`/`policy` 放行）→ `abi.unload` 忙于 InputHook 捕获（`Stop()` 后卸载成功）。
- 压力维度（`audit-gaps.md` #4）：本切片覆盖"队列满载负载"（burst + `InputService` 丢弃计数前后相等）、"回调重入"（OnChar 内 `Stop()`、observer 内 `close()`）、"暂停/恢复"（suspend 段）、"卸载忙"（abi 段）；窗口切换与 Hook 装/卸沿用既有用例。
- `tests/js/input_slice.cpp`：`subscribe`/`bind`/`unbind`、自注入抑制、hook 关断与 key history。
- 事件回调统一依赖 Host 的 subscription/callback 计数与 teardown，卸载失败路径由 `HostAbi` contract 覆盖。
