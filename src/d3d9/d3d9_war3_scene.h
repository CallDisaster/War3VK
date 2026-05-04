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
        uint32_t semanticSceneLivePaletteRefreshAttemptCount = 0;
        uint32_t semanticSceneLivePaletteRefreshHitCount = 0;
        uint32_t semanticSceneLivePaletteRefreshMissCount = 0;
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
        uint32_t semanticSceneSubmittedPaletteMotionSampleCount = 0;
        uint32_t semanticSceneSubmittedPaletteMotionNewRuntimeCount = 0;
        uint32_t semanticSceneSubmittedPaletteMotionChangedCount = 0;
        uint32_t semanticSceneSubmittedPaletteMotionStableCount = 0;
        uint64_t semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr = 0;
        uint64_t semanticSceneSubmittedPaletteMotionLastPrevHash = 0;
        uint64_t semanticSceneSubmittedPaletteMotionLastHash = 0;
        uint32_t semanticSceneSkinnedDynamicIndexSliceCount = 0;
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
