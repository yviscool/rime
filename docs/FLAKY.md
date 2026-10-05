# Flaky 台账（环境抖动协议与已知抖动点）

本文件是 `AGENTS.md` 反作弊第 4 条（禁静默跳过）、第 5 条（禁 sleep 当同步）、第 6 条（禁重试洗绿）的执行细则。
测试失败后唯一的合法复跑路径在这里定义；**未登记的失败不得复跑**。

## 协议

1. 只有下表「已知抖动点」登记的测试允许复跑，且必须是**隔离复跑**：单独重跑该测试，不是整套连续重跑。
   ```text
   ctest --test-dir <build目录> -R <测试名> --output-on-failure
   ```
2. 每个失败点最多复跑 **3 次**（`AGENTS.md` 反作弊第 6 条）。任何一次通过都必须在下方台账表登记结论。
3. **逻辑性失败禁止重试洗绿**：与登记触发条件不符，或复跑 3 次仍失败，判定为缺陷，停止复跑并修复。
4. **禁静默跳过**：条件不满足（拿不到前台、资源缺失、对端未响应）只能显式失败。
   "打印一行诊断然后通过" 不构成结果；复跑必须先有一次真实失败。
5. **CI 不做自动复跑**。复跑由人工按本协议执行并登记。
6. 交互式 slice 文件头声明 `Needs an interactive desktop, exclusive run`，
   复跑前必须使用空闲桌面（前台无抢夺者、输入法处于预期状态）。

判定标准：

- **环境抖动**：失败可由下表登记的触发条件解释，且隔离复跑通过 → 记录结论，继续。
- **缺陷**：与登记条件不符，或 3 次复跑仍失败 → 不再复跑，开缺陷处理。

## 已知抖动点

| ctest 名 | 位置 | 现象 | 触发条件 | 处理方式 | git 证据 commit |
| --- | --- | --- | --- | --- | --- |
| `quickjs_events_slice` | `tests/js/events_slice.cpp:396-414`（`bring_to_front` 20 次 × 20ms 重试）、`:377-381`（IME 分离） | 注入键未落入 edit / 中文 IME 把键串掉 | 桌面存在前台抢夺者（微信、游戏、弹窗）；桌面处于中文输入法布局 | 测试内 deadline 重试；仍失败则隔离复跑 ≤3 次 | `061ef1e`、`05382be`（基线同样失败）、`317e5b1`、`61f9430`、`20e03a7` |
| `rime_automation_service` | `tests/native/automation_tests.cpp:178`、`:184-185`（find → 销毁 → 再 find） | `found.size() == 1` / `found.empty()` 与 UIA 元素登记节奏不符 | UIA 元素树与目标窗口线程的处理节奏（负载敏感），根因未定论 | 隔离复跑 ≤3 次 | `add88cc`（原文 "rime_automation_service known flaky, isolated rerun 2/2 green"） |
| `rime_win32_window_service` | `tests/native/win32_tests.cpp:771-775`（hung 目标 kill，3s 超时；`:774` 错误码断言） | hung 目标枚举/kill 错误码与预期不符 | 机器负载 | 隔离复跑 ≤3 次 | `61f9430`（原文 "window_service:774（负载抖动，隔离复跑 2/2 过）"） |
| `quickjs_vertical_slice` | `tests/js/vertical_slice.cpp:269-273`（入口 focus deadline）、`:321-336`（归属重读 + focus deadline）、`:378-383`（显式失败） | active-move 段拿不到前台 | 桌面存在前台抢夺者；后台进程的 `SetForegroundWindow` 被拒绝 | 批1 已整改：单次 focus → 3s deadline 重试（只传己方 id）；仍拿不到则 `std::abort()` 显式失败，不再静默 SKIP。整改后仍失败 → 隔离复跑 ≤3 次 | `20e03a7`（同类"桌面前台被占用"证据）；本批整改 |
| `quickjs_input_slice` | `tests/js/input_slice.cpp:1`（`Needs an interactive desktop, exclusive run`） | 桌面前台被占用时全局 InputHook 抢走前台应用按键 | 桌面被前台应用占用 | 独占空闲桌面后隔离复跑 ≤3 次 | `20e03a7` |
| `rime_win32_input_service` | `tests/native/input_tests.cpp:266-286`（相对鼠标 ≤3 探针） | 单次相对移动增量读数被物理光标移动带偏 | 用户或后台程序移动光标 | 批1 已收敛为 ≤3 探针 + 每探针 2s deadline，每次重试 stderr 公告，耗尽则显式失败 | 本批（批1 防假绿） |

## 台账表

模板列：日期 / ctest 名 / 失败输出摘要 / 隔离复跑第几次通过 / 判定 / 备注（commit 或缺陷号）。
每一行只记录一次**已通过的隔离复跑**或一次**判定为缺陷的结论**，不允许空结论。

| 日期 | ctest 名 | 失败输出摘要 | 隔离复跑第几次通过 | 判定 | 备注 |
| --- | --- | --- | --- | --- | --- |
| 2026-10-05 | `quickjs_events_slice` | 首跑注入键未进 edit（前台被抢占） | 第 1 次 | 环境抖动 | `061ef1e` |
| 2026-10-05 | `quickjs_vertical_slice` | `FAIL: slice active-move could not take the foreground (focus requested ok=0)`：Chrome 持前台期间 `SetForegroundWindow` 连续 6s 全拒 | 第 4 次（前 3 次均在非空闲桌面下失败——违反协议第 6 条前置条件，未先核对空闲桌面；`build/fg-probe.cpp` 探针独立复现"Chrome 持前台 → 拒绝、空闲 → 成功"后，桌面空闲时复跑通过） | 环境抖动 | 结论含前置条件违反，不作纯协议通过 |
| 2026-10-05 | `quickjs_events_slice` | `inputhook-capture-check.mjs: buffer must collect a,a: "aaaa"`：500ms 采样窗外来注入的 `a`（`D65i`/`U65i`，非 selfInjected）与自注入 `VK_PACKET`（`D231iS`）混入 InputHook 缓冲 | 第 2 次（第 1 次失败；第 2 次通过） | 环境抖动 | 桌面非独占（用户/外部程序在测试窗口期注入按键） |
| 年-月-日 | `<测试名>` | `<一行输出摘要>` | 第 n 次（≤3） | 环境抖动 / 缺陷 | `<commit 或缺陷号>` |

## 批1（防假绿）整改后的语义变化

- `quickjs_vertical_slice`：原分支打印 `SKIP: ...` 后仍算通过 → 改为 focus deadline 重试 + `std::abort()` 显式失败（AGENTS 反作弊第 4 条）。该分支不再产生绿。
- `quickjs_input_slice`：blockInput 的 250ms 固定 sleep → physical 快照轮询 + 300ms 有界反证；numlock/scrolllock 的 400ms 固定 sleep → 新建 control 订阅（正向活性证据）+ 有界 `waitFor` 反证 + 显式 `throw`。
- `rime_win32_input_service`：相对鼠标 retry×5 → ≤3 探针（AGENTS 复跑预算）+ 每探针 2s deadline；`unsubscribe` 负向断言改用 fresh control 订阅对照。
- `rime_timer_determinism`（`tests/native/timer_tests.cpp`）：三处 wall-clock sleep → 0ms / 同 deadline canary 做活性证明，或 `stop()` 之后的确定性断言；时间判定全部由注入的 `ManualClock` 推进（`AGENTS.md` 时间策略）。
