#include "war3_palette_taxonomy_emission.h"

// M2-5（2026-09-18）：palette taxonomy 发射块从 d3d9_device.cpp :21202-21580
// 逐字节迁入本模块。正文 = 迁移前工作树 device.cpp 的那 379 行，唯一机械差异
// 是删除其中一行别名 `auto& stats = m_war3Scene.shadowStats;` —— 该引用改为
// 本函数第一个参数（模块侧与 legacy 侧正文逐字节相同，见下方 BEGIN/END 标记）。
//
// 迁移前 device.cpp（未提交工作树，非 git blob；副本 %TEMP%/device_pre_m2_5.cpp）
//   = 2,273,048 B /
//   SHA-256 D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE
// 证据见 docs/plan/2026-09-18-m2-5-taxonomy-extraction-record.md。
//
// 块内三张探针表（PaletteProbeEntry 8192 / LeaseKeyAttributionEntry 8192 /
// StrictProbeEntry 8192）保持函数内 static thread_local 定长 std::array +
// 位掩码取槽：存储域随块迁入本翻译单元，初始化时机（首次执行到该语句）与
// 存活域（线程）不变。

#include "../../d3d9_war3_scene.h"
#include "../render/war3_current_draw_contract.h"
#include "../render/war3_visible_renderables.h"
#include "war3_live_palette_selection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk::war3::semantic {

