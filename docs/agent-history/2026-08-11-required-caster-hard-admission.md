# 2026-08-11 必需方向光 Caster 硬容量准入候选

## 运行证据

`war3_perf_report_2026_08_11_09_43_25.html` 的 4483 帧中有 1567 帧 producer incomplete，
共记录 87650 次必需 caster omission。其中 87368 次来自 priority soft-budget skip，
而真实 Arena 峰值仅 288.367 MiB，低于每代 384 MiB 硬上限；Arena overflow、partial
transaction、allocation failure 和 replay range reject 均为零。

方向光 CSM 是原子 publication：任何一个必需 caster 被软阈值跳过都会使整份 candidate
失效。旧图的同 epoch 恢复窗口耗尽后 receiver 变为零强度，下一帧完整 candidate 又让全部
阴影同时恢复，因此产生了用户观察到的“全体同步消失—恢复”周期，而不是单个模型闪烁。

## 候选修改

- `ShadowCaptureBudgetPolicy` 明确标记必需方向光 caster。
- 必需 caster 的 position/blend/UV/index bundle 只按当前代真实硬剩余容量决定准入；
  低/中优先级软阈值不得将原子 CSM 变成 partial candidate。
- 384 MiB/代际和既有事务/fail-closed 不变；超过硬容量仍拒绝，不扩大 Arena、不发布残缺图，
  也不延长 8 帧 last-complete 窗口。
- 新增 `SoftPriorityBudget` completeness 原因，避免将软策略拒绝误报为 freeze failure。
- 纯数值 runnable 穷举 288 MiB required bundle 的全部排列，证明低于 384 MiB 时准入与
  Warcraft draw order 无关，同时覆盖硬容量拒绝和字节求和溢出。

## 验收边界

离线合同和 DLL 构建只能证明准入规则已闭合，不能宣称物理闪烁已经修复。实机 A/B 需要在
相同低视角树木场景确认 producer incomplete、soft-priority omission、receiver zero-strength
全部归零，且 Arena 峰值、TDR 和 GPU 事件不回归。细枝/草影的亚像素边缘抖动属于独立的
Issue #4 过滤稳定性问题，不由本候选处理。
