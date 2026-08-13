#pragma once

#include "d3d9_war3_fog_volume.h"
#include "d3d9_war3_light.h"
#include "d3d9_war3_pipeline.h"
#include "../dxvk/dxvk_hash.h"
#include "../dxvk/dxvk_buffer.h"

#include <array>
#include <unordered_map>

namespace dxvk {

    class D3D9DeviceEx;

    /**
     * @brief War3 体积光 Pass（CSM 对齐的世界空间散射）
     */
    class War3VolumetricLightPass final : public War3RenderPass {
    public:
        explicit War3VolumetricLightPass(D3D9DeviceEx* device);
        ~War3VolumetricLightPass();

        War3InsertionPoint Point() const override { return War3InsertionPoint::BeforeUi; }
        void Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) override;

    private:
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

        const DxvkPipelineLayout* createPipelineLayout() const;
        const DxvkPipelineLayout* createCompositePipelineLayout() const;
        const DxvkPipelineLayout* createFroxelInjectLayout() const;
        const DxvkPipelineLayout* createFroxelTemporalLayout() const;
        const DxvkPipelineLayout* createFroxelIntegrateLayout() const;
        VkPipeline getPipeline(const PipelineKey& key);
        VkPipeline getCompositePipeline(const PipelineKey& key);
        VkPipeline createPipeline(const PipelineKey& key) const;
        VkPipeline createCompositePipeline(const PipelineKey& key) const;

        void ensureResources(VkExtent3D extent, VkFormat colorFormat,
                             VkFormat depthFormat, uint32_t resolutionDivisor,
                             bool storageEffect);
        bool ensureFroxelResources(VkExtent3D fullExtent,
                                   War3VolumetricQuality quality);
        void invalidateFroxelHistory();
        void ensurePointShadowFallbackResources(
            const Rc<DxvkCommandList>& ctx);
        void copyColor(const Rc<DxvkCommandList>& ctx, const Rc<DxvkImageView>& srcView);
        void copyDepth(const Rc<DxvkCommandList>& ctx, const Rc<DxvkImageView>& srcView);
        bool drawVolumetricLight(const Rc<DxvkCommandList>& ctx,
                                 const War3PipelineInput& input,
                                 const War3PointLightFrameSnapshot& pointLights,
                                 const War3FogVolumeFrameSnapshot& fogVolumes,
                                 const std::array<uint32_t,
                                     War3PointLightFrameSnapshot::kMaxLights>&
                                     selectedPointIndices,
                                 uint32_t selectedPointCount,
                                 const Vector4& cameraPos,
                                 float farClearRaw, float rawDepthQuantum,
                                 bool farIsOne,
                                 const VkRect2D& effectScissor,
                                 uint32_t& outPointShadowedLightCount);
        bool compositeVolumetricLight(const Rc<DxvkCommandList>& ctx,
                                      const War3PipelineInput& input,
                                      const VkRect2D& compositeScissor);

        D3D9DeviceEx* m_parent = nullptr;
        Rc<DxvkDevice> m_device;

        Rc<DxvkSampler> m_linearSampler;
        const DxvkPipelineLayout* m_layout = nullptr;
        const DxvkPipelineLayout* m_compositeLayout = nullptr;
        const DxvkPipelineLayout* m_froxelInjectLayout = nullptr;
        const DxvkPipelineLayout* m_froxelTemporalLayout = nullptr;
        const DxvkPipelineLayout* m_froxelIntegrateLayout = nullptr;
        VkPipeline m_froxelInjectPipeline = VK_NULL_HANDLE;
        VkPipeline m_froxelTemporalPipeline = VK_NULL_HANDLE;
        VkPipeline m_froxelIntegratePipeline = VK_NULL_HANDLE;
        std::unordered_map<PipelineKey, VkPipeline, DxvkHash, DxvkEq> m_pipelines;
        std::unordered_map<PipelineKey, VkPipeline, DxvkHash, DxvkEq> m_compositePipelines;

        Rc<DxvkImage> m_colorCopy;
        Rc<DxvkImageView> m_colorCopyView;
        VkExtent3D m_cachedExtent = {0, 0, 1};
        VkFormat m_cachedFormat = VK_FORMAT_UNDEFINED;

        Rc<DxvkImage> m_depthCopy;
        Rc<DxvkImageView> m_depthCopyView;
        VkExtent3D m_cachedDepthExtent = {0, 0, 1};
        VkFormat m_cachedDepthFormat = VK_FORMAT_UNDEFINED;

        Rc<DxvkImage> m_effectImage;
        Rc<DxvkImageView> m_effectView;
        Rc<DxvkImageView> m_effectStorageView;
        VkExtent3D m_cachedEffectExtent = {0, 0, 1};
        VkFormat m_cachedEffectFormat = VK_FORMAT_UNDEFINED;
        bool m_effectStorageEnabled = false;

        // Froxel current field and ping-pong stable histories remain in
        // GENERAL for compute read/write. Rc command-list tracking owns every
        // in-flight use; CPU history metadata is invalidated by the exact
        // map/device/frame/grid contract without freeing GPU memory from JASS.
        Rc<DxvkImage> m_froxelCurrentImage;
        Rc<DxvkImageView> m_froxelCurrentView;
        std::array<Rc<DxvkImage>, 2> m_froxelHistoryImages;
        std::array<Rc<DxvkImageView>, 2> m_froxelHistoryViews;
        VkExtent3D m_froxelGridExtent = {0u, 0u, 0u};
        War3VolumetricQuality m_froxelResourceQuality =
            War3VolumetricQuality::LegacyRayMarch;
        uint32_t m_froxelHistoryIndex = 0u;
        bool m_froxelHistoryValid = false;
        uint64_t m_froxelHistoryFrameSerial = 0u;
        uint64_t m_froxelHistoryMapEpoch = 0u;
        uint64_t m_froxelHistoryDeviceEpoch = 0u;
        float m_froxelHistoryNear = 0.0f;
        float m_froxelHistoryFar = 0.0f;
        Matrix4 m_froxelHistoryViewProj = {};
        Vector4 m_froxelHistoryCameraPos = Vector4(0.0f);

        // Legal fail-lit textureCubeArray descriptor for volume-only frames
        // where no exact point-shadow publication is available.
        Rc<DxvkImage> m_pointShadowFallbackCube;
        Rc<DxvkImageView> m_pointShadowFallbackCubeView;
        bool m_pointShadowFallbackReady = false;

        // CSM / 点光 UBO：每帧更新。改为环形缓冲，消除与上一帧 volumetric draw
        // 的跨帧 WAR 依赖（单缓冲时 pre-barrier 需 fragment→transfer 排空）。
        // 槽数必须严格大于 D3D9 MaxFrameLatency(20)，使被复用的槽在 GPU 上早已
        // 退休；按 input.frameSerial 选槽。
        static constexpr uint32_t kUboRingSlots = 24u;
        std::array<Rc<DxvkBuffer>, kUboRingSlots> m_csmUniformBuffers;
        std::array<Rc<DxvkBuffer>, kUboRingSlots> m_lightBuffers;
        std::array<Rc<DxvkBuffer>, kUboRingSlots> m_fogVolumeBuffers;
    };

} // namespace dxvk
