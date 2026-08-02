#pragma once

#include "d3d9_war3_pipeline.h"
#include "d3d9_war3_csm.h"
#include "war3/render/war3_point_shadow_cpu_plan.h"

#include "../dxvk/dxvk_hash.h"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <functional> // [Fix] for std::function
#include <algorithm>  // [Fix] for std::max/min
#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <vector>
#include "d3d9_util.h"

namespace dxvk {

    namespace war3::render {
      class War3HybridRayTracing;
    }

    class D3D9DeviceEx;
    struct War3PointLightFrameSnapshot;

    /**
     * @brief Immutable, fail-closed point-cube publication for volumetric use.
     *
     * The volume pass receives this by value after ShadowReceiver::Run. Stable
     * light ids decouple its consumer-local top-K order from cube-array layers.
     */
    struct War3VolumetricPointShadowSnapshot {
      static constexpr uint32_t kMaxLights = 4u;

      struct Light {
        int32_t lightId = 0;
        uint32_t cubeLayer = 0u;
        uint32_t faceValidMask = 0u;
        Vector4 lightPosRange = Vector4(0.0f);
        float shadowIntensity = 0.0f;
        float bias = 0.0f;
      };

      Rc<DxvkImageView> cubeView;
      Rc<DxvkSampler> sampler;
      std::array<Light, kMaxLights> lights = {};
      uint32_t lightCount = 0u;
      uint32_t resolution = 0u;
      uint64_t lightGeneration = 0u;
      uint64_t frameSerial = 0u;
      // Frame whose command recording actually committed the sampled cube.
      // This may be older than frameSerial only for explicit temporal reuse.
      uint64_t publishedFrameSerial = 0u;
      uint64_t contentSignature = 0u;
      // x=pcfNear y=pcfFar z=texelBiasScale w=rangeFadeStart
      Vector4 filterParams = Vector4(0.0f);

      bool valid() const {
        return cubeView && sampler && lightCount > 0u && resolution > 0u &&
               lightGeneration != 0u && frameSerial != 0u &&
               publishedFrameSerial != 0u;
      }
    };

    /**
     * @brief 体积光专用太阳 ortho 阴影快照（与相机 CSM 解耦）。
     */
    struct War3VolumetricSunShadowSnapshot {
      Rc<DxvkImageView> depthView;
      // [0]=近级（可选），[1]=远级；cascadeCount=1 时仅 [0] 有效。
      Matrix4 lightViewProj[2] = {};
      Vector4 lightDirection = Vector4(0.0f, 0.0f, -1.0f, 0.0f);
      Vector4 worldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
      uint32_t resolution = 0u;
      uint32_t cascadeCount = 1u;
      uint64_t frameSerial = 0u;
      float softRadius = 1.35f;
      float receiverBias = 0.006f;
      float radiusNear = 0.0f;
      float radiusFar = 0.0f;

      bool valid() const {
        return depthView && resolution > 0u && frameSerial != 0u &&
               cascadeCount >= 1u && cascadeCount <= 2u;
      }
    };

    /**
     * @brief War3 Shadow Receiver Pass（DXVK 版本）
     *
     * 当前阶段：
     * - BeforeUi 插入点执行：CSM ShadowMap（caster）+ 深度重建 receiver（PCF 软边 + 级联混合）
     * - 通过 `War3RenderSettings` 可调整：级联数、分辨率、PCF 半径、强度、调试输出等
     *
     * 后续规划：
     * - PCSS（接触硬化）/更多质量档位
     * - alpha-test caster、更多 POSITION 格式支持
     * - 与后处理链（曝光/色调映射/LUT）整合
     */
    /**
     * @brief 点阴影 CPU plan 的小 POD 输入（2026-07-21 优化）。
     *
     * Worker_Prepare 原来每帧两次深拷贝整个 War3PipelineInput（caster 大结构
     * vector + 每条约 16 KB 的 shadowPalettes 矩阵），而 preparePointShadowCpuPlan
     * 实际只读 settings、3 个 shadowStats 字段与 palette 的 hash。本结构按值
     * 持有这些字段，worker 捕获成本从 MB 级降到几百字节。
     */
    struct War3PointShadowCpuPlanInput {
        War3RenderSettings settings{};
        uint64_t frameSerial = 0;
        uint64_t dynamicPoseSignature = 0;
        uint32_t dynamicPoseCount = 0;
        uint32_t dynamicSkinnedOutputCount = 0;
        std::vector<uint64_t> paletteHashes;
        /** 仅在 replayDraws == nullptr 的兜底路径用于 BuildShadowReplayDraws；
         *  worker 路径总是带 draws，不需要此指针；同步路径指向真实 scene，
         *  其生命周期覆盖本次同步调用。 */
        const War3FrameScene* sceneForReplayFallback = nullptr;
    };

    struct ShadowTaaDiagnostics {
      uint32_t requestedMode = 0u;
      uint32_t effectiveMode = 0u;
      uint32_t shaderMode = 0u;
      uint32_t historyValid = 0u;
      uint32_t historyReadable = 0u;
      uint64_t historyGeneration = 0u;
      uint32_t lastInvalidationReason = 0u;
      // Temporal receiver frames for which the fixed-wall current-only path
      // was eligible. Pixel atomics are intentionally avoided.
      uint64_t fixedWallBypassCount = 0u;
      uint64_t settingsRevision = 0u;
    };

    enum class PointShadowPersistentBeginRejectReason : uint32_t {
      None = 0u,
      ModeOff = 1u,
      WorkerPrepareDisabled = 2u,
      NoPointShadowWork = 3u,
      MissingSettings = 4u,
      PointShadowDisabled = 5u,
      NoPointLights = 6u,
      NoReplayDraws = 7u,
      InvalidLightSnapshot = 8u,
      InvalidFrameSerial = 9u,
      InvalidRendererEpoch = 10u,
      WorkerCreateFailed = 11u,
      WorkerUnavailable = 12u,
      PreviousJobNotReady = 13u,
      JobSerialExhausted = 14u,
      ReplayDrawCountOverflow = 15u,
      NullCaster = 16u,
      SubmitInvalidGeneration = 17u,
      SubmitStaleGeneration = 18u,
      SubmitBusy = 19u,
      SubmitStopping = 20u,
      SubmitUnavailable = 21u,
      AllocationFailure = 22u,
      UnexpectedException = 23u,
      NoShadowCastingLights = 24u,
    };

    /**
     * @brief POD-only live state for the opt-in point-shadow prepare worker.
     *
     * All counters are cumulative for one receiver instance. The render path
     * stores only numeric state; JSON/UI code may translate enum values after
     * leaving the hot path.
     */
    struct PointShadowPersistentDiagnostics {
      uint32_t configuredMode = 0u;
      uint32_t effectiveMode = 0u;
      uint32_t lastBeginRejectReason = static_cast<uint32_t>(
          PointShadowPersistentBeginRejectReason::ModeOff);
      uint32_t workerCreated = 0u;
      uint32_t workerAvailable = 0u;
      uint64_t lastFrameSerial = 0u;
      uint64_t beginAttempts = 0u;
      uint64_t beginEligible = 0u;
      uint64_t workerCreateCount = 0u;
      uint64_t workerThreadStarts = 0u;
      uint64_t accepted = 0u;
      uint64_t ready = 0u;
      uint64_t deadlineFallback = 0u;
      uint64_t rejectedFallback = 0u;
      uint64_t observeMatch = 0u;
      uint64_t mismatch = 0u;
      uint64_t consumed = 0u;
      uint64_t failed = 0u;
      uint64_t busy = 0u;
    };

