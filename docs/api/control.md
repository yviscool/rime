# `@rime/control` API 设计（落地文档）

> 状态：设计 accepted，Phase 1 待实现。依据：AHK `lib/win.cpp:335-613`
>（ControlClick）、`script_autoit.cpp:339-922`（列表/Edit/复选框族）、
> `window.cpp:810-919`（定位）语义研报 + 本仓库 `window/automation`
> 现有能力研报。`coverage.json` 中 control 域 42 条 `contract-only`
> 是本设计的输入，不是输出——实现以 `actions.json` 条目 +
> contract 测试为准。

## 1. 建模：TS 作为 Windows automation language 时控件是什么

AHK 把控件表达为“窗口 + 一段含糊的 spec 字符串”（ClassNN/文本二义、
失败回退），这是兼容包袱，不是模型。Rime 的模型：

```text
Window（稳定 id，已有）── resolve ──► ControlHandle（稳定 id，同 id 空间）
                                              │
                        ┌─────────────────────┼─────────────────────┐
                        ▼                     ▼                     ▼
                   Direct（Win32 消息）   Semantic（UIA）      查询（同步安全）
```

- 控件 == 窗口 id 空间里的另一个 id（复用 `ControlInfo` + `next_class_nn`，
  `window.hpp:184-191`，裸 `HWND` 永不出 lane）。没有第二套 id 体系。
- 同一个动词按后端分流（`strategy: 'message' | 'uia' | 'auto'`），
  禁止把 UIA、坐标点击、OCR 熔成一个不可解释调用（AGENTS 架构规则）。
- 默认后端 = AHK 语义的后端（大多是 Win32 消息），不是“最新”的后端：
  自绘/游戏控件只认 `PostMessage`，UIA 在那里是死路（AHK 注释实证）。

## 2. TS 表达（`sdk/src/control.ts`，模块 `rime:control`）

```ts
// 入口：从 Window 句柄 resolve，spec 显式区分（AHK 的"末字符数字"猜测不要）。
const win = await Window.find({ title: "记事本" });
const c = await win.control({ classNN: "Edit1" });
//  | { text: "..." } | { hwnd: number } | { point: {x, y}, space: 'window-client' }
//  | { auto: string }   // 兼容档：复刻 AHK 两遍回退（ClassNN -> 文本）
// 句柄级选项（不是逐调用）：{ strategy: 'message' | 'uia' | 'auto' }，默认 'auto'。

// 高频单发快捷方式（Playwright 式 page.click 对应物）：resolve+动作一次往返，
 // 简单脚本不付两跳 ceremony 税。
await win.clickControl({ classNN: "Button1" }, { button: "left" });
await win.setControlText({ classNN: "Edit1" }, "...");
await win.getControlText({ classNN: "Edit1" });  // -> string

// 点击：默认不抢焦点（AHK NA 语义为默认；AHK 兼容档默认 true）。
// 注意这是与 AHK 唯一行为分歧的默认值：移植脚本必须显式传 activate:true，
// 否则原来依赖激活副作用的脚本会静默改变行为。pos 缺省 space 为 control-client。
await c.click({ button: "left", count: 1, phase: "downUp" | "down" | "up",
                activate: false, pos: { x, y, space: "control-client" } });
await c.wheel({ axis: "vertical", ticks: -3 });   // 内部 ClientToScreen

// 文本：setText/getText 走 WM_SETTEXT/GETTEXT；sendText 走 WM_CHAR（安全默认）。
// strategy 不在逐调用上传，句柄创建时定一次（win.control(spec, { strategy })）。
await c.setText("...");            // getText/setText 的 strategy 只有 auto 特殊：
await c.getText();                 // -> string（auto = 先消息，无效再 UIA）
await c.sendText("原文");          // RAW_TEXT，无解析
await c.sendKeys("^{End}");        // 解析版；含修饰时文档化全局副作用
await c.paste("...");              // EM_REPLACESEL（AHK EditPaste），与 setText 同级异步。

// 焦点：focus 异步（含 ControlDelay），getFocused 同步查询。
await c.focus();
await c.getFocused();              // -> ControlHandle | null

// 复选框：默认复刻 AHK（预检 + 中心合成点击，不用 BM_SETCHECK）。
await c.setChecked(true | "toggle");  // { ensureActive?: boolean }
await c.isChecked();               // -> boolean

// 列表/组合/Tab/Edit 族（Phase 2，本期只定形状）：
await c.combo.select({ index: 1 } | { text: "..." }, { notifyParent: true });
await c.edit.line(3);              // 1-based，对齐 AHK
await c.edit.selectedText();

// 可见/启用/几何/样式：纯查询同步（单 Win32 调用，无阻塞；AHK 轮询范式
// 要求热循环无 await 税——Promise 化一个 100ns 查询是千倍开销），变更异步。
// 同步先例：window settings 读写 + automation.release（仓库已有同步形态）。
c.isVisible(); c.isEnabled(); c.rect();  // -> {x,y,w,h} 顶层客户区相对；同步
c.getStyle(); c.getHwnd();               // 同步（GetWindowLong；id 回稳定 id）
await c.show(); await c.hide();    // show 恒 NOACTIVATE（AHK 语义）
await c.move({ x, y });            // 相对顶层客户区，内部做两次 MapWindowPoints
await c.setEnabled(true);

// 调试/兼容刚需（一等保留）：
await c.sendMessage(0x00F5, 0, 0, { timeout: 2000 });  // 超时+取消，进 Trace
c.postMessage(0x0201, 0, 0);        // 同步入队（Post 不阻塞）

// 订阅式资源照抄 automation.release 范式：
c.dispose();  // -> boolean，同步
```

