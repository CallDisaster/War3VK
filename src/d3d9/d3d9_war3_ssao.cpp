#include "d3d9_war3_ssao.h"
#include "d3d9_war3_debug.h"
#include "d3d9_device.h"

#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_util.h"
#include "../dxvk/dxvk_access.h"

#include <war3_fullscreen_vert.h>
#include <war3_ssao.h>

#include <algorithm>
#include <cstring>

namespace dxvk {

    namespace {
        struct SsaoPushConstants {
            uint32_t colorSampler;
            uint32_t depthSampler;
            float radiusPx;
            float strength;
            float bias;
            float power;
            Matrix4 invViewProj;
            Vector4 viewport;
            Vector4 viewportZ;
            Vector4 fade; // x=fadeNear, y=fadeFar
        };

        VkImageSubresourceLayers toLayers(const VkImageSubresourceRange& range) {
            VkImageSubresourceLayers layers = { };
            layers.aspectMask = range.aspectMask;
            layers.mipLevel = range.baseMipLevel;
            layers.baseArrayLayer = range.baseArrayLayer;
            layers.layerCount = range.layerCount;
            return layers;
        }
    }

    War3SsaoPass::War3SsaoPass(D3D9DeviceEx* device)
        : m_parent(device)
        , m_device(device->GetDXVKDevice()) {
        DxvkSamplerKey linearKey = { };
        linearKey.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST);
        linearKey.setAddressModes(
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        linearKey.setUsePixelCoordinates(false);
        m_linearSampler = m_device->createSampler(linearKey);

        m_layout = createPipelineLayout();
    }

    War3SsaoPass::~War3SsaoPass() {
        auto vk = m_device->vkd();
        for (auto& kv : m_pipelines) {
            if (kv.second != VK_NULL_HANDLE)
                vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
        }
    }

    const DxvkPipelineLayout* War3SsaoPass::createPipelineLayout() const {
        std::array<DxvkDescriptorSetLayoutBinding, 2> bindings = {
            DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
            DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
        };
        return m_device->createBuiltInPipelineLayout(
            DxvkPipelineLayoutFlag::UsesSamplerHeap,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            sizeof(SsaoPushConstants),
            bindings.size(),
            bindings.data());
    }

    VkPipeline War3SsaoPass::getPipeline(const PipelineKey& key) {
        auto it = m_pipelines.find(key);
        if (it != m_pipelines.end())
            return it->second;

        VkPipeline pipeline = createPipeline(key);
        m_pipelines.insert({ key, pipeline });
        return pipeline;
    }

    VkPipeline War3SsaoPass::createPipeline(const PipelineKey& key) const {
        util::DxvkBuiltInGraphicsState state = { };
        state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
        state.fs = util::DxvkBuiltInShaderStage(war3_ssao, nullptr);
        state.colorFormat = key.format;
        state.sampleCount = key.samples;

        return m_device->createBuiltInGraphicsPipeline(m_layout, state);
    }

    void War3SsaoPass::ensureResources(VkExtent3D extent, VkFormat colorFormat, VkFormat depthFormat) {
        if (!m_colorCopy || m_cachedExtent.width != extent.width ||
            m_cachedExtent.height != extent.height || m_cachedFormat != colorFormat) {
            m_cachedExtent = extent;
            m_cachedFormat = colorFormat;

            DxvkImageCreateInfo info = { };
            info.type = VK_IMAGE_TYPE_2D;
            info.format = colorFormat;
            info.flags = 0;
            info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
            info.extent = extent;
            info.numLayers = 1;
            info.mipLevels = 1;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                        | VK_PIPELINE_STAGE_TRANSFER_BIT;
            info.access = VK_ACCESS_SHADER_READ_BIT
                        | VK_ACCESS_TRANSFER_READ_BIT
                        | VK_ACCESS_TRANSFER_WRITE_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            m_colorCopy = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            DxvkImageViewKey viewInfo;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            viewInfo.format = colorFormat;
            viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            viewInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.mipIndex = 0;
            viewInfo.mipCount = 1;
            viewInfo.layerIndex = 0;
            viewInfo.layerCount = 1;
            VkComponentMapping mapping = {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY };
            viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);

            m_colorCopyView = m_colorCopy->createView(viewInfo);
        }

