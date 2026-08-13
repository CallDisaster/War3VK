# Semantic producer 开发态 Observe 能力

Release 的 Compact WorkTable 与 Producer Claim Ledger 继续固定为 Off。
此前两个 runtime 入口直接受 Release freeze 常量拦截，使单独以
`-Dwarvk_shadow_observers_dev=true` 配置的 DLL 同样无法收集证据。

本候选让开发 observer DLL 仅接受 mode `1`：

- `DXVK_WAR3_SEMANTIC_COMPACT_WORK_TABLE=1` 运行 WorkTable Observe；
- `DXVK_WAR3_SEMANTIC_PRODUCER_CLAIM_LEDGER=1` 运行 Claim Observe；
- `0`、`2` 及其他值全部解析为 Off，不能进入 Consume。

这不改变任何 caster、packet、replay 或 Release 行为。后续必须收集至少
10,000 帧，确认 mismatch/false-positive/false-negative 为零且 Observe 税不超过
0.15 ms/frame，才有资格单独设计 Consume 候选。
