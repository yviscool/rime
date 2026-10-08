# Rime Performance Specification

> 性能哲学：不要证明 Rime 的 JavaScript 比 AHK 快。要证明 Rime 作为
> Desktop Runtime，在真实 Windows 自动化场景下以可预测的低延迟、低资源
> 占用完成工作，且其架构层增加的成本远小于 Windows 操作本身的成本。

## 1. 报告规范（强制）

1. 只报百分位：`p50 / p90 / p95 / p99 / p99.9 / max`，禁止只报均值。
2. 每次报告必须声明：机器（CPU/RAM/OS）、构建（Release/Debian、x64、
   编译器版本）、被测 commit、QuickJS 版本、迭代次数、桌面状态
   （idle / loaded）。
3. 单次运行数字是指示值（indicative），不是基线：不进断言，不进
   ctest（见 `tests/native/bench.cpp` 头注释：bench 故意不注册为 test）。
4. 对比必须同条件：Release vs Release，x64 vs x64，同版本 Windows。

## 2. 层级（对齐测试真实性 L1-L6）

| 层 | 对象 | 工具 | 状态 |
|---|---|---|---|
| L1 队列/分发 | `event.push+try_pop`、`event.post->handler` | `rime_bench` | 已有 |
| L2 输入延迟 | `input.inject->hook callback`（真 F24 注入） | `rime_bench --input-latency`（需交互桌面） | 已有 |
| L3 动作管线 | `action.submit` / `action.pump` / `kernel.allows` / `executor.noop` | `rime_bench` | 已有 |
| L3 冷启动代理 | `runtime.create+start+stop`、`worker.start+stop`、`js.host.create+destroy`、`js.host.eval-trivial`、`js.1M-calls`（仅引用） | `rime_bench`（JS 行需 `quickjs` 预设） | 已有 |
| L4 端到端任务 | Notepad 激活 / 剪贴板往返 / 窗口搜索激活 | 待建（fixture 窗口 + 双实现） | 计划 |
| L4 首批 | `l4.query/focus/confirmed`、`l4.clipboard.*`、`l4.uia.*`、`l4wf.focus.submit/pump/kernel_direct/service_direct`（同动作四层分解） | `rime_bench --l4-*`（交互桌面，手动） | 已有 |
| L5 桌面压力 | 10k events/s mouse-move：CPU、队列深度、丢弃率、GC | 待建 | 计划 |
| L5 首批 | `pressure.mouse-blast` + `inject->callback`（真实 SendInput） | `rime_bench --pressure`（交互桌面，手动） | 已有 |
| L6 AHK 对照 | 同一 scenario.json，AHK 与 Rime 双实现，统一采集 | 待建 | 计划 |
| L6 首场景 | `benchmarks/activate-window`（AHK 脚本 + Rime `--l4-activate`） | 手动双跑，机器声明强制 | 已有 |

L0（纯语言循环对比）只作参考，不作结论：Rime 用 QuickJS，
语言微基准的胜负与用户感知的自动化延迟无关。

## 3. 核心问题（每层必须回答）

- L3：架构税是多少？`submit + pump` 相对其内部 `Win32` 调用占比？
  目标：runtime 开销 << Windows 操作本身（个位数百分比）。
- L1/L2：尾延迟，不是均值。`p99.9` 决定用户是否感到“卡”。
- L4：从物理输入到副作用完成（end-to-end），不是从 JS 到 Win32。
- Cold start：Host/QuickJS/模块加载各占多少？Reload 是否 < 5ms？

## 4. 目标带（非承诺，是设计输入）

1. 核心自动化延迟：`Rime <= 1.2 x AHK` 即接受。
2. 关键路径（gesture / context / window search）：`Rime < AHK`。
3. 复杂场景（调度 + 合并 + 缓存）：利用架构优势明显领先。
4. 快、稳、省三角：latency、throughput、resources 三项同时报告，
   不接受以内存换延迟的单项胜利。
5. 输入延迟回归线（P0-2）：`input.inject->hook` 与 AHK `send->hook`
   同机双跑并记录差距；每次 `--input-latency` 必须附带同次运行的六列分层
   （`input.prep/service/inject/traverse/hook/queue`，见 `input_probe.hpp`）。
   当前（Run 2026-10-08-H）：Rime p50 ≈ AHK × 1.7，p99 基本持平；分层显示
   service ≈ 2~9us、hook ≈ 2us（proc+enqueue 无可砍之处），差距主体是
   pump 线程唤醒的调度方差（`input.queue` p50 37~65us，随桌面负载漂移）——
   这是“绝不在 hook 线程跑脚本”架构决策的标价，不是待修的肥肉；任何改动
   hook/pump 路径的优化必须先出分层数据再谈。
6. 冷启动下限线（P0-2）：进程内 `js.abi.load+execute` 是 host 进程级
   启动时间的下限，禁止与 AHK 进程数直接对照；每次报告同时给出
   `js.abi.load+execute` 与 `quickjs.new+free` / `host.create+destroy`
   分解。
7. 架构税分解线（P0-2）：`l4wf.window.focus` 必须附带同次运行的
   `l4wf.focus.submit / .pump / .kernel_direct / .service_direct`
   四列；禁止跨 harness 相减归因（例如拿 workflow 的 focus 减
   activate 的 focus 当作“JSON 税”）。
8. UIA 作用域回归线（P0-3）：每次 `--l4-uia` 必须同时报告
   `l4.uia.find`（桌面根）与 `l4.uia.find_scoped`（窗口子树）；
   只报前者、不报后者的数字视为不完整。SDK 侧全桌面搜索须显式
   `allowDesktopRoot: true`（`sdk/src/automation.ts`），默认无参
   `find` 触发一次性 `console.warn`。

## 5. 反作弊（与 AGENTS.md 测试九条同级）

1. 禁均值当结论；禁单次 for 循环当证据。
2. 禁 Debug vs Release、x86 vs x64 的错位对比。
3. 低于 L4 的数字不得为行为背书；L1 数字永不单独引用。
4. 环境噪声（Defender、后台应用）必须在报告中声明桌面状态；
   高负载声明缺失的数字视为无效。
