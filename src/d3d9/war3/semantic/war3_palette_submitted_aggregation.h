#pragma once

// M2-5B（2026-09-18，device.cpp 语义职责迁移的 M2-5 余项 B4）：把
// d3d9_device.cpp 的 taxonomy 诊断块内「每帧 submitted skinned palette 聚合」
// 子块抽成本模块内的**纯函数**。
//
// 迁出范围（逐字节）：pre-M2-5B device.cpp :22200-22244 的 45 行
// 'if (skinned) { ... }'（Phase 7.48 每帧 skinned palette 滚动聚合）。
// 唯一机械差异：块内的 st 引用改为本函数第一个参数（模块侧与 legacy 侧
// 正文逐字节相同，见 .cpp 的 BEGIN/END 标记）。
//
// 职责：写入 7 个 War3ShadowCaptureStats 字段——
//   semanticSceneSubmittedSkinnedPaletteZeroHashCount
//   semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash
//   semanticSceneSubmittedSkinnedPaletteCombinedHash（顺序敏感滚动 FNV-1a，
//     先 lo 后 hi：累加顺序不得改写）
//   semanticSceneSubmittedSkinnedPaletteDistinctSampleCount
//   semanticSceneSubmittedSkinnedPaletteRunningLastHash（内部 scratch）
//   semanticSceneSubmittedSkinnedPaletteRunningSameHashRun（内部 scratch）
//   semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax
//
// 边界：device.cpp 侧的**调用点编排**（诊断门
// War3SemanticPaletteDiagnosticsRuntime()、casterKey 有效性前置条件）留在原位；
// 本模块不访问任何 device 成员、registry / hook 全局（参数全部显式传入）。
//
// 等价证据：
//   src/d3d9/war3/semantic/tests/
//     war3_palette_submitted_aggregation_legacy_reference.inc（生成器 --m2-5b）
//   src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp（宿主机差分）
//   AutoTest/test_war3_palette_m2_5b_equivalence_static.py（独立门禁）
//   docs/plan/2026-09-18-m2-5-remainder-record.md

#include <cstdint>

namespace dxvk {
struct War3ShadowCaptureStats;
} // namespace dxvk

namespace dxvk::war3::semantic {

// pre-M2-5B device.cpp :22200-22244 的 skinned palette 聚合块（逐字节）。
void War3AggregateSubmittedSkinnedPalette(
    dxvk::War3ShadowCaptureStats& st, bool skinned);

} // namespace dxvk::war3::semantic