    enum class CsmResolutionFallbackReason : uint32_t {
      None = 0u,
      MemoryBudget = 1u,
      AllocationFailure = 2u,
    };

    struct CsmResolutionDiagnostics {
      uint32_t requestedResolution = 4096u;
      uint32_t effectiveResolution = 0u;
      uint32_t fallbackReason = 0u;
      uint32_t fallbackLatched = 0u;
      uint32_t memoryBudgetSupported = 0u;
      uint64_t memoryBudgetBytes = 0u;
      uint64_t memoryAvailableBytes = 0u;
      uint64_t resourceGeneration = 0u;
      uint64_t resourceRebuildCount = 0u;
    };

    ShadowTaaDiagnostics QueryShadowTaaDiagnostics();
    CsmResolutionDiagnostics QueryCsmResolutionDiagnostics();
    PointShadowPersistentDiagnostics
    QueryPointShadowPersistentDiagnostics();
    void PublishShadowTaaDiagnostics(const ShadowTaaDiagnostics& diagnostics);
    void PublishCsmResolutionDiagnostics(
        const CsmResolutionDiagnostics& diagnostics);
    void PublishPointShadowPersistentDiagnostics(
        const PointShadowPersistentDiagnostics& diagnostics);

    class War3ShadowReceiverPass final : public War3RenderPass {
    public:
        explicit War3ShadowReceiverPass(D3D9DeviceEx* device);
        ~War3ShadowReceiverPass();

        War3InsertionPoint Point() const override { return War3InsertionPoint::BeforeUi; }
        void Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) override;
        
        Rc<DxvkSampler> getFallbackSampler(bool useMip, float mipLodBias);
        bool GetVolumetricShadowSnapshot(uint64_t expectedFrameSerial,
                                         Rc<DxvkImageView>& outShadowMapView,
                                         War3CsmData& outCsmData,
                                         uint32_t& outShadowResolution,
                                         Vector4& outSunDir,
                                         Vector4& outWorldUp) const;
        /**
         * @brief 取本帧已结算的体积太阳 ortho 阴影。
         * @param expectedFrameSerial 体积 pass 的当前 frameSerial
         * @param outSnapshot 输出快照
         * @return 快照与 serial 精确匹配且资源有效
         */
        bool GetVolumetricSunShadowSnapshot(
            uint64_t expectedFrameSerial,
            War3VolumetricSunShadowSnapshot& outSnapshot) const;
        bool GetVolumetricPointShadowSnapshot(
            uint64_t expectedLightGeneration,
            uint64_t expectedFrameSerial,
            War3VolumetricPointShadowSnapshot& outSnapshot) const;
        uint32_t GetShadowSamplerIndex() const;

        // Phase 7.2: submitted / replay / executed 对账
        // Run() 每帧会刷新这些字段，调用方在 Run() 返回后可读取并写入 War3ShadowCaptureStats
        struct ShadowReconciliationCounters {
          uint32_t shadowCastersCount = 0;
          uint32_t replayDrawsCount = 0;
          uint32_t shadowMapDrawnCasters = 0;
          uint32_t cascadeCulledCount = 0;
          uint32_t shadowMapPreparedDrawCount = 0;
          uint32_t shadowMapAlphaTestPreparedCount = 0;
          uint32_t shadowMapAlphaPromotedPreparedCount = 0;
          uint32_t shadowMapDynamicPreparedCount = 0;
          uint32_t shadowMapStaticPreparedCount = 0;
          uint32_t shadowMapOtherPreparedCount = 0;
          uint32_t shadowMapTerrainDoodadPreparedCount = 0;
          uint32_t shadowMapTerrainS1PreparedCount = 0;
          uint32_t shadowMapCascade0DrawnCount = 0;
          uint32_t shadowMapCascade1DrawnCount = 0;
          uint32_t shadowMapCascade2DrawnCount = 0;
          uint32_t shadowMapCascade3DrawnCount = 0;
          uint32_t shadowMapCascade0CulledCount = 0;
          uint32_t shadowMapCascade1CulledCount = 0;
          uint32_t shadowMapCascade2CulledCount = 0;
          uint32_t shadowMapCascade3CulledCount = 0;
          uint32_t unionCullMode = 0;
          uint32_t unionCullObserveFrameCount = 0;
          uint32_t unionCullCandidateCount = 0;
          uint32_t unionCullProofAcceptedCount = 0;
          uint32_t unionCullFailVisibleCount = 0;
          uint32_t unionCullDynamicConservativeCount = 0;
          uint32_t unionCullUnknownOrStaleCount = 0;
          uint32_t unionCullC2WouldCullCount = 0;
          uint32_t unionCullC3WouldCullCount = 0;
          uint32_t unionCullBothFarWouldCullCount = 0;
          uint32_t unionCullFalseNegativeCount = 0;
          uint32_t unionCullFalsePositiveCount = 0;
          uint32_t shadowMapTerrainDoodadCascade0DrawnCount = 0;
          uint32_t shadowMapTerrainDoodadCascade1DrawnCount = 0;
          uint32_t shadowMapTerrainDoodadCascade2DrawnCount = 0;
          uint32_t shadowMapTerrainDoodadCascade3DrawnCount = 0;
          uint32_t shadowMapTerrainS1Cascade0DrawnCount = 0;
          uint32_t shadowMapTerrainS1Cascade1DrawnCount = 0;
          uint32_t shadowMapTerrainS1Cascade2DrawnCount = 0;
          uint32_t shadowMapTerrainS1Cascade3DrawnCount = 0;
          uint32_t skinnedCasterCount = 0;
          uint32_t skinnedPreparedCount = 0;
          uint32_t skinnedInvalidBufferCount = 0;
          uint32_t skinnedInvalidPipelineCount = 0;
          uint32_t skinnedDrawnCount = 0;
          uint32_t shadowTaaActive = 0;
          uint32_t shadowTaaRuntimeModuleEnabled = 0;
          uint32_t shadowTaaRequestedMode = 0;
          uint32_t shadowTaaEffectiveMode = 0;
          uint32_t shadowTaaBlockedSemanticDynamic = 0;
          uint32_t shadowTaaBlockedSunMotion = 0;
          uint32_t shadowTaaBlockedCsmFallback = 0;
          uint32_t receiverReuseShadowMap = 0;
          uint32_t receiverInputValid = 0;
          uint32_t receiverInputRejectReason = 0;
          uint32_t receiverNeedPass = 0;
          uint32_t receiverNeedShadowMap = 0;
          uint32_t receiverHasCompleteShadowMap = 0;
          uint32_t receiverHasUsableDirectionalShadow = 0;
          uint32_t receiverActiveStrengthMilli = 0;
          uint32_t receiverUboStrengthMilli = 0;
          uint32_t receiverDebugMode = 0;
          uint32_t receiverCsmCascadeCount = 0;
          uint32_t receiverRunEntryFlags = 0;
          uint32_t receiverRunEarlyReturnReason = 0;
          uint32_t shadowMapExecutedThisFrame = 0;
          uint32_t receiverSettingsShadowsEnabled = 0;
          uint32_t receiverSettingsOutlineEnabled = 0;
          uint32_t receiverSettingsRawStrengthMilli = 0;
          uint32_t receiverComputedShadowStrengthMilli = 0;
          uint32_t receiverHasSunShadow = 0;
          uint32_t receiverHasPointShadow = 0;
          uint32_t receiverNeedOutlinePass = 0;
          uint32_t receiverZeroStrengthFrameCount = 0;
          uint32_t receiverDrawnWithZeroStrengthCount = 0;
          uint32_t receiverNoCompleteShadowMapCount = 0;
          uint32_t receiverNoShadowMapImageCount = 0;
          uint32_t receiverNoShadowMapSampleViewCount = 0;
          uint32_t receiverNoCandidateCsmCount = 0;
          uint32_t receiverCsmFallbackToLastGoodCount = 0;
          uint32_t receiverHoldInvalidCsmCount = 0;
          uint32_t receiverHoldEmptyReplayCount = 0;
          uint32_t receiverHoldIdentityChurnCount = 0;
          uint32_t receiverReuseInvalidatedAfterEnsureCount = 0;
          uint32_t shadowMapRenderSkippedNoResourcesCount = 0;
          uint32_t shadowMapRenderSkippedNoMatrixBufferCount = 0;
          uint32_t receiverViewportX = 0;
          uint32_t receiverViewportY = 0;
          uint32_t receiverViewportWidth = 0;
          uint32_t receiverViewportHeight = 0;
          uint64_t shadowMatrixSceneKey = 0;
          uint64_t shadowMatrixUploadSerial = 0;
          uint64_t shadowMatrixBufferObjectPtr = 0;
          uint64_t shadowMatrixBufferOffset = 0;
          uint64_t shadowMatrixBufferSize = 0;
          uint64_t shadowMatrixBufferGpuAddress = 0;
          uint64_t receiverCameraHash = 0;
          uint64_t receiverSunDirectionHash = 0;
          uint64_t receiverCsmHash = 0;
          uint64_t receiverCameraDeltaNano = 0;
          uint64_t receiverSunDeltaNano = 0;
          uint64_t receiverCsmDeltaNano = 0;
          uint64_t receiverSnappedCenterDeltaTexelsNano = 0;
          uint64_t receiverTexelSizeDeltaNano = 0;
          uint64_t replayBackingHash = 0;
          uint64_t stage13ReplayContentHash = 0;
          uint64_t stage13ReplayBackingHash = 0;
          uint32_t stage13ReplayDrawCount = 0;
          uint64_t shadowMapRenderSerial = 0;
          uint64_t shadowMapImagePtr = 0;
          uint64_t shadowMapSampleViewPtr = 0;
          uint64_t shadowCurrentImagePtr = 0;
          uint64_t shadowCurrentViewPtr = 0;
          uint64_t shadowHistoryReadImagePtr = 0;
          uint64_t shadowHistoryReadViewPtr = 0;
          uint64_t shadowHistoryWriteImagePtr = 0;
          uint64_t shadowHistoryWriteViewPtr = 0;
          uint32_t shadowVisibilityExecutedThisFrame = 0;
          uint32_t shadowMotionVectorExecutedThisFrame = 0;
          uint32_t receiverDrawExecutedThisFrame = 0;
          uint32_t shadowHistoryWriteExecutedThisFrame = 0;
          uint32_t shadowTaaMode = 0;
          uint32_t shadowHistoryValidBefore = 0;
          uint32_t shadowHistoryValidAfter = 0;
          uint32_t shadowHistoryReadIndex = 0;
          uint32_t shadowHistoryWriteIndex = 0;
          uint32_t shadowHistoryAdvancedThisFrame = 0;
          uint32_t shadowHistoryAdvanceSkippedIncomplete = 0;
          uint32_t shadowHistoryInvalidationMask = 0;
          uint32_t shadowReceiverSampleSource = 0; // 0 none, 1 map, 2 current, 3 history
          uint64_t gpuSkinVsShadowDirectAttempts = 0u;
          uint64_t gpuSkinVsShadowDirectInputRejects = 0u;
          uint64_t gpuSkinVsShadowDirectStateRejects = 0u;
          uint64_t gpuSkinVsShadowDirectDrawsSubmitted = 0u;
          uint64_t gpuSkinVsShadowDirectBindingsCleared = 0u;
          uint64_t gpuSkinVsShadowReplayDirectional = 0u;
          uint64_t gpuSkinVsShadowReplayPoint = 0u;
          uint64_t gpuSkinVsShadowReplayUnknown = 0u;
        } reconciliation;

