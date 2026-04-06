# 11 - MainLoop 逻辑层未知项压缩与模块级优化计划（第四轮，2026-02-25）

## 目标
1. 将 MainLoop 逻辑层“未追踪”项尽可能压缩。
2. 基于上一轮已逆向出的 MainLoop 模块，输出模块级耗时与职责映射。
3. 给出“可执行 + 有收益预估 + 有风险分级”的优化计划。

---

## 本轮执行（已落地）

## 1) 分析模式开关统一（编译期）
- 文件：`src/d3d9/war3/core/war3_internal_test_config.h`
- 新增：
  - `kNativeMainLoopCoverageAnalysisMode=true`
- 联动开启：
  - `kNativeMainLoopDeepPhaseHookEnabled`
  - `kNativeMainThreadWaitHookEnabled`
  - `kNativeMainThreadWaitDeepHookEnabled`
  - `kNativeJassVmPerfTrackingEnabled`
  - `kNativeJassVmDeepHooksEnabled`
  - `kNativeOptimizationPerfTrackingEnabled`

目的：优先最大化逻辑层可观测覆盖，先把“未知项”拆出来，再谈优化。

## 2) Dispatch 模块语义分桶（case -> 模块）
- 文件：`src/d3d9/war3/hooks/war3_hook_lifecycle.cpp`
- 新增 `GetEventDispatchModuleScopeName(int msgType)`，将 case0~14 映射到可读模块：
  - `StateFinalize`
  - `LoadBlockType1/2/8/9/11/12/13/14/16/27/28`
  - `MainCallbackGate`
- 在 `Hook_EventDispatch` 中同时记录：
  - `War3MainLoop/Dispatch/CaseX`
  - `War3MainLoop/DispatchModule/*`

目的：不改行为语义下，把逻辑分发成本从“case 编号”变成“模块语义”。

## 3) 报告聚合扩展
- 文件：`src/d3d9/war3/tools/war3_perf_monitor.cpp`
- MainLoop Stage 聚合增加 `DispatchModule/*`。
- 语义树分类增加 `War3MainLoop/DispatchModule/* -> Logic/Dispatch`。
- 新增显式分桶：
  - `Other/UntrackedActive (MainLoop Active Gap)`

目的：即使仍有残余未知，也不再只沉到 `Other/Untracked` 黑箱里。

---

## 编译与自动化结果

## 编译
- `ninja -C build32` 通过。

