// AUTO-GENERATED legacy reference for the M2-5B submitted-skinned-palette
// aggregation migration (M2-5 remainder item B4).
//
// DO NOT EDIT. This file is the verbatim pre-migration text of the per-frame
// submitted skinned palette aggregation block that M2-5B moved out of
// src/d3d9/d3d9_device.cpp (:22200-22244) into
// src/d3d9/war3/semantic/war3_palette_submitted_aggregation.{h,cpp} as
// War3AggregateSubmittedSkinnedPalette.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-5b
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2-5B work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\device_pre_m2_5b.cpp)
//   SHA-256(pre-M2-5B work-tree d3d9_device.cpp) = D0E80399703CBF3B7567BAD138782B50FF6093705835F2392A932F9543554811
//   size = 2250786 bytes, captured 2026-09-18T04:15:05 (machine local time)
//   The pre-M2-5B state equals the post-M2-4 work tree (same SHA as the M2-4
//   record's "post-migration d3d9_device.cpp"); like M1/M2-1/M2-2/M2-3/M2-5/M2-4
//   this is a work-tree file hash, not a git blob hash (the work is uncommitted).
//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization):
//   * This file is included AFTER the M2-1/M2-2/M2-3/M2-5/M2-4 .inc files and
//     shares their 'm2_legacy_reference' namespace. Nothing inside the block is
//     rewritten: the block only reads/writes the first parameter (named st, the
//     same identifier as in device.cpp) and calls the unqualified pure template
//     bit::fnv1a_iter, which resolves through 'using namespace dxvk;' exactly as
//     the pre-M2-5B device.cpp text did.
//   * dxvk::War3ShadowCaptureStats is the SHARED real contract type (the same
//     header on both sides, deliberately not rewritten).
//   * EXACTLY ZERO lines are deleted relative to the device.cpp text. The
//     generator fail-closes unless the module .cpp text between the BEGIN/END
//     markers is byte-identical to this file's body.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {

// --- pre-migration aggregation block: d3d9_device.cpp :22200-22244 (verbatim; not one byte deleted or rewritten) ---
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

} // namespace m2_legacy_reference
