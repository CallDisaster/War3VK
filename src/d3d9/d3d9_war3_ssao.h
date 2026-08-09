#pragma once

#include "d3d9_war3_pipeline.h"
#include "war3/render/war3_owned_image_layout.h"
#include "../dxvk/dxvk_hash.h"

#include <unordered_map>

namespace dxvk {

    class D3D9DeviceEx;

    /**
     * @brief War3 SSAO Pass（内置）
     */
    class War3SsaoPass final : public War3RenderPass {
    public:
        explicit War3SsaoPass(D3D9DeviceEx* device);
        ~War3SsaoPass();

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
        VkPipeline getPipeline(const PipelineKey& key);
        VkPipeline createPipeline(const PipelineKey& key) const;

        void ensureResources(VkExtent3D extent, VkFormat colorFormat, VkFormat depthFormat);
        void copyColor(const Rc<DxvkCommandList>& ctx, const Rc<DxvkImageView>& srcView);
        void copyDepth(const Rc<DxvkCommandList>& ctx, const Rc<DxvkImageView>& srcView);
        void drawSsao(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input);

        D3D9DeviceEx* m_parent = nullptr;
        Rc<DxvkDevice> m_device;

        Rc<DxvkSampler> m_linearSampler;
        const DxvkPipelineLayout* m_layout = nullptr;
        std::unordered_map<PipelineKey, VkPipeline, DxvkHash, DxvkEq> m_pipelines;

        Rc<DxvkImage> m_colorCopy;
        Rc<DxvkImageView> m_colorCopyView;
        war3::render::War3OwnedImageLayoutState m_colorCopyLayout;
        VkExtent3D m_cachedExtent = {0, 0, 1};
        VkFormat m_cachedFormat = VK_FORMAT_UNDEFINED;

        Rc<DxvkImage> m_depthCopy;
        Rc<DxvkImageView> m_depthCopyView;
        war3::render::War3OwnedImageLayoutState m_depthCopyLayout;
        VkExtent3D m_cachedDepthExtent = {0, 0, 1};
        VkFormat m_cachedDepthFormat = VK_FORMAT_UNDEFINED;
    };

} // namespace dxvk