    private:
        Rc<DxvkSampler> m_fallbackSampler; // 用于ShadowPass的备用采样器
        Rc<DxvkSampler> m_fallbackSamplerMip; // Alpha阴影允许Mip的采样器
        std::unordered_map<int32_t, Rc<DxvkSampler>> m_fallbackSamplerMipBias; // Alpha阴影 Mip + LOD bias 的采样器缓存

        struct PipelineKey {
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

            bool eq(const PipelineKey& other) const {
                return format == other.format && samples == other.samples;
            }
            size_t hash() const {
                DxvkHashState h;
                h.add(uint32_t(format));
                h.add(uint32_t(samples));
                return h;
            }
        };

        struct Pipeline {
            const DxvkPipelineLayout* layout = nullptr;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };

        struct ShadowCasterPipelineKey {
            VkFormat positionFormat = VK_FORMAT_UNDEFINED;
            uint32_t positionStride = 0;
            uint32_t positionOffset = 0;
            VkFormat blendWeightFormat = VK_FORMAT_R32_SFLOAT;
            uint32_t blendWeightOffset = 0;
            VkFormat blendIndexFormat = VK_FORMAT_R8G8B8A8_USCALED;
            uint32_t blendIndexOffset = 0;
            uint32_t blendBinding = 0;
            uint32_t blendStride = 0;
            VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            // ===== Alpha测试支持：UV顶点属性 =====
            VkFormat uvFormat = VK_FORMAT_UNDEFINED;  // UV格式 (通常 R32G32_SFLOAT)
            uint32_t uvOffset = 0;                     // UV在顶点中的偏移
            uint32_t uvStride = 0;                     // UV步长 (通常与Position相同)
            uint32_t uvBinding = 0;                    // 0=position, 1=blend, 2=separate
            bool alphaTestEnabled = false;             // 是否启用Alpha测试
            bool casterMaskEnabled = false;            // CSM caster-kind R8 output
            bool pointShadowRadialDepth = false;       // point cube writes distance/range
            uint8_t outlineMode = 0;                   // 0=OccludedFill, 1=Silhouette

            bool eq(const ShadowCasterPipelineKey& other) const {
                return positionFormat == other.positionFormat
                    && positionStride == other.positionStride
                    && positionOffset == other.positionOffset
                    && blendWeightFormat == other.blendWeightFormat
                    && blendWeightOffset == other.blendWeightOffset
                    && blendIndexFormat == other.blendIndexFormat
                    && blendIndexOffset == other.blendIndexOffset
                    && blendBinding == other.blendBinding
                    && blendStride == other.blendStride
                    && topology == other.topology
                    && uvFormat == other.uvFormat
                    && uvOffset == other.uvOffset
                    && uvStride == other.uvStride
                    && uvBinding == other.uvBinding
                    && alphaTestEnabled == other.alphaTestEnabled
                    && casterMaskEnabled == other.casterMaskEnabled
                    && pointShadowRadialDepth == other.pointShadowRadialDepth
                    && outlineMode == other.outlineMode;
            }
            size_t hash() const {
                DxvkHashState h;
                h.add(uint32_t(positionFormat));
                h.add(positionStride);
                h.add(positionOffset);
                h.add(uint32_t(blendWeightFormat));
                h.add(blendWeightOffset);
                h.add(uint32_t(blendIndexFormat));
                h.add(blendIndexOffset);
                h.add(blendBinding);
                h.add(blendStride);
                h.add(uint32_t(topology));
                h.add(uint32_t(uvFormat));
                h.add(uvOffset);
                h.add(uvStride);
                h.add(uvBinding);
                h.add(uint32_t(alphaTestEnabled));
                h.add(uint32_t(casterMaskEnabled));
                h.add(uint32_t(pointShadowRadialDepth));
                h.add(uint32_t(outlineMode));
                return h;
            }
        };

