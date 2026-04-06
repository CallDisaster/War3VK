# 07 - Warcraft III MainLoop 全链路拆解（2026-02-24）

## 目标
1. 完整还原 War3 逻辑层 MainLoop 结构（1.27a）。
2. 将 MainLoop 涉及模块全部接入可观测性能计时。
3. 给出 CPU 障碍排序与可落地优化方案（含收益预估）。

## 结论速览
- 主循环核心不是“单函数一把梭”，而是 `sub_6F05F710` 驱动的 **事件调度 + 等待门 + 阶段回调**。
- `WaitGate(0x158940)` 是等待门（`WaitForSingleObject`），高占比通常代表节拍等待，不等于业务计算热点。
- 本轮补点后，MainLoop 关键阶段已经可单独观察：`PrepareDispatch / TickUpdate / FinalizeDispatch / RunCallbacks / Reschedule / QueueFlush / WaitGate`。
- 在 2K 全屏 AutoTest 场景下，真正可优化的主线程活跃 CPU 主要集中于：
  - `Engine/TickUpdate`
  - `Engine/PrepareDispatch`
  - 渲染队列提交（`FQ_Dispatch_Opaque` + `FQ_Total_Trans`）

---

## 1. 主循环完整结构（IDA 还原）

### 1.1 入口链
- `GameMain(0x6F0297FF)`
  - 初始化日志/系统
  - `GameApplication_Init()`
  - 进入事件调度框架

### 1.2 调度框架
- `GameApplication_Init(0x6F027FF0)`
  - 初始化 EventScheduler
  - 注册回调与任务
- `EventScheduler_RunTasks(0x6F05DF90)`
  - 构造上下文并进入循环调度

### 1.3 主循环核心
- `sub_6F05F710`（MainLoop 核心）
  - **准备阶段**：`SelectWorker(0x05DE80)` / `PrepareWait(0x05DEE0)`
  - **等待阶段**：`WaitGate(0x158940)` 或 `SleepGate(0x1648A0)`
  - **超时执行阶段**：
    - `PrepareDispatch(0x05FCA0)`
    - `RunCallbacks(0x0603B0)`
    - `MessagePump(0x059B00)` -> `Dispatch(0x05A310)`
    - `FinalizeDispatch(0x05FD10)`
    - `QueueFlush(0x05B080)`
    - `TickUpdate(0x05FC10)`
  - **收口/重排阶段**：
    - `FinalizeWorker(0x05DCE0)` / `FinalizeTick(0x05FB10)`
    - `ComputeWakeDelta(0x060500)`
    - `Reschedule(0x05EE90)`

### 1.4 Dispatch 子模块（完整 case）
- `EventDispatch(0x05A310)` 覆盖 `case0~case14`。
- 本轮已将性能计时从粗分组（System/Input/Game）升级为 `Case0~Case14` 精确分桶。

| Case | 子函数 | 关键行为（IDA） | 备注 |
|---|---|---|---|
| 0 | `0x059D70` | 循环处理标记并调用 `0x05A160` | 状态收口类 |
| 1 | `0x059DC0` | `LoadResourceBlock(type=1)` | 资源/事件载入 |
| 2 | `0x05A2A0` | 批量 `LoadResourceBlock(type=1)` | 资源批处理 |
| 3 | `0x059E40` | `LoadResourceBlock(type=27)` | 文本/编码相关参数 |
| 4 | `0x05A270` | `LoadResourceBlock(type=28)` | 控制类事件 |
| 5 | `0x059890` + `0x059E00` | 主回调门控 + `sub_6F0573C0` | 主事件回调路径 |
| 6 | `0x059E10` | 状态清零后 `LoadResourceBlock(type=2)` | 重置/同步类 |
| 7 | `0x059E90` | `LoadResourceBlock(type=8)` | 输入/位掩码设置 |
| 8 | `0x059F00` | `LoadResourceBlock(type=9)` | 输入/位掩码清除 |
| 9 | `0x059F70` | `LoadResourceBlock(type=11)` | 输入状态组合 |
| 10 | `0x05A060` | `LoadResourceBlock(type=12)` | 本轮实测命中最多的 dispatch case |
| 11 | `0x05A1F0` | `LoadResourceBlock(type=16)` | 逻辑命令类 |
| 12 | `0x05A0E0` | `LoadResourceBlock(type=13)` | 时间/状态参数写入 |
| 13 | `0x05A160` | `LoadResourceBlock(type=14)` + 收口 | 逻辑收口类 |
| 14 | `0x059890` | 主回调门控 | 回调门路径 |