void War3EmitSemanticPaletteTaxonomy(
    dxvk::War3ShadowCaptureStats& stats, bool skinned,
    War3SemanticPaletteSource paletteSourceThisSubmit,
    const dxvk::war3::render::CurrentDrawAuthoritativeSample* currentDrawSample,
    dxvk::war3::render::PaletteProvenance drawTimeCapturedPaletteProvenance,
    const std::vector<Matrix4>* effectiveCanonicalPalette,
    uint32_t effectiveCanonicalPaletteCount,
    uint32_t paletteSlotIndexThisSubmit, uint64_t submittedPaletteHash,
    bool fromStalePoseRestore,
    uint64_t m_war3ShadowPersistentFrameSerial) {
    // --- BEGIN M2-5 verbatim pre-migration emission block (d3d9_device.cpp :21202-21580, minus :21203) ---
    if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {
      // palette source 分桶。
      switch (paletteSourceThisSubmit) {
      case War3SemanticPaletteSource::None:
        stats.semanticSceneSubmittedSkinnedPaletteSourceNoneCount++;
        break;
      case War3SemanticPaletteSource::DrawTimeCaptured:
        stats.semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount++;
        // Phase 1：DrawTimeCaptured 内部按 provenance 细分。
        if (currentDrawSample != nullptr) {
          using PP = dxvk::war3::render::PaletteProvenance;
          switch (drawTimeCapturedPaletteProvenance) {
          case PP::TrustedBlendedWriter:
            stats.semanticSceneSubmittedSkinnedPaletteProvenanceTrustedBlendedWriterCount++;
            break;
          case PP::RawGlobalArena:
            stats.semanticSceneSubmittedSkinnedPaletteProvenanceRawGlobalArenaCount++;
            break;
          case PP::ProducerPartPacket:
            stats.semanticSceneSubmittedSkinnedPaletteProvenanceProducerPartPacketCount++;
            break;
          case PP::RangeCopyPoseRebuild:
            stats.semanticSceneSubmittedSkinnedPaletteProvenanceRangeCopyPoseRebuildCount++;
            break;
          case PP::CModelFallback:
            stats.semanticSceneSubmittedSkinnedPaletteProvenanceCModelFallbackCount++;
            break;
          default:
            stats.semanticSceneSubmittedSkinnedPaletteProvenanceUnknownCount++;
            break;
          }
        } else {
          stats.semanticSceneSubmittedSkinnedPaletteProvenanceUnknownCount++;
        }
        break;
      case War3SemanticPaletteSource::SubmitTimeGlobalSlot:
        stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount++;
        break;
      case War3SemanticPaletteSource::SubmitTimeBlendedPaletteCache:
        stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount++;
        break;
      case War3SemanticPaletteSource::SubmitTimePublishedPoseRegistry:
        stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount++;
        break;
      case War3SemanticPaletteSource::SubmitTimeCModelFallback:
        stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount++;
        break;
      case War3SemanticPaletteSource::OwnedPartSnapshot:
        stats.semanticSceneSubmittedSkinnedPaletteSourceOwnedPartSnapshotCount++;
        // Do not count an owned producer publication as draw-time capture or
        // as a fresh read from the global slot. Exact source is in evidence.
        stats.semanticSceneSubmittedSkinnedPaletteProvenanceProducerPartPacketCount++;
        break;
      }

      // 稳定 part 身份下的帧间稳定性采样：仅在能从 manifestPartLeaseKey
      // 确认 part 身份时才入表。无 lease key 的 skinned packet 不参与。
      uint64_t stablePartKey = 0u;
      uint32_t contractPayloadWord11C = 0u;
      uint32_t contractCapturedPaletteCount = 0u;
      if (currentDrawSample != nullptr) {
        stablePartKey = dxvk::war3::render::VisibleRenderableRegistry::
            computeShadowManifestPartKey(currentDrawSample->contract);
        contractPayloadWord11C = currentDrawSample->contract.payloadWord11C;
        contractCapturedPaletteCount =
            currentDrawSample->contract.capturedPaletteCount;
      }
      // Phase 7.29：在 leaseKey 基础上叠加 payload11C 作为 strict slice 判定。
      // 这把 key 只用于 probe，不替代 lease 身份；目的是回答"是否
      // paletteCountChurn / LargeDelta 在 strict 粒度下消失"。
      uint64_t strictSliceKey = 0u;
      if (stablePartKey != 0u) {
        strictSliceKey = bit::fnv1a_init();
        strictSliceKey = bit::fnv1a_iter(strictSliceKey, stablePartKey);
        strictSliceKey =
            bit::fnv1a_iter(strictSliceKey, contractPayloadWord11C);
      }
      if (stablePartKey != 0u && submittedPaletteHash != 0u) {
        struct PaletteProbeEntry {
          uint64_t lastFrame = 0u;
          uint64_t lastHash = 0u;
          uint32_t lastSlotIndex = 0xFFFFFFFFu;
          War3SemanticPaletteSource lastSource =
              War3SemanticPaletteSource::None;
          std::array<uint64_t, 4> hashWindow = {0u, 0u, 0u, 0u};
          std::array<uint32_t, 4> slotWindow = {
              0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
          uint8_t windowCursor = 0u;
          uint8_t windowFill = 0u;
          // Phase 7.28：记录上一帧提交时第一根矩阵的 translation 与 count，
          // 用来估算帧间 palette 是连续动画（小位移）还是跳变（大位移）。
          float lastFirstMatrixTx = 0.0f;
          float lastFirstMatrixTy = 0.0f;
          float lastFirstMatrixTz = 0.0f;
          uint32_t lastPaletteCount = 0u;
          bool hasLastFirstMatrix = false;
          // Phase 7.30 Step A：记录上一帧该 stable part 是否来自 core stale-pose
          // restore（allowStalePoseForCore 分支）；用来把本帧的 LargeDelta 
          // 分类为 stale→live 过渡 vs live→live 真动画。
          bool lastFromStaleRestore = false;
        };
        // thread_local 开放寻址小型 LRU：key 碰撞时直接覆盖，采样本身允许
        // 丢失；目标是提供"大多数 stable part 的 palette hash 稳不稳"
        // 的观察，不是精确审计。
        static constexpr size_t kPaletteProbeEntries = 8192u;
        static thread_local std::array<uint64_t, kPaletteProbeEntries>
            s_paletteProbeKeys = {};
        static thread_local std::array<PaletteProbeEntry, kPaletteProbeEntries>
            s_paletteProbeEntries = {};
        const size_t slot =
            size_t(stablePartKey) & (kPaletteProbeEntries - 1u);
        auto& storedKey = s_paletteProbeKeys[slot];
        auto& entry = s_paletteProbeEntries[slot];
        const bool sameKey = storedKey == stablePartKey;
        // Phase 7.28：抓本帧 first matrix 的 translation 做帧间幅度比较。
        float currentFirstTx = 0.0f;
        float currentFirstTy = 0.0f;
        float currentFirstTz = 0.0f;
        bool hasCurrentFirstMatrix = false;
        if (effectiveCanonicalPalette != nullptr &&
            !effectiveCanonicalPalette->empty()) {
          const Matrix4& first = (*effectiveCanonicalPalette)[0];
          // Matrix4 用列优先保存；translation 在最后一列（row .w）。
          currentFirstTx = first[3][0];
          currentFirstTy = first[3][1];
          currentFirstTz = first[3][2];
          hasCurrentFirstMatrix = true;
        }
        if (sameKey) {
          stats.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount++;
          if (entry.lastHash != submittedPaletteHash) {
            stats.semanticSceneSubmittedSkinnedPaletteHashChurnCount++;
          }
          if (entry.lastSource != paletteSourceThisSubmit) {
            stats.semanticSceneSubmittedSkinnedPaletteSourceChurnCount++;
          }
          if (entry.lastSlotIndex != paletteSlotIndexThisSubmit &&
              paletteSourceThisSubmit ==
                  War3SemanticPaletteSource::SubmitTimeGlobalSlot &&
              entry.lastSlotIndex != 0xFFFFFFFFu &&
              paletteSlotIndexThisSubmit != 0xFFFFFFFFu) {
            stats.semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount++;
          }
          // Phase 7.28：帧间 first-matrix translation delta 分桶。
          // 连续动画一般 < 0.1f；>= 1.0f 更像 slot 被别的对象覆盖导致错读。
          // palette count 变化也作为"结构性错位"的旁证。
          if (entry.hasLastFirstMatrix && hasCurrentFirstMatrix &&
              entry.lastHash != submittedPaletteHash) {
            const float dx = currentFirstTx - entry.lastFirstMatrixTx;
            const float dy = currentFirstTy - entry.lastFirstMatrixTy;
            const float dz = currentFirstTz - entry.lastFirstMatrixTz;
            const float deltaSq = dx * dx + dy * dy + dz * dz;
            if (deltaSq > 1.0f) {
              // 大跳变：疑似 slot 复用错读。
              stats
                  .semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount++;
              // Phase 7.30 Step A：按"上一帧是否 stale restore"归因。
              //   stale→live：Codex 判定的 stutter-catchup 过渡，属于"不是真动画"。
              //   live→live：连续两帧都 live 时仍跳，属于"真动画" 或 "arena 错读"。
              // 注意这一帧本身是否 stale 不影响归因，重点是"上一帧是否用旧 pose 垫"。
              if (entry.lastFromStaleRestore && !fromStalePoseRestore) {
                stats
                    .semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount++;
              } else if (!entry.lastFromStaleRestore && !fromStalePoseRestore) {
                stats
                    .semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount++;
              }
            } else if (deltaSq > 0.01f) {
              // 中等跳变：可见的动画或相机切换。
              stats
                  .semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount++;
            } else {
              // 小位移：正常连续动画。
              stats
                  .semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount++;
            }
          }
          if (entry.lastPaletteCount != 0u &&
              entry.lastPaletteCount != effectiveCanonicalPaletteCount) {
            stats.semanticSceneSubmittedSkinnedPaletteCountChurnCount++;
          }
        } else {
          // 新 key 或被别的 key 冲掉的 slot：重置历史窗口。
          entry = PaletteProbeEntry{};
          storedKey = stablePartKey;
        }
        // 环形窗口：记录最近 4 帧的 hash 与 slotIndex，用于 unique-in-window。
        entry.hashWindow[entry.windowCursor] = submittedPaletteHash;
        entry.slotWindow[entry.windowCursor] = paletteSlotIndexThisSubmit;
        entry.windowCursor = (entry.windowCursor + 1u) % uint8_t(4u);
        if (entry.windowFill < 4u) {
          entry.windowFill++;
        } else {
          // 满窗口后才统计 uniqueness，避免早期数据污染。
          std::array<uint64_t, 4> uniqueHashes = {0u, 0u, 0u, 0u};
          std::array<uint32_t, 4> uniqueSlots = {
              0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
          uint32_t uniqueHashCount = 0u;
          uint32_t uniqueSlotCount = 0u;
          for (uint32_t i = 0u; i < 4u; ++i) {
            const uint64_t h = entry.hashWindow[i];
            bool seen = false;
            for (uint32_t j = 0u; j < uniqueHashCount; ++j) {
              if (uniqueHashes[j] == h) {
                seen = true;
                break;
              }
            }
            if (!seen)
              uniqueHashes[uniqueHashCount++] = h;
            const uint32_t s = entry.slotWindow[i];
            if (s == 0xFFFFFFFFu)
              continue;
            bool seenSlot = false;
            for (uint32_t j = 0u; j < uniqueSlotCount; ++j) {
              if (uniqueSlots[j] == s) {
                seenSlot = true;
                break;
              }
            }
            if (!seenSlot)
              uniqueSlots[uniqueSlotCount++] = s;
          }
          if (uniqueHashCount >
              stats.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax) {
            stats.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax =
                uniqueHashCount;
          }
          if (uniqueSlotCount >
              stats
                  .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax) {
            stats
                .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax =
                uniqueSlotCount;
          }
        }
        entry.lastFrame = m_war3ShadowPersistentFrameSerial;
        entry.lastHash = submittedPaletteHash;
        entry.lastSlotIndex = paletteSlotIndexThisSubmit;
        entry.lastSource = paletteSourceThisSubmit;
        if (hasCurrentFirstMatrix) {
          entry.lastFirstMatrixTx = currentFirstTx;
          entry.lastFirstMatrixTy = currentFirstTy;
          entry.lastFirstMatrixTz = currentFirstTz;
          entry.hasLastFirstMatrix = true;
        }
        entry.lastPaletteCount = effectiveCanonicalPaletteCount;
        // Phase 7.30 Step A：把"本帧这份 packet 是 stale-pose restored"的状态
        // 落进 entry，下一帧 deltaSq>=1.0 分类时就能识别 stale→live 过渡。
        entry.lastFromStaleRestore = fromStalePoseRestore;
        if (fromStalePoseRestore) {
          stats
              .semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount++;
        }

        // Phase 7.29：在 leaseKey 下聚合 payload11C / capturedPaletteCount
        // 的"多值存在"证据。只要本帧同一个 leaseKey 看到过第二个不同的
        // payload11C 或 paletteCount，就说明 leaseKey 对 palette attribution
        // 来说是过粗的。用单帧 thread_local 小型表累积；在 stats 里只计
        // "本次采样首次检测到多值"的事件数，避免同帧重复累加。
        struct LeaseKeyAttributionEntry {
          uint64_t leaseKey = 0u;
          uint64_t frame = 0u;
          uint32_t firstPayload11C = 0u;
          uint32_t firstPaletteCount = 0u;
          bool multi11CReported = false;
          bool multiPaletteCountReported = false;
        };
        static constexpr size_t kLeaseAttrEntries = 8192u;
        static thread_local std::array<LeaseKeyAttributionEntry,
                                       kLeaseAttrEntries>
            s_leaseAttrEntries = {};
        const size_t leaseAttrSlot =
            size_t(stablePartKey) & (kLeaseAttrEntries - 1u);
        auto& leaseAttrEntry = s_leaseAttrEntries[leaseAttrSlot];
        if (leaseAttrEntry.leaseKey != stablePartKey ||
            leaseAttrEntry.frame != m_war3ShadowPersistentFrameSerial) {
          leaseAttrEntry = LeaseKeyAttributionEntry{};
          leaseAttrEntry.leaseKey = stablePartKey;
          leaseAttrEntry.frame = m_war3ShadowPersistentFrameSerial;
          leaseAttrEntry.firstPayload11C = contractPayloadWord11C;
          leaseAttrEntry.firstPaletteCount = contractCapturedPaletteCount;
        } else {
          if (!leaseAttrEntry.multi11CReported &&
              leaseAttrEntry.firstPayload11C != contractPayloadWord11C) {
            leaseAttrEntry.multi11CReported = true;
            stats
                .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount++;
          }
          if (!leaseAttrEntry.multiPaletteCountReported &&
              leaseAttrEntry.firstPaletteCount != contractCapturedPaletteCount) {
            leaseAttrEntry.multiPaletteCountReported = true;
            stats
                .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount++;
          }
        }
      }

      // Phase 7.29：strict slice probe。
      // 如果在 strict key 下 paletteCountChurn / LargeDelta 基本归零，
      // 说明问题是 lease key 粒度过粗；capture/snapshot 机制本身没有
      // 错配。此时下一刀应该是"给 palette attribution 专门拆一把
      // 包含 payload11C（甚至 F0）的 key"，而不是迁移 snapshot key。
      if (strictSliceKey != 0u && submittedPaletteHash != 0u) {
        struct StrictProbeEntry {
          uint64_t lastFrame = 0u;
          uint64_t lastHash = 0u;
          uint32_t lastPaletteCount = 0u;
          float lastFirstMatrixTx = 0.0f;
          float lastFirstMatrixTy = 0.0f;
          float lastFirstMatrixTz = 0.0f;
          bool hasLastFirstMatrix = false;
        };
        static constexpr size_t kStrictProbeEntries = 8192u;
        static thread_local std::array<uint64_t, kStrictProbeEntries>
            s_strictProbeKeys = {};
        static thread_local std::array<StrictProbeEntry, kStrictProbeEntries>
            s_strictProbeEntries = {};
        const size_t strictSlot =
            size_t(strictSliceKey) & (kStrictProbeEntries - 1u);
        auto& strictStoredKey = s_strictProbeKeys[strictSlot];
        auto& strictEntry = s_strictProbeEntries[strictSlot];
        const bool strictSameKey = strictStoredKey == strictSliceKey;
        float strictCurrentTx = 0.0f;
        float strictCurrentTy = 0.0f;
        float strictCurrentTz = 0.0f;
        bool strictHasCurrentFirstMatrix = false;
        if (effectiveCanonicalPalette != nullptr &&
            !effectiveCanonicalPalette->empty()) {
          const Matrix4& first = (*effectiveCanonicalPalette)[0];
          strictCurrentTx = first[3][0];
          strictCurrentTy = first[3][1];
          strictCurrentTz = first[3][2];
          strictHasCurrentFirstMatrix = true;
        }
        if (strictSameKey) {
          stats.semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount++;
          if (strictEntry.lastHash != submittedPaletteHash) {
            stats
                .semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount++;
          }
          if (strictEntry.hasLastFirstMatrix && strictHasCurrentFirstMatrix &&
              strictEntry.lastHash != submittedPaletteHash) {
            const float dx = strictCurrentTx - strictEntry.lastFirstMatrixTx;
            const float dy = strictCurrentTy - strictEntry.lastFirstMatrixTy;
            const float dz = strictCurrentTz - strictEntry.lastFirstMatrixTz;
            const float deltaSq = dx * dx + dy * dy + dz * dz;
            if (deltaSq > 1.0f) {
              stats
                  .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount++;
            } else if (deltaSq > 0.01f) {
              stats
                  .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount++;
            } else {
              stats
                  .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount++;
            }
          }
          if (strictEntry.lastPaletteCount != 0u &&
              strictEntry.lastPaletteCount != effectiveCanonicalPaletteCount) {
            stats
                .semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount++;
          }
        } else {
          strictEntry = StrictProbeEntry{};
          strictStoredKey = strictSliceKey;
        }
        strictEntry.lastFrame = m_war3ShadowPersistentFrameSerial;
        strictEntry.lastHash = submittedPaletteHash;
        strictEntry.lastPaletteCount = effectiveCanonicalPaletteCount;
        if (strictHasCurrentFirstMatrix) {
          strictEntry.lastFirstMatrixTx = strictCurrentTx;
          strictEntry.lastFirstMatrixTy = strictCurrentTy;
          strictEntry.lastFirstMatrixTz = strictCurrentTz;
          strictEntry.hasLastFirstMatrix = true;
        }
      }
    }
    // --- END M2-5 verbatim pre-migration emission block ---
}

} // namespace dxvk::war3::semantic
