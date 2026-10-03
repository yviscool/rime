# TS 标准库分层与 Windows 建模

状态：`design`（分层与口径已拍板，L2 建模分阶段落地）。

本文是台账状态口径与 M3/L2 阶段的执行依据。**设计权威是 [`docs/AHK-TS-WINDOWS-API-DESIGN.md`](../AHK-TS-WINDOWS-API-DESIGN.md) §0 的六个核心设计问题与 [`future-runtime.md`](./future-runtime.md)**：命名、参数形状、同步性、模块归属以前者为准；本文只回答分层、度量口径与落地顺序，不重复其结论（异步判定见其 §8，不暴露清单见其 §9/§10，Windows 对象图见 future-runtime §2）。

## 1. 分层

```text
L0  ECMAScript          字符串/数字/数组/RegExp/Intl/JSON/async/模块 —— 一行不重写
L1  领域值类型          WindowId / ProcessId / SubscriptionId / Chord / 快照与
                        CancellationToken（类型见设计文档 §2、future-runtime §1；
                        YYYYMMDDHH24MISS 只是序列化格式，不是类型）
L2  能力模块（语言本体） window / input / automation / process / clipboard / screen /
                        fs / registry —— 查询、变更、事件、能力门禁（future-runtime §3-§7）
L3  Runtime 控制         timer / hook / suspend / policy / lifecycle（M2 已完成）
L4  还原保留层           JS 覆盖不了的语义能力，以自有命名实现于 @rime/runtime-language
                        （非 AHK 名称兼容层，见 §6）
```

- **度量换轨**：253 md_func + 101 g_BIF 的台账度量的是**能力覆盖**（还原保留 + L0 等价 + L2/L3 服务），不是公共 API 形状的复制度（计划 §0.1）。核心语言的完成度由 L2 的查询/变更/事件/能力覆盖回答。
- **L0 优先原则**：任何 ECMAScript 已有且语义足够的能力，专属代码为零，台账记 `js-native`（§3）。

## 2. Windows 建模（L2）

四个统一动词，每个能力模块一致（详设计文档 §1/§3、future-runtime §1/§11）：

| 动词 | 形态 | 规则 |
|---|---|---|
| 查询 | 快照或异步读，返回领域值 | 跨 lane/UIA 必异步；结果是不可变快照 |
| 变更 | `Context → Intent → Action IR → Action Kernel` | 可检查、可 Trace、可取消、有明确结果与失败原因 |
| 事件 | `Subscription { id, close() }` | 关闭态+回调计数；回调在 JS 线程；shutdown 可排空并诊断 |
| 能力 | `windows.*.read/write/inject` 等 capability | 参数校验先于门禁；读写面登记在 `actions.json` |

- **引用**：JS 永远只见稳定 ID（可重新验证的身份），永不裸 `HWND`/`HANDLE`/COM 指针；跨线程只传不可变快照与序列化值。
- **树模型**：UIA 元素树是跨进程结构的主模型，Win32 是本进程/快速路径；两者不混成一个不可解释的调用。
- **WinTitle**：显式 `WindowQuery`（caption/class/process/regex…）谓词 DSL，不引入 AHK `SetTitleMatchMode` 式全局可变态；匹配模式是查询的参数，不是进程状态。
- **设置**：脚本可见设置是显式对象读写（`settings.window` 先例），无隐式全局。

## 3. 台账状态口径

| 状态 | 含义 | 计为终态 |
|---|---|---|
| `implemented` | 真实代码 + contract 测试（测试 ID 入台账） | 是 |
| `js-native` | L0 已覆盖：等价表达式与语义差异入档，零专属代码 | 是（映射与差异记录替代测试 ID） |
| `unsupported-by-policy` | §5 黑名单：理由与替代路径入档 | 是 |
| `contract-only` | 过渡态：目标模块/阶段已定，待实现（含 L4 还原保留候选） | 否 |

