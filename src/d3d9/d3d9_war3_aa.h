#pragma once

#include "d3d9_war3_pipeline.h"
#include "war3/render/war3_owned_image_layout.h"

#include "../dxvk/dxvk_hash.h"

namespace dxvk {

    class D3D9DeviceEx;

    /**
     * @brief War3 Anti-Aliasing Pass
     *
     * 支持两种抗锯齿模式：
     * - FXAA：单 pass，快速但略微模糊
     * - SMAA：三 pass（边缘检测 + 混合权重 + 邻域混合），高质量
     *
     * 插入点：BeforeUi（阴影之后）
     */
    class War3AAPass final : public War3RenderPass {
    public:
        explicit War3AAPass(D3D9DeviceEx* device);
        ~War3AAPass();

        War3InsertionPoint Point() const override { return War3InsertionPoint::BeforeUi; }
        void Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) override;

    private:
        struct PipelineKey {
            VkFormat format = VK_FORMAT_UNDEFINED;
            War3AAMode mode = War3AAMode::None;

            bool eq(const PipelineKey& other) const {
                return format == other.format && mode == other.mode;
            }
            size_t hash() const {
                DxvkHashState h;
                h.add(uint32_t(format));
                h.add(uint32_t(mode));
                return h;
            }
        };

        D3D9DeviceEx* m_parent = nullptr;
        Rc<DxvkDevice> m_device;

        // 中间颜色副本（AA 输入）
        Rc<DxvkImage> m_colorCopy;
        Rc<DxvkImageView> m_colorCopyView;
        war3::render::War3OwnedImageLayoutState m_colorCopyLayout;
        VkExtent3D m_cachedExtent = {0, 0, 1};
        VkFormat m_cachedFormat = VK_FORMAT_UNDEFINED;

        // SMAA 中间缓冲区
        Rc<DxvkImage> m_edgesTex;      // 边缘检测输出 (RG8)
        Rc<DxvkImageView> m_edgesView;
        war3::render::War3OwnedImageLayoutState m_edgesLayout;
        Rc<DxvkImage> m_blendTex;      // 混合权重输出 (RGBA8)
        Rc<DxvkImageView> m_blendView;
        war3::render::War3OwnedImageLayoutState m_blendLayout;

        // SMAA 查找表纹理
        Rc<DxvkImage> m_areaTex;       // 160x560, RG8
        Rc<DxvkImageView> m_areaView;
        war3::render::War3OwnedImageLayoutState m_areaLayout;
        Rc<DxvkImage> m_searchTex;     // 64x16, R8
        Rc<DxvkImageView> m_searchView;
        war3::render::War3OwnedImageLayoutState m_searchLayout;
        bool m_lookupTablesCreated = false;

        // Samplers
        Rc<DxvkSampler> m_linearSampler;
        Rc<DxvkSampler> m_pointSampler;

        // Pipelines
        const DxvkPipelineLayout* m_fxaaLayout = nullptr;
        VkPipeline m_fxaaPipeline = VK_NULL_HANDLE;

        const DxvkPipelineLayout* m_smaaEdgeLayout = nullptr;
        const DxvkPipelineLayout* m_smaaBlendLayout = nullptr;
        const DxvkPipelineLayout* m_smaaNeighborLayout = nullptr;
        VkPipeline m_smaaEdgePipeline = VK_NULL_HANDLE;
        VkPipeline m_smaaBlendPipeline = VK_NULL_HANDLE;
        VkPipeline m_smaaNeighborPipeline = VK_NULL_HANDLE;

        std::unordered_map<PipelineKey, VkPipeline, DxvkHash, DxvkEq> m_pipelineCache;

        void ensureResources(const Rc<DxvkCommandList>& ctx, VkExtent3D extent, VkFormat format);
        void ensureSmaaResources(const Rc<DxvkCommandList>& ctx, VkExtent3D extent);
        void createSmaaLookupTextures(const Rc<DxvkCommandList>& ctx);
        void createSamplers();
        void createFxaaPipeline(VkFormat format);
        void createSmaaPipelines(VkFormat format);

        void copyColorToInput(const Rc<DxvkCommandList>& ctx,
                              const Rc<DxvkImageView>& srcView);

        void runFxaa(const Rc<DxvkCommandList>& ctx,
                     const Rc<DxvkImageView>& dstView,
                     const War3AASettings& settings);

        void runSmaa(const Rc<DxvkCommandList>& ctx,
                     const Rc<DxvkImageView>& dstView,
                     const War3AASettings& settings);
    };

} // namespace dxvk
