# Rime Performance Records

真实跑分记录（非基线、不断言）。每条记录必须带机器声明；比较只在
同机、同构建、同负载下有效（见 SPEC.md 第 1 节）。

## Run 2026-10-06-A（首轮，`-O2` mingw）

- 机器：Intel Core 5 120U / 15.6GB / Windows 11，loaded 开发机
- 构建：mingw gcc 13.2，`-O2 -DNDEBUG`，无 JS 行（dev 等价，`RIME_BENCH_HAVE_JS` 关）
- 命令：`rime_bench.exe`（默认 suite）

```
action.submit+pump   p50=1.0us   p99=1.6us    (submit 0.4 + pump 0.6，自洽)
kernel.allows        <0.1us      executor.noop <0.1us
event.post->handler  p50=0.2us   p99=0.4us
runtime.create       p50=0.9us   worker.start+stop p50=47us
```

## Run 2026-10-06-B（quickjs 预设，Debug）

- 同机器；构建：mingw gcc 13.2 Debug + QuickJS（`build/quickjs`）
- 命令：默认 suite（含 JS 行）

```
action.submit+pump   p50=4.7us   p99=10.5us
js.host.create+destroy  p50=341us
js.host.eval-trivial    p50=5.8us
js.1M-calls             201ns/call（仅引用）
```

## Run 2026-10-06-C（Release，`-O2` + QuickJS）

- 同机器；构建：`build/bench-release`（mingw，`-O2 -DNDEBUG`）
- 命令：默认 suite

```
action.submit+pump   p50=1.2us   p99=2.5us
js.abi.load+execute  p50=596us   (Host 411 + 模块求值增量 ~185us)
quickjs.new+free     p50=111us   timer.create+destroy p50=84us
js.host.create+destroy  p50=181us
js.host.eval-trivial    p50=1.9us
js.1M-calls             65ns/call（仅引用）
```

冷启动账本：QuickJS 111 + 线程 84 + 注册 ~41 + 求值 ~185（Debug/Release
比例可迁移，绝对值以 MSVC 为准）。

## Run 2026-10-06-D（L4/L5，Release 同构建）

```
--l4-activate 200:  query p50=155us / focus p50=15us / confirmed p50=252~584us
--l4-clipboard 100: write 124us + read 143us = roundtrip 269us（自洽）
--l4-uia 10:        find ~143ms（桌面根遍历，主导一切）/ invoke ~0.3ms
                    + find_scoped（同按钮窗口子树对照，P0-3 新增；以实测
                    两列之比为准，禁止以外推倍数当结论）
--l4-workflow 30:   resolve 250 + write 54 + focus 468 + move 437 = task 1211us
--pressure 2000x2:  achieved 3867/s, received=2000, dropped=0,
                    inject->callback p50=363us p99=817us
```

注意：Action 管线 mediated 的 focus（468us）比直调服务（15us）贵约
400us——此前差额按“JSON payload 解析 + Trace + 调度”定性记录，未经
同次分解验证。现 `--l4-workflow` 每次运行附带同动作四层分解
（`l4wf.focus.submit / .pump / .kernel_direct / .service_direct`，见
`tests/native/bench.cpp run_l4_workflow` 末尾的 breakdown 段）：以该
四列为准重写此结论，禁止跨 harness 相减归因（SPEC.md §4.7）。

## Run 2026-10-06-E（AHK L6 首场景）

- 二进制：AutoHotkey v2.0.28 64-bit；同机器；离屏 toolwindow fixture
- 命令：`AutoHotkey64.exe benchmarks/activate-window/ahk/activate.ahk 200`

```
ahk.activate  p50=111ms  p99=120ms  success=200/200
```

- 第一版用 `WinWaitActive` 测得 220ms flat，怀疑等待量子；换 1ms 轮询
  仍 111ms；纯 `WinActivate` 循环（无等待）109ms/iter——耗时在实现
  内部（前台锁重试/退避），不在等待策略。
- 对照口径：`l4.focus.confirmed`（请求 + 轮询确认）vs `ahk.activate`
  （请求 + 轮询确认）。`l4.focus`（纯请求）不得与之对比。

## 待补

- MSVC Release 基线（本地无 cl，以 CI `bench` workflow artifact 为准）。
- Gesture/launcher 路径（仓库尚无对应能力）。
- 趋势：夜间 workflow 落盘 artifact 后，在此追加。

## Run 2026-10-08-A（新分层列首光，非基线）

- 机器：与 Run F 同机（Core 5 120U / Win11，loaded：StatsBall/IME/微信等并存）
- 构建：MinGW gcc Debug（`build/seamcheck`，`RIME_WARNINGS_AS_ERRORS=OFF`），
  仅证新分层列能跑通；**禁止与 Release 记录对比**（SPEC §5.2）。
- 命令：`rime_bench --l4-workflow 10`、`rime_bench --l4-uia 5`