- 终态公式（计划 §0 同步口径）：`终态 = implemented | js-native | unsupported-by-policy`；`excluded`（版本差异别名）不进分母。
- `js-native` 行必须给出**等价 TS 表达式**与**可观察差异**（无差异写"无"）；差异真实存在且有用户价值时，转入 L4 还原保留（`contract-only` → 实现后 `implemented`），**不新造 AHK 约定包装**。
- `sdk-owned` 作为过渡态逐步清零，收敛到 `contract-only`。

## 4. 异步边界

- 判定权威是设计文档 §8：**同步** = 纯 parser、枚举转换、已取得快照的字段访问；**异步** = 所有窗口/控件/输入/剪贴板/进程/屏幕 API（跨 UI/MTA/IO lane、可能阻塞或等待消息泵），事件订阅返回 `Subscription` 而非 Promise，等待类 API 支持 timeout + cancellation。
- 工程判据一句话：**在 JS 线程上可能超过 ~1ms、可能死锁、或可能等待用户/他进程的，一律异步**，携带 `deadlineMs` + `AbortSignal`（`cancellation` 字段约定）。
- 现有实现与 §8 的偏差（如 `getKeyState` 等同步快照式读）记入设计文档 §12 的偏差清单，随异步边界审计收敛，不在本文私改结论。
- 慢调用必须可取消、可超时、可诊断；临界区（SendInput、剪贴板交换、窗口激活）内新事件只能排队或按明确策略合并。

## 5. 不暴露给上层（黑名单）

1. 裸 `HANDLE`/`HWND`/COM 指针与任何跨线程拥有状态的裸指针。
2. Hook 线程、UI 线程、消息泵的实现细节；第二套脚本消息泵。
3. AHK 式隐式全局可变态（`SetTitleMatchMode`、`A_*` 写入）与隐式伪线程/抢占模型（`Thread`/`Critical` 的线程中断语义）。
4. 裸内存与引擎布局互操作：`DllCall`/`NumPut`/`NumGet`/`StrGet`/`StrPut`/`NumGet`、DLL 装载、`ObjGetCapacity`/`ObjSetCapacity`（AHK 对象字段数组容量是引擎内部布局，非可移植语言概念）。
5. 散落的全局函数——公共 API 只走模块与稳定 ID。
6. 无 deadline 的无限等待（API 层面不提供这种签名）。

（`ComObj*` 系不在本黑名单：其去向是计划 §0.3 与 audit-gaps 的 COM 边界定档，按隔离插件信任模型处理，台账暂记 `contract-only` 至 M8 定档。）

## 6. L4 还原保留层（@rime/runtime-language）

计划 §0.1 拍板：**不存在 AHK 名称兼容层**；AHK 只是能力研究样本与语义核对测试的 oracle。因此：

- **JS 已覆盖的不写代码**：`Trim/Abs/StrLen/Is*` 等记 `js-native`，给等价表达式与差异说明。
- **JS 覆盖不了的语义能力做还原保留**：`Round` 半值远离零、`FormatTime` 时间格式化、`DateAdd/DateDiff` 日历算术、`RegExMatch/RegExReplace` 富匹配对象、`SplitPath/StrSplit`、`Format`、`Sort`/`Random`/`VerCompare` 等——以**自有命名与参数形状**实现（future-runtime 命名有定义的从其定义；没有的在实现阶段先出命名提案再落码），测试用 AHK 原版语义用例守住能力（计划 §0.1"语义以对照原版的测试守住"）。
- 每项：AHK 源引用（file:line）+ 差异表 + 对照测试；PCRE 构造无 JS 等价时**显式抛错**，不静默错配。
- 实现保持纯函数、不反向依赖 L2 Windows 能力（可测、可移植）。

## 7. 落地顺序

1. 状态口径进台账工具（`matrix` 识别终态公式与 `js-native` 说明）。
2. 分类研究：253+101 逐项归档，产出映射表进 `coverage.json`/`core-builtins.json`。
3. L4 还原保留项：先命名提案，后实现与对照测试（`contract-only → implemented`）。
4. L2 Windows 建模下一阶段：WinTitle 谓词统一、窗口事件订阅、异步边界审计（对照设计 §8/§12）。
