#pragma once

#include "war3/render/war3_render_state.h"

#include "../dxvk/dxvk_buffer.h"
#include "../dxvk/dxvk_image.h"   // 用于 Rc<DxvkImageView> (Alpha测试阴影)
#include "../dxvk/dxvk_sampler.h" // 用于 Rc<DxvkSampler>

#include "../util/util_matrix.h"

#include <array>
#include <vector>
#include <cstdint>

namespace dxvk {

    struct War3WorldCameraState {
        bool valid = false;
        Matrix4 view;
        Matrix4 proj;
        Matrix4 viewProj;
        Matrix4 invViewProj;
        // 用于深度重建与 UI 区域裁剪的主世界 viewport/scissor
        D3DVIEWPORT9 viewport = { };
        RECT scissor = { };
        uint32_t frameIndex = 0;
    };

    struct War3ShadowMatrixPalette {
        uint64_t hash = 0;
        std::array<Matrix4, 256> worldMatrices = { };
    };

    struct War3ShadowCasterDraw {
        bool indexed = true;

        // Vertex input (position only)
        // 注意：DXVK 的 D3D9 buffer 可能在同一帧内被 invalidate（更换 VkBuffer backing）。
        // 若仅存 DxvkBufferSlice，在 shadow caster 重放时 getSliceInfo() 可能拿到“新的 VkBuffer”，
        // 进而读取到错误/被覆盖的数据（表现为阴影残缺/错位/乱飞）。
        // 因此需要在 capture 时固化 VkBuffer/offset，并持有对应 allocation 保活。
        Rc<DxvkBuffer> positionStorage;
        DxvkResourceBufferInfo positionInfo;
        uint32_t positionStride = 0;
        uint32_t positionOffset = 0;
        VkFormat positionFormat = VK_FORMAT_UNDEFINED;

        // Index input
        Rc<DxvkBuffer> indexStorage;
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
        DxvkResourceBufferInfo uvInfo;      // UV缓冲区信息
        uint32_t uvStride = 0;              // UV步长
        uint32_t uvOffset = 0;              // UV偏移
        VkFormat uvFormat = VK_FORMAT_UNDEFINED;  // UV格式

        // 漫反射纹理 (Stage 0 纹理，用于读取Alpha通道)
        Rc<DxvkImageView> diffuseTexture;   // 纹理视图 (Ref Count)
        Rc<DxvkSampler>   diffuseSampler;   // 采样器 (Ref Count)
        DxvkDescriptor    textureDescriptor; // 完整的描述符 (View + Sampler)，用于绑定
        uint32_t          diffuseSamplerIndex = 0; // [NEW] Bindless Sampler Index

        // 分类信息（用于调试/过滤）
        War3RenderState::StageCategory category = War3RenderState::StageCategory::Unknown;
        War3BatchTag batchTag = War3BatchTag::Unknown;
        uint32_t batchHandle = 0; // jHandle（来自 ExecBatch TLS），用于描边筛选
        uint8_t objectKind = 0;   // dxvk::war3::render::ObjectKind

        // ===== 级联阴影剔除（粗略包围球）=====
        // 说明：用于 CPU 端对每个 CSM 级联做保守剔除，减少“每级联全量重放”的 drawcall 膨胀。
        // 这是保守优化：半径偏大只会减少剔除命中率，不会造成阴影缺失。
        Vector4 boundsCenter = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        float boundsRadius = 0.0f; // 0 表示未提供 bounds（不做剔除）
    };

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

