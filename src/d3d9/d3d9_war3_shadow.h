#pragma once

#include "d3d9_war3_pipeline.h"
#include "d3d9_war3_csm.h"

#include "../dxvk/dxvk_hash.h"

#include <chrono>
#include <unordered_map>
#include <functional> // [Fix] for std::function
#include <algorithm>  // [Fix] for std::max/min
#include <vector>
#include "d3d9_util.h"

namespace dxvk {

    class D3D9DeviceEx;

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
    class War3ShadowReceiverPass final : public War3RenderPass {
    public:
        explicit War3ShadowReceiverPass(D3D9DeviceEx* device);
        ~War3ShadowReceiverPass();

        War3InsertionPoint Point() const override { return War3InsertionPoint::BeforeUi; }
        void Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) override;
        
        Rc<DxvkSampler> getFallbackSampler(bool useMip, float mipLodBias);

        // Phase 7.2: submitted / replay / executed 对账
        // Run() 每帧会刷新这些字段，调用方在 Run() 返回后可读取并写入 War3ShadowCaptureStats
        struct ShadowReconciliationCounters {
          uint32_t shadowCastersCount = 0;
          uint32_t replayDrawsCount = 0;
          uint32_t shadowMapDrawnCasters = 0;
          uint32_t cascadeCulledCount = 0;
          uint32_t skinnedCasterCount = 0;
          uint32_t skinnedPreparedCount = 0;
          uint32_t skinnedInvalidBufferCount = 0;
          uint32_t skinnedInvalidPipelineCount = 0;
          uint32_t skinnedDrawnCount = 0;
          uint32_t shadowTaaActive = 0;
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
          uint32_t receiverDrawExecutedThisFrame = 0;
          uint32_t shadowTaaMode = 0;
          uint32_t shadowHistoryValidBefore = 0;
          uint32_t shadowHistoryValidAfter = 0;
          uint32_t shadowHistoryReadIndex = 0;
          uint32_t shadowHistoryWriteIndex = 0;
          uint32_t shadowReceiverSampleSource = 0; // 0 none, 1 map, 2 current, 3 history
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
            bool alphaTestEnabled = false;             // 是否启用Alpha测试
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
                    && alphaTestEnabled == other.alphaTestEnabled
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
                h.add(uint32_t(alphaTestEnabled));
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
        std::vector<uint32_t> m_shadowDrawIndicesScratch;

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
            } lights[16];      // Max 16 lights
        };

        // CSM ShadowMap（深度 2D array）
        War3CsmConfig m_csmConfig;
        War3CsmCalculator m_csm;
        War3CsmData m_csmData;
        bool m_hasCompleteShadowMap = false;
        uint32_t m_lastShadowMapCasterCount = 0;
        uint64_t m_lastDynamicPoseSignature = 0;
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
        uint32_t m_shadowMapLayers = 0;
        uint32_t m_shadowMapResolution = 0;
        
        // [NEW] Point Light Cube Shadow Map
        static constexpr uint32_t kMaxPointShadowLights = 1;  // 限制投射阴影的点光源数量
        static constexpr uint32_t kPointShadowResolution = 512; // 每个面的分辨率
        Rc<DxvkImage> m_pointShadowCube;                      // Cube Depth Texture
        Rc<DxvkImageView> m_pointShadowCubeView;              // Cube 采样视图
        std::array<Rc<DxvkImageView>, 6> m_pointShadowFaceViews; // 6个面的渲染视图
        bool m_pointShadowReady = false;
        
        // Point Shadow 渲染所需的矩阵
        struct PointShadowData {
            Vector4 lightPos;   // xyz=position, w=range
            Matrix4 faceViewProj[6]; // 6个面的 ViewProj 矩阵
        };
        PointShadowData m_pointShadowData;
        float m_pointShadowBias = 1.0f;
        bool m_pointLightsEnabled = false;
        bool m_hasPointLights = false;
        bool m_pointShadowEnabled = false;
        
        // [NEW] Point Shadow UBO for receiver shader
        struct PointShadowUniform {
            Vector4 lightPos;   // xyz=position, w=range
            float bias;
            float enabled;
            float pad[2];
        };
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
        void ensureShadowResources(uint32_t cascadeCount, uint32_t resolution);
        // 上传“骨骼调色板 + 非混合 worldMatrix”到 SSBO（按帧去重）
        DxvkResourceBufferInfo ensureShadowMatrixBuffer(
            const Rc<DxvkCommandList>& ctx,
            const War3PipelineInput& input,
            const std::vector<const War3ShadowCasterDraw*>* replayDraws = nullptr);
        void ensurePointShadowResources();  // [NEW] 点光源 Cube Shadow Map
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
        void renderPointShadow(const Rc<DxvkCommandList>& ctx,
                               const War3PipelineInput& input); // [NEW] 点光源阴影
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