错误形态沿仓库约定：形状错同步 `TypeError`（坏 spec/非法 `pos.space`/
`count < 0`/`variation` 越界）；执行错异步 `ActionError{code}`，codes 复用
`timeout/cancelled/capability_denied/target_gone`，加 `unsupported`
（无 invoke pattern 等 UIA 缺口）。期权 `NativeActionOptions`
（`deadlineMs/signal→cancellationId`）与 `signal` 剥离规则开箱即用。

## 3. 同步划分（哪些异步，为什么）

| 操作 | 形态 | 理由 |
|---|---|---|
| `resolve`（ClassNN/文本/HWND/point） | 异步 Action | 复用窗口搜索通道；目标挂起时 Enum 可阻塞 |
| 一发快捷方式 | 与被包装动作同形态 | Playwright 式 `page.click` 对应物，resolve+动作一次往返 |
| `click`（含 count×delay 循环） | 异步，可取消 | AHK `ControlDelay` 是泵消息 sleep；临界区（抢焦点/输入状态） |
| `focus` | 异步 | attach + `ControlDelay`；`getFocused` 同步（`GetGUIThreadInfo` + `IsChild` 纯查询） |
| `setText/getText/paste` | **必须异步** | `SendMessageTimeout` 最长 5s/次且调用线程无响应直到返回 |
| `sendText/sendKeys` | 异步，可取消 | 逐字符 Post + 取消点在字符边界 |
| `setChecked/isChecked` | 异步 | get 含 2s 超时；set 含合成点击 + delay |
| combo/list/tab/edit 族 | **必须异步** | 2s 超时，`GetItems` 逐项 5s，加 `limit`（AHK 一次性取全部，Rime 不学） |
| show/hide/move/setEnabled | 变更异步 | 变更含 delay |
| isVisible/isEnabled/rect/getStyle/getHwnd | **同步** | 单 Win32 调用无阻塞；热轮询（wait-until-visible）禁不起 await 税。同步先例：window settings + `automation.release` |
| `sendMessage` | 异步+超时+取消 | 挂窗口/跨进程死锁是常态；`postMessage` 同步入队 |
| `dispose` | 同步 boolean | O(1) 注册表擦除（照抄 `automation.release`） |

## 4. 不暴露给上层的东西（明确清单）

1. **裸 `HWND`/`HINSTANCE`/`HMENU`**——稳定 id 是唯一句柄（AGENTS 资源规则；
   `WinGetControlsHwnd` 的 `hwnd` 字段是 id，不是裸指针，不得仿造）。
2. **`lParam/wParam` 的手工拼装**——`pos.space` 坐标系进类型，内部做
   `ClientToScreen/ScreenToClient`；调用方永远不算 `MAKELPARAM`。
3. **ClassNN 自动回退歧义**——显式 `classNN/text/hwnd/point` 四选一；
   `auto` 档明确标注“兼容行为，可能命中意外控件”。
4. **全局 `ControlDelay`**——不设进程级可变标志；每个变更 API 接受
   `settle?: ms`，由集中调度策略实现（AGENTS 调度规则）。
5. **`MK_*` 修饰合成与 `AttachThreadInput` 细节**——临界区内部事务，
   不进 Trace 以外任何可观察面；`sendKeys` 含修饰时的全局副作用必须
   写进文档（AHK 注释实证：修饰走全局 `keybd_event`）。