    struct War3ShadowCaptureStats {
        uint32_t considered = 0;
        uint32_t captured = 0;
        uint32_t capturedIndexed = 0;
        uint32_t capturedNonIndexed = 0;
        uint32_t capturedTerrain = 0;
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
        // Phase 7.55 v4：诊断 capture path 早退原因
        uint32_t drawTimeVBCacheRejectNoRenderablePart = 0;
        uint32_t drawTimeVBCacheRejectNoDecl = 0;
        uint32_t drawTimeVBCacheRejectNoPosition = 0;
        uint32_t drawTimeVBCacheRejectInvalidStride = 0;
        uint32_t drawTimeVBCacheRejectNoSlice = 0;
        uint32_t drawTimeVBCacheRejectInvalidRange = 0;
        uint32_t drawTimeVBCacheRejectInsufficientLength = 0;
        uint32_t drawTimeVBCacheRejectNoBuffer = 0;
        uint32_t drawTimeVBCacheTotalEntered = 0;
        // Phase 7.69：draw-time GPU copy 成本账本。semantic 路线为了 Pose 正确
        // 每帧冻结 War3 当帧 VB/IB slice；这里记录 copy 命令数、字节数和 buffer
        // reallocation，以便和 legacy VB/IB intercept 基线对账。
        uint32_t drawTimeVBCachePositionCopyCount = 0;
        uint64_t drawTimeVBCachePositionCopyBytes = 0;
        uint32_t drawTimeVBCachePositionAllocCount = 0;
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
        // Phase 7.70：同帧重复捕获去重账本。
        // SameFrameDedupHit 是命中“数据指纹未变”的次数（跳过 GPU copy）。
        // SameFrameDedupMiss 是数据真的变了，必须重做 GPU copy 的次数。
        // SameFrameStateRefresh 是去重命中后仍然刷新了 alpha/world/texture 状态。
        uint32_t drawTimeVBCacheSameFrameDedupHit = 0;
        uint32_t drawTimeVBCacheSameFrameDedupMiss = 0;
        uint32_t drawTimeVBCacheSameFrameStateRefresh = 0;
        uint32_t drawTimeSemanticProducerVisibleCandidateCount = 0;
        uint32_t drawTimeSemanticProducerFreshEntryCount = 0;
        uint32_t drawTimeSemanticProducerSubmittedCount = 0;
        uint32_t drawTimeSemanticProducerMissNoFreshEntryCount = 0;
        uint32_t drawTimeSemanticProducerFallbackCurrentDrawCount = 0;
        uint32_t semanticSceneRejectedPathBlockerCount = 0;
        uint32_t semanticSceneDirectDrawTimePrebuildBypassAttemptCount = 0;
        uint32_t semanticSceneDirectDrawTimePrebuildBypassHitCount = 0;
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
        uint32_t semanticSceneShadowMapPreparedDrawCount = 0;         // 有效 prepared draw 数（排序/级联重放输入）
        uint32_t semanticSceneShadowMapAlphaTestPreparedCount = 0;    // prepare 后会执行 alpha discard 的 draw 数
        uint32_t semanticSceneShadowMapAlphaPromotedPreparedCount = 0; // alphaBlend+UV+diffuse promote 成 alpha shadow 的 draw 数
        uint32_t semanticSceneShadowMapDynamicPreparedCount = 0;      // Unit/Effect/skinned prepared draw 数
        uint32_t semanticSceneShadowMapStaticPreparedCount = 0;       // Building/Destructible/Terrain prepared draw 数
        uint32_t semanticSceneShadowMapOtherPreparedCount = 0;        // 其他分类 prepared draw 数
        uint32_t semanticSceneShadowMapCascade0DrawnCount = 0;
        uint32_t semanticSceneShadowMapCascade1DrawnCount = 0;
        uint32_t semanticSceneShadowMapCascade2DrawnCount = 0;
        uint32_t semanticSceneShadowMapCascade3DrawnCount = 0;
        uint32_t semanticSceneShadowMapCascade0CulledCount = 0;
        uint32_t semanticSceneShadowMapCascade1CulledCount = 0;
        uint32_t semanticSceneShadowMapCascade2CulledCount = 0;
        uint32_t semanticSceneShadowMapCascade3CulledCount = 0;
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
        uint32_t semanticSceneShadowTaaMode = 0;                 // 0 off, 1 current-only, 2 current+history
        uint32_t semanticSceneShadowHistoryValidBefore = 0;      // history validity before receiver draw
        uint32_t semanticSceneShadowHistoryValidAfter = 0;       // history validity after receiver draw
        uint32_t semanticSceneShadowHistoryReadIndex = 0;        // ping-pong read index used by this frame
        uint32_t semanticSceneShadowHistoryWriteIndex = 0;       // ping-pong write index used by this frame
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

    struct War3FrameScene {
        War3WorldCameraState worldCamera;
        War3ShadowCaptureStats shadowStats;
        War3ShadowPersistentGeometryPool shadowPersistentPool;
        std::vector<War3ShadowMatrixPalette> shadowPalettes;
        std::vector<War3ShadowInstanceRef> shadowInstances;
        std::vector<War3ShadowFallbackDraw> shadowFallbacks;
        std::vector<War3ShadowCasterDraw> shadowCasters;
    };

} // namespace dxvk