        struct ShadowCasterPipeline {
            const DxvkPipelineLayout* layout = nullptr;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };

        struct PreparedShadowCaster {
            bool valid = false;
            ShadowCasterPipeline pipeline = {};
            size_t pipelineHash = 0;
            VkImageView alphaImageView = VK_NULL_HANDLE;
            VkBuffer positionBuffer = VK_NULL_HANDLE;
            VkBuffer indexBuffer = VK_NULL_HANDLE;
            bool effectiveAlphaTest = false;
            bool gpuSkinDirectRequested = false;
            bool gpuSkinDirectInputExact = false;
            bool gpuSkinDirectStateExact = false;
            bool lifetimeResourcesTracked = false;
        };

        struct OutlineEdgePipelineKey {
            VkFormat format = VK_FORMAT_UNDEFINED;

            bool eq(const OutlineEdgePipelineKey& other) const {
                return format == other.format;
            }
            size_t hash() const {
                DxvkHashState h;
                h.add(uint32_t(format));
                return h;
            }
        };

        D3D9DeviceEx* m_parent = nullptr; // D3D9DeviceEx 指针
        Rc<DxvkDevice> m_device;
        
        // Shadow Map Resources (D3D9)
        Com<IDirect3DTexture9> m_shadowTexture;
        Com<IDirect3DSurface9> m_shadowSurface;
        
        Rc<DxvkSampler> m_samplerLinear;
        Rc<DxvkSampler> m_shadowSampler;
        Rc<DxvkSampler> m_shadowSamplerLinear;
        Rc<DxvkSampler> m_shadowSamplerActive;
        float m_shadowCasterBiasConstant = 1.0f;
        float m_shadowCasterBiasSlope = 1.5f;
        float m_shadowCasterBiasClamp = 0.0f;
        const DxvkPipelineLayout* m_layout = nullptr;
        const DxvkPipelineLayout* m_shadowCasterLayout = nullptr;

        std::unordered_map<PipelineKey, Pipeline, DxvkHash, DxvkEq> m_pipelines;
        std::unordered_map<ShadowCasterPipelineKey, ShadowCasterPipeline, DxvkHash, DxvkEq> m_shadowCasterPipelines;
        std::vector<PreparedShadowCaster> m_shadowPreparedScratch;
        std::vector<uint32_t> m_shadowSortedDrawIndicesScratch;
        std::vector<uint32_t> m_shadowDrawIndicesScratch;
        std::vector<uint32_t> m_shadowTerrainMaskDrawIndicesScratch;

        // VS-S1 跨帧累计账本；Run() 的 per-frame reconciliation 只发布快照，
        // 不得把 clean-pair 所需的单调累计值清零。
        uint64_t m_gpuSkinVsShadowDirectAttempts = 0u;
        uint64_t m_gpuSkinVsShadowDirectInputRejects = 0u;
        uint64_t m_gpuSkinVsShadowDirectStateRejects = 0u;
        uint64_t m_gpuSkinVsShadowDirectDrawsSubmitted = 0u;
        uint64_t m_gpuSkinVsShadowDirectBindingsCleared = 0u;
        uint64_t m_gpuSkinVsShadowReplayDirectional = 0u;
        uint64_t m_gpuSkinVsShadowReplayPoint = 0u;
        uint64_t m_gpuSkinVsShadowReplayUnknown = 0u;

        // ShadowTAA 相关全屏 pass（固定输出格式，单例 pipeline）
        VkPipeline m_motionVectorPipeline = VK_NULL_HANDLE;      // R16G16_SFLOAT
        VkPipeline m_shadowVisibilityPipeline = VK_NULL_HANDLE;  // R8_UNORM（当前帧阴影因子）
        
        // [NEW] 单位被遮挡描边管线
        const DxvkPipelineLayout* m_outlineLayout = nullptr;
        const DxvkPipelineLayout* m_outlineMaskLayout = nullptr;
        std::unordered_map<ShadowCasterPipelineKey, VkPipeline, DxvkHash, DxvkEq> m_outlinePipelines;
        std::unordered_map<ShadowCasterPipelineKey, VkPipeline, DxvkHash, DxvkEq> m_outlineMaskVisiblePipelines;
        std::unordered_map<ShadowCasterPipelineKey, VkPipeline, DxvkHash, DxvkEq> m_outlineMaskAllPipelines;
        // [NEW] MRT Single-Pass Pipelines
        std::unordered_map<ShadowCasterPipelineKey, VkPipeline, DxvkHash, DxvkEq> m_outlineMaskMRTPipelines;
        const DxvkPipelineLayout* m_outlineEdgeLayout = nullptr;
        std::unordered_map<OutlineEdgePipelineKey, VkPipeline, DxvkHash, DxvkEq> m_outlineEdgePipelines;

        Rc<DxvkImage> m_outlineMaskVisible;
        Rc<DxvkImageView> m_outlineMaskVisibleView;
        Rc<DxvkImage> m_outlineMaskAll;
        Rc<DxvkImageView> m_outlineMaskAllView;
        VkExtent3D m_outlineMaskExtent = {0, 0, 1};

        // 中间颜色副本（可采样）
        Rc<DxvkImage> m_colorCopy;
        Rc<DxvkImageView> m_colorCopyView;
        VkExtent3D m_cachedExtent = {0, 0, 1};
        VkFormat m_cachedFormat = VK_FORMAT_UNDEFINED;

        // 中间深度副本（可采样，格式与当前 depth surface 对齐）
        Rc<DxvkImage> m_depthCopy;
        Rc<DxvkImageView> m_depthCopyView;   // 2D array，用于 ShadowReceiver
        Rc<DxvkImageView> m_depthCopyView2D; // 2D 视图，用于描边遮罩
        VkExtent3D m_cachedDepthExtent = {0, 0, 1};
        VkFormat m_cachedDepthFormat = VK_FORMAT_UNDEFINED;

        // Motion Vector Buffer（用于 ShadowTAA / 未来全屏 TAA）
        // 说明：当前阶段仅提供“相机运动向量”（基于深度 + prevViewProj 重投影）。
        Rc<DxvkImage> m_motionVectorImage;
        Rc<DxvkImageView> m_motionVectorView;
        VkExtent3D m_mvCachedExtent = {0, 0, 1};

        // Shadow TAA 资源：当前帧阴影因子（未滤波）+ 历史（Ping-Pong）
        Rc<DxvkImage> m_shadowCurrent;
        Rc<DxvkImageView> m_shadowCurrentView;
        VkExtent3D m_shadowCurrentExtent = {0, 0, 1};

