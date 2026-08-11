#pragma once

#include "war3/render/war3_render_state.h"
#include "war3/render/war3_shadow_bounds_policy.h"
#include "war3/gpu_skin/war3_gpu_skin_types.h"

#include "../dxvk/dxvk_buffer.h"
#include "../dxvk/dxvk_image.h"   // 用于 Rc<DxvkImageView> (Alpha测试阴影)
#include "../dxvk/dxvk_sampler.h" // 用于 Rc<DxvkSampler>

#include "../util/util_matrix.h"

#include <array>
#include <vector>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dxvk {

    // Diagnostic histogram contract for the native render stages attached to
    // accepted shadow casters. Bins 0..31 map to the engine stage verbatim;
    // the last bin collects negative and out-of-range stages.
    constexpr size_t kWar3ShadowStageHistogramStageCount = 32u;
    constexpr size_t kWar3ShadowStageHistogramBinCount =
        kWar3ShadowStageHistogramStageCount + 1u;
    constexpr size_t kWar3ShadowCategoryHistogramBinCount = 7u;

    struct War3WorldCameraState {
        bool valid = false;
        Matrix4 view;
        Matrix4 proj;
        Matrix4 viewProj;
        Matrix4 invViewProj;
        // 用于深度重建与 UI 区域裁剪的主世界 viewport/scissor
        D3DVIEWPORT9 viewport = { };
        RECT scissor = { };
        // 0..2 resource slot retained for same-frame state diagnostics only.
        uint32_t frameIndex = 0;
        // Monotonic frame in which these matrices were actually captured.
        uint64_t frameSerial = 0;
    };

    // A single immediately preceding perspective capture may bridge a
    // transient orthographic/overlay frame. Anything older must not be paired
    // with current depth or re-published to post effects.
    constexpr uint64_t kWar3WorldCameraFallbackMaxAgeFrames = 1u;

    inline bool War3WorldCameraIsFreshForFrame(
        const War3WorldCameraState& camera, uint64_t currentFrameSerial) {
      return camera.valid && currentFrameSerial != 0u &&
          camera.frameSerial != 0u &&
          camera.frameSerial <= currentFrameSerial &&
          currentFrameSerial - camera.frameSerial <=
              kWar3WorldCameraFallbackMaxAgeFrames;
    }

    struct War3ShadowMatrixPalette {
        uint64_t hash = 0;
        std::array<Matrix4, 256> worldMatrices = { };
    };

    // VS-S1 阴影重放只持有 generation-pinned 输入的值语义和强引用。
    // compute 输出 VB 仍留在 War3ShadowCasterDraw 中，任何校验失败都可原路兜底。
    struct War3GpuSkinDrawInput {
        war3::gpu_skin::GpuSkinInputLeaseDesc desc = {};
        DxvkBufferSlice staticSource;
        DxvkBufferSlice palette;
        uint64_t storageLeaseId = 0u;
        uint64_t storagePageGeneration = 0u;
        uint32_t storagePageId = 0u;
        // true 仅用于 VS-B1：CPU kernel 已跳过，shader/caster 不得回到
        // 原生动态位置 VB。
        bool irreversible = false;
        bool valid = false;

        explicit operator bool() const noexcept {
            return valid && staticSource.defined() && palette.defined() &&
                   storageLeaseId != 0u && storagePageGeneration != 0u &&
                   storagePageId != 0u;
        }
    };

    enum class War3ShadowPartLifecycleState : uint8_t {
        RequiredCurrent = 0u,
        OptionalHistorical = 1u,
        GraceOneFrame = 2u,
        Tombstoned = 3u,
    };

    struct War3ShadowCasterDraw {
        bool indexed = true;

        // Unified ownership tuple. Raw pointers, JASS handles and VkBuffer
        // slices are meaningful only inside this exact map/device epoch.
        uint64_t mapEpoch = 0u;
        uint64_t deviceEpoch = 0u;

        War3GpuSkinDrawInput gpuSkinInput = {};

        // Vertex input (position only)
        // 注意：DXVK 的 D3D9 buffer 可能在同一帧内被 invalidate（更换 VkBuffer backing）。
        // 若仅存 DxvkBufferSlice，在 shadow caster 重放时 getSliceInfo() 可能拿到“新的 VkBuffer”，
        // 进而读取到错误/被覆盖的数据（表现为阴影残缺/错位/乱飞）。
        // 因此需要在 capture 时固化 VkBuffer/offset，并持有对应 allocation 保活。
        Rc<DxvkBuffer> positionStorage;
        Rc<DxvkResourceAllocation> positionPinnedAllocation;
        DxvkResourceBufferInfo positionInfo;
        uint32_t positionStride = 0;
        uint32_t positionOffset = 0;
        VkFormat positionFormat = VK_FORMAT_UNDEFINED;

        // Index input
        Rc<DxvkBuffer> indexStorage;
        Rc<DxvkResourceAllocation> indexPinnedAllocation;
        DxvkResourceBufferInfo indexInfo;
        VkIndexType indexType = VK_INDEX_TYPE_UINT32;

        // Frozen blend weights/indices storage (for dynamic buffers)
        Rc<DxvkBuffer> blendStorage;
        DxvkResourceBufferInfo blendInfo;

        // Draw params
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        uint32_t indexCount = 0;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t firstVertex = 0;
        uint32_t minVertexIndex = 0;
        uint32_t numVertices = 0;

        // Fixed function transform (best-effort)
        Matrix4 worldMatrix;

        // Vertex blending (fixed function / indexed blending)
        bool vertexBlendEnabled = false;
        bool vertexBlendIndexed = false;
        uint8_t vertexBlendCount = 0;   // 0..3 (weights count), see D3DRS_VERTEXBLEND
        uint32_t paletteIndex = 0;      // index into War3FrameScene::shadowPalettes
        uint32_t blendWeightOffset = 0;
        VkFormat blendWeightFormat = VK_FORMAT_UNDEFINED;
        uint32_t blendIndexOffset = 0;
        VkFormat blendIndexFormat = VK_FORMAT_UNDEFINED;
        uint32_t blendStride = 0;
        uint32_t blendBinding = 0;

        // ===== Alpha测试支持 (用于树叶、栅栏等透明物体的正确阴影) =====
        bool alphaTestEnabled = false;      // 是否启用Alpha测试
        float alphaRef = 0.5f;              // Alpha阈值 (低于此值的像素不投射阴影)
        bool alphaBlendEnabled = false;     // 是否启用 AlphaBlend
        bool depthWriteEnabled = false;     // 是否写入深度
        bool depthTestEnabled = false;      // 是否测试深度
        bool additiveBlend = false;         // 是否为加法混合（特效/发光）

        // UV流 (纹理坐标，用于采样漫反射贴图)
        Rc<DxvkBuffer> uvStorage;           // UV数据缓冲区
        Rc<DxvkResourceAllocation> uvPinnedAllocation;
        DxvkResourceBufferInfo uvInfo;      // UV缓冲区信息
        uint32_t uvStride = 0;              // UV步长
        uint32_t uvOffset = 0;              // UV偏移
        VkFormat uvFormat = VK_FORMAT_UNDEFINED;  // UV格式
        // Vertex-input binding that owns location 3. Binding 0 aliases the
        // position stream, binding 1 aliases the blend stream, and binding 2
        // is an independently captured UV stream.
        uint32_t uvBinding = 0;

        bool HasUsableUvBinding() const {
            if (uvFormat == VK_FORMAT_UNDEFINED || uvStride == 0u ||
                uvOffset >= uvStride ||
                uvBinding > 2u)
                return false;
            if (uvBinding == 0u) {
                return positionInfo.buffer != VK_NULL_HANDLE &&
                       positionInfo.size != 0u &&
                       uvStride == positionStride;
            }
            if (uvStorage == nullptr || uvInfo.buffer == VK_NULL_HANDLE ||
                uvInfo.size == 0u)
                return false;
            if (uvBinding == 1u && blendBinding == 1u) {
                return blendStorage != nullptr &&
                       blendInfo.buffer == uvInfo.buffer &&
                       blendInfo.offset == uvInfo.offset &&
                       blendInfo.size == uvInfo.size &&
                       blendStride == uvStride;
            }
            return true;
        }

        // 漫反射纹理 (Stage 0 纹理，用于读取Alpha通道)
        Rc<DxvkImageView> diffuseTexture;   // 纹理视图 (Ref Count)
        Rc<DxvkSampler>   diffuseSampler;   // 采样器 (Ref Count)
        // Capture-time descriptor snapshot. This is identity/diagnostic data
        // only: DxvkImage backing storage can be relocated, which invalidates
        // the VkImageView stored in this value. Replay must resolve the current
        // descriptor from diffuseTexture immediately before recording.
        DxvkDescriptor    textureDescriptor;
        uint32_t          diffuseSamplerIndex = 0; // [NEW] Bindless Sampler Index

        const DxvkDescriptor* CurrentTextureDescriptor() const {
            if (diffuseTexture == nullptr)
                return nullptr;
            const DxvkDescriptor* descriptor = diffuseTexture->getDescriptor();
            return descriptor != nullptr &&
                   descriptor->legacy.image.imageView != VK_NULL_HANDLE
                ? descriptor
                : nullptr;
        }

        // 分类信息（用于调试/过滤）
        War3RenderState::StageCategory category = War3RenderState::StageCategory::Unknown;
        War3BatchTag batchTag = War3BatchTag::Unknown;
        int16_t stage = -1;
        uint32_t batchHandle = 0; // jHandle（来自 ExecBatch TLS），用于描边筛选
        uint8_t objectKind = 0;   // dxvk::war3::render::ObjectKind
        bool pathBlocker = false; // 已在 producer 端识别为路径/视野阻断器
        bool pathBlockerGeometryMarker = false; // LOSBlocker.mdl-like tiny hidden marker geometry
        // 2026-05-31：caster 自带身份，供 finalize 阶段权威 path blocker 清扫。
        // 各 append 站点已解析的 rawcode/jHandle 直接写入，draw-time consumer
        // 无需依赖 widget cache 反查。
        uint32_t rawcode = 0;
        uint32_t jHandle = 0;

        // Metadata-only producer evidence. These fields are diagnostic and
        // cannot be used to manufacture/replay geometry.
        void* shadowRenderablePart = nullptr;
        uint32_t shadowLayerIndex = 0u;
        uint64_t shadowMetadataKeyHash = 0u;
        // Collision-resistant current-frame geometry identity.  Kept
        // separate from metadataKeyHash so a cache hash can never masquerade
        // as proof that blocker/alpha metadata was actually found.
        uint64_t shadowExactGeometryKeyHash = 0u;
        // Explicit authoritative Unit identity proof for anonymous Stage11
        // draws. ObjectKind and metadata/cache hashes are not identity proof:
        // both can be inherited or synthesized by fallback producers.
        bool shadowUnitIdentityProven = false;
        uint64_t alphaMetadataFrameSerial = 0u;
        uint8_t shadowMetadataBlockerReason = 0u;
        War3ShadowPartLifecycleState shadowPartLifecycleState =
            War3ShadowPartLifecycleState::RequiredCurrent;
        bool alphaPayloadComplete = false;
        uint32_t shadowActualIndexMin = 0u;
        uint32_t shadowActualIndexMax = 0u;
        bool shadowActualIndexDomainKnown = false;
        bool shadowFullVertexDomainFallback = false;
        bool shadowIndexHintMismatch = false;

        // ===== 级联阴影剔除（粗略包围球）=====
        // 说明：用于 CPU 端对每个 CSM 级联做保守剔除，减少“每级联全量重放”的 drawcall 膨胀。
        // 这是保守优化：半径偏大只会减少剔除命中率，不会造成阴影缺失。
        Vector4 boundsCenter = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        float boundsRadius = 0.0f; // 0 表示未提供 bounds（不做剔除）
        war3::render::War3ShadowBoundsProvenance boundsProvenance =
            war3::render::War3ShadowBoundsProvenance::Unknown;
        uint64_t boundsSourceGeneration = 0u;
        uint64_t boundsFrameSerial = 0u;
        bool boundsIdentityProven = false;
        bool boundsSourceWasSkinned = false;
        bool boundsFrameLocalDynamic = false;
        bool boundsAnimatedAttachment = false;
    };

    // fullVertexDomainFallback describes how much backing storage had to be
    // copied, not how many vertices the indexed draw can reference.  When the
    // immutable IB is CPU-opaque, indexCount is still a strict upper bound on
    // the number of unique referenced vertices.  Keep this distinction in one
    // shared helper so producer, final sweep, and replay cannot drift apart.
    inline uint32_t War3ShadowReferencedVertexUpperBound(
        const War3ShadowCasterDraw& draw) {
      if (draw.indexed && draw.shadowFullVertexDomainFallback &&
          !draw.shadowActualIndexDomainKnown && draw.indexCount != 0u) {
        return draw.indexCount;
      }
      return draw.numVertices != 0u ? draw.numVertices : draw.vertexCount;
    }

    enum class War3ShadowReplayMode : uint8_t {
        Unsupported = 0,
        FixedWorld,
        PaletteSkinnedFF,
        SnapshotFallback,
        NativeProjectorHint,
        ProxyCaster,
    };

    struct War3ShadowSemanticContext {
        void* renderablePart = nullptr;
        void* sceneNode = nullptr;
        void* worldObjectEntry = nullptr;
        void* runtimeModelPtr = nullptr;
        void* modelResourcePtr = nullptr;
        const war3::render::RenderObjectInfo* object = nullptr;
        uint32_t jHandle = 0;
        uint32_t rawcode = 0;
        uint64_t modelKey = 0;
        bool hasPoseTransform = false;
        bool poseFromSpriteFrame = false;
        Matrix4 poseTransform;
        float poseScale = 1.0f;
        float poseHeight = 0.0f;
        uint32_t poseMatrixCount = 0;
        uint64_t poseMatrixHash = 0;
        war3::render::ObjectKind objectKind =
            static_cast<war3::render::ObjectKind>(0);
        War3BatchTag tag = War3BatchTag::Unknown;
        int stage = -1;
        bool pathBlocker = false;

        bool HasStableIdentity() const {
            return renderablePart != nullptr || sceneNode != nullptr ||
                   worldObjectEntry != nullptr || object != nullptr ||
                   runtimeModelPtr != nullptr || modelKey != 0u ||
                   jHandle != 0u || rawcode != 0u ||
                   static_cast<uint32_t>(objectKind) != 0u;
        }
    };

    struct War3ShadowGeometryKey {
        uint64_t sourceHash = 0;
        uint64_t layoutHash = 0;
        War3ShadowReplayMode mode = War3ShadowReplayMode::Unsupported;

        bool operator==(const War3ShadowGeometryKey& other) const {
            return sourceHash == other.sourceHash &&
                   layoutHash == other.layoutHash &&
                   mode == other.mode;
        }
    };

    struct War3ShadowPersistentGeometry {
        War3ShadowGeometryKey key = {};

        Rc<DxvkBuffer> positionStorage;
        DxvkResourceBufferInfo positionInfo = {};
        uint32_t positionStride = 0;
        uint32_t positionOffset = 0;
        VkFormat positionFormat = VK_FORMAT_UNDEFINED;

        Rc<DxvkBuffer> indexStorage;
        DxvkResourceBufferInfo indexInfo = {};
        VkIndexType indexType = VK_INDEX_TYPE_UINT32;

        Rc<DxvkBuffer> blendStorage;
        DxvkResourceBufferInfo blendInfo = {};
        uint32_t blendWeightOffset = 0;
        VkFormat blendWeightFormat = VK_FORMAT_UNDEFINED;
        uint32_t blendIndexOffset = 0;
        VkFormat blendIndexFormat = VK_FORMAT_UNDEFINED;
        uint32_t blendStride = 0;
        uint32_t blendBinding = 0;

        Rc<DxvkBuffer> uvStorage;
        DxvkResourceBufferInfo uvInfo = {};
        uint32_t uvStride = 0;
        uint32_t uvOffset = 0;
        VkFormat uvFormat = VK_FORMAT_UNDEFINED;
        uint32_t uvBinding = 0;

        bool alphaTestEnabled = false;
        float alphaRef = 0.5f;
        Rc<DxvkImageView> diffuseTexture;
        Rc<DxvkSampler> diffuseSampler;
        DxvkDescriptor textureDescriptor = {};
        uint32_t diffuseSamplerIndex = 0;

        bool indexed = true;
        bool vertexBlendEnabled = false;
        bool vertexBlendIndexed = false;
        bool pathBlockerGeometryMarker = false;
        uint8_t vertexBlendCount = 0;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        uint32_t indexCount = 0;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t firstVertex = 0;
        uint32_t minVertexIndex = 0;
        uint32_t numVertices = 0;

        Vector4 localBoundsCenter = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        float localBoundsRadius = 0.0f;
        // Bounds are authoritative only when they were computed from the
        // exact vertex domain copied into this immutable geometry record.
        // The source generation is rewritten to the registry-owned geometry
        // generation after insertion, so cache hits never rely on a stale
        // upload/ring generation.
        uint64_t localBoundsSourceGeneration = 0u;
        bool localBoundsIdentityProven = false;
        uint64_t totalBytes = 0;
        uint64_t lastSeenFrame = 0;
    };

    struct War3ShadowInstanceRef {
        uint32_t geometryId = 0;
        uint32_t materialId = 0;
        uint32_t replayDrawIndex = ~0u;
        uint32_t batchHandle = 0;
        uint32_t paletteIndex = 0;
        Matrix4 worldMatrix;
        Vector4 boundsCenter = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        float boundsRadius = 0.0f;
        War3RenderState::StageCategory category =
            War3RenderState::StageCategory::Unknown;
        War3BatchTag batchTag = War3BatchTag::Unknown;
        uint8_t objectKind = 0;
        War3ShadowReplayMode mode = War3ShadowReplayMode::Unsupported;
    };

    struct War3ShadowFallbackDraw {
        War3ShadowCasterDraw snapshot = {};
        const char* reason = "unknown";
        War3ShadowReplayMode mode = War3ShadowReplayMode::SnapshotFallback;
        uint32_t normalizedHandle = 0;
        void* worldObjectEntry = nullptr;
        void* sceneNode = nullptr;
        void* runtimeModelPtr = nullptr;
        uint64_t modelKey = 0;
    };

    struct War3ShadowPersistentGeometryPool {
        uint64_t bytesCap = 0;
        uint64_t bytesUsed = 0;
        uint64_t bytesEvicted = 0;
        uint32_t liveGeometryCount = 0;
        uint32_t promotedThisFrame = 0;
        uint32_t evictedThisFrame = 0;
    };

    // This is deliberately a producer-owned, value-only contract.  A replay
    // list can be internally valid while a required Stage11 caster never made
    // it into that list because exact backing admission was deferred.  The
    // consumer must therefore validate this seal before it is allowed to
    // clear or publish a new shadow target.
    enum class War3RequiredCasterOmissionReason : uint32_t {
        PositionAllocBudget = 1u << 0u,
        UvAllocBudget = 1u << 1u,
        IndexAllocBudget = 1u << 2u,
        AllocationFailure = 1u << 3u,
        FallbackByteBudget = 1u << 4u,
        ArenaAdmission = 1u << 5u,
        FreezeFailure = 1u << 6u,
        SoftPriorityBudget = 1u << 7u,
    };

    inline constexpr uint64_t War3SaturatingAddU64(uint64_t value,
                                                    uint64_t increment,
                                                    bool& overflow) noexcept {
      if (std::numeric_limits<uint64_t>::max() - value < increment) {
        overflow = true;
        return std::numeric_limits<uint64_t>::max();
      }
      return value + increment;
    }

    struct War3ShadowProducerCompleteness {
        // Default-unsealed is intentionally not a complete empty scene.
        bool sealed = false;
        bool counterOverflow = false;
        uint64_t sealFrameSerial = 0u;
        uint64_t mapEpoch = 0u;
        uint64_t deviceEpoch = 0u;
        uint32_t reasonMask = 0u;
        uint64_t requiredCasterOmissionCount = 0u;
        uint64_t exactBudgetDeferredUniqueCasterCount = 0u;
        uint64_t positionAllocBudgetCount = 0u;
        uint64_t uvAllocBudgetCount = 0u;
        uint64_t indexAllocBudgetCount = 0u;
        uint64_t allocationFailureCount = 0u;
        uint64_t fallbackByteBudgetCount = 0u;
        uint64_t arenaAdmissionCount = 0u;
        uint64_t freezeFailureCount = 0u;
        uint64_t softPriorityBudgetCount = 0u;

        void note(War3RequiredCasterOmissionReason reason,
                  bool uniqueCaster) noexcept {
          reasonMask |= static_cast<uint32_t>(reason);
          uint64_t* counter = nullptr;
          switch (reason) {
          case War3RequiredCasterOmissionReason::PositionAllocBudget:
            counter = &positionAllocBudgetCount; break;
          case War3RequiredCasterOmissionReason::UvAllocBudget:
            counter = &uvAllocBudgetCount; break;
          case War3RequiredCasterOmissionReason::IndexAllocBudget:
            counter = &indexAllocBudgetCount; break;
          case War3RequiredCasterOmissionReason::AllocationFailure:
            counter = &allocationFailureCount; break;
          case War3RequiredCasterOmissionReason::FallbackByteBudget:
            counter = &fallbackByteBudgetCount; break;
          case War3RequiredCasterOmissionReason::ArenaAdmission:
            counter = &arenaAdmissionCount; break;
          case War3RequiredCasterOmissionReason::FreezeFailure:
            counter = &freezeFailureCount; break;
          case War3RequiredCasterOmissionReason::SoftPriorityBudget:
            counter = &softPriorityBudgetCount; break;
          }
          *counter = War3SaturatingAddU64(*counter, 1u, counterOverflow);
          if (uniqueCaster) {
            requiredCasterOmissionCount = War3SaturatingAddU64(
                requiredCasterOmissionCount, 1u, counterOverflow);
          }
        }

        void noteExactBudgetDeferredUniqueCaster() noexcept {
          exactBudgetDeferredUniqueCasterCount = War3SaturatingAddU64(
              exactBudgetDeferredUniqueCasterCount, 1u, counterOverflow);
        }

        void seal(uint64_t frameSerial, uint64_t newMapEpoch,
                  uint64_t newDeviceEpoch) noexcept {
          sealFrameSerial = frameSerial;
          mapEpoch = newMapEpoch;
          deviceEpoch = newDeviceEpoch;
          sealed = true;
        }

        bool accepts(uint64_t frameSerial, uint64_t expectedMapEpoch,
                     uint64_t expectedDeviceEpoch) const noexcept {
          return sealed && !counterOverflow &&
              sealFrameSerial == frameSerial && mapEpoch == expectedMapEpoch &&
              deviceEpoch == expectedDeviceEpoch &&
              requiredCasterOmissionCount == 0u;
        }
    };

    struct War3ShadowCaptureStats {
        uint32_t considered = 0;
        uint32_t captured = 0;
        uint32_t capturedIndexed = 0;
        uint32_t capturedNonIndexed = 0;
        uint32_t capturedTerrain = 0;
        // Populated only when DXVK_WAR3_SHADOW_STAGE_HISTOGRAM=1. This is an
        // exact per-frame census over the finalized shadowCasters vector, not
        // a sampled hot-hook counter. It lets a final-frame screenshot be
        // correlated with the producer stage that disappeared.
        std::array<uint32_t, kWar3ShadowStageHistogramBinCount>
            shadowCasterStageHistogram = {};
        std::array<uint32_t, kWar3ShadowCategoryHistogramBinCount>
            shadowCasterCategoryHistogram = {};
        // Diagnostic-only Stage13 continuity chain. Stage13 is native
        // WorldObjects group 2 (decorations/effects), which is the actual
        // producer family used by the bridge/ramp test map. These counters
        // distinguish "the draw never reached capture" from a demand gate,
        // an already-committed BeforeUi boundary, or a later caster filter.
        uint32_t stage13CaptureAttemptCount = 0;
        uint32_t stage13CaptureRejectedNoDemandCount = 0;
        uint32_t stage13CaptureRejectedAfterBeforeUiCount = 0;
        uint32_t stage13CaptureConsideredCount = 0;
        uint32_t beforeUiStage13BoundaryCandidateCount = 0;
        uint32_t beforeUiStage13BoundaryCommitCount = 0;
        // Stage13 retained-caster identity diagnostics. These are exact
        // per-frame counters, intentionally kept outside sampled perf scopes:
        // they explain whether the O(1) source-generation key is unavailable,
        // cold/churning, or actually avoiding the referenced-byte scan.
        uint32_t stage13RetentionBaseEligibleCount = 0;
        uint32_t stage13SourcePositionInvalidCount = 0;
        uint32_t stage13SourceIndexInvalidCount = 0;
        uint32_t stage13SourceIdentityValidCount = 0;
        uint32_t stage13SourceIdentityHitCount = 0;
        uint32_t stage13SourceIdentityMissCount = 0;
        uint32_t stage13StrongScanCount = 0;
        uint32_t stage13SnapshotBuildCount = 0;
        uint32_t stage13SnapshotContentRekeyCount = 0;
        // Byte traffic for the production current-frame lane and the
        // diagnostic-only retained lane. These stay separate so an A/B report
        // cannot confuse GPU freeze copies with host snapshot materialization.
        uint64_t stage13FreezeCopyBytes = 0;
        uint64_t stage13CpuSnapshotCopyBytes = 0;
        uint64_t stage13RetentionSnapshotBytes = 0;
        uint32_t stage13RetainedEntryCountMax = 0;
        uint32_t stage13RetainedContentMatchCount = 0;
        uint32_t stage13RetainedIdentityMatchCount = 0;
        uint32_t stage13RetainedWorldMatchCount = 0;
        uint32_t stage13RetainedMaterialMatchCount = 0;
        uint32_t stage13RetainedLayoutMatchCount = 0;
        uint32_t stage13RetainedAllSemanticMatchCount = 0;
        // Stage-specific terrain gauges. Stage 10 remains the native terrain
        // doodad lane and Stage1 is ordinary terrain. Bridge/ramp objects in
        // the dedicated test map were subsequently proven to use Stage13;
        // their independent continuity counters live above.
        uint32_t terrainDoodadCaptureAttemptCount = 0;
        uint32_t terrainDoodadCaptureAcceptedCount = 0;
        uint32_t terrainDoodadDynamicSourceCount = 0;
        uint32_t terrainDoodadWorldIdentityLikeCount = 0;
        uint32_t terrainDoodadWorldNonIdentityCount = 0;
        uint32_t terrainS1CaptureAttemptCount = 0;
        uint32_t terrainS1CaptureAcceptedCount = 0;
        uint32_t terrainS1WorldIdentityLikeCount = 0;
        uint32_t terrainS1WorldNonIdentityCount = 0;
        uint32_t terrainS1WorldNonFiniteCount = 0;
        uint32_t terrainS1ForceIdentityWorldCount = 0;
        uint64_t terrainS1WorldMatrixHash = 0;
        uint64_t terrainS1WorldTranslationMilliMax = 0;
        uint32_t capturedWorldObject = 0;
        uint32_t capturedUnitObject = 0;
        uint32_t capturedUnitVertexBlend = 0;
        uint32_t persistentUnitInstanceCount = 0;
        uint32_t forcedFallbackWorldFreezeCount = 0;
        uint32_t forcedFallbackUnitFreezeCount = 0;

        uint32_t skippedNotCaster = 0;
        uint32_t skippedNoZTest = 0;
        uint32_t skippedOverlay = 0;
        uint32_t skippedAlphaTest = 0;
        uint32_t skippedVertexShader = 0;
        uint32_t skippedVertexBlend = 0;
        uint32_t skippedPositionT = 0;
        uint32_t skippedNoPosition = 0;
        uint32_t skippedPosFormat = 0;
        uint32_t skippedMissingPerDrawUpload = 0;
        uint32_t persistentRejectNoIdentity = 0;
        uint32_t persistentRejectUnsupportedMode = 0;
        uint32_t persistentRejectDynamicSource = 0;
        uint32_t persistentRejectAlphaBlend = 0;
        uint32_t persistentRejectMissingStorage = 0;
        uint32_t persistentRejectCreateOrBudget = 0;

        uint32_t persistentGeometryCount = 0;
        uint32_t persistentInstanceCount = 0;
        uint32_t staticPersistentCount = 0;
        uint32_t dynamicPoseCount = 0;
        uint32_t dynamicSkinnedOutputCount = 0;
        uint32_t fallbackSnapshotCount = 0;
        uint32_t fallbackDrawCount = 0;
        uint32_t fallbackDrawCountTerrain = 0;
        uint32_t fallbackDrawCountWorldObject = 0;
        uint32_t fallbackDrawCountUnitObject = 0;
        uint32_t semanticBridgeHit = 0;
        uint32_t semanticBridgeMiss = 0;
        uint32_t semanticBridgeBypassed = 0;
        uint32_t semanticSceneSubmitted = 0;
        uint32_t semanticSceneSubmittedUnit = 0;
        uint32_t semanticSceneSubmittedSkinned = 0;
        uint32_t semanticSceneSubmittedSkinnedNonUnitResolvedCount = 0;
        uint32_t semanticSceneSubmittedSkinnedUnknownPacketKindCount = 0;
        uint32_t semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount = 0;
        uint32_t semanticSceneSubmittedSkinnedGroupNonZeroCount = 0;
        uint32_t semanticSceneSubmittedSkinnedTransparentQueueCount = 0;
        uint32_t semanticSceneSubmittedSkinnedMissingUnitPtrCount = 0;
        uint32_t semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount = 0;
        uint32_t semanticSceneSubmittedBuilding = 0;
        uint32_t semanticSceneSubmittedDestructible = 0;
        uint32_t semanticSceneSubmittedCutout = 0;
        uint32_t semanticSceneSubmittedAlphaBlend = 0;
        uint32_t semanticSceneMaterialObservedCutoutCount = 0;
        uint32_t semanticSceneMaterialObservedAlphaBlendCount = 0;
        uint32_t semanticSceneRejectedCutoutSkinnedContract = 0;
        uint32_t semanticSceneRejectedAlphaBlendSkinnedContract = 0;
        uint32_t semanticSceneRejectedCutoutGeometry = 0;
        uint32_t semanticSceneRejectedAlphaBlendGeometry = 0;
        uint32_t semanticSceneRejectedCutoutVisualPolicy = 0;
        uint32_t semanticSceneRejectedAlphaBlendVisualPolicy = 0;
        uint32_t semanticSceneMaterialLayerContractResolvedCount = 0;
        uint32_t semanticSceneMaterialLayerContractFailedCount = 0;
        uint32_t semanticSceneMaterialBlendMode0Count = 0;
        uint32_t semanticSceneMaterialBlendMode1Count = 0;
        uint32_t semanticSceneMaterialBlendMode2PlusCount = 0;
        uint32_t semanticSceneDirectCurrentDrawLayerIndexNonZeroCount = 0;
        uint32_t semanticSceneMaterialLastMeshIndex = 0;
        uint32_t semanticSceneMaterialLastLayerIndex = 0;
        uint32_t semanticSceneMaterialLastLayerCount = 0;
        uint32_t semanticSceneMaterialLastBlendOrDrawMode = 0;
        uint32_t semanticSceneLivePaletteRefreshAttemptCount = 0;
        uint32_t semanticSceneLivePaletteRefreshHitCount = 0;
        uint32_t semanticSceneLivePaletteRefreshMissCount = 0;
        uint32_t semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount = 0;
        uint32_t semanticScenePaletteOverrideNoComposeCount = 0;
        uint32_t semanticScenePaletteOverrideWouldComposeCount = 0;
        uint32_t semanticScenePalettePacketWorldComposeCount = 0;
        uint64_t semanticSceneLivePaletteRefreshLastRuntimeModelPtr = 0;
        uint32_t semanticSceneLivePaletteRefreshLastMatrixCount = 0;
        uint64_t semanticSceneLivePaletteRefreshLastMatrixHash = 0;
        uint32_t semanticSceneLivePaletteMotionSampleCount = 0;
        uint32_t semanticSceneLivePaletteMotionNewRuntimeCount = 0;
        uint32_t semanticSceneLivePaletteMotionRawChangedCount = 0;
        uint32_t semanticSceneLivePaletteMotionRawStableCount = 0;
        uint32_t semanticSceneLivePaletteMotionGroupChangedCount = 0;
        uint32_t semanticSceneLivePaletteMotionGroupStableCount = 0;
        uint64_t semanticSceneLivePaletteMotionLastRuntimeModelPtr = 0;
        uint64_t semanticSceneLivePaletteMotionLastPrevRawHash = 0;
        uint64_t semanticSceneLivePaletteMotionLastRawHash = 0;
        uint64_t semanticSceneLivePaletteMotionLastPrevGroupHash = 0;
        uint64_t semanticSceneLivePaletteMotionLastGroupHash = 0;
        uint32_t semanticSceneDrawTimePoseAttemptCount = 0;
        uint32_t semanticSceneDrawTimePosePublishedCount = 0;
        uint32_t semanticSceneDrawTimePoseRejectUiOrEffectCount = 0;
        uint32_t semanticSceneDrawTimePoseRejectVertexShaderCount = 0;
        uint32_t semanticSceneDrawTimePoseRejectNoVertexBlendCount = 0;
        uint32_t semanticSceneDrawTimePoseRejectNoContextCount = 0;
        uint32_t semanticSceneDrawTimePoseRejectNoRuntimeModelCount = 0;
        uint32_t semanticSceneDrawTimePoseDedupedCount = 0;
        uint32_t semanticSceneDrawTimePoseChangedCount = 0;
        uint32_t semanticSceneDrawTimePoseStableCount = 0;
        uint64_t semanticSceneDrawTimePoseLastRuntimeModelPtr = 0;
        uint64_t semanticSceneDrawTimePoseLastPrevHash = 0;
        uint64_t semanticSceneDrawTimePoseLastHash = 0;
        // Phase 7.55：draw-time D3D palette 聚合 hash。每帧 reset，每次
        // War3TryPublishSemanticDrawTimePose 成功时滚动 FNV1a。
        // 用于对比 frozen 段：如果这个值每帧变但 CombinedHash 冻结，
        // 说明 draw-time D3D palette 是 fresh 的，问题在消费链路。
        uint64_t semanticSceneDrawTimePoseCombinedHash = 0;
        uint32_t semanticSceneDrawTimePoseCombinedSampleCount = 0;
        // Phase 7.55：draw-time VB cache 诊断
        uint32_t drawTimeVBCacheCaptureCount = 0;
        uint32_t drawTimeVBCacheConsumeHitCount = 0;
        uint32_t drawTimeVBCacheConsumeMissCount = 0;
        // Producer-completeness diagnostics.  They mirror the scene-owned
        // seal so runtime reports can explain fail-closed publication without
        // treating generic transparent/blocker rejects as missing casters.
        uint64_t producerRequiredCasterOmissionCount = 0u;
        uint64_t producerExactBudgetDeferredUniqueCasterCount = 0u;
        uint64_t producerPositionAllocBudgetCount = 0u;
        uint64_t producerUvAllocBudgetCount = 0u;
        uint64_t producerIndexAllocBudgetCount = 0u;
        uint64_t producerAllocationFailureCount = 0u;
        uint64_t producerFallbackByteBudgetCount = 0u;
        uint64_t producerArenaAdmissionCount = 0u;
        uint64_t producerFreezeFailureCount = 0u;
        uint64_t producerSoftPriorityBudgetCount = 0u;
        uint64_t producerSealFrameSerial = 0u;
        uint64_t producerSealMapEpoch = 0u;
        uint64_t producerSealDeviceEpoch = 0u;
        uint32_t producerCompletenessReasonMask = 0u;
        uint32_t producerCompletenessSealed = 0u;
        uint32_t producerCompletenessCounterOverflow = 0u;
        uint64_t drawTimeVBCacheStaticLiveBytes = 0u;
        uint64_t drawTimeVBCacheStaticProtectedBytes = 0u;
        uint64_t drawTimeVBCacheStaticOverCapBytes = 0u;
        uint64_t drawTimeVBCacheStaticOverCapFrameCount = 0u;
        uint64_t drawTimeVBCacheStaticEvictedBytes = 0u;
        uint64_t drawTimeVBCacheStaticEvictedEntryCount = 0u;
        // Phase 7.55 v4：诊断 capture path 早退原因
        uint32_t drawTimeVBCacheRejectNoRenderablePart = 0;
        // A renderable part may contain several independent layers. Capturing
        // without the authoritative dispatch/GPU-skin layer would recreate
        // the old cross-layer VB/IB overwrite bug, so such draws fail closed.
        uint32_t drawTimeVBCacheRejectNoLayerContext = 0;
        uint32_t drawTimeVBCacheRejectContractLookup = 0;
        uint32_t drawTimeVBCacheRejectContractFreshness = 0;
        uint32_t drawTimeVBCacheRejectContractStage = 0;
        uint32_t drawTimeVBCacheRejectContractRenderFrame = 0;
        uint32_t drawTimeVBCacheRejectContractInstance = 0;
        uint32_t drawTimeVBCacheRejectContractSlice = 0;
        uint32_t drawTimeVBCacheRejectNoDecl = 0;
        uint32_t drawTimeVBCacheRejectNoPosition = 0;
        uint32_t drawTimeVBCacheRejectInvalidStride = 0;
        uint32_t drawTimeVBCacheRejectNoSlice = 0;
        uint32_t drawTimeVBCacheRejectInvalidRange = 0;
        uint32_t drawTimeVBCacheRejectInsufficientLength = 0;
        uint32_t drawTimeVBCacheRejectNoBuffer = 0;
        // Original draw was indexed, but its mandatory IB backing was not
        // captured. The entry stays incomplete and must not be submitted as a
        // non-indexed fallback.
        uint32_t drawTimeVBCacheRejectIncompleteIndex = 0;
        uint32_t drawTimeVBCacheTotalEntered = 0;
        // Phase 7.69：draw-time GPU copy 成本账本。semantic 路线为了 Pose 正确
        // 每帧冻结 War3 当帧 VB/IB slice；这里记录 copy 命令数、字节数和 buffer
        // reallocation，以便和 legacy VB/IB intercept 基线对账。
        uint32_t drawTimeVBCachePositionCopyCount = 0;
        uint64_t drawTimeVBCachePositionCopyBytes = 0;
        uint32_t drawTimeVBCachePositionAllocCount = 0;
        uint32_t drawTimeAllocObserverEnabled = 0;
        uint32_t drawTimePositionAllocRequestCount = 0;
        uint32_t drawTimePositionAllocNewEntryCount = 0;
        uint32_t drawTimePositionAllocMissingBackingCount = 0;
        uint32_t drawTimePositionAllocCapacityGrowthCount = 0;
        uint32_t drawTimePositionAllocLeaseDetachCount = 0;
        uint32_t drawTimePositionAllocStaticRequestCount = 0;
        uint32_t drawTimePositionAllocDynamicRequestCount = 0;
        uint32_t drawTimePositionDeferredNewEntryCount = 0;
        uint32_t drawTimePositionDeferredMissingBackingCount = 0;
        uint32_t drawTimePositionDeferredCapacityGrowthCount = 0;
        uint32_t drawTimePositionDeferredLeaseDetachCount = 0;
        uint32_t drawTimePositionProofUniqueCount = 0;
        uint32_t drawTimePositionProofDuplicateCount = 0;
        uint32_t drawTimePositionProofInvalidCount = 0;
        uint32_t drawTimePositionProofSetOverflowCount = 0;
        uint64_t drawTimePositionProofUniqueBytes = 0u;
        uint64_t drawTimePositionProofDuplicateBytes = 0u;
        uint32_t drawTimeDirectStaticPositionBindCount = 0;
        uint64_t drawTimeDirectStaticPositionBytes = 0u;
        uint32_t drawTimeDirectStaticIndexBindCount = 0;
        uint64_t drawTimeDirectStaticIndexBytes = 0u;
        uint32_t drawTimeDirectUploadPositionBindCount = 0;
        uint64_t drawTimeDirectUploadPositionBytes = 0u;
        uint32_t drawTimeDirectUploadUvBindCount = 0;
        uint64_t drawTimeDirectUploadUvBytes = 0u;
        uint32_t drawTimeDirectUploadIndexBindCount = 0;
        uint64_t drawTimeDirectUploadIndexBytes = 0u;
        uint32_t drawTimeDirectUploadCandidateCount = 0;
        uint32_t drawTimeDirectUploadRejectNoProofCount = 0;
        uint32_t drawTimeDirectUploadRejectNoStorageCount = 0;
        uint32_t drawTimeDirectUploadRejectRangeCount = 0;
        uint32_t drawTimePositionAllocDirectMutableRequestCount = 0;
        uint32_t drawTimeVBCacheUvCopyCount = 0;
        uint64_t drawTimeVBCacheUvCopyBytes = 0;
        uint32_t drawTimeVBCacheUvSharedPositionCount = 0;
        uint32_t drawTimeVBCacheUvAllocCount = 0;
        uint32_t drawTimeVBCacheIndexCopyCount = 0;
        uint64_t drawTimeVBCacheIndexCopyBytes = 0;
        uint32_t drawTimeVBCacheIndexAllocCount = 0;
        uint32_t drawTimeVBCacheIndexedUnknownRangeFallbackCount = 0;
        uint32_t drawTimeVBCacheUnitCaptureCount = 0;
        uint32_t drawTimeVBCacheBuildingCaptureCount = 0;
        uint32_t drawTimeVBCacheDestructibleCaptureCount = 0;
        uint32_t drawTimeVBCacheEffectCaptureCount = 0;
        uint32_t drawTimeVBCacheOtherKindCaptureCount = 0;
        uint32_t drawTimeVBCacheAlphaTestStateCaptureCount = 0;
        uint32_t drawTimeVBCacheAlphaBlendStateCaptureCount = 0;
        uint32_t drawTimeVBCacheDiffuseTextureCaptureCount = 0;
        uint32_t gpuSkinShadowBackingHitCount = 0;
        uint32_t gpuSkinShadowBackingRejectCount = 0;
        uint32_t gpuSkinShadowBackingFallbackCount = 0;
        uint64_t gpuSkinShadowSkippedCpuCopyBytes = 0;
        // VS-S1 阴影重放累计账本。顺序与 AutoTest 的
        // shadowDirect / shadowReplay schema 完全一致。
        uint64_t gpuSkinVsShadowDirectAttempts = 0u;
        uint64_t gpuSkinVsShadowDirectInputRejects = 0u;
        uint64_t gpuSkinVsShadowDirectStateRejects = 0u;
        uint64_t gpuSkinVsShadowDirectDrawsSubmitted = 0u;
        uint64_t gpuSkinVsShadowDirectBindingsCleared = 0u;
        uint64_t gpuSkinVsShadowReplayDirectional = 0u;
        uint64_t gpuSkinVsShadowReplayPoint = 0u;
        uint64_t gpuSkinVsShadowReplayUnknown = 0u;
        // Phase 7.70：同帧重复捕获去重账本。
        // SameFrameDedupHit 是命中“数据指纹未变”的次数（跳过 GPU copy）。
        // SameFrameDedupMiss 是数据真的变了，必须重做 GPU copy 的次数。
        // SameFrameStateRefresh 是去重命中后仍然刷新了 alpha/world/texture 状态。
        uint32_t drawTimeVBCacheSameFrameDedupHit = 0;
        uint32_t drawTimeVBCacheSameFrameDedupMiss = 0;
        uint32_t drawTimeVBCacheSameFrameStateRefresh = 0;
        uint32_t drawTimeGenerationBackedPositionReuseCount = 0;
        uint32_t drawTimeGenerationBackedUvReuseCount = 0;
        uint32_t drawTimeGenerationBackedIndexReuseCount = 0;
        uint64_t drawTimeGenerationBackedCopyBytesSaved = 0;
        uint32_t drawTimeSemanticProducerVisibleCandidateCount = 0;
        uint32_t drawTimeSemanticProducerFreshEntryCount = 0;
        uint32_t drawTimeSemanticProducerClaimedCount = 0;
        uint32_t drawTimeSemanticProducerSubmittedCount = 0;
        uint32_t drawTimeSemanticProducerMissNoFreshEntryCount = 0;
        uint32_t drawTimeSemanticProducerFallbackCurrentDrawCount = 0;
        // Stage11 current-frame geometry is authoritative whenever its
        // producer has already submitted (or explicitly rejected) the exact
        // logical slice.  Count generic records suppressed by that ownership
        // decision so runtime gates can prove that the two representations no
        // longer alternate for the same part.
        uint32_t drawTimeSemanticProducerOwnedDirectGroupedSkipCount = 0;
        uint32_t drawTimeSemanticProducerLifecycleMergedCount = 0;
        uint32_t semanticSceneRejectedPathBlockerCount = 0;
        // Phase 7.73：路径阻断器拒绝来源分桶。每个 reject 站点用独立 counter，
        // 让 full trace 能直接指认 path blocker 走的是哪条出口。
        // - EarlyBypass：早期 bypass 分支（非 terrain，已构建 semantic 后）
        // - EligibilityGate：War3ShouldSubmitSemanticPacket 入口
        // - AppendEntry：War3TryAppendSemanticShadowPacket 主入口（packet.rawcode）
        // - AppendEntryByJHandle：append 入口的 jHandle 兜底反查
        // - AppendVbBlend：append 函数内 v4 vertex-blend 分支（skinned 命中）
        // - FastAppend：tryAppendDrawTimeFastEligible（先看 packet 后看 entry.rawcode）
        // - DirectGroupedRecords：directRecords 完整重建 / 简化重建路径
        // - Producer：War3TryPopulateDrawTimeSemanticProducer 提交时
        // - StaticSupplement：trySupplementDirectCurrentDrawStaticScene
        // - LegacyCapture：War3TryCaptureShadowCaster legacy 主体（很少触发）
        uint32_t semanticSceneRejectedPathBlockerEarlyBypassCount = 0;
        uint32_t semanticSceneRejectedPathBlockerEligibilityGateCount = 0;
        uint32_t semanticSceneRejectedPathBlockerAppendEntryCount = 0;
        uint32_t semanticSceneRejectedPathBlockerAppendEntryByJHandleCount = 0;
        uint32_t semanticSceneRejectedPathBlockerAppendVbBlendCount = 0;
        uint32_t semanticSceneRejectedPathBlockerFastAppendCount = 0;
        uint32_t semanticSceneRejectedPathBlockerDirectGroupedCount = 0;
        // Generation-tagged DirectGrouped control-plane work table. Mode:
        // 0=Off, 1=Observe, 2=Consume. Only exact current-generation sealed
        // items may bypass repeated ownership/policy/key work.
        uint32_t semanticSceneCompactWorkTableMode = 0;
        uint32_t semanticSceneCompactWorkTableCandidateCount = 0;
        uint32_t semanticSceneCompactWorkTableSealedCount = 0;
        uint32_t semanticSceneCompactWorkTableConsumedCount = 0;
        uint32_t semanticSceneCompactWorkTableFallbackCount = 0;
        uint32_t semanticSceneCompactWorkTableRejectStageCount = 0;
        uint32_t semanticSceneCompactWorkTableRejectFreshnessCount = 0;
        uint32_t semanticSceneCompactWorkTableRejectPolicyCount = 0;
        uint32_t semanticSceneCompactWorkTableRejectFrameCount = 0;
        uint32_t semanticSceneCompactWorkTableRejectIdentityCount = 0;
        uint32_t semanticSceneCompactWorkTableMismatchCount = 0;
        // Observe-only pre-build predictor for a future Producer Claim
        // Ledger. The strict key retains payloadWord11C; the logical key
        // deliberately omits that known draw-local field. Neither key may
        // authorize a skip until both have been compared with the existing
        // post-build exact-owner decision on real maps.
        uint32_t semanticSceneProducerClaimObserveMode = 0;
        uint32_t semanticSceneProducerClaimExactKeyCount = 0;
        uint32_t semanticSceneProducerClaimCandidateCount = 0;
        uint32_t semanticSceneProducerClaimCanonicalOwnedCount = 0;
        uint32_t semanticSceneProducerClaimMissingKeyCount = 0;
        uint32_t semanticSceneProducerClaimUnresolvedCount = 0;
        uint32_t semanticSceneProducerClaimStrictPredictedCount = 0;
        uint32_t semanticSceneProducerClaimStrictMatchCount = 0;
        uint32_t semanticSceneProducerClaimStrictFalsePositiveCount = 0;
        uint32_t semanticSceneProducerClaimStrictFalseNegativeCount = 0;
        uint32_t semanticSceneProducerClaimLogicalPredictedCount = 0;
        uint32_t semanticSceneProducerClaimLogicalMatchCount = 0;
        uint32_t semanticSceneProducerClaimLogicalFalsePositiveCount = 0;
        uint32_t semanticSceneProducerClaimLogicalFalseNegativeCount = 0;
        uint32_t semanticSceneProducerClaimConsumeDeniedCount = 0;
        uint32_t semanticSceneRejectedPathBlockerProducerCount = 0;
        uint32_t semanticSceneRejectedPathBlockerStaticSupplementCount = 0;
        uint32_t semanticSceneRejectedPathBlockerLegacyCaptureCount = 0;
        uint32_t semanticSceneDirectDrawTimePrebuildBypassAttemptCount = 0;
        uint32_t semanticSceneDirectDrawTimePrebuildBypassHitCount = 0;
        // fast-append bounds 数据源一致性探针。Pose 来自本次 draw 捕获时已经
        // 完成的 semantic augment；SceneRead 是原有 sceneNode + VirtualQuery 路径。
        uint32_t semanticSceneFastAppendBoundsPoseAvailableCount = 0;
        uint32_t semanticSceneFastAppendBoundsSceneReadSuccessCount = 0;
        uint32_t semanticSceneFastAppendBoundsPoseDeltaLe1Count = 0;
        uint32_t semanticSceneFastAppendBoundsPoseDeltaLe4Count = 0;
        uint32_t semanticSceneFastAppendBoundsPoseDeltaLe16Count = 0;
        uint32_t semanticSceneFastAppendBoundsPoseDeltaGt16Count = 0;
        uint32_t semanticSceneFastAppendBoundsPoseDeltaMaxMilli = 0;
        uint32_t semanticSceneSubmittedPaletteMotionSampleCount = 0;
        uint32_t semanticSceneSubmittedPaletteMotionNewRuntimeCount = 0;
        uint32_t semanticSceneSubmittedPaletteMotionChangedCount = 0;
        uint32_t semanticSceneSubmittedPaletteMotionStableCount = 0;
        uint64_t semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr = 0;
        uint64_t semanticSceneSubmittedPaletteMotionLastPrevHash = 0;
        uint64_t semanticSceneSubmittedPaletteMotionLastHash = 0;
        uint32_t semanticSceneSkinnedDynamicIndexSliceCount = 0;
        uint32_t semanticSceneSubmittedOwnedGroupSlots = 0;
        uint32_t semanticSceneSubmittedExplicitBlendContract = 0;
        uint32_t semanticSceneSubmittedSingleMatrixGroupSkinning = 0;
        uint32_t semanticSceneSubmittedMultiGroupSlotSkinning = 0;
        uint32_t semanticSceneSkinnedMinUniqueGroupSlots = 0;
        uint32_t semanticSceneSkinnedMaxUniqueGroupSlots = 0;
        uint32_t semanticSceneSkinnedGroupSlotsUnique1Count = 0;
        uint32_t semanticSceneSkinnedGroupSlotsUnique2To4Count = 0;
        uint32_t semanticSceneSkinnedGroupSlotsUnique5To8Count = 0;
        uint32_t semanticSceneSkinnedGroupSlotsUnique9To16Count = 0;
        uint32_t semanticSceneSkinnedGroupSlotsUnique17PlusCount = 0;
        uint32_t semanticSceneExplicitBlendUnavailableCurrentDraw = 0;
        uint32_t semanticSceneCurrentDrawContractKnownCount = 0;
        uint32_t semanticSceneCurrentDrawPaletteReadyCount = 0;
        uint32_t semanticSceneCurrentDrawGroupSlotReadyCount = 0;
        uint32_t semanticSceneCurrentDrawResolveReadyCount = 0;
        uint32_t semanticSceneCurrentDrawMissNoContract = 0;
        uint32_t semanticSceneCurrentDrawMissNoPalette = 0;
        uint32_t semanticSceneCurrentDrawMissNoGroupSlots = 0;
        uint32_t semanticSceneCurrentDrawMissStaleVisibleFrame = 0;
        uint32_t semanticSceneCurrentDrawResolveReadyRejectedCount = 0;
        uint32_t semanticSceneCanonicalReadyCount = 0;
        uint32_t semanticSceneCanonicalReadyCutoutCount = 0;
        uint32_t semanticSceneCanonicalReadyAlphaBlendCount = 0;
        uint32_t semanticSceneCanonicalRejectNoStableIdentity = 0;
        uint32_t semanticSceneCanonicalRejectNoMesh = 0;
        uint32_t semanticSceneCanonicalRejectNoWorldTransform = 0;
        uint32_t semanticSceneCanonicalRejectNoPalette = 0;
        uint32_t semanticSceneCanonicalRejectNoSlotContract = 0;
        uint32_t semanticSceneCanonicalRejectStaleProducer = 0;
        uint32_t semanticSceneCanonicalRejectInvalidVertexIndex = 0;
        uint32_t semanticSceneCanonicalRejectExplicitBlendIncomplete = 0;
        uint32_t semanticSceneCanonicalRejectAfterReadyCount = 0;
        uint32_t semanticSceneSkinnedFullIndexFallbackCount = 0;
        uint32_t semanticSceneSkinnedMissingVisibleIndexSliceRejectCount = 0;
        uint64_t semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr = 0;
        uint32_t semanticSceneSkinnedFullIndexFallbackLastIndexCount = 0;
        uint32_t semanticSceneSubmittedFrameLocal = 0;
        uint32_t semanticSceneInputDrawCount = 0;
        uint32_t semanticSceneInputSkinnedCount = 0;
        uint32_t semanticSceneTailBoundaryCandidateCount = 0;
        uint32_t semanticSceneTailBoundaryCommitCount = 0;
        uint32_t semanticScenePopulateAttemptCount = 0;
        uint32_t semanticScenePopulateUnitsOnlyCount = 0;
        uint32_t semanticScenePopulateLastReturnReason = 0;
        uint32_t semanticScenePopulateLastProducerPublishAttemptDelta = 0;
        uint32_t semanticScenePopulateLastProducerPublishReadyDelta = 0;
        uint32_t semanticScenePopulateLastProducerQueryAttemptDelta = 0;
        uint32_t semanticScenePopulateLastProducerQueryHitDelta = 0;
        uint32_t semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta = 0;
        uint32_t semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta = 0;
        uint32_t semanticScenePopulateLastProducerGroupDecodeAttemptDelta = 0;
        uint32_t semanticScenePopulateLastProducerGroupDecodeHitDelta = 0;
        uint32_t semanticSceneDirectCurrentDrawRecordCount = 0;
        uint32_t semanticSceneDirectCurrentDrawBuiltPacketCount = 0;
        uint32_t semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount = 0;
        uint32_t semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount = 0;
        uint32_t semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount = 0;
        uint32_t semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount = 0;
        uint32_t semanticSceneDirectSelectionKeyUnitPtrCount = 0;
        uint32_t semanticSceneDirectSelectionKeyJHandleCount = 0;
        uint32_t semanticSceneDirectSelectionKeyRuntimeModelCount = 0;
        uint32_t semanticSceneDirectSelectionKeyWorldObjectCount = 0;
        uint32_t semanticSceneDirectSelectionKeySceneNodeCount = 0;
        uint32_t semanticSceneDirectSelectionKeyModelMeshCount = 0;
        uint32_t semanticSceneDirectSelectionKeyRenderablePartCount = 0;
        // === Phase 7.1: caster selection stability diagnostics ===
        // --- 本帧 gauge（每次 populate 刷新，不累加） ---
        uint32_t semanticSceneDirectLastRawRecordCount = 0;           // snapshot 返回的原始 record 数
        uint32_t semanticSceneDirectLastEligibleRecordCount = 0;      // 通过 alpha/cutout 预筛 + packet build 成功的 record 数
        uint32_t semanticSceneDirectLastSubmittedRecordCount = 0;     // 最终 append 成功的 record 数
        uint32_t semanticSceneDirectLastUniqueObjectCount = 0;        // 本帧 eligible 中 unique sceneNode 数
        uint32_t semanticSceneDirectLastSubmittedObjectCount = 0;     // 本帧 submit 中 unique sceneNode 数
        uint32_t semanticSceneDirectLastRecordCapPartialObjectCount = 0;  // 因 recordCap 截断导致 geoset 不完整的 object 数
        uint32_t semanticSceneDirectLastScanCapPartialObjectCount = 0;    // 因 scanCap 截断导致 geoset 不完整的 object 数（当前为 0，真实计算待后续推进）
        uint32_t semanticSceneDirectLastMinGeosetsPerObject = 0;      // 本帧 eligible 中最少 geoset/object
        uint32_t semanticSceneDirectLastMaxGeosetsPerObject = 0;      // 本帧 eligible 中最多 geoset/object
        uint64_t semanticSceneDirectLastSubmittedIdentityHash = 0;    // 本帧提交集合 identity hash
        uint32_t semanticSceneDirectRecordCapSkipObjectCount = 0;     // recordCap 导致的整 object 跳过次数（separate from append fail）
        uint32_t semanticSceneDirectRecordCapAppendFailCount = 0;     // recordCap 内 append 失败的 record 数
        // --- 累计 counter ---
        uint32_t semanticSceneDirectRecordCapHitCount = 0;            // recordCap 命中次数
        uint32_t semanticSceneDirectRecordCapTruncatedRecordCount = 0; // recordCap 截断的 record 总数
        uint32_t semanticSceneDirectScanCapHitCount = 0;              // scanCap 命中次数
        uint32_t semanticSceneDirectIdentityChurnCount = 0;           // 帧间 identity hash 变化次数
        uint32_t semanticSceneDirectObjectGroupedSubmitCount = 0;     // object-grouped submit 模式执行次数
        uint32_t semanticSceneDirectObjectGroupedSkipCount = 0;       // object-grouped 因预算跳过整个 object 的次数
        uint32_t semanticSceneDirectSelectionLeaseActiveKeyCount = 0; // 本帧 sticky lease 参与 selection 的 key 数
        uint32_t semanticSceneDirectSelectionLeasePrunedKeyCount = 0; // sticky lease 过期清理的 key 数
        uint32_t semanticSceneDirectSelectionLeaseSubmittedKeyCount = 0; // sticky lease 续租的 submitted key 数
        uint32_t semanticSceneDirectStickyFillBudgetRecordCount = 0; // sticky completion 本帧允许的 record 预算
        uint32_t semanticSceneDirectStickyFillAppendedCount = 0; // preselection 阶段用余量补入的 previous record 数
        uint32_t semanticSceneDirectStickyFillSubmittedCount = 0; // append 阶段实际越过 base cap 的 previous record 数
        uint32_t semanticSceneDirectStickyFillMissedCount = 0; // 余量仍不足而跳过的 previous record 数
        uint32_t semanticSceneDirectPartLeaseRestoredCount = 0; // one-frame part packet lease 本帧恢复的 packet 数
        uint32_t semanticSceneDirectPartLeaseUpdatedCount = 0; // 本帧写入/续租的 part packet lease 数
        uint32_t semanticSceneDirectPartLeaseExpiredCount = 0; // 本帧过期清理的 part packet lease 数
        uint32_t semanticSceneDirectPartLeaseRejectedDynamicMeshCount = 0; // 动态 position backing 不允许跨帧 lease
        uint32_t semanticSceneDirectPartLeaseRejectedNotSelfContainedCount = 0; // 缺 owned backing/palette/group-slot 等自包含数据
        uint32_t semanticSceneDirectPartLeaseRejectedUnsafeBackingCount = 0; // main-world visible backing 复核失败
        uint32_t semanticSceneDirectPartLeaseRejectedSelfRenewCount = 0; // lease 恢复项不允许再次续租
        uint32_t semanticSceneDirectPartLeaseBudgetLimitCount = 0; // lease 恢复被本帧预算上限挡住
        uint32_t semanticSceneShadowManifestPartLeaseRestoredCount = 0; // manifest-owned part lease 本帧恢复的 packet 数
        uint32_t semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount = 0; // live submitted packet 写入 manifest lease
        uint32_t semanticSceneShadowManifestPartLeaseExpiredCount = 0; // manifest lease 过期清理
        uint32_t semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount = 0; // pose evidence 超过 skinned TTL
        uint32_t semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount = 0; // slice evidence 超过 TTL
        uint32_t semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount = 0; // main-world/material/backing 复核失败
        uint32_t semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount = 0; // packet 缺少可跨帧自持有数据
        uint32_t semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount = 0; // restored packet 不允许续租
        uint32_t semanticSceneShadowManifestPartLeaseBudgetLimitCount = 0; // manifest lease 恢复被预算挡住
        uint32_t semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount = 0; // Phase 7.25: one-frame core restore 允许 stale pose 的 part 数
        uint32_t semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount = 0; // Phase 7.25: lease restore 用同帧 CModel pose 刷新 palette
        uint32_t semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount = 0; // Phase 7.25: CModel pose 存在但无法重建 self-contained runtime palette
        uint32_t semanticSceneShadowManifestPartLeasePaletteRefreshAttemptCount = 0; // Phase 7.37: restored skinned lease 尝试用 producer palette 刷新
        uint32_t semanticSceneShadowManifestPartLeasePaletteRefreshHitCount = 0; // Phase 7.37: producer/global-slot palette 刷新成功
        uint32_t semanticSceneShadowManifestPartLeasePaletteRefreshMissCount = 0; // Phase 7.37: producer/global-slot palette 刷新失败
        uint32_t semanticSceneShadowManifestPartLeasePaletteRefreshAppliedCount = 0; // Phase 7.37: fresh palette 已覆盖 lease packet
        uint32_t semanticSceneShadowManifestPartLeasePaletteRefreshFallbackCount = 0; // Phase 7.37: 刷新失败后实际沿用旧 palette 兜底提交
        uint32_t semanticSceneShadowManifestObjectCoreCompleteCount = 0; // Phase 7.25: 上一轮 core part set 已凑齐的 object 数
        uint32_t semanticSceneShadowManifestObjectCoreIncompleteSkipCount = 0; // Phase 7.25: core 缺件而整 object 跳过次数
        uint32_t semanticSceneShadowManifestPartOmittedIncompleteCoreCount = 0; // Phase 7.25: 因 core 缺件省略的候选 part 数
        // Phase 7.25 core epoch planner 专属计数器。
        uint32_t semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount = 0; // 本帧由 live 提交扩容或推进 committed core 的 object 数
        uint32_t semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount = 0; // 本帧需要 lease 补齐才凑成 committed core 的 object 数
        uint32_t semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount = 0; // 本帧 live+lease 仍无法覆盖 committed core 的 object 数
        uint32_t semanticSceneShadowManifestObjectCoreEpochMissingPartCount = 0; // 本帧因 core 缺件被跳过的 part 总数
        uint32_t semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount = 0; // restored/leased packet 尝试自续 committed core 被拒绝的次数
        uint32_t semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount = 0; // lease 确认失效时同步从 committed/observation core 精确移除的 part 数
        uint32_t semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount = 0; // lease 失效收缩后不再含任何 core/observation part 的 object 数
        uint32_t semanticSceneShadowManifestLeaseExpiredBackingOnlyCount = 0; // lease expiry 仅释放 packet/backing，不改 authoritative core
        uint32_t semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount = 0; // 连续两帧 authoritative live manifest 缺席后退休
        uint32_t semanticSceneShadowManifestMissingRequiredPartCount = 0; // 当前 group 缺少 committed required part；仅跳过该 part
        uint32_t semanticSceneShadowManifestGraceUsedCount = 0; // 使用一帧 self-contained grace 的 part 数
        uint32_t semanticSceneShadowManifestTombstoneRetiredCount = 0; // 权威 tombstone 立即退休的记录数
        // Phase 7.28：skinned palette content stability probe。
        // 当 `semanticSceneSubmittedSkinned` > 0 时，这些计数反映本帧提交的
        // skinned packet 的 palette 稳定性与来源分布。
        // Source 分布（按 submitted skinned packet 计数）：
        uint32_t semanticSceneSubmittedSkinnedPaletteSourceNoneCount = 0;                // packet fallback（submit 未能取到 live palette）
        uint32_t semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount = 0;    // current-draw 捕获（最稳定）
        uint32_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount = 0;// Game.dll+0xBC6BD0 slot 通路
        uint32_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount = 0;// QueryBlendedPaletteBySlotIndex
        uint32_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount = 0;
        // Phase 7.48：per-frame submitted skinned palette 聚合诊断。
        // 目的：区分"指标错觉"（lastSubmittedPaletteHash 只是最后一个 caster）
        // 与"真冻结"（整帧所有 caster palette 都锁住）。
        //   CombinedHash：本帧所有 skinned submit 的 palette hash 做 FNV1a 滚动聚合，
        //     不混 worldMatrix/mesh content，只看 palette。如果它跨帧也多帧不变，
        //     那就是真冻结；如果它每帧都变，lastSubmittedPaletteHash 多帧不变就是
        //     指标错觉（场景只有 1 个主 skinned caster，最后一个 append 恰好相同）。
        //   DistinctSampleCount：本帧里出现过的不同 palette hash 数量。单 caster
        //     场景下 = submitted 数；多 caster idle 场景下 < submitted。
        //   ConsecutiveSameHashCountMax：同一次本帧 submit 序列里连续相同 hash 的
        //     最长 run。如果 = submitted 总数，说明本帧所有 caster hash 都一样
        //     （idle unit 场景可能出现）。
        //   FirstSubmittedHash：本帧第一个 append 的 palette hash（可与
        //     lastSubmittedPaletteHash 对比验证 "最后一个和第一个是不是同一个对象"）。
        uint64_t semanticSceneSubmittedSkinnedPaletteCombinedHash = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteDistinctSampleCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteZeroHashCount = 0;  // palette hash 为 0（无 trusted）
        // 内部 scratch，用来算 ConsecutiveSameHashCountMax。不需要透传到上层。
        uint64_t semanticSceneSubmittedSkinnedPaletteRunningLastHash = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteRunningSameHashRun = 0;
        // 稳定 part 身份下的 palette / slot 稳定性：
        uint32_t semanticSceneSubmittedSkinnedPaletteStablePartSampleCount = 0;          // 拥有稳定 manifestPartLeaseKey 的 skinned submit 个数
        uint32_t semanticSceneSubmittedSkinnedPaletteHashChurnCount = 0;                 // 同 key 本帧 hash 与上一帧不一致
        uint32_t semanticSceneSubmittedSkinnedPaletteSourceChurnCount = 0;               // 同 key 本帧 source 与上一帧不一致
        uint32_t semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount = 0;            // 同 key 本帧 slotIndex 与上一帧不一致（仅 SubmitTimeGlobalSlot 有值）
        uint32_t semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax = 0;          // 最近 N 帧内同 key 看到过的不同 hash 数量峰值
        uint32_t semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax = 0;     // 最近 N 帧内同 key 看到过的不同 slotIndex 数量峰值
        // Phase 7.28：帧间 first-matrix translation delta 分桶。
        // 只在 hash 真的变了时记录；用来区分"正常动画 <0.01"、"可见位移 >=0.01"
        // 和"疑似 slot 错读 >=1.0"。
        uint32_t semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteCountChurnCount = 0;                // 同 key paletteCount 帧间不同（结构性错位）
        // Phase 7.29：差分探针——leaseKey 是否把不同 slice 合并了。
        //   Multi11CValueCount：同一 leaseKey 在同一帧内被看到过两个不同 payload11C
        //   MultiPaletteCountValueCount：同一 leaseKey 在同一帧内 capturedPaletteCount 多值
        uint32_t semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount = 0;
        // Phase 7.29：strict slice key（leaseKey + payload11C）下的重采样。
        // 用来检验 leaseKey 粒度是否把不同 slice 合并导致 paletteCountChurn。
        uint32_t semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount = 0;
        // Phase 7.30 Step A：stale→live 过渡归因。
        // PaletteStaleRestoreSubmittedCount：本帧提交的 packet 是 stale-pose restored
        //   （eligible.fromStalePoseRestore=true）。等价于 manifestPartLeaseRestoredPoseStaleCore
        //   的"实际进入提交链"版本。
        // PaletteAfterStaleRestoreLargeDeltaCount：上一帧对该 stable part 记录为 stale
        //   restored，本帧换成 live（fromStalePoseRestore=false）时 firstMatrix deltaSq>=1.0。
        //   这就是 stutter-catchup 的直接证据；若它 ≈ PaletteFirstMatrixLargeDeltaCount，
        //   说明 LargeDelta 几乎全来自 stale→live 过渡，而不是真动画。
        // PaletteLiveToLiveLargeDeltaCount：连续两帧都 live 时仍出现的 LargeDelta，
        //   属于"真动画快速位移" 或 "palette arena 错读"；修 palette writer 覆盖率
        //   时需要把这条归零。
        uint32_t semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount = 0;
        // Phase 7.30 Action B：attribution-key snapshot 命中次数。
        // 只在"per-thread cache miss + global byPart miss"后走 attribution
        // 表的 hit 才计；可直接反映"renderablePart 换地址时 snapshot 被救
        // 回"的情况。
        uint32_t semanticSceneDirectPaletteAttributionSnapshotHitCount = 0;
        // Phase 7.30 Action B 第二刀：capture 端 trusted palette 源的使用情况。
        uint32_t semanticSceneDirectPaletteCaptureTrustedSourceHitCount = 0;
        uint32_t semanticSceneDirectPaletteCaptureTrustedSourceMissCount = 0;
        // Phase 1 Producer Packet Takeover：palette provenance 分桶。
        // 记录 submit 端实际消费的 skinned palette 来自哪条路径。
        uint32_t semanticSceneSubmittedSkinnedPaletteProvenanceTrustedBlendedWriterCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteProvenanceRawGlobalArenaCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteProvenanceProducerPartPacketCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteProvenanceRangeCopyPoseRebuildCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteProvenanceCModelFallbackCount = 0;
        uint32_t semanticSceneSubmittedSkinnedPaletteProvenanceUnknownCount = 0;
        uint32_t semanticSceneDirectStickyPartSelectionRetainedCount = 0; // part-level sticky 后保留的 record 数
        uint32_t semanticSceneDirectStickyPartSelectionDroppedCount = 0; // part-level sticky 后丢弃的新 part record 数
        uint32_t semanticSceneDirectStickyPartSelectionFallbackCount = 0; // part-level sticky 因保留过少回退次数
        // === Phase 7.5: object completeness diagnostics ===
        uint32_t semanticSceneDirectManifestObjectCount = 0; // snapshot/preselection 中的 object bin 数
        uint32_t semanticSceneDirectManifestObservedPartCount = 0; // snapshot 观测到的 record/part 数
        uint32_t semanticSceneDirectManifestShadowEligiblePartCount = 0; // alpha visual policy 后仍可作为 caster 的 part 数
        uint32_t semanticSceneDirectObjectCompleteEligibleCount = 0; // eligible part 全部 build/slice 成功的 object 数
        uint32_t semanticSceneDirectObjectIncompleteByScanCapCount = 0; // scanCap 命中时可能已被截断的 object 数
        uint32_t semanticSceneDirectObjectIncompleteByAlphaPolicyCount = 0; // 因 alpha/cutout visual policy 排除部分 part 的 object 数
        uint32_t semanticSceneDirectObjectIncompleteBySliceUnresolvedCount = 0; // skinned visible slice 无法解析的 object 数
        uint32_t semanticSceneDirectObjectIncompleteByPacketBuildFailCount = 0; // packet build 失败导致 part 缺失的 object 数
        uint32_t semanticSceneDirectObjectIncompleteByAppendFailCount = 0; // append 失败导致 submitted part 缺失的 object 数
        uint32_t semanticSceneDirectSubmittedCompleteObjectCount = 0; // 本帧完整提交的 object 数
        uint32_t semanticSceneDirectSubmittedPartialObjectCount = 0; // 本帧部分提交的 object 数
        uint32_t semanticSceneDirectPreparedSliceAuthoritativeCount = 0; // prepared-backing 权威 slice 数（探针接入前应为 0）
        uint32_t semanticSceneDirectPreparedSliceFallbackLayerIndexCount = 0; // 仍使用 layerIndex/subIndex fallback 的 slice 数
        uint32_t semanticSceneDirectPreparedSliceMissingCount = 0; // skinned object 缺 visible slice 数
        uint32_t semanticScenePreparedProbeAttemptCount = 0;
        uint32_t semanticScenePreparedProbeContextReadyCount = 0;
        uint32_t semanticScenePreparedProbeBackingReadableCount = 0;
        uint32_t semanticScenePreparedSliceRecordedCount = 0;
        uint32_t semanticScenePreparedSliceQueryAttemptCount = 0;
        uint32_t semanticScenePreparedSliceQueryHitCount = 0;
        uint32_t semanticScenePreparedSliceQueryMissCount = 0;
        // === Phase 7.21: persistent shadow manifest diagnostics ===
        uint32_t semanticSceneShadowManifestObjectCount = 0;
        uint32_t semanticSceneShadowManifestPartCount = 0;
        uint32_t semanticSceneShadowManifestStableObjectCount = 0;
        uint32_t semanticSceneShadowManifestNewObjectCount = 0;
        uint32_t semanticSceneShadowManifestExpiredObjectCount = 0;
        uint32_t semanticSceneShadowManifestFreshPartCount = 0;
        uint32_t semanticSceneShadowManifestLeaseablePartCount = 0;
        uint32_t semanticSceneShadowManifestPoseStalePartCount = 0;
        uint32_t semanticSceneShadowManifestSliceStalePartCount = 0;
        uint32_t semanticSceneShadowManifestExpiredPartCount = 0;
        uint32_t semanticSceneShadowManifestMultiSlicePartCount = 0;
        uint32_t semanticSceneShadowManifestPayload11CChurnCount = 0;
        uint32_t semanticSceneShadowManifestRenderablePartChurnCount = 0;
        uint32_t semanticSceneShadowManifestCModelPoseHitCount = 0;
        uint32_t semanticSceneShadowManifestCModelPoseMissCount = 0;
        uint32_t semanticSceneShadowManifestCModelPoseNoRuntimeCount = 0;
        uint64_t
            semanticSceneShadowManifestPoseFreshGenerationVerifierMismatchCount =
                0;
        uint64_t semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr = 0;
        uint32_t semanticSceneShadowManifestCModelPoseLastMatrixCount = 0;
        uint64_t semanticSceneShadowManifestCModelPoseLastMatrixHash = 0;
        uint32_t semanticSceneSubmittedObjectJaccardMilli = 0;
        uint32_t semanticSceneSubmittedPartJaccardMilli = 0;
        uint32_t semanticSceneVisibleLookupPartLayerHitCount = 0;
        uint32_t semanticSceneVisibleLookupSingleFallbackCount = 0;
        uint32_t semanticSceneVisibleLookupMissCount = 0;
        uint32_t semanticSceneDirectMainWorldBackingNotCheckedCount = 0;
        uint32_t semanticSceneDirectMainWorldBackingPassCount = 0;
        uint32_t semanticSceneDirectMainWorldBackingFailNoRenderablePartCount = 0;
        uint32_t semanticSceneDirectMainWorldBackingFailLookupMissCount = 0;
        uint32_t semanticSceneDirectMainWorldBackingFailNonMainQueueCount = 0;
        uint32_t semanticSceneDirectMainWorldBackingFailNonWorldGroupCount = 0;
        uint32_t semanticSceneDirectMainWorldBackingFailIdentityMismatchCount = 0;
        uint32_t semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount = 0;
        uint32_t semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount = 0;
        // === Phase 7.2: single-caster flicker root cause diagnostics ===
        // --- 单 caster contract 稳定性 per-frame gauge ---
        uint64_t semanticSceneDirectLastSubmittedSceneNode = 0;       // 本帧最近一次 authoritative submitted record 的 sceneNode
        uint64_t semanticSceneDirectLastSubmittedPaletteHash = 0;     // 本帧最近一次 authoritative submitted 的 palette hash
        uint64_t semanticSceneDirectLastSubmittedGroupHash = 0;       // 本帧最近一次 authoritative submitted 的 groupHash
        uint64_t semanticSceneDirectLastSubmittedStableGroupHash = 0; // 本帧最近一次 authoritative submitted 的 stable groupHash（不含 stream1Ptr）
        uint64_t semanticSceneDirectLastSubmittedStream1Ptr = 0;      // 本帧最近一次 authoritative submitted 的 stream1Ptr
        uint64_t semanticSceneDirectLastSubmittedGeometrySourceHash = 0; // 本帧最近一次 authoritative submitted 的 geometry sourceHash
        uint64_t semanticSceneDirectLastSubmittedRenderablePart = 0;  // 本帧最近一次 authoritative submitted 的 renderablePart
        uint64_t semanticSceneDirectLastSubmittedMeshData = 0;        // 本帧最近一次 authoritative submitted 的 meshData
        // --- 帧间 churn counters（累计） ---
        uint32_t semanticSceneDirectPaletteHashChurnCount = 0;        // palette hash 帧间变化次数
        uint32_t semanticSceneDirectGroupHashChurnCount = 0;          // group hash 帧间变化次数
        uint32_t semanticSceneDirectStableGroupHashChurnCount = 0;    // stable group hash 帧间变化次数
        uint32_t semanticSceneDirectStream1PtrChurnCount = 0;         // stream1Ptr 帧间变化次数
        uint32_t semanticSceneDirectGeometrySourceHashChurnCount = 0; // geometry source hash 帧间变化次数
        uint32_t semanticSceneDirectSameCasterComparisonCount = 0;    // 同 caster 成功比较次数
        uint32_t semanticSceneDirectIdentitySkippedChurnCount = 0;    // 因 identity 变化跳过的 churn 比较次数
        uint32_t semanticSceneDirectPaletteRootDeltaSampleCount = 0;  // palette hash 变化且可比较 root matrix 的次数
        uint32_t semanticSceneDirectPaletteRootHashChangedTinyDeltaCount = 0; // palette hash 变但 root matrix 基本不动
        uint32_t semanticSceneDirectPaletteRootHashChangedSmallDeltaCount = 0; // root matrix 小幅变化
        uint32_t semanticSceneDirectPaletteRootHashChangedMediumDeltaCount = 0; // root matrix 中等变化
        uint32_t semanticSceneDirectPaletteRootHashChangedLargeDeltaCount = 0; // root matrix 大跳变，疑似采样窗口/对象串线
        uint32_t semanticSceneDirectPaletteRootMaxDeltaMilli = 0;     // root matrix 最大元素 delta * 1000
        // --- append-time geometry key 诊断（从 War3TryAppendSemanticShadowPacket 写回） ---
        uint64_t semanticSceneLastAppendedGeometrySourceHash = 0;
        uint32_t semanticSceneLastAppendedGeometryId = 0;
        uint64_t semanticSceneLastAppendedRenderablePart = 0;
        uint64_t semanticSceneLastAppendedMeshData = 0;
        // --- 影子提交 -> 重放 -> 执行对账 ---
        uint32_t semanticSceneShadowCastersCount = 0;                 // shadowCasters.size() 本帧
        uint32_t semanticSceneReplayDrawsCount = 0;                   // replayDraws count 本帧
        uint32_t semanticSceneShadowMapDrawnCasters = 0;              // renderShadowMap 实际 draw 调用数
        uint32_t semanticSceneShadowMapCascadeCulledCount = 0;        // cascade cull 跳过数
        uint32_t semanticSceneTerrainBoundsCullMode = 0;
        // Development-observer diagnostics for the producer proof chain.
        // These counters never authorize culling; they identify the first
        // missing prerequisite before a terrain draw reaches the final policy.
        uint32_t semanticSceneTerrainBoundsProducerS1AttemptCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerFallbackAttemptCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerExactRangeCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerMissingExactRangeCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerUpSourceAttemptCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerMappedSourceAttemptCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerNoSourceCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerSpanAcceptedCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerSpanRejectedCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerSpanNullBaseRejectCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerSpanNotCpuReadableRejectCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerSpanMissingOwnerRejectCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerSpanMissingGenerationRejectCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerSpanRangeRejectCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerSpanAddressOverflowRejectCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerComputeSuccessCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerComputeFailureCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerValidSphereCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerInvalidSphereCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerPublishedExactCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerDomainCacheLookupCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerDomainCacheHitCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerDomainCacheMissCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerDomainCacheCollisionMissCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerDomainCacheStoreCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerDomainCacheEvictionCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerHintComparableCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerHintExactCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerHintSupersetCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerHintUnderCoverageCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerHintInvalidCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerHintRangeAcceptedCount = 0;
        uint32_t semanticSceneTerrainBoundsProducerHintRangeRejectedCount = 0;
        uint32_t semanticSceneTerrainBoundsCandidateCount = 0;
        uint32_t semanticSceneTerrainBoundsProofAcceptedCount = 0;
        uint32_t semanticSceneTerrainBoundsFailVisibleCount = 0;
        std::array<uint32_t,
            war3::render::kWar3ShadowBoundsCullRejectReasonCount>
            semanticSceneTerrainBoundsRejectReasonHistogram = {};
        uint32_t semanticSceneTerrainBoundsWouldCullCount = 0;
        uint32_t semanticSceneTerrainBoundsAppliedCullCount = 0;
        uint32_t semanticSceneTerrainBoundsC0WouldCullCount = 0;
        uint32_t semanticSceneTerrainBoundsC1WouldCullCount = 0;
        uint32_t semanticSceneTerrainBoundsC2WouldCullCount = 0;
        uint32_t semanticSceneTerrainBoundsC3WouldCullCount = 0;
        uint32_t semanticSceneObjectBoundsCandidateCount = 0;
        uint32_t semanticSceneObjectBoundsProofAcceptedCount = 0;
        uint32_t semanticSceneObjectBoundsFailVisibleCount = 0;
        std::array<uint32_t,
            war3::render::kWar3ShadowBoundsCullRejectReasonCount>
            semanticSceneObjectBoundsRejectReasonHistogram = {};
        uint32_t semanticSceneObjectBoundsWouldCullCount = 0;
        uint32_t semanticSceneObjectBoundsAppliedCullCount = 0;
        uint32_t semanticSceneShadowMapPreparedDrawCount = 0;         // 有效 prepared draw 数（排序/级联重放输入）
        uint32_t semanticSceneShadowMapAlphaTestPreparedCount = 0;    // prepare 后会执行 alpha discard 的 draw 数
        uint32_t semanticSceneShadowMapAlphaPromotedPreparedCount = 0; // alphaBlend+UV+diffuse promote 成 alpha shadow 的 draw 数
        uint32_t semanticSceneShadowMapDynamicPreparedCount = 0;      // Unit/Effect/skinned prepared draw 数
        uint32_t semanticSceneShadowMapStaticPreparedCount = 0;       // Building/Destructible/Terrain prepared draw 数
        uint32_t semanticSceneShadowMapOtherPreparedCount = 0;        // 其他分类 prepared draw 数
        uint32_t semanticSceneShadowMapTerrainDoodadPreparedCount = 0;
        uint32_t semanticSceneShadowMapTerrainS1PreparedCount = 0;
        uint32_t semanticSceneShadowMapCascade0DrawnCount = 0;
        uint32_t semanticSceneShadowMapCascade1DrawnCount = 0;
        uint32_t semanticSceneShadowMapCascade2DrawnCount = 0;
        uint32_t semanticSceneShadowMapCascade3DrawnCount = 0;
        uint32_t semanticSceneShadowMapCascade0CulledCount = 0;
        uint32_t semanticSceneShadowMapCascade1CulledCount = 0;
        uint32_t semanticSceneShadowMapCascade2CulledCount = 0;
        uint32_t semanticSceneShadowMapCascade3CulledCount = 0;
        // Joint-consumer culling admission remains observation-only. These
        // counters describe C2/C3 static-rigid opportunities and parity with
        // the canonical late CSM decision; they never authorize a draw skip.
        uint32_t semanticSceneUnionCullMode = 0;
        uint32_t semanticSceneUnionCullObserveFrameCount = 0;
        uint32_t semanticSceneUnionCullCandidateCount = 0;
        uint32_t semanticSceneUnionCullProofAcceptedCount = 0;
        uint32_t semanticSceneUnionCullFailVisibleCount = 0;
        uint32_t semanticSceneUnionCullDynamicConservativeCount = 0;
        uint32_t semanticSceneUnionCullUnknownOrStaleCount = 0;
        uint32_t semanticSceneUnionCullC2WouldCullCount = 0;
        uint32_t semanticSceneUnionCullC3WouldCullCount = 0;
        uint32_t semanticSceneUnionCullBothFarWouldCullCount = 0;
        uint32_t semanticSceneUnionCullFalseNegativeCount = 0;
        uint32_t semanticSceneUnionCullFalsePositiveCount = 0;
        uint32_t semanticSceneShadowMapTerrainDoodadCascade0DrawnCount = 0;
        uint32_t semanticSceneShadowMapTerrainDoodadCascade1DrawnCount = 0;
        uint32_t semanticSceneShadowMapTerrainDoodadCascade2DrawnCount = 0;
        uint32_t semanticSceneShadowMapTerrainDoodadCascade3DrawnCount = 0;
        uint32_t semanticSceneShadowMapTerrainS1Cascade0DrawnCount = 0;
        uint32_t semanticSceneShadowMapTerrainS1Cascade1DrawnCount = 0;
        uint32_t semanticSceneShadowMapTerrainS1Cascade2DrawnCount = 0;
        uint32_t semanticSceneShadowMapTerrainS1Cascade3DrawnCount = 0;
        uint32_t semanticSceneShadowMapSkinnedCasterCount = 0;        // replay 中的 skinned caster 数
        uint32_t semanticSceneShadowMapSkinnedPreparedCount = 0;      // 实际 prepared 的 skinned 数
        uint32_t semanticSceneShadowMapSkinnedInvalidBufferCount = 0; // skinned replay 因 buffer 无效被拒绝
        uint32_t semanticSceneShadowMapSkinnedInvalidPipelineCount = 0; // skinned replay 因 pipeline 无效被拒绝
        uint32_t semanticSceneShadowMapSkinnedDrawnCount = 0;         // 实际 drawn 的 skinned 数
        uint32_t semanticSceneShadowTaaActive = 0;                    // 本帧 shadow TAA/history 是否活跃
        uint32_t semanticSceneReceiverReuseShadowMap = 0;             // 本帧是否复用旧 shadow map
        uint32_t semanticSceneReceiverInputValid = 0;                 // 本帧 receiver 输入是否通过主世界自洽检查
        uint32_t semanticSceneReceiverInputRejectReason = 0;          // receiver 输入拒绝原因（0=none）
        uint32_t semanticSceneReceiverNeedPass = 0;                   // 本帧是否需要 receiver pass
        uint32_t semanticSceneReceiverNeedShadowMap = 0;              // 本帧 receiver 是否需要 directional shadow map
        uint32_t semanticSceneReceiverHasCompleteShadowMap = 0;       // receiver 是否持有完整 shadow map
        uint32_t semanticSceneReceiverHasUsableDirectionalShadow = 0;  // UBO 是否会启用 directional shadow
        uint32_t semanticSceneReceiverActiveStrengthMilli = 0;        // activeShadowStrength * 1000
        uint32_t semanticSceneReceiverUboStrengthMilli = 0;           // ubo shadow strength * 1000
        uint32_t semanticSceneReceiverDebugMode = 0;                  // War3ShadowDebugMode
        uint32_t semanticSceneReceiverCsmCascadeCount = 0;            // 当前 receiver 使用的 cascade 数
        uint32_t semanticSceneReceiverRunEntryFlags = 0;              // Phase 7.39: Run() publish 时刻条件 bitset
        uint32_t semanticSceneReceiverRunEarlyReturnReason = 0;       // Phase 7.39: 0 none, 1 input, 2 disabled, 3 no-work, 4 csm
        uint32_t semanticSceneShadowMapExecutedThisFrame = 0;         // Phase 7.39: 本帧 renderShadowMap 是否真实执行
        uint32_t semanticSceneReceiverSettingsShadowsEnabled = 0;     // Phase 7.39: settings->shadows.enabled snapshot
        uint32_t semanticSceneReceiverSettingsOutlineEnabled = 0;     // Phase 7.39: settings->occludedOutline.enabled snapshot
        uint32_t semanticSceneReceiverSettingsRawStrengthMilli = 0;   // Phase 7.39: 日夜覆盖前 settings strength
        uint32_t semanticSceneReceiverComputedShadowStrengthMilli = 0;// Phase 7.39: 日夜覆盖后的 shadow strength
        uint32_t semanticSceneReceiverHasSunShadow = 0;               // Phase 7.39: strength/debug 使 directional shadow 有工作
        uint32_t semanticSceneReceiverHasPointShadow = 0;             // Phase 7.39: point shadow gate
        uint32_t semanticSceneReceiverNeedOutlinePass = 0;            // Phase 7.39: occluded-outline gate
        // === Phase 7.10: receiver global flicker diagnostics ===
        uint32_t semanticSceneReceiverZeroStrengthFrameCount = 0;      // receiver 需要 pass 但 UBO strength 为 0 的帧
        uint32_t semanticSceneReceiverDrawnWithZeroStrengthCount = 0;  // receiver fullscreen pass 执行但 directional strength 为 0
        uint32_t semanticSceneReceiverNoCompleteShadowMapCount = 0;    // 需要 shadow map 但没有完整图
        uint32_t semanticSceneReceiverNoShadowMapImageCount = 0;       // m_shadowMap 缺失
        uint32_t semanticSceneReceiverNoShadowMapSampleViewCount = 0;  // m_shadowMapSampleView 缺失
        uint32_t semanticSceneReceiverNoCandidateCsmCount = 0;         // 本帧 CSM 计算无候选
        uint32_t semanticSceneReceiverCsmFallbackToLastGoodCount = 0;  // CSM 失败但复用 last-good CSM
        uint32_t semanticSceneReceiverHoldInvalidCsmCount = 0;         // 因 invalid CSM hold last-good
        uint32_t semanticSceneReceiverHoldEmptyReplayCount = 0;        // 因 empty replay hold last-good
        uint32_t semanticSceneReceiverHoldIdentityChurnCount = 0;      // 因 semantic identity churn hold last-good
        uint32_t semanticSceneReceiverReuseInvalidatedAfterEnsureCount = 0; // 资源 ensure 后取消 reuse
        uint32_t semanticSceneShadowMapRenderSkippedNoResourcesCount = 0;   // shadow map render 因资源缺失跳过
        uint32_t semanticSceneShadowMapRenderSkippedNoMatrixBufferCount = 0; // shadow map render 因矩阵 buffer 缺失跳过
        uint32_t semanticSceneReceiverViewportX = 0;
        uint32_t semanticSceneReceiverViewportY = 0;
        uint32_t semanticSceneReceiverViewportWidth = 0;
        uint32_t semanticSceneReceiverViewportHeight = 0;
        // === Phase 7.45: downstream shadow-map / receiver resource fingerprints ===
        uint64_t semanticSceneShadowMatrixSceneKey = 0;          // palette + replay + world matrix content key used for SSBO upload
        uint64_t semanticSceneShadowMatrixUploadSerial = 0;      // advances only when the shadow matrix SSBO is rewritten
        uint64_t semanticSceneShadowMatrixBufferObjectPtr = 0;   // DxvkBuffer object pointer id
        uint64_t semanticSceneShadowMatrixBufferOffset = 0;      // active SSBO slice offset
        uint64_t semanticSceneShadowMatrixBufferSize = 0;        // active SSBO slice size
        uint64_t semanticSceneShadowMatrixBufferGpuAddress = 0;  // active SSBO GPU address when available
        // Diagnostic-only continuity fingerprints. These remain zero unless
        // DXVK_WAR3_CSM_CONTINUITY_TRACE=1. They intentionally separate
        // camera/sun/CSM movement from Stage13 draw content and rotating GPU
        // backing so a bridge/ramp pop can be correlated to the exact domain
        // that changed on the captured frame.
        uint64_t semanticSceneReceiverCameraHash = 0;
        uint64_t semanticSceneReceiverSunDirectionHash = 0;
        uint64_t semanticSceneReceiverCsmHash = 0;
        uint64_t semanticSceneReceiverCameraDeltaNano = 0;
        uint64_t semanticSceneReceiverSunDeltaNano = 0;
        uint64_t semanticSceneReceiverCsmDeltaNano = 0;
        uint64_t semanticSceneReceiverSnappedCenterDeltaTexelsNano = 0;
        uint64_t semanticSceneReceiverTexelSizeDeltaNano = 0;
        uint64_t semanticSceneReplayBackingHash = 0;
        uint64_t semanticSceneStage13ReplayContentHash = 0;
        uint64_t semanticSceneStage13ReplayBackingHash = 0;
        uint32_t semanticSceneStage13ReplayDrawCount = 0;
        uint64_t semanticSceneShadowMapRenderSerial = 0;         // advances only after renderShadowMap succeeds
        uint64_t semanticSceneShadowMapImagePtr = 0;             // DxvkImage object pointer id
        uint64_t semanticSceneShadowMapSampleViewPtr = 0;        // DxvkImageView object pointer id
        uint64_t semanticSceneShadowCurrentImagePtr = 0;         // ShadowTAA current visibility image pointer id
        uint64_t semanticSceneShadowCurrentViewPtr = 0;          // ShadowTAA current visibility view pointer id
        uint64_t semanticSceneShadowHistoryReadImagePtr = 0;     // history image sampled by this frame
        uint64_t semanticSceneShadowHistoryReadViewPtr = 0;      // history view sampled by this frame
        uint64_t semanticSceneShadowHistoryWriteImagePtr = 0;    // history image written by this frame
        uint64_t semanticSceneShadowHistoryWriteViewPtr = 0;     // history storage view written by this frame
        uint32_t semanticSceneShadowVisibilityExecutedThisFrame = 0; // ShadowTAA visibility prepass actually ran
        uint32_t semanticSceneReceiverDrawExecutedThisFrame = 0;     // fullscreen receiver actually drew
        uint32_t semanticSceneShadowTaaMode = 0;                 // 0 DirectInline, 1 PrepassCurrentOnly, 2 TemporalCurrentOnly, 3 TemporalHistory
        uint32_t semanticSceneShadowHistoryValidBefore = 0;      // history validity before receiver draw
        uint32_t semanticSceneShadowHistoryValidAfter = 0;       // history validity after receiver draw
        uint32_t semanticSceneShadowHistoryReadIndex = 0;        // ping-pong read index used by this frame
        uint32_t semanticSceneShadowHistoryWriteIndex = 0;       // ping-pong write index used by this frame
        uint32_t semanticSceneShadowHistoryAdvancedThisFrame = 0; // receiver really wrote and committed history
        uint32_t semanticSceneShadowHistoryAdvanceSkippedIncomplete = 0; // TAA requested but receiver/resource write was incomplete
        uint32_t semanticSceneShadowHistoryInvalidationMask = 0;
        uint32_t semanticSceneShadowReceiverSampleSource = 0;    // 0 none, 1 direct shadow map, 2 current visibility, 3 history TAA
        uint32_t semanticSceneLastInputDrawCount = 0;
        uint32_t semanticSceneLastInputSkinnedCount = 0;
        uint32_t semanticSceneLastSubmittedDrawCount = 0;
        uint32_t semanticSceneLastUnitsOnlyFilteredCount = 0;
        uint32_t semanticSceneCatchupAttemptCount = 0;
        uint32_t semanticSceneCatchupSuccessCount = 0;
        uint32_t semanticSceneSkippedEmptyFrameCount = 0;
        uint32_t semanticSceneZeroSubmitCount = 0;
        uint32_t semanticSceneSelectedFrameEligibleZeroCount = 0;
        uint32_t semanticSceneReusableFrameForcedCount = 0;
        uint32_t semanticSceneReusableFrameUnavailableCount = 0;
        uint32_t semanticSceneReusableFrameRejectedNativeValidationCount = 0;
        uint64_t semanticSceneLastFrameSerial = 0;
        uint64_t semanticSceneLastSelectedFrameSerial = 0;
        uint64_t semanticSceneLastReusableFrameSerial = 0;
        uint64_t semanticSceneLastSourcePublishRevision = 0;
        uint64_t semanticSceneLastTargetPublishRevision = 0;
        uint32_t semanticSceneSkippedUnitsOnlyFilter = 0;
        uint32_t semanticSceneAcceptedExplicitResourceOwnerRigid = 0;
        uint32_t semanticSceneRejectedNoVertex = 0;
        uint32_t semanticSceneRejectedSkinnedContract = 0;
        uint32_t semanticSceneRejectedGeometry = 0;
        uint32_t semanticSceneRejectedGeometryFrameLocal = 0;
        uint32_t semanticSceneRejectedGeometryPersistent = 0;
        uint32_t semanticSceneHostRejectNonUnitKind = 0;
        uint32_t semanticSceneHostRejectTransparent = 0;
        uint32_t semanticSceneHostRejectGroup = 0;
        uint32_t semanticSceneHostRejectBuilding = 0;
        uint32_t semanticSceneHostRejectNoStableResource = 0;
        uint32_t semanticSceneHostRejectNoIdentity = 0;
        uint32_t semanticSceneHostRejectNoUnitPtr = 0;
        uint32_t semanticSceneHostRejectNotSkinned = 0;
        uint32_t semanticSceneHostRejectStaticWorldRawcode = 0;
        uint32_t semanticSceneBypassUnitLikeCount = 0;
        uint32_t semanticSceneBypassUnitLikeWithRuntimeModel = 0;
        uint32_t semanticSceneBypassUnitLikeWithModelResource = 0;
        uint32_t semanticSceneBypassUnitLikeWithPose = 0;
        uint32_t semanticSceneBypassUnitLikeWithRenderable = 0;
        uint32_t semanticSceneBypassPublishedVisibleCandidate = 0;
        uint32_t semanticSceneBypassPublishMiss = 0;
        uint32_t semanticFallbackPruned = 0;
        uint32_t semanticFallbackPrunedByHandle = 0;
        uint32_t semanticFallbackPrunedByWorldObjectEntry = 0;
        uint32_t semanticFallbackPrunedBySceneNode = 0;
        uint32_t semanticFallbackPrunedByRuntimeModel = 0;
        uint32_t duplicateGeometryInstances = 0;
        uint32_t uniqueGeometryCount = 0;
        uint32_t reuseEligibleDuplicates = 0;
        uint32_t potentialFreezeReuseHits = 0;
        uint32_t instancedGeometryDrawsSaved = 0;
        uint32_t skippedFreezeBudget = 0;
        uint32_t skippedPriorityBudget = 0;
        uint32_t skippedCasterCap = 0;
        uint32_t degradedAlphaBudget = 0;
        uint32_t budgetExceeded = 0;
        uint64_t persistentPoolBytesUsed = 0;
        uint64_t persistentPoolBytesEvicted = 0;
        uint64_t fallbackBudgetBytes = 0;
        uint64_t fallbackBudgetUsedBytes = 0;
        uint64_t fallbackArenaBytes = 0;
        uint64_t dynamicPoseSignature = 0;
    };

    enum War3CompactWorkFlags : uint8_t {
        War3CompactWorkValid = 1u << 0,
        War3CompactWorkSealed = 1u << 1,
        War3CompactWorkExactOwner = 1u << 2,
        War3CompactWorkStaticWorld = 1u << 3,
        War3CompactWorkProducerAllowed = 1u << 4,
        War3CompactWorkPathBlocker = 1u << 5,
        War3CompactWorkPreviouslySelected = 1u << 6,
    };

    // Immutable POD row used while the canonical path and the compact path are
    // compared. Persistent storage below is SoA so consumers read only the
    // columns needed by their gate instead of chasing the full draw contract.
    struct War3CompactWorkItem {
        uint64_t frameGeneration = 0u;
        uint64_t selectionKey = 0u;
        uint32_t priorityScore = 0u;
        uint8_t flags = 0u;
    };

    static_assert(std::is_standard_layout_v<War3CompactWorkItem>);
    static_assert(std::is_trivially_copyable_v<War3CompactWorkItem>);

    inline bool War3CompactWorkHasFlag(
        const War3CompactWorkItem& item, War3CompactWorkFlags flag) {
      return (item.flags & uint8_t(flag)) != 0u;
    }

    struct War3CompactWorkTable {
        uint64_t generation = 0u;
        std::vector<uint64_t> frameGenerations;
        std::vector<uint64_t> selectionKeys;
        std::vector<uint32_t> priorityScores;
        std::vector<uint8_t> flags;

        void reset(uint64_t nextGeneration, size_t reserveCount) {
          generation = nextGeneration;
          frameGenerations.clear();
          selectionKeys.clear();
          priorityScores.clear();
          flags.clear();
          if (frameGenerations.capacity() < reserveCount)
            frameGenerations.reserve(reserveCount);
          if (selectionKeys.capacity() < reserveCount)
            selectionKeys.reserve(reserveCount);
          if (priorityScores.capacity() < reserveCount)
            priorityScores.reserve(reserveCount);
          if (flags.capacity() < reserveCount)
            flags.reserve(reserveCount);
        }

        void append(const War3CompactWorkItem& item) {
          frameGenerations.push_back(item.frameGeneration);
          selectionKeys.push_back(item.selectionKey);
          priorityScores.push_back(item.priorityScore);
          flags.push_back(item.flags);
        }

        void appendInvalid(size_t count) {
          for (size_t i = 0u; i < count; ++i)
            append({generation, 0u, 0u, 0u});
        }

        bool load(size_t index, War3CompactWorkItem& item) const {
          if (index >= frameGenerations.size() ||
              index >= selectionKeys.size() ||
              index >= priorityScores.size() || index >= flags.size())
            return false;
          item = {frameGenerations[index], selectionKeys[index],
                  priorityScores[index], flags[index]};
          return true;
        }
    };

    struct War3FrameScene {
        War3WorldCameraState worldCamera;
        War3ShadowCaptureStats shadowStats;
        War3ShadowProducerCompleteness producerCompleteness;
        War3ShadowPersistentGeometryPool shadowPersistentPool;
        std::vector<War3ShadowMatrixPalette> shadowPalettes;
        std::vector<War3ShadowInstanceRef> shadowInstances;
        std::vector<War3ShadowFallbackDraw> shadowFallbacks;
        std::vector<War3ShadowCasterDraw> shadowCasters;
    };

} // namespace dxvk