```
l4wf.window.focus  p50=328us | submit 3.2 + pump 142 = 145us mediated
l4wf.focus.kernel_direct p50=135us | service_direct p50=16us
l4.uia.find        p50=77ms  (桌面根，loaded)
l4.uia.find_scoped p50=4.1ms (同按钮窗口子树，约 19x)
```

- 读法：pump ≈ kernel_direct（队列本身只占 ~7us）；kernel_direct 与
  service_direct 之间 ~119us 才是 executor + Trace + payload 的真实区间，
  旧 "400us 差额在 JSON+Trace+调度" 定性被收窄为可度量项。
- 主循环 mediated focus（328）与 breakdown 内 submit+pump（145）的差值是
  相位效应（前台初建 vs 已稳），不是第二次架构税——同 harness 内对比才
  有效，跨段相减仍禁止。

## Run 2026-10-06-F（发键延迟双边）

- 同机器（`tools/bench-env.ps1`）：Core 5 120U / 15.6GB / Win11 28000，
  AHK 2.0.28，commit 9490b1e
- Rime：`rime_bench --input-latency 200`（F24 down+up 经 service.send）
- AHK：`benchmarks/send-latency/ahk/send-latency.ahk 200`
  （字母探针键 + KeyOpt N + 自建离屏 Edit，clean=yes）

```
rime inject->hook  p50=285us  p99=641us
ahk  send->hook    p50=29us   p99=273us   received=200/200 reordered=0
```

- **AHK 赢约 10 倍，认。** Rime 路径上多了批量串行化、marker 打标、
  hook 线程排队三道工序——这是 self/foreign 区分能力的标价，不是噪声。
- **2026-10-08 修订**：本 run 的 285us 在同机复测中不再现（见 Run H）。
  保留原文不改写；当前结论以 Run H 为准。
- 探针副产品（已写入 scenario.json caveats）：AHK OnKeyDown 只对
  KeyOpt N 注册的键触发；F13-F24/Shift 到不了它，只有字符键行；
  离屏窗口任何模式都收不到字符（Edit 恒空），完整性只能靠 hook
  计数 + 前台保持来证。

## Run 2026-10-06-G（冷启动 PK，一半）

- AHK：`benchmarks/cold-start/ahk/coldstart.ahk 20`

```
ahk.cold-spawn               p50=31ms
ahk.cold-spawn-to-first-line p50=20ms  （含 CreateProcess，不含轮询量子）
```

- Rime 侧 pending：host 进程级计时需要 bootstrap 接线；
  进程内 `js.abi.load+execute`（251us Release）是下限，**禁止**与
  AHK 进程数对照引用。

## 热字串状态

- Rime 侧：JS 解析层真实存在（`events_module.cpp` Hotstring 解析 +
  `sdk/src/input` types），端到端（键入 → 展开）基准待建。
- 在端到端跑通前，热字串不进对照表。

## Run 2026-10-08-H（发键延迟重测 + 六层分解，非基线）

- 同机器 loaded 桌面；Rime 为 query 当日 HEAD（含 `input_probe`），
  Debug（seamcheck）与 Release（bench-release，mingw `-O2`）双跑；
  AHK 2.0.28 同机背靠背（`send-latency.ahk 100`）。
- 命令：`rime_bench --input-latency 100`（F24 down+up，probe 每迭代 arm）。

```
rime headline      p50=53~91us  p99=169~314us   (两次运行间漂移，见下)
rime service       p50=2~9us    (validate+build+marker)
rime inject        p50=169~248us (SendInput syscall 本体；与回调重叠，见下)
rime traverse      p50=15~18us  (OS 遍历：RIT + hook 链)
rime hook          p50=2us      (proc + enqueue：无可砍之处)
rime queue         p50=37~65us  (pump 唤醒 + 投递：调度方差主导)
ahk  send->hook    p50=31us     p99=176us       (复现 Run F 的 29us)
```

- Run F 的 285us 不再现：当日数应为负载/代码期 artifact，如实降级为
  历史记录（原文保留 + 修订注记），不再作为差距依据。
- 关键结构发现：multicore 下回调 routinely 落在 SendInput 返回**之前**
  （callback p50 69us < SendInput 时长 p50 169us+），故跨线程首尾相减
  会得出负数——分层必须用因果有序的内部戳点（`input_probe.hpp` 注释
  有完整论证），禁止拿 headline 减 send-side。
- 自洽校验：service + traverse + hook + queue ≈ headline（7.7 + 14.7 +
  2.4 + 37.1 ≈ 62 vs 68.7，余量为回调尾 + prep 0.1us）。
- 结论：p50 差距约 1.7x，p99 基本持平；残差主体是 pump 线程唤醒的调度
  方差（37~65us 随桌面负载漂移，两次运行 literal 不同），即“绝不在
  hook 线程跑脚本”架构决策的标价。proc/服务侧无可砍的肥肉（hook 2us
  证明了三道 GetAsyncKeyState 也淹没在噪声里），故本次**不做优化改动**，
  只立回归线（SPEC §4.5）：后续凡动 hook/pump 路径，先出六列数据再谈。