        std::array<Rc<DxvkImage>, 2> m_shadowHistory = { };
        std::array<Rc<DxvkImageView>, 2> m_shadowHistoryView = { };
        std::array<Rc<DxvkImageView>, 2> m_shadowHistoryStorageView = { };
        VkExtent3D m_shadowHistoryExtent = {0, 0, 1};
        uint32_t m_shadowHistoryIndex = 0; // 当前作为“历史读取”的索引
        bool m_shadowHistoryValid = false; // 历史是否已写入过（避免首次启用时读到旧数据）
        bool m_shadowTaaWasActiveLastFrame = false; // 上一帧是否执行了 ShadowTAA（用于避免断档后混入陈旧历史）
        // A history image is readable only when it names the exact scene/map
        // contract that produced it. These fields are committed atomically at
        // the end of a complete Visibility+Motion+Receiver+HistoryWrite frame.
        bool m_shadowTaaHistoryContractValid = false;
        Matrix4 m_shadowTaaHistoryView = {};
        Matrix4 m_shadowTaaHistoryViewProj = {};
        Matrix4 m_shadowTaaHistoryProjection = {};
        Vector4 m_shadowTaaHistorySunDirection =
            Vector4(0.0f, 0.0f, 1.0f, 0.0f);
        uint32_t m_shadowTaaHistoryViewportX = 0u;
        uint32_t m_shadowTaaHistoryViewportY = 0u;
        uint32_t m_shadowTaaHistoryViewportWidth = 0u;
        uint32_t m_shadowTaaHistoryViewportHeight = 0u;
        float m_shadowTaaHistoryViewportMinZ = 0.0f;
        float m_shadowTaaHistoryViewportMaxZ = 1.0f;
        uint64_t m_shadowTaaHistoryCsmHash = 0u;
        uint64_t m_shadowTaaHistoryReplayContentHash = 0u;
        uint64_t m_shadowTaaHistoryReplayBackingHash = 0u;
        uint64_t m_shadowTaaHistoryDynamicPoseSignature = 0u;
        uint64_t m_shadowTaaHistoryLifecycleSerial = 0u;
        uint64_t m_shadowTaaHistoryStagePolicyRevision = 0u;
        uint64_t m_shadowTaaHistoryMapResourceGeneration = 0u;
        uint64_t m_shadowTaaHistoryResourceGeneration = 0u;
        uint64_t m_shadowMapResourceGeneration = 0u;
        uint64_t m_shadowTaaResourceGeneration = 0u;
        uint64_t m_shadowLifecycleTombstoneSerialSeen = 0u;
        uint64_t m_shadowStagePolicyRevisionSeen = 0u;
        Vector4 m_shadowTaaPreviousSunDirection =
            Vector4(0.0f, 0.0f, 1.0f, 0.0f);
        bool m_shadowTaaHasPreviousSunDirection = false;
        bool m_shadowTaaModeInitialized = false;
        War3ShadowTaaMode m_shadowTaaRequestedModeSeen =
            War3ShadowTaaMode::DirectInline;
        uint64_t m_shadowTaaSettingsRevisionSeen = 0u;
        uint64_t m_shadowTaaHistoryGeneration = 0u;
        uint32_t m_shadowTaaLastInvalidationReason = 0u;
        uint64_t m_shadowTaaFixedWallBypassCount = 0u;

        bool m_csmResolutionFallbackLatched = false;
        CsmResolutionFallbackReason m_csmResolutionFallbackReason =
            CsmResolutionFallbackReason::None;
        uint32_t m_csmRequestedResolution = 4096u;
        uint32_t m_csmEffectiveResolution = 0u;
        uint64_t m_csmResourceRebuildCount = 0u;
        bool m_csmMemoryBudgetSupported = false;
        uint64_t m_csmMemoryBudgetBytes = 0u;
        uint64_t m_csmMemoryAvailableBytes = 0u;

        // Receiver shader 常量（CSM 矩阵、split 等）
        Rc<DxvkBuffer> m_shadowUniformBuffer;
        
        // Point Light Buffer (Array of lights)
        Rc<DxvkBuffer> m_lightBuffer;
        
        struct LightUniform {
            uint32_t count;
            uint32_t pad[3];
            struct {
                Vector4 pos;   // xyz, w=range
                Vector4 color; // rgb, w=intensity
                // x=authored shadow intensity; yzw=per-frame view-space
                // position. Hoisting the uniform world->view transform out of
                // the full-resolution fragment loop keeps the 48-byte ABI.
                Vector4 params;
            } lights[16];      // Max 16 lights
        };
        static_assert(sizeof(LightUniform) == 784u,
                      "LightUniform must match receiver GLSL scalar layout");
        // Run() captures one immutable manager snapshot and materializes the
        // direct-light payload once. Point-shadow prepare and receiver upload
        // must consume the same generation/order to avoid light/shadow drift.
        LightUniform m_pointLightFrameUniform = {};
        // Canonical shadow-capable prefix from the same immutable snapshot.
        // Contact rays must not spend work on authored shadowIntensity=0 lights.
        uint32_t m_pointRayEligibleLightCount = 0;
        // A1 half-resolution Hi-Z result. These references are cleared at the
        // start of every Run and published only after the full compute chain
        // and frame/light generation tuple have matched.
        std::unique_ptr<war3::render::War3HybridRayTracing>
            m_hybridRayTracing;
        Rc<DxvkImageView> m_pointRayHiZVisibilityView;
        Rc<DxvkImageView> m_pointRayHiZView;
        uint32_t m_pointRayHiZLightCount = 0;
        uint64_t m_pointRayHiZFrameSerial = 0;
        uint64_t m_pointRayHiZResourceGeneration = 0;
        uint64_t m_pointRayHiZLightGeneration = 0;
        bool m_hybridRayTracingUnavailable = false;

        // CSM ShadowMap（深度 2D array）
        War3CsmConfig m_csmConfig;
        War3CsmCalculator m_csm;
        War3CsmData m_csmData;
        bool m_hasCompleteShadowMap = false;
        // Current-frame transaction settlement for external CSM consumers.
        // Run clears this before any fallible work and republishes only at its
        // normal end after a complete/rendered or explicit reusable map exists.
        uint64_t m_shadowPublicationSettledFrameSerial = 0u;
        uint32_t m_lastShadowMapCasterCount = 0;
        uint64_t m_lastDynamicPoseSignature = 0;
        uint64_t m_lastShadowMapReplayContentHash = 0u;
        uint64_t m_lastShadowMapReplayBackingHash = 0u;
        uint64_t m_lastShadowMapStagePolicyRevision = 0u;
        uint64_t m_lastShadowMapCsmHash = 0u;
        uint64_t m_lastShadowMapResourceGeneration = 0u;
        uint32_t m_shadowAdaptiveFrameIndex = 0;
        uint32_t m_transientEmptyReplayHoldFramesRemaining = 0;
        uint32_t m_recentSemanticDynamicHoldFramesRemaining = 0;
        uint32_t m_semanticIdentityChurnHoldFramesRemaining = 0;
        uint32_t m_semanticCoverageDropHoldStreak = 0;
        uint64_t m_lastShadowMapSemanticIdentityHash = 0;
        uint64_t m_pendingShadowMapSemanticIdentityHash = 0;
        uint32_t m_pendingShadowMapSemanticIdentityStableFrames = 0;
        Vector4 m_lastShadowMapSunDir = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
        float m_lastShadowMapStrength = 0.0f;
        bool m_hasLastShadowMapLighting = false;
        War3WorldCameraState m_lastGoodReceiverCamera = {};
        bool m_hasLastGoodReceiverCamera = false;

