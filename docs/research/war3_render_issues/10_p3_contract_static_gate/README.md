# 研究方向十：P3 契约化 + 渲染优化静态门控（2026-02-24）

## 目标与约束
1. 仅做 `P3 桥接契约化`，不改外部行为语义。
2. 渲染优化按静态门控执行：预计收益 `<5%` 不实现，只保留方案。
3. 本轮验证仅静态口径（编译 + 代码路径一致性），不启动 War3、不出跑分结论。

## 本轮契约化落地
1. 新增 Dispatch 契约层：
   - `src/d3d9/war3/hooks/war3_dispatch_contract.h`
   - `src/d3d9/war3/hooks/war3_dispatch_contract.cpp`
2. 新增 Queue 接管策略层：
   - `src/d3d9/war3/hooks/war3_queue_takeover_policy.h`
   - `src/d3d9/war3/hooks/war3_queue_takeover_policy.cpp`
3. `war3_hook_render.cpp` 收敛为“编排层”：
   - 迁移 `DispatchLocalMergeState` / `DispatchTagStageCacheState` / `QueryTagStageCached`；
   - 迁移 `HasTransparentTakeoverPrerequisites` / `ShouldUseConservativeQueueTakeover` 及保守接管统计；
   - 接管决策改为 `War3QueueTakeoverDecision` 契约结果驱动。

## 静态收益门控方法
- 公式：`预计收益(ms) = 热点当前耗时(ms) × 可消减比例`
- 阈值：`预计收益(ms) >= AvgFrameMs × 5%`
- 参考：近期 `AvgFrameMs=10~12ms`，门槛 `0.5~0.6ms`

## 候选清单与门控结果（静态）
| 候选 | 热点基线（静态参考） | 可消减比例估计 | 预计收益(ms) | 是否过门槛（>=0.5~0.6ms） | 风险 | 结论 |
|---|---:|---:|---:|---|---|---|
| C1 `Dispatch_Common/Special` 分支裁剪与前置归并 | `~1.0-1.3ms` | `35%-45%` | `0.35-0.59` | 临界/部分通过 | 中 | 进入下一轮实现候选（先小步 A/B） |
| C2 `QueryTagStageCached` 二级命中压缩 | `~0.25-0.35ms` | `25%-35%` | `0.06-0.12` | 否 | 低 | 仅保留方案 |
| C3 `FQ_Total_Trans` 条件重排 | `~0.45-0.55ms` | `10%-20%` | `0.05-0.11` | 否 | 中 | 仅保留方案 |
| C4 `Hook_FlushSortedItems` 早退链路压缩 | `~1.2-1.8ms` | `30%-45%` | `0.36-0.81` | 是 | 中 | 进入下一轮实现候选（优先） |
| C5 `SceneCollector` 零成本跳过再收紧 | `~0.08-0.12ms` | `20%-30%` | `0.02-0.04` | 否 | 低 | 仅保留方案 |

> 说明：该表用于“静态优先级筛选”，不是实际性能结论。

## 下一步执行优先级（仅计划，不落代码）
1. P4.1：优先推进 `C4`（`FlushSortedItems` 早退链路）。
2. P4.2：推进 `C1`（`Dispatch` 路径归并），以语义等价为前提分批上线。
3. P4.3：`C2/C3/C5` 保留为低优先级研究项，等待基线抬升后再评估。

## 静态验收结果
1. 架构目标达成：`war3_hook_render.cpp` 从 `1721` 行下降至 `1201` 行（下降约 `30.2%`）。
2. 迁移后策略逻辑集中到新增契约模块，`war3_hook_render.cpp` 以 Hook 编排为主。
3. 本轮不做行为开关变更、不改默认阈值、不做 War3 跑分。

