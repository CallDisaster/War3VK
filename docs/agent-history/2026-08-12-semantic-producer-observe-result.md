# Semantic producer Observe 结果

使用 `build32_observe` 在隔离桌面运行“生与死”420 秒，开启 Compact
WorkTable 与 Producer Claim Ledger 的 Observe，关闭 terrain/union bounds
观察及细粒度 breakdown。测试没有产生新的 GPU 事件、incident 或 TDR，测试后
`E:\Work\War3\d3d9.dll` 已恢复到夜间基线哈希。

结果不满足 Consume 门：

- Compact WorkTable：1,333,755 candidates，0 sealed，全部 fallback；
  mismatch 为 0 仅表示没有任何条目进入可比较的 sealed 状态。
- Producer Claim：1,241,783 candidates，1,241,783 missing key；
  canonical owned 1,152,061，strict/logical false-negative 均为 1,152,061，
  predicted 为 0。
- `ResourceStoreBuild` 与 `ManifestCopy` 的报告均值约为 0.045 ms/frame 和
  0.033 ms/frame，低于 0.15 ms/frame 的独立优化门。

因此 Release 继续保持 WorkTable 与 Claim Ledger 为 Off。本轮不根据零
mismatch 宣称 parity，因为没有 sealed/predicted 样本；后续只有在上游能发布
完整 source/material/alpha/route 身份后才重新评估。

报告：
`E:\Work\War3\WarVK\Log\war3_perf_report_auto_2026_08_12_02_19_58.html`。