        // Vertex blending palette buffer (WorldMatrices[paletteIndex*256 + i])
        // Uses a ring buffer to avoid CPU/GPU data races across frames-in-flight.
        // Several passes can consume the shared shadow-matrix SSBO in the same
        // frame (shadow map, outline mask, unit outline), and high caster load
        // can leave several frames in flight. Three slices can overwrite a
        // slice still referenced by queued GPU work, which presents as regular
        // skinned-shadow flicker. Keep this ring deliberately wider than the
        // nominal swapchain frame count and per-frame pass count.
        static constexpr uint32_t kPaletteRingCount = 12;
        Rc<DxvkBuffer> m_vertexBlendPaletteBuffer;
        Rc<DxvkBuffer> m_dummyPaletteBuffer; // 点光源阴影未使用骨骼时的占位SSBO
        void* m_vertexBlendPaletteMapPtr = nullptr;
        VkDeviceSize m_vertexBlendPaletteCapacity = 0;
        VkDeviceSize m_paletteStride = 0;        // Per-ring-segment stride (only grows, never shrinks)
        uint64_t m_paletteFrameId = 0;           // Frame counter for ring rotation
        uint32_t m_paletteBaseMatrixIndex = 0;   // Base matrix index for current frame (set after upload)

        // 当前帧矩阵缓冲区 slice（包含：骨骼调色板 + 每个 draw 的 worldMatrix）
        // 说明：用于统一 shadow caster / outline mask / unit outline 的矩阵来源，避免 ring offset 不一致导致读取错位。
        DxvkResourceBufferInfo m_shadowMatrixBufferInfo = {};
        uint64_t m_shadowMatrixUploadedFrame = 0; // RenderObjectRegistry::getFrameNumber()
        uint32_t m_shadowMatrixObjectBase = 0;    // = paletteCount * 256
        uint64_t m_shadowMatrixSceneKey = 0;       // palette/world-matrix content key for same-frame semantic updates
        uint64_t m_shadowMatrixUploadSerial = 0;   // advances only on real uploads; selects the ring slice
        uint64_t m_shadowMapRenderSerial = 0;      // advances only after a real shadow map render

        Rc<DxvkImage> m_shadowMap;
        Rc<DxvkImageView> m_shadowMapSampleView;
        std::array<Rc<DxvkImageView>, 4> m_shadowMapLayerViews = { };
        Rc<DxvkImage> m_shadowCasterMask;
        Rc<DxvkImageView> m_shadowCasterMaskSampleView;
        std::array<Rc<DxvkImageView>, 4> m_shadowCasterMaskLayerViews = { };
        uint32_t m_shadowMapLayers = 0;
        uint32_t m_shadowMapResolution = 0;

        // 体积光专用太阳 ortho 阴影（与相机 CSM 资源完全分离，最多 2 层）
        Rc<DxvkImage> m_volumeSunShadowMap;
        Rc<DxvkImageView> m_volumeSunShadowSampleView;
        std::array<Rc<DxvkImageView>, 2> m_volumeSunShadowLayerViews = {};
        uint32_t m_volumeSunShadowResolution = 0;
        uint32_t m_volumeSunShadowLayers = 0;
        Matrix4 m_volumeSunLightViewProj[2] = {};
        War3VolumeSunOrtho m_volumeSunOrthoNear = {};
        War3VolumeSunOrtho m_volumeSunOrthoFar = {};
        uint64_t m_volumeSunPublishedFrameSerial = 0u;
        bool m_volumeSunShadowReady = false;
        float m_volumeSunSoftRadius = 1.35f;
        float m_volumeSunReceiverBias = 0.006f;
        // renderShadowMap 临时路径：跳过 terrain mask、使用 volume 目标
        bool m_volumeSunRenderPathActive = false;
        
        // [NEW] Point Light Cube Shadow Map
        static constexpr uint32_t kMaxPointShadowLights = 4;  // 限制投射阴影的点光源数量
        uint32_t m_pointShadowResolution = 0;                 // 当前 cube face 分辨率
        uint32_t m_pointShadowCapacityLights = 0;             // 当前 cube array 实际容量（1..4）
        Rc<DxvkImage> m_pointShadowCube;                      // Cube Depth Texture
        Rc<DxvkImageView> m_pointShadowCubeView;              // CubeArray 采样视图
        bool m_pointShadowCubeLayoutInitialized = false;
        // Receiver shader 静态声明 textureCubeArray；点阴影未就绪时也必须
        // 绑定维度匹配的合法 view，不能用 CSM texture2DArray 充当 fallback。
        Rc<DxvkImage> m_pointShadowNeutralCube;
        Rc<DxvkImageView> m_pointShadowNeutralCubeView;
        bool m_pointShadowNeutralReady = false;
        std::array<Rc<DxvkImageView>, kMaxPointShadowLights * 6> m_pointShadowFaceViews; // 每光源6个面的渲染视图
        std::array<bool, kMaxPointShadowLights> m_pointShadowReady = {};
        uint32_t m_pointShadowReadyCount = 0;
        
        // Point Shadow 渲染所需的矩阵
        struct PointShadowData {
            Vector4 lightPos;   // xyz=position, w=range
            Matrix4 faceViewProj[6]; // 6个面的 ViewProj 矩阵
            float shadowIntensity = 0.0f;
        };
        std::array<PointShadowData, kMaxPointShadowLights> m_pointShadowData = {};
        float m_pointShadowBias = 1.0f;
        Vector4 m_pointShadowFilterParams =
            Vector4(0.65f, 1.15f, 0.50f, 0.78f);
        uint32_t m_pointShadowDebugLightIndex = 0;
        bool m_pointLightsEnabled = false;
        bool m_hasPointLights = false;
        bool m_pointShadowEnabled = false;
        Vector4 m_pointLightCameraPos = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        // 点光阴影时序复用状态：仅当 light/caster 签名稳定时隔帧跳过 cube 重渲。
        uint64_t m_pointShadowContentSignature = 0;
        // Generation of the immutable light snapshot that produced the
        // currently published cube. A cube is sampled only when this and the
        // current CPU plan's semantic content signature both match.
        uint64_t m_pointShadowPublishedLightGeneration = 0;
        uint64_t m_pointShadowPublishedFrameSerial = 0;
        uint32_t m_pointShadowPublishedLightCount = 0;
        std::array<int32_t, kMaxPointShadowLights>
            m_pointShadowPublishedLightIds = {};
        // Allocation failures are retried with bounded cadence. Without this,
        // a persistent OOM would attempt the same cube allocation every frame.
        uint32_t m_pointShadowFailedResolution = 0;
        uint32_t m_pointShadowFailedCapacityLights = 0;
        uint64_t m_pointShadowRunSerial = 0;
        uint64_t m_pointShadowResourceRetryAfterSerial = 0;
        uint32_t m_pointShadowTemporalAge = 0;
        // face round-robin：越大越旧，优先更新。
        std::array<std::array<uint32_t, 6>, kMaxPointShadowLights>
            m_pointShadowFaceAge = {};
        std::array<uint8_t, kMaxPointShadowLights> m_pointShadowFaceValidMask = {};
        std::vector<uint32_t> m_pointShadowCasterIndicesScratch;
        std::vector<uint32_t> m_pointShadowFaceCasterIndicesScratch;
        /**
         * @brief Worker_Prepare 产出的点阴影 CPU 计划（主线程 CSM 与 worker 重叠）。
         */
        struct PointShadowCpuPlan {
          bool ready = false;
          bool shouldRender = false;
          // Set only by the explicit stable-signature temporal cadence path.
          // Other shouldRender=false outcomes revoke publication instead.
          bool reusePublished = false;
          bool forceFullFaceUpdate = false;
          bool failed = false;
          uint32_t shadowLightCount = 0;
          uint32_t resourceCapacityLights = 1;
          uint32_t maxFacesPerFrame = 6;
          uint32_t resolution = 1024;
          uint32_t maxCastersPerFace = 0;
          uint64_t lightGeneration = 0;
          uint64_t lightFrameSerial = 0;
          uint64_t contentSignature = 0;
          std::array<uint8_t, kMaxPointShadowLights> updateMask = {};
          // [light * 6 + face] CPU quality accounting. These counters make an
          // explicitly configured performance cap observable instead of
          // silently turning missing casters into apparent cube-map errors.
          std::array<uint32_t, kMaxPointShadowLights * 6>
              faceCandidateCount = {};
          std::array<uint32_t, kMaxPointShadowLights * 6> faceKeptCount = {};
          std::array<uint32_t, kMaxPointShadowLights * 6> faceDroppedCount = {};
          // [light * 6 + face] 的 caster 索引（相对 replayDraws）
          std::array<std::vector<uint32_t>, kMaxPointShadowLights * 6>
              faceCasters = {};
        };
        PointShadowCpuPlan m_pointShadowCpuPlan = {};
        std::future<void> m_pointShadowPrepareFuture;
        std::atomic<bool> m_pointShadowPrepareRunning{false};