        if (!m_depthCopy || m_cachedDepthExtent.width != extent.width ||
            m_cachedDepthExtent.height != extent.height || m_cachedDepthFormat != depthFormat) {
            m_cachedDepthExtent = extent;
            m_cachedDepthFormat = depthFormat;

            DxvkImageCreateInfo info = { };
            info.type = VK_IMAGE_TYPE_2D;
            info.format = depthFormat;
            info.flags = 0;
            info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
            info.extent = extent;
            info.numLayers = 1;
            info.mipLevels = 1;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                        | VK_PIPELINE_STAGE_TRANSFER_BIT;
            info.access = VK_ACCESS_SHADER_READ_BIT
                        | VK_ACCESS_TRANSFER_READ_BIT
                        | VK_ACCESS_TRANSFER_WRITE_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

            m_depthCopy = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            DxvkImageViewKey viewInfo;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            viewInfo.format = depthFormat;
            viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            viewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            viewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
            viewInfo.mipIndex = 0;
            viewInfo.mipCount = 1;
            viewInfo.layerIndex = 0;
            viewInfo.layerCount = 1;
            VkComponentMapping mapping = {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY };
            viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);

            m_depthCopyView = m_depthCopy->createView(viewInfo);
        }
    }

    void War3SsaoPass::copyColor(const Rc<DxvkCommandList>& ctx, const Rc<DxvkImageView>& srcView) {
        if (!m_colorCopy || !m_colorCopyView)
            return;

        const VkImageLayout srcLayout = srcView->getLayout();

        VkImageMemoryBarrier2 barriers[2] = { };
        for (auto& b : barriers)
            b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };

        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[0].oldLayout     = srcLayout;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image         = srcView->image()->handle();
        barriers[0].subresourceRange = srcView->imageSubresources();

        barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image         = m_colorCopy->handle();
        barriers[1].subresourceRange = m_colorCopyView->imageSubresources();

        VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        depInfo.imageMemoryBarrierCount = 2;
        depInfo.pImageMemoryBarriers = barriers;
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

        VkImageCopy2 copyRegion = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
        copyRegion.srcSubresource = toLayers(srcView->imageSubresources());
        copyRegion.dstSubresource = toLayers(m_colorCopyView->imageSubresources());
        copyRegion.extent = srcView->image()->info().extent;

        VkCopyImageInfo2 copyInfo = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
        copyInfo.srcImage = srcView->image()->handle();
        copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copyInfo.dstImage = m_colorCopy->handle();
        copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copyInfo.regionCount = 1;
        copyInfo.pRegions = &copyRegion;

        ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copyInfo);

        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout     = srcLayout;

        barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

        ctx->track(srcView->image(), DxvkAccess::Read);
        ctx->track(m_colorCopy, DxvkAccess::Write);
    }

    void War3SsaoPass::copyDepth(const Rc<DxvkCommandList>& ctx, const Rc<DxvkImageView>& srcView) {
        if (!m_depthCopy || !m_depthCopyView)
            return;

        const VkImageLayout srcLayout = srcView->getLayout();

        VkImageMemoryBarrier2 barriers[2] = { };
        for (auto& b : barriers)
            b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };

        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[0].oldLayout     = srcLayout;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image         = srcView->image()->handle();
        barriers[0].subresourceRange = srcView->imageSubresources();

        barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image         = m_depthCopy->handle();
        barriers[1].subresourceRange = m_depthCopyView->imageSubresources();

        VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        depInfo.imageMemoryBarrierCount = 2;
        depInfo.pImageMemoryBarriers = barriers;
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

        VkImageCopy2 copyRegion = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
        copyRegion.srcSubresource = toLayers(srcView->imageSubresources());
        copyRegion.dstSubresource = toLayers(m_depthCopyView->imageSubresources());
        copyRegion.extent = srcView->image()->info().extent;

        VkCopyImageInfo2 copyInfo = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
        copyInfo.srcImage = srcView->image()->handle();
        copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copyInfo.dstImage = m_depthCopy->handle();
        copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copyInfo.regionCount = 1;
        copyInfo.pRegions = &copyRegion;

        ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copyInfo);

        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout     = srcLayout;

        barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

        ctx->track(srcView->image(), DxvkAccess::Read);
        ctx->track(m_depthCopy, DxvkAccess::Write);
    }

    void War3SsaoPass::drawSsao(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) {
        if (!m_colorCopyView || !m_depthCopyView || !m_layout)
            return;
        if (!m_linearSampler)
            return;
        if (!input.scene.worldCamera.valid)
            return;

        const auto& settings = input.settings->postFx.ssao;
        auto colorImage = input.colorView->image();

        PipelineKey key = { };
        key.format = colorImage->info().format;
        key.samples = colorImage->info().sampleCount;

        VkPipeline pipeline = getPipeline(key);
        if (pipeline == VK_NULL_HANDLE)
            return;

        VkExtent3D extent = colorImage->info().extent;

        VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        attachment.imageView = input.colorView->handle();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        renderInfo.renderArea.offset = { 0u, 0u };
        renderInfo.renderArea.extent = { extent.width, extent.height };
        renderInfo.layerCount = 1u;
        renderInfo.colorAttachmentCount = 1u;
        renderInfo.pColorAttachments = &attachment;

        ctx->cmdBeginRendering(&renderInfo);

        VkViewport viewport = { };
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = float(extent.width);
        viewport.height = float(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = { };
        scissor.offset = { 0, 0 };
        scissor.extent = { extent.width, extent.height };

        ctx->cmdSetViewport(1, &viewport);
        ctx->cmdSetScissor(1, &scissor);

        std::array<DxvkDescriptorWrite, 2> descriptors = { };
        descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[0].descriptor = m_colorCopyView->getDescriptor();
        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = m_depthCopyView->getDescriptor();

        SsaoPushConstants pc = { };
        pc.colorSampler = m_linearSampler->getDescriptor().samplerIndex;
        pc.depthSampler = m_linearSampler->getDescriptor().samplerIndex;
        pc.radiusPx = settings.radiusPx;
        pc.strength = settings.strength;
        pc.bias = settings.bias;
        pc.power = settings.power;
        pc.invViewProj = input.scene.worldCamera.invViewProj;
        pc.viewport = Vector4(
            float(input.scene.worldCamera.viewport.X),
            float(input.scene.worldCamera.viewport.Y),
            float(input.scene.worldCamera.viewport.Width),
            float(input.scene.worldCamera.viewport.Height));
        pc.viewportZ = Vector4(
            input.scene.worldCamera.viewport.MinZ,
            input.scene.worldCamera.viewport.MaxZ,
            0.0f,
            0.0f);
        const float fadeNear = std::clamp(settings.fadeNear, 0.0f, 0.95f);
        const float fadeFar = std::clamp(settings.fadeFar, fadeNear + 0.01f, 1.0f);
        pc.fade = Vector4(fadeNear, fadeFar, 0.0f, 0.0f);

        ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                             VK_PIPELINE_BIND_POINT_GRAPHICS,
                             pipeline);

        ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
                           m_layout,
                           descriptors.size(),
                           descriptors.data(),
                           sizeof(pc),
                           &pc);

        ctx->cmdDraw(3, 1, 0, 0);

        ctx->cmdEndRendering();

        ctx->track(input.colorView->image(), DxvkAccess::Write);
        ctx->track(m_colorCopy, DxvkAccess::Read);
        ctx->track(m_depthCopy, DxvkAccess::Read);
        ctx->track(m_linearSampler);
    }

    void War3SsaoPass::Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) {
        if (!input.settings)
            return;
        if (!input.settings->postFx.enabled || !input.settings->postFx.ssao.enabled)
            return;
        if (!input.colorView || !input.depthView)
            return;

        VkExtent3D extent = input.colorView->image()->info().extent;
        ensureResources(extent,
                        input.colorView->image()->info().format,
                        input.depthView->image()->info().format);

        copyColor(ctx, input.colorView);
        copyDepth(ctx, input.depthView);
        drawSsao(ctx, input);
    }

} // namespace dxvk