6. **`WM_NOTIFY` 跨进程直发**——凡需父窗口配合的一律 `WM_COMMAND`
   或合成按键（AHK 注释：notify 跨进程发不出去）；不提供裸 notify API。
7. **无 HWND 控件的坐标点击**——WPF/Qt 自绘走 UIA 路径；消息路径在
   `controls()` 无对应 id 时直接 `target_gone`，不静默降级为屏幕坐标
   （降级即不可解释）。

## 5. 底层实现（window 层实际怎么做）

- **定位**：复用 `WindowService`（窗口）+ `EnumChildWindows` 后代枚举 +
  共享 `next_class_nn`（`window_match.cpp:171-197`，大小写不敏感，跟仓库
  现有语义；AHK 的大小写敏感版不跟）。`point` 复用 `window_at.cpp`
  的 `EnumChildFindPoint` 复刻（含“被包围者胜出”规则）。
- **Direct executor**：新建 `ControlExecutor`（Win32 消息直驱），与
  `WindowExecutor`/`ClipboardExecutor` 同注册形态；`sendMessageTimeout`
  统一 `SMTO_ABORTIFHUNG` + 分级超时（状态/列表 2s，文本 5s），跑后台
  lane + 取消，JS/UI 线程永不直等。
- **Semantic 复用**：`UiaService::find/read/invoke` 现成的三件套；
  `setText(auto)` 先消息、无效再 UIA；`ValuePattern` 优先 Edit 类。
- **Capability**：`windows.automation.control`（已在 `actions.json`
  注册为 `planned`，指向本文档；读/写分离沿 `windows.window.read/write`
  例， action 类型随 `ControlExecutor` 落地时一并注册——checker 要求
  每个注册类型都有 executor 字面量接受，空 registry 先行会红）。
- **Registry**：action 类型（`control.resolve/click/focus/setText/getText/
  sendText/sendKeys/...` + `matrix:check` executor 字面量）随
  `ControlExecutor` 第一个可执行版本一起进 `actions.json`（仓库铁律
  在 checker 面前让步：无 executor 的类型不得先行）。

## 6. 测试（按真实性分级）

- L2：spec 解析（四种显式形态 + `auto` 回退 + 非法 `space`/`count`），
  期望字面量（照 `window_logic_tests.cpp`）。
- L5：fixture 窗口（BUTTON + Edit + ListBox，自带 pump 防 `SendMessage`
  死锁，照 `automation_tests.cpp:27-107`）：resolve 唯一性 → click 致
  `WM_COMMAND/BN_CLICKED` → setText/getText 回环 → 超时/亡窗口
  `InvalidContract/TargetGone` → `dispose` 恰一次 true → 毁窗后 stale
  `TargetGone`。
- Phase 2（combo/list/tab/edit）在 L5 fixture 加对应控件后再测，不提前。

## 7. Phase 切分

- **Phase 1**（本期）：resolve + 一发快捷方式 + click/wheel + focus/getFocused +
  setText/getText/paste + sendText/sendKeys + setChecked/isChecked +
  可见/启用/几何/样式（查询同步、变更异步）+ sendMessage/postMessage + dispose。
- **Phase 2**：combo/list/tab/edit 族 + `notifyParent` + `limit` 分页。
- **不做**：`ControlDelay` 全局开关（由调度策略替代）；ClassNN 大小写
  敏感版（跟仓库现有语义）；裸 notify API（见 §4.6）。

## 8. 设计 review 记录（对抗性评审结论）

- 骨架（句柄 + options 对象 + 错误二分）是最 TS 的形状（Playwright 形），保留。
- 一发快捷方式是补上的缺口：纯句柄强迫简单脚本付两跳 ceremony 税。
- 纯查询同步化是最大的一处翻转：`isVisible/isEnabled/rect/getStyle/getHwnd`
  是单 Win32 调用，Promise 化即千倍税；仓库已有同步先例（settings/release）。
- `strategy` 从逐调用搬到句柄级：逐调用是参数臃肿。
- `activate: false` 默认是全篇最具争议的一处：与 AHK 默认行为分歧，
  移植脚本必须显式传参；保留但文档警告，兼容档默认 true。
- `EditPaste` 补进 Phase 1（单消息，高频）。
- registry 策略：capability 先 `planned`，action 类型随 ControlExecutor 落地
  （checker 要求每个注册类型有 executor 字面量，空注册先行会红——已验证）。