        using PointShadowPersistentPrepareWorker =
            war3::render::War3PointShadowPersistentPrepareWorker<
                war3::render::War3PointShadowCpuPlanRequestPayload,
                war3::render::War3PointShadowCpuPlanResultPayload,
                war3::render::War3PointShadowCpuPlanner>;
        using PointShadowPersistentRequest =
            war3::render::War3PointShadowCpuPlanRequest;
        using PointShadowPersistentResultPayload =
            war3::render::War3PointShadowCpuPlanResultPayload;

        // The persistent path is lazily allocated only for explicit
        // Observe/Consume. The release default remains Off and therefore does
        // not create a thread or change the existing std::async path.
        std::unique_ptr<PointShadowPersistentPrepareWorker>
            m_pointShadowPersistentWorker;
        PointShadowPersistentRequest m_pointShadowPersistentRequestScratch = {};
        war3::render::War3PointShadowPrepareGenerationTuple
            m_pointShadowPersistentPendingGeneration = {};
        war3::render::War3PointShadowCpuPlanSealTuple
            m_pointShadowPersistentPendingSeal = {};
        war3::render::War3PointShadowCpuPlanSettings
            m_pointShadowPersistentExpectedSettings = {};
        war3::render::War3PointShadowCpuHistory
            m_pointShadowPersistentExpectedHistory = {};
        std::array<war3::render::War3PointShadowCpuLight,
                   kMaxPointShadowLights>
            m_pointShadowPersistentExpectedLights = {};
        uint32_t m_pointShadowPersistentExpectedLightCount = 0u;
        uint64_t m_pointShadowPersistentExpectedDynamicPoseSignature = 0u;
        uint32_t m_pointShadowPersistentExpectedDynamicPoseCount = 0u;
        uint32_t m_pointShadowPersistentExpectedDynamicSkinnedOutputCount = 0u;
        bool m_pointShadowPersistentPending = false;
        uint64_t m_pointShadowPersistentRendererEpoch = 0u;
        uint64_t m_pointShadowPersistentJobSerial = 0u;
        uint64_t m_pointShadowPersistentBeginAttempts = 0u;
        uint64_t m_pointShadowPersistentBeginEligible = 0u;
        uint64_t m_pointShadowPersistentWorkerCreateCount = 0u;
        uint64_t m_pointShadowPersistentAccepted = 0u;
        uint64_t m_pointShadowPersistentDeadlineFallback = 0u;
        uint64_t m_pointShadowPersistentRejectedFallback = 0u;
        uint64_t m_pointShadowPersistentObserveMatch = 0u;
        uint64_t m_pointShadowPersistentObserveMismatch = 0u;
        uint64_t m_pointShadowPersistentConsumed = 0u;
        PointShadowPersistentBeginRejectReason
            m_pointShadowPersistentLastBeginRejectReason =
                PointShadowPersistentBeginRejectReason::ModeOff;
        
        // [NEW] Point Shadow UBO for receiver shader
        struct PointShadowLightUniform {
            Vector4 lightPos;   // xyz=position, w=range
            float bias = 0.0f;
            float enabled = 0.0f;
            float shadowIntensity = 0.0f;
            float pad0 = 0.0f;
        };
        struct PointShadowUniform {
            uint32_t lightCount = 0;
            uint32_t debugLightIndex = 0;
            uint32_t samplerIndex = 0;
            uint32_t pad2 = 0;
            Vector4 filterParams = Vector4(0.65f, 1.15f, 0.50f, 0.78f);
            std::array<PointShadowLightUniform, 4> lights = {};
        };
        static_assert(sizeof(PointShadowLightUniform) == 32u,
                      "PointShadowLightUniform must match GLSL scalar layout");
        static_assert(sizeof(PointShadowUniform) == 160u,
                      "PointShadowUniform must match GLSL scalar layout");
        static_assert(offsetof(PointShadowUniform, samplerIndex) == 8u,
                      "Point-shadow sampler ABI drift");
        static_assert(offsetof(PointShadowUniform, filterParams) == 16u,
                      "Point-shadow filter ABI drift");
        Rc<DxvkBuffer> m_pointShadowUniformBuffer;
        
        // 日夜循环状态（只在 worldCamera.valid 时初始化）
        bool m_timeInitialized = false;
        std::chrono::steady_clock::time_point m_timeStart;

        // 上一帧相机矩阵（用于 motion vector / Shadow TAA 重投影）
        Matrix4 m_prevViewMatrix;
        Matrix4 m_prevProjMatrix;
        Matrix4 m_prevViewProj;
        bool m_hasPrevFrameData = false;

        // 时间平滑：将 JASS/Hook 侧“台阶式”的 TIME_OF_DAY 变为逐帧连续值
        // 说明：Hook 侧通常 100ms 更新一次 TIME_OF_DAY，会导致太阳方向/CSM 以 10Hz 抖动。
        // 这里保持 Hook 采样频率不变，但渲染侧做一个轻量 PLL 平滑（积分 + 误差回正）。
        bool m_timeSmoothingInitialized = false;
        float m_time01Smoothed = 0.0f;
        float m_time01LastRaw = 0.0f;
        float m_time01Speed = 0.0f;  // time01 每秒变化量（约等于 1 / dayLengthSeconds）
        std::chrono::steady_clock::time_point m_time01LastUpdate;
        std::chrono::steady_clock::time_point m_time01LastRawSample;
        
        // 配置参数 (可根据需求调整或暴露给 Config)
        float m_dayLengthSeconds = 480.0f;       // 8分钟 = 24小时
        float m_startTime01 = 0.22f;             // 进游戏起始时间 (0.25=日出, 0.5=正午)
        
        // 阴影限制参数
        float m_minSunAltitudeDeg = 35.0f;       // 最小高度角 (保证阴影不拉太长, ~1.43x)
        float m_maxSunAltitudeDeg = 55.0f;       // 最大高度角 (防止自阴影过多 0.7x)

    public:
        // [事件系统] 日夜阶段定义
        enum class War3DayNightPhase {
            Night,      // 深夜 (Moon)
            Dawn,       // 拂晓 (Transition: Night -> Day)
            Day,        // 白天 (Sun)
            Dusk        // 黄昏 (Transition: Day -> Night)
        };

        using War3DayNightEventCallback = std::function<void(War3DayNightPhase phase, float progress)>;

        // 注册事件回调
        void SetEventCallback(War3DayNightEventCallback cb) { m_eventCb = cb; }