---

## 2. 代码落地（本轮）

### 2.1 AddressBook 新增字段
- `enginePrepareWait`
- `enginePrepareDispatch`
- `engineFinalizeDispatch`
- `engineTickUpdate`
- `engineFinalizeWorker`
- `engineComputeWakeDelta`

### 2.2 Lifecycle 新增 Hook（仅透传计时）
- `War3MainLoop/Engine/PrepareWait`
- `War3MainLoop/Engine/PrepareDispatch`
- `War3MainLoop/Engine/FinalizeDispatch`
- `War3MainLoop/Engine/TickUpdate`
- `War3MainLoop/Engine/FinalizeWorker`
- `War3MainLoop/Engine/ComputeWakeDelta`

### 2.3 Perf 报告聚合同步
- MainLoop Stage 表增加上述新阶段。
- Dispatch 分桶支持 Case0~Case14 展示。

---

## 3. 自动化实测（2K 全屏）

- 报告：`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_02_24_03_50_25.html`
- 关键指标：
  - `avgFps = 93.758`
  - `avgFrameTimeMs = 10.666`
  - `avgMainThreadCpuMs = 3.727`
  - `avgProcessCpuMs = 6.488`
  - `activeFrameTimeMs = 4.230`
  - `avgIdleWaitCpuMs = 10.666`
  - `cpuCoveragePct = 100%`

### 3.1 关键阶段（本轮新增后可见）
- `Engine/WaitGate`：`10.711ms`（等待门，非纯业务）
- `Engine/TickUpdate`：`1.468ms`（活跃路径大头）
- `Engine/PrepareDispatch`：`0.917ms`
- `Dispatch/Case10`：有稳定命中（约 `0.002ms`）

### 3.2 活跃 CPU 热点（可优化候选）
- `Engine/TickUpdate`（逻辑节拍推进）
- `Engine/PrepareDispatch`（超时分支前置）
- `FQ_Dispatch_Opaque` + `FQ_Total_Trans`（渲染提交）
- `Shadow/Main` + `ShadowMap`（渲染特性成本）

---

## 4. CPU 障碍判定

## 4.1 最大“看起来很大”的项
- `WaitGate` 占比高是**节拍等待**，不是直接优化目标。
- 如果盲目把 WaitGate 当热点，会误判方向。

## 4.2 真正可优化障碍（按优先级）
1. `TickUpdate + PrepareDispatch`（逻辑活跃主路径）
2. `FQ_Dispatch_Opaque + FQ_Total_Trans`（渲染队列提交）
3. `Shadow/Main + ShadowMap`（特性级 CPU/GPU 共同开销）

---

## 5. 可行优化方案（仅渲染层，含预估收益）

| 优先级 | 方案 | 风险 | 预估收益（2K 全屏） | 说明 |
|---|---|---|---|---|
| P0 | 降低计时 Hook 常驻开销（仅录制时计时，未录制最短路径） | 低 | +1 ~ +3 FPS | 已有基础，继续清理高频 scope/原子计数。 |
| P1 | 强化 Opaque 提交接管（`FQ_Dispatch_Opaque` 快路径） | 中 | +3 ~ +8 FPS | 优先做 Opaque，透明保持保守策略。 |
| P1 | 透明队列分级接管（`FQ_Total_Trans` 分层） | 中-高 | +2 ~ +6 FPS | 需严格做材质/混合顺序回归。 |
| P2 | Shadow 特性分档（距离/级联/更新频率） | 中 | +2 ~ +5 FPS（CPU） + GPU 稳定性提升 | 优先保画质不退，做自适应而非硬降级。 |
| P2 | Dispatch Case 热点定向旁路（先打 `Case10`） | 中 | +1 ~ +4 FPS | 需在 IDA 确认 case10 语义边界后实施。 |
| P3 | `RenderBatch_Submit` 前置聚合（更激进） | 高 | +5 ~ +15 FPS | 收益高但回归风险最高，需 A/B 灰度。 |

---

## 6. 下一步（执行顺序）
1. 先做 **P0 + P1(Opaque)**，验证“收益确定性”。
2. 再做透明分级接管（P1-Transparent），每步都走 AutoTest + 截图回归。
3. 最后评估 P3（`RenderBatch_Submit` 前置聚合）是否进入实验分支。