## AutoTest（2K 全屏，60s）
- 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_02_25_04_02_51.html`
- 关键指标：
  - `avgFps = 102.516`
  - `avgFrameTimeMs = 9.755`
  - `avgMainThreadCpuMs = 4.521`
  - `avgProcessCpuMs = 7.816`
  - `avgWorkerThreadsCpuMs = 3.297`
  - `activeFrameTimeMs = 3.958`
  - `avgTrackedActiveCpuMs = 3.958`
  - `avgUntrackedActiveCpuMs = 0.000`
  - `cpuCoveragePct = 100.0`
  - `mainThreadCpuCoveragePct = 99.968`

结论：主线程逻辑层活跃段未追踪项已压缩到接近 0（按当前采样口径）。

---

## MainLoop 模块耗时与职责（本轮实测）

> 口径说明：`Engine/WaitGate` 为等待门，不是业务计算热点；优化优先看 Active 段。

| 模块 | Avg CPU (ms/frame) | Calls/Frame | 模块职责 | 优化潜力 |
|---|---:|---:|---|---|
| `Engine/WaitGate` | 9.709 | 2.34 | 主循环等待门（Idle） | 非直接优化目标（节拍门） |
| `Engine/TickUpdate` | 0.384 | 1.17 | Tick 推进、阶段切换、部分提交收口 | 高（逻辑层第一热点） |
| `Pump` | 0.118 | 1.00 | 消息泵驱动 | 中（更多是结构性开销） |
| `Engine/PrepareDispatch` | 0.072 | 1.17 | 超时分支前置准备 | 中（第二热点） |
| `Dispatch` | 0.006 | 0.56 | 事件分发总入口 | 低（本轮量级较小） |
| `Dispatch/Case10` | 0.004 | 0.56 | 主命中 case（Type12） | 中（可针对性） |
| `DispatchModule/LoadBlockType12` | 0.003 | 0.56 | `LoadResourceBlock(type12)` | 中（case10子模块） |
| `Engine/RunCallbacks` | 0.002 | 1.17 | 到期回调执行 | 中（可能在场景变化时抬升） |
| `Engine/Reschedule` | 0.001 | 1.17 | 下轮唤醒重排 | 低 |
| `Engine/FinalizeDispatch` | 0.001 | 1.17 | 分发收口 | 低 |
| `Engine/QueueFlush` | 0.001 | 1.17 | 队列执行收口 | 低（当前场景） |

---

## 未知项压缩结果

## 1) MainThread 活跃段
- 本轮前：`avgUntrackedActiveCpuMs` 常见为多毫秒（历史窗口 6~10ms）。
- 本轮后：`avgUntrackedActiveCpuMs = 0.000`。

## 2) 仍需注意的“剩余大头”
- `avgWorkerThreadsCpuMs = 3.297ms`（进程级并行消耗）。
- 这部分不属于 MainLoop 主线程逻辑活跃段，更多是渲染后端/驱动并行线程开销。

---

## 优化收益最大化计划（模块职责 + 方案 + 收益）

## P0（先做，低风险，预计收益可复现）
1. `Engine/TickUpdate` 子路径再拆分（目标：把 0.384ms 拆到具体子函数）
   - 方案：补 `TickUpdate` 内关键子调用 hook（仅透传计时）。
   - 预估收益：可直接优化空间 `0.10~0.25ms`（1~2.5% 帧时）。
   - 风险：低（不改逻辑）。
2. `Engine/PrepareDispatch` 预分支快退
   - 方案：对空队列/空事件条件做前置判定，避免进入深层准备路径。
   - 预估收益：`0.03~0.08ms`。
   - 风险：中（需严格语义一致验证）。

## P1（中风险，中收益）
1. `DispatchModule/LoadBlockType12` 定向优化
   - 方案：对 type12 高频路径做参数缓存/重复提交抑制（仅在语义可证明无副作用时）。
   - 预估收益：`0.02~0.10ms`（随场景抖动）。
   - 风险：中（类型语义需 IDA 再核验）。
2. `RunCallbacks` 细分为 TopN 回调地址桶
   - 方案：统计“回调地址 TopN（耗时/次数）”，只优化稳定高占比项。
   - 预估收益：`0.02~0.12ms`（在脚本密集图可能更高）。
   - 风险：中。

## P2（高风险，收益不确定）
1. 调度节拍策略（WaitGate 参数/唤醒窗口）
   - 方案：仅在“非网络对战/可控场景”实验，不进主线默认。
   - 预估收益：帧率波动相关，不给稳定承诺。
   - 风险：高（时序/同步风险）。

---

## 结论
1. “逻辑层未知项”在主线程活跃段已基本压缩完成（当前口径 0ms 未追踪）。
2. 逻辑层可优化最大头是 `TickUpdate`，其次 `PrepareDispatch` 与 `LoadBlockType12`。
3. 接下来若以 FPS 绝对提升为目标，收益更大的仍在渲染提交与渲染后端并行开销；逻辑层优化更适合做“稳定性与尾延迟收敛”。

---

## 第四轮续作（方案全落实，2026-02-25）

## 已落实项
1. `Dispatch` 聚合快路径
   - 将 `Hook_EventDispatch` 的三层 per-call scope（`Dispatch + Case + Module`）改为 thread-local 聚合桶；
   - 在 `FlushMainLoopCycleToPerf` 每循环批量上报：
     - `War3MainLoop/Dispatch/Case0~14/Other`
     - `War3MainLoop/DispatchModule/*`。
2. `RunCallbacks` TopN 来源分桶
   - `Hook_EngineRunCallbacks` 记录 `__builtin_return_address(0)`；
   - 每循环输出 `War3MainLoop/Engine/RunCallbacks/Caller_XXXXXXXX` + `Caller_Other`。
3. `TickUpdate` 子路径拆解
   - 通过“调用前后 cycle 相位增量”拆分：
     - `War3MainLoop/Engine/TickUpdate/Self`
     - `War3MainLoop/Engine/TickUpdate/Sub/*`。
4. `PrepareDispatch` 低开销路径
   - 从 `cpuScope` 改为手工计时 `addCpuSample`，减少高频路径开销。

## 60 秒 AutoTest 验证
- 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_02_25_04_29_44.html`
- 关键指标：
  - `avgFps = 99.339`
  - `avgFrameTimeMs = 10.067`
  - `avgMainThreadCpuMs = 4.801`
  - `activeFrameTimeMs = 4.142`
  - `avgTrackedActiveCpuMs = 4.142`
  - `avgUntrackedActiveCpuMs = 0.000`
  - `cpuCoveragePct = 100.0`
- 结果：`ok=true`，无崩溃，`2560x1440` 截图基线匹配。

## 第四轮续作（二次收敛，2026-02-25）

## 本次增量优化
1. `RunCallbacks/PrepareDispatch/TickUpdate` 根节点上报从“逐调用”改为“每循环批量上报”。
2. `TickUpdate` 的 `Sub/* + Self` 改为 thread-local 聚合后一次性刷入 PerfMonitor。
3. 保持语义不变：仍输出 `TickUpdate/Self`、`TickUpdate/Sub/*` 与 `RunCallbacks/Caller_*`。

## 60 秒 AutoTest 验证（本次）
- 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_02_25_04_45_11.html`
- 关键指标：
  - `avgFps = 100.883`
  - `avgFrameTimeMs = 9.912`
  - `avgMainThreadCpuMs = 4.611`
  - `avgProcessCpuMs = 8.245`
  - `activeFrameTimeMs = 3.664`
  - `avgTrackedActiveCpuMs = 3.664`
  - `avgUntrackedActiveCpuMs = 0.000`
  - `cpuCoveragePct = 100.0`
- 结果：`ok=true`，无崩溃，`2560x1440` 截图基线匹配。

## 证据补齐
- 第二次 60 秒复测（上一版遗漏写入）：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_02_25_04_34_34.html`（`ok=true`）。
