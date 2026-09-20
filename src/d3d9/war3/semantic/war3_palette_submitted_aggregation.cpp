#include "war3_palette_submitted_aggregation.h"

// M2-5B（2026-09-18）：M2-5 余项 B4——「每帧 submitted skinned palette 聚合」
// 从 d3d9_device.cpp pre-M2-5B :22200-22244 逐字节迁入本模块。
//
// 迁移前 device.cpp（未提交工作树，非 git blob；副本 %TEMP%/device_pre_m2_5b.cpp）
//   = 2,250,786 B /
//   SHA-256 D0E80399703CBF3B7567BAD138782B50FF6093705835F2392A932F9543554811
// 捕获时间：2026-09-18T04:15:05 (machine local time)。证据见
// docs/plan/2026-09-18-m2-5-remainder-record.md。
//
// 块内唯一外部调用是纯模板哈希 bit::fnv1a_iter（util/util_bit.h）。

#include "../../d3d9_war3_scene.h"
#include "../../../util/util_bit.h"

#include <cstdint>

namespace dxvk::war3::semantic {

void War3AggregateSubmittedSkinnedPalette(
    dxvk::War3ShadowCaptureStats& st, bool skinned) {
    // --- BEGIN M2-5B verbatim pre-migration aggregation block (d3d9_device.cpp :22200-22244) ---
      if (skinned) {
        const uint64_t curPaletteHash =
            st.semanticSceneDirectLastSubmittedPaletteHash;
        if (curPaletteHash == 0u) {
          st.semanticSceneSubmittedSkinnedPaletteZeroHashCount++;
        }
        if (st.semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash == 0u)
          st.semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash =
              curPaletteHash;
        if (st.semanticSceneSubmittedSkinnedPaletteCombinedHash == 0u) {
          st.semanticSceneSubmittedSkinnedPaletteCombinedHash =
              curPaletteHash != 0u ? curPaletteHash : 0x9E3779B97F4A7C15ULL;
          st.semanticSceneSubmittedSkinnedPaletteDistinctSampleCount = 1u;
          st.semanticSceneSubmittedSkinnedPaletteRunningLastHash =
              curPaletteHash;
          st.semanticSceneSubmittedSkinnedPaletteRunningSameHashRun = 1u;
          st.semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax =
              1u;
        } else {
          // 滚动 FNV1a：把 hash 按字节序列迭代进去。只要任一 caster 的 palette 变化，
          // CombinedHash 就会变；如果整帧所有 caster 都换了同一套 palette，
          // CombinedHash 就锁住。
          const uint32_t lo = uint32_t(curPaletteHash & 0xFFFFFFFFu);
          const uint32_t hi = uint32_t((curPaletteHash >> 32u) & 0xFFFFFFFFu);
          st.semanticSceneSubmittedSkinnedPaletteCombinedHash =
              bit::fnv1a_iter(
                  bit::fnv1a_iter(
                      st.semanticSceneSubmittedSkinnedPaletteCombinedHash, lo),
                  hi);
          if (curPaletteHash !=
              st.semanticSceneSubmittedSkinnedPaletteRunningLastHash) {
            st.semanticSceneSubmittedSkinnedPaletteDistinctSampleCount++;
            st.semanticSceneSubmittedSkinnedPaletteRunningLastHash =
                curPaletteHash;
            st.semanticSceneSubmittedSkinnedPaletteRunningSameHashRun = 1u;
          } else {
            st.semanticSceneSubmittedSkinnedPaletteRunningSameHashRun++;
            if (st.semanticSceneSubmittedSkinnedPaletteRunningSameHashRun >
                st.semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax) {
              st.semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax =
                  st.semanticSceneSubmittedSkinnedPaletteRunningSameHashRun;
            }
          }
        }
      }
    // --- END M2-5B verbatim pre-migration aggregation block ---
}

} // namespace dxvk::war3::semantic
