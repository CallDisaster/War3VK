#pragma once

// M2-5（2026-09-18，device.cpp 语义职责迁移的 palette taxonomy 发射块）：
// 把 d3d9_device.cpp :21202-21580 的发射块抽成本模块内的**纯函数**。
//
// 迁出范围（逐字节）：pre-M2-5 device.cpp :21202-21580 的 379 行——skinned
// palette taxonomy 发射块，含：
//   * 34 个 War3ShadowCaptureStats 字段的写入（来源 8 桶 / provenance 6 桶 /
//     稳定性·churn·窗口·stale 归因 20 桶）；
//   * 3 张跨帧存活探针表：PaletteProbeEntry(8192)、LeaseKeyAttributionEntry(8192)、
//     StrictProbeEntry(8192)，全部保持函数内 static thread_local 定长 std::array
//     + 位掩码取槽（存储域随块迁入本翻译单元，初始化时机与存活域不变）。
// 唯一机械差异：块首行别名 `auto& stats = m_war3Scene.shadowStats;` 删除，
// 改为本函数第一个参数（模块侧与 legacy 侧正文逐字节相同）。
//
// 边界：device.cpp 侧的**调用点编排**（B1/B2 等）本轮不动；本模块不访问任何
// device 成员、registry / hook 全局（参数全部显式传入：stats 引用、skinned、
// 来源与 provenance、current-draw 样本指针、effective palette 指针与计数、
// slotIndex、submitted hash、stale-pose 标记、帧号）。
//
// 等价证据：src/d3d9/war3/semantic/tests/
//   war3_palette_taxonomy_emission_legacy_reference.inc（入库生成器 --m2-5）
//   war3_live_palette_selection_test.cpp（宿主机差分，加法式扩展）
//   AutoTest/test_war3_palette_taxonomy_equivalence_static.py（独立门禁）
//   docs/plan/2026-09-18-m2-5-taxonomy-extraction-record.md

#include "../../../util/util_matrix.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk {
struct War3ShadowCaptureStats;
} // namespace dxvk

namespace dxvk::war3::render {
struct CurrentDrawAuthoritativeSample;
enum class PaletteProvenance : uint32_t;
} // namespace dxvk::war3::render

namespace dxvk::war3::semantic {

// 定义在 war3_live_palette_selection.h（M2-2 已迁）；此处只前置声明，
// 重量头不进本头文件。
enum class War3SemanticPaletteSource : uint32_t;

// 纯发射函数：只写 stats，返回值 void；所有状态（3 张探针表）都是本函数的
// 函数内 static thread_local，不依赖任何外部全局。
void War3EmitSemanticPaletteTaxonomy(
    dxvk::War3ShadowCaptureStats& stats, bool skinned,
    War3SemanticPaletteSource paletteSourceThisSubmit,
    const dxvk::war3::render::CurrentDrawAuthoritativeSample* currentDrawSample,
    dxvk::war3::render::PaletteProvenance drawTimeCapturedPaletteProvenance,
    const std::vector<Matrix4>* effectiveCanonicalPalette,
    uint32_t effectiveCanonicalPaletteCount,
    uint32_t paletteSlotIndexThisSubmit, uint64_t submittedPaletteHash,
    bool fromStalePoseRestore,
    uint64_t m_war3ShadowPersistentFrameSerial);

} // namespace dxvk::war3::semantic