    private:
        War3DayNightEventCallback m_eventCb;
        War3DayNightPhase m_currentPhase = War3DayNightPhase::Day;
        
        // 辅助：更新阶段并触发事件
        void UpdatePhase(float altitudeRad, bool isRising, float transitionRad);
        
        // 缓存的 worldUp
        Vector4 m_cachedWorldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);  

        // 辅助：从开尔文色温计算 RGB
        static Vector4 kelvinToRgb(float k);

        Pipeline getPipeline(VkFormat format, VkSampleCountFlagBits samples);
        const DxvkPipelineLayout* createPipelineLayout() const;
        Pipeline createPipeline(const PipelineKey& key) const;

        ShadowCasterPipeline getShadowCasterPipeline(const ShadowCasterPipelineKey& key);
        const DxvkPipelineLayout* createShadowCasterPipelineLayout() const;
        ShadowCasterPipeline createShadowCasterPipeline(const ShadowCasterPipelineKey& key) const;

        // [NEW] MRT Outline Pipeline Helpers
        const DxvkPipelineLayout* createOutlineMaskPipelineLayout() const;
        ShadowCasterPipeline createOutlineMaskPipeline(const ShadowCasterPipelineKey& key) const;

        void ensureCopyResources(VkExtent3D extent, VkFormat format);
        void ensureDepthCopyResources(VkExtent3D extent, VkFormat format);
        void ensureMotionVectorResources(VkExtent3D extent);
        void ensureShadowTaaResources(VkExtent3D extent);
        bool ensureShadowResources(uint32_t cascadeCount, uint32_t resolution);
        // 上传“骨骼调色板 + 非混合 worldMatrix”到 SSBO（按帧去重）
        DxvkResourceBufferInfo ensureShadowMatrixBuffer(
            const Rc<DxvkCommandList>& ctx,
            const War3PipelineInput& input,
            const std::vector<const War3ShadowCasterDraw*>* replayDraws = nullptr);
        void ensurePointShadowResources(uint32_t resolution,
                                        uint32_t capacityLights);
        void ensurePointShadowNeutralResources(); // 1x1x6 legal CubeArray fallback
        void copyColor(const Rc<DxvkCommandList>& ctx,
                       const Rc<DxvkImageView>& dstView);
        void copyDepth(const Rc<DxvkCommandList>& ctx,
                       const Rc<DxvkImageView>& srcDepthView);
        void renderMotionVectors(const Rc<DxvkCommandList>& ctx,
                                 const War3PipelineInput& input);
        void renderShadowVisibility(const Rc<DxvkCommandList>& ctx,
                                    const War3PipelineInput& input);
        void ensureOutlineMaskResources(VkExtent3D extent);
        bool renderShadowMap(
            const Rc<DxvkCommandList>& ctx,
            const War3PipelineInput& input,
            const std::vector<const War3ShadowCasterDraw*>* replayDraws = nullptr);
        /**
         * @brief 渲染体积专用太阳 ortho 深度（单层 texture2DArray）。
         * @note 复用本帧 replay draws 与矩阵 SSBO；不改表面 CSM 资源。
         */
        bool renderVolumeSunShadow(
            const Rc<DxvkCommandList>& ctx,
            const War3PipelineInput& input,
            const std::vector<const War3ShadowCasterDraw*>* replayDraws);
        void ensureVolumeSunShadowResources(uint32_t resolution);
        void invalidateVolumeSunShadowPublication();
        /**
         * @brief A2 Worker_Prepare：在主线程渲染 CSM 期间，后台预计算点阴影 face/caster 列表。
         * @param input 管线输入（settings/camera/frameIndex）。
         * @param lightSnapshot Run 内锁定的同帧不可变光源快照。
         * @param replayDraws 本帧已 seal 的 caster 指针列表（生命周期须覆盖 wait）。
         * @note 仅 CPU 工作，禁止触碰 DXVK command list / GPU 资源。
         */
        void beginPointShadowCpuPrepare(
            const War3PipelineInput& input,
            const War3PointLightFrameSnapshot& lightSnapshot,
            const std::vector<const War3ShadowCasterDraw*>* replayDraws);
        /** @brief 等待点阴影 CPU prepare 完成（无在途任务时立即返回）。 */
        void waitPointShadowCpuPrepare();
        /** @brief Build and submit one renderer-owned value request. Never
         *         waits; rejection leaves the exact request in caller storage. */
        void beginPointShadowPersistentPrepare(
            const War3PipelineInput& input,
            const War3PointLightFrameSnapshot& lightSnapshot,
            const std::vector<const War3ShadowCasterDraw*>* replayDraws);
        /** @brief Non-blocking exact collection. Non-ready/failed/stale returns
         *         no proposal and the caller performs the canonical same-frame
         *         synchronous build. */
        std::optional<PointShadowPersistentResultPayload>
        tryCollectPointShadowPersistentProposal(
            const War3PipelineInput& input,
            const War3PointLightFrameSnapshot& lightSnapshot,
            const std::vector<const War3ShadowCasterDraw*>* replayDraws);
        void recyclePointShadowPersistentStorage(
            war3::render::War3PointShadowCpuPlanOwnedStorage&& storage);
        bool adoptPointShadowPersistentProposal(
            PointShadowPersistentResultPayload& proposal,
            const War3PointLightFrameSnapshot& lightSnapshot);
        bool adoptPointShadowPersistentReuseProposal(
            PointShadowPersistentResultPayload& proposal,
            const War3PointLightFrameSnapshot& lightSnapshot);
        bool adoptPointShadowPersistentRenderProposal(
            PointShadowPersistentResultPayload& proposal,
            const War3PointLightFrameSnapshot& lightSnapshot);
        bool pointShadowPersistentProposalMatchesCanonical(
            const PointShadowPersistentResultPayload& proposal) const;
        /** @brief Reset per-frame point-shadow plan state without discarding
         *         the 24 face-vector capacities retained from prior frames. */
        void resetPointShadowCpuPlanPreservingCapacity();
        /** @brief Fail-closed publication reset; the allocated cube may remain
         *         cached, but no receiver may sample its old light slots. */
        void invalidatePointShadowPublishedState();
        /** @brief Whether the current immutable plan names the exact published
         *         cube content/light generation. */
        bool pointShadowPublishedStateMatchesCurrentPlan() const;
        /**
         * @brief 点阴影 CPU 计划：签名、face budget、range/face caster 列表。
         * @return false 表示本帧应跳过 GPU cube 渲染（关闭/无灯/时序复用）。
         */
        bool preparePointShadowCpuPlan(
            const War3PointShadowCpuPlanInput& input,
            const War3PointLightFrameSnapshot& lightSnapshot,
            const std::vector<const War3ShadowCasterDraw*>* replayDraws);
        void renderPointShadow(const Rc<DxvkCommandList>& ctx,
                               const War3PipelineInput& input,
                               const War3PointLightFrameSnapshot& lightSnapshot,
                               const std::vector<const War3ShadowCasterDraw*>* replayDraws = nullptr); // [NEW] 点光源阴影
        void drawReceiver(const Rc<DxvkCommandList>& ctx,
                          const Rc<DxvkImageView>& dstView);
        
        // [NEW] 单位被遮挡描边
        void renderUnitOutline(const Rc<DxvkCommandList>& ctx,
                               const War3PipelineInput& input);
        void renderUnitOutlineScreenSpace(const Rc<DxvkCommandList>& ctx,
                                          const War3PipelineInput& input);
    };


    // Efficient ring-buffered pool for shadowing vertex/index data


} // namespace dxvk
