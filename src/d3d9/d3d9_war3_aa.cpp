#include "d3d9_war3_aa.h"
#include "d3d9_war3_debug.h"
#include "d3d9_device.h"
#include "war3/tools/war3_perf_monitor.h"

#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_buffer.h"
#include "../dxvk/dxvk_util.h"
#include "../dxvk/dxvk_access.h"

#include <war3_fullscreen_vert.h>
#include <war3_fxaa.h>
#include <war3_smaa_edge.h>
#include <war3_smaa_blend.h>
#include <war3_smaa_neighbor.h>
#include "smaa_area_tex.inl"
#include "smaa_search_tex.inl"

#include <algorithm>
#include <cstring>

namespace dxvk {

    namespace {
        struct FxaaPushConstants {
            uint32_t colorSampler;
            float invWidth;
            float invHeight;
            float qualitySubpix;
            float qualityEdgeThreshold;
            float qualityEdgeThresholdMin;
        };

        struct SmaaEdgePushConstants {
            uint32_t colorSampler;
            float invWidth;
            float invHeight;
            float threshold;
        };

        struct SmaaBlendPushConstants {
            uint32_t edgeSampler;
            uint32_t areaSampler;
            uint32_t searchSampler;
            float invWidth;
            float invHeight;
            int maxSearchSteps;
            int maxSearchStepsDiag;
        };

        struct SmaaNeighborPushConstants {
            uint32_t colorSampler;
            uint32_t blendSampler;
            float invWidth;
            float invHeight;
        };

        bool UploadLookupTexture(const Rc<DxvkCommandList>& ctx,
                                 const Rc<DxvkDevice>& device,
                                 const Rc<DxvkImage>& image,
                                 war3::render::War3OwnedImageLayoutState&
                                     layoutState,
                                 VkExtent3D extent,
                                 const void* data,
                                 size_t dataSize,
                                 const char* debugName) {
            if (!ctx || !device || !image)
                return false;
            if (!data || dataSize == 0)
                return false;

            DxvkBufferCreateInfo bufferInfo = { };
            bufferInfo.size = dataSize;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
            bufferInfo.access = VK_ACCESS_TRANSFER_READ_BIT;
            bufferInfo.debugName = debugName;

            auto uploadBuffer = device->createBuffer(
                bufferInfo,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (!uploadBuffer)
                return false;

            std::memcpy(uploadBuffer->mapPtr(0), data, dataSize);
            auto uploadSlice = uploadBuffer->getSliceInfo();

            const auto writeTransition = layoutState.plan(
                image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const auto subresources = image->getAvailableSubresources();
            VkImageMemoryBarrier2 barrier =
                war3::render::MakeWar3OwnedImageBarrier(
                    writeTransition, image->handle(), subresources);
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depInfo.imageMemoryBarrierCount = 1;
            depInfo.pImageMemoryBarriers = &barrier;
            ctx->cmdPipelineBarrier(DxvkCmdBuffer::InitBuffer, &depInfo);
            war3::render::CommitWar3OwnedImageLayout(
                layoutState, writeTransition, *image, subresources);

            VkBufferImageCopy2 imageRegion = { VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 };
            imageRegion.bufferOffset = uploadSlice.offset;
            imageRegion.imageExtent = extent;
            imageRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            imageRegion.imageSubresource.layerCount = 1u;

            VkCopyBufferToImageInfo2 imageCopy = { VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2 };
            imageCopy.srcBuffer = uploadSlice.buffer;
            imageCopy.dstImage = image->handle();
            imageCopy.dstImageLayout = image->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            imageCopy.regionCount = 1;
            imageCopy.pRegions = &imageRegion;
            ctx->cmdCopyBufferToImage(DxvkCmdBuffer::InitBuffer, &imageCopy);

            const auto readTransition = layoutState.plan(
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
            barrier = war3::render::MakeWar3OwnedImageBarrier(
                readTransition, image->handle(), subresources);
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            depInfo.imageMemoryBarrierCount = 1;
            depInfo.pImageMemoryBarriers = &barrier;
            ctx->cmdPipelineBarrier(DxvkCmdBuffer::InitBuffer, &depInfo);
            war3::render::CommitWar3OwnedImageLayout(
                layoutState, readTransition, *image, subresources);

            ctx->track(image, DxvkAccess::Write);
            return true;
        }
    }

    War3AAPass::War3AAPass(D3D9DeviceEx* device)
        : m_parent(device)
        , m_device(device->GetDXVKDevice()) {

        createSamplers();

        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            WAR3_RENDER_LOG("DXVK War3AAPass: Initialized\n");
        }
    }

    War3AAPass::~War3AAPass() {
        auto vk = m_device->vkd();
        if (m_fxaaPipeline != VK_NULL_HANDLE)
            vk->vkDestroyPipeline(vk->device(), m_fxaaPipeline, nullptr);
        if (m_smaaEdgePipeline != VK_NULL_HANDLE)
            vk->vkDestroyPipeline(vk->device(), m_smaaEdgePipeline, nullptr);
        if (m_smaaBlendPipeline != VK_NULL_HANDLE)
            vk->vkDestroyPipeline(vk->device(), m_smaaBlendPipeline, nullptr);
        if (m_smaaNeighborPipeline != VK_NULL_HANDLE)
            vk->vkDestroyPipeline(vk->device(), m_smaaNeighborPipeline, nullptr);
        for (auto& kv : m_pipelineCache) {
            if (kv.second != VK_NULL_HANDLE)
                vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
        }
    }

    void War3AAPass::createSamplers() {
        DxvkSamplerKey linearKey = { };
        linearKey.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST);
        linearKey.setAddressModes(
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        linearKey.setUsePixelCoordinates(false);
        m_linearSampler = m_device->createSampler(linearKey);

        DxvkSamplerKey pointKey = { };
        pointKey.setFilter(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST);
        pointKey.setAddressModes(
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        pointKey.setUsePixelCoordinates(false);
        m_pointSampler = m_device->createSampler(pointKey);
    }

    void War3AAPass::Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) {
        // [Perf] Add timing scope for AA pass
        auto perfScope = war3::War3PerfMonitor::instance().scope("PostFX/AA", ctx);

        // Debug: Log every call to track execution
        static bool s_firstRunLog = false;
        if (!s_firstRunLog) {
            s_firstRunLog = true;
            WAR3_RENDER_LOG("DXVK War3AAPass::Run() called for first time\n");
        }

        if (!input.settings) {
            static bool s_logNoSettings = false;
            if (!s_logNoSettings) {
                s_logNoSettings = true;
                WAR3_RENDER_LOG("DXVK War3AAPass::Run() early return: no settings\n");
            }
            return;
        }
        
        if (!input.settings->postFx.enabled) {
            static bool s_logDisabled = false;
            if (!s_logDisabled) {
                s_logDisabled = true;
                WAR3_RENDER_LOG("DXVK War3AAPass::Run() early return: postFx disabled\n");
            }
            return;
        }

        const auto& aaSettings = input.settings->postFx.aa;
        
        // Debug: Log AA mode
        static int s_lastLoggedMode = -999;
        if (static_cast<int>(aaSettings.mode) != s_lastLoggedMode) {
            s_lastLoggedMode = static_cast<int>(aaSettings.mode);
            WAR3_RENDER_LOG("DXVK War3AAPass: AA mode=%d\n", s_lastLoggedMode);
        }
        
        if (aaSettings.mode == War3AAMode::None) {
            static bool s_logNone = false;
            if (!s_logNone) {
                s_logNone = true;
                WAR3_RENDER_LOG("DXVK War3AAPass::Run() early return: AA mode=None\n");
            }
            return;
        }

        if (!input.colorView) {
            static bool s_logNoColor = false;
            if (!s_logNoColor) {
                s_logNoColor = true;
                WAR3_RENDER_LOG("DXVK War3AAPass::Run() early return: no colorView\n");
            }
            return;
        }

        auto colorImage = input.colorView->image();
        VkExtent3D extent = colorImage->info().extent;
        VkFormat format = colorImage->info().format;

        // Debug: Log first successful pass
        static bool s_logSuccess = false;
        if (!s_logSuccess) {
            s_logSuccess = true;
            WAR3_RENDER_LOG("DXVK War3AAPass: Starting AA pass %ux%u format=%u mode=%d\n",
                           extent.width, extent.height, static_cast<uint32_t>(format),
                           static_cast<int>(aaSettings.mode));
        }

        // Ensure resources
        ensureResources(ctx, extent, format);

        // Copy current RT to input
        copyColorToInput(ctx, input.colorView);

        // Run selected AA algorithm
        if (aaSettings.mode == War3AAMode::FXAA) {
            runFxaa(ctx, input.colorView, aaSettings);
        } else {
            // SMAA (any quality level)
            runSmaa(ctx, input.colorView, aaSettings);
        }

        // Debug: Log every frame AA execution (only once)
        static bool s_logExec = false;
        if (!s_logExec) {
            s_logExec = true;
            WAR3_RENDER_LOG("DXVK War3AAPass: AA pass executed successfully\n");
        }
    }

    void War3AAPass::ensureResources(const Rc<DxvkCommandList>& ctx, VkExtent3D extent, VkFormat format) {
        if (m_cachedExtent.width == extent.width &&
            m_cachedExtent.height == extent.height &&
            m_cachedFormat == format &&
            m_colorCopy != nullptr) {
            return;
        }

        m_cachedExtent = extent;
        m_cachedFormat = format;

        // Create color copy (input for AA)
        DxvkImageCreateInfo colorInfo = { };
        colorInfo.type = VK_IMAGE_TYPE_2D;
        colorInfo.format = format;
        colorInfo.flags = 0;
        colorInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        colorInfo.extent = extent;
        colorInfo.numLayers = 1;
        colorInfo.mipLevels = 1;
        colorInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        colorInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        colorInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        colorInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        colorInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        m_colorCopy = m_device->createImage(colorInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        m_colorCopyLayout.reset();

        VkComponentMapping mapping = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY };

        DxvkImageViewKey viewKey = { };
        viewKey.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewKey.format = format;
        viewKey.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        viewKey.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        viewKey.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        viewKey.mipIndex = 0;
        viewKey.mipCount = 1;
        viewKey.layerIndex = 0;
        viewKey.layerCount = 1;
        viewKey.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
        m_colorCopyView = m_colorCopy->createView(viewKey);

        // Create FXAA pipeline if needed
        if (m_fxaaPipeline == VK_NULL_HANDLE)
            createFxaaPipeline(format);

        // Create SMAA pipelines and resources if needed
        ensureSmaaResources(ctx, extent);

        WAR3_RENDER_LOG("DXVK War3AAPass: Resources created %ux%u format=%u\n",
                       extent.width, extent.height, static_cast<uint32_t>(format));
    }

    void War3AAPass::ensureSmaaResources(const Rc<DxvkCommandList>& ctx, VkExtent3D extent) {
        // Create lookup textures first time
        if (!m_lookupTablesCreated) {
            createSmaaLookupTextures(ctx);
        }

        VkComponentMapping mapping = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY };

        // Edges texture (RG8)
        if (m_edgesTex == nullptr ||
            m_edgesTex->info().extent.width != extent.width ||
            m_edgesTex->info().extent.height != extent.height) {

            DxvkImageCreateInfo edgesInfo = { };
            edgesInfo.type = VK_IMAGE_TYPE_2D;
            edgesInfo.format = VK_FORMAT_R8G8_UNORM;
            edgesInfo.flags = 0;
            edgesInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
            edgesInfo.extent = extent;
            edgesInfo.numLayers = 1;
            edgesInfo.mipLevels = 1;
            edgesInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            edgesInfo.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            edgesInfo.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            edgesInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            edgesInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            m_edgesTex = m_device->createImage(edgesInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            m_edgesLayout.reset();

            DxvkImageViewKey edgesViewKey = { };
            edgesViewKey.viewType = VK_IMAGE_VIEW_TYPE_2D;
            edgesViewKey.format = VK_FORMAT_R8G8_UNORM;
            edgesViewKey.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            edgesViewKey.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            edgesViewKey.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
            edgesViewKey.mipIndex = 0;
            edgesViewKey.mipCount = 1;
            edgesViewKey.layerIndex = 0;
            edgesViewKey.layerCount = 1;
            edgesViewKey.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
            m_edgesView = m_edgesTex->createView(edgesViewKey);
        }

        // Blend texture (RGBA8)
        if (m_blendTex == nullptr ||
            m_blendTex->info().extent.width != extent.width ||
            m_blendTex->info().extent.height != extent.height) {

            DxvkImageCreateInfo blendInfo = { };
            blendInfo.type = VK_IMAGE_TYPE_2D;
            blendInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            blendInfo.flags = 0;
            blendInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
            blendInfo.extent = extent;
            blendInfo.numLayers = 1;
            blendInfo.mipLevels = 1;
            blendInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            blendInfo.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            blendInfo.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            blendInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            blendInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            m_blendTex = m_device->createImage(blendInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            m_blendLayout.reset();

            DxvkImageViewKey blendViewKey = { };
            blendViewKey.viewType = VK_IMAGE_VIEW_TYPE_2D;
            blendViewKey.format = VK_FORMAT_R8G8B8A8_UNORM;
            blendViewKey.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            blendViewKey.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            blendViewKey.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
            blendViewKey.mipIndex = 0;
            blendViewKey.mipCount = 1;
            blendViewKey.layerIndex = 0;
            blendViewKey.layerCount = 1;
            blendViewKey.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
            m_blendView = m_blendTex->createView(blendViewKey);
        }
    }

    void War3AAPass::createSmaaLookupTextures(const Rc<DxvkCommandList>& ctx) {
        if (!ctx) {
            Logger::err("War3AAPass: SMAA lookup upload skipped (null command list)");
            return;
        }
        VkComponentMapping mapping = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY };

        // Area texture: 160x560, RG8
        {
            DxvkImageCreateInfo areaInfo = { };
            areaInfo.type = VK_IMAGE_TYPE_2D;
            areaInfo.format = VK_FORMAT_R8G8_UNORM;
            areaInfo.flags = 0;
            areaInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
            areaInfo.extent = { 160, 560, 1 };
            areaInfo.numLayers = 1;
            areaInfo.mipLevels = 1;
            areaInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            areaInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            areaInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            areaInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            areaInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            m_areaTex = m_device->createImage(areaInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            m_areaLayout.reset();

            DxvkImageViewKey areaViewKey = { };
            areaViewKey.viewType = VK_IMAGE_VIEW_TYPE_2D;
            areaViewKey.format = VK_FORMAT_R8G8_UNORM;
            areaViewKey.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            areaViewKey.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            areaViewKey.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
            areaViewKey.mipIndex = 0;
            areaViewKey.mipCount = 1;
            areaViewKey.layerIndex = 0;
            areaViewKey.layerCount = 1;
            areaViewKey.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
            m_areaView = m_areaTex->createView(areaViewKey);

            if (!dxvk::smaa::areaTexSize ||
                dxvk::smaa::areaTexWidth != 160 ||
                dxvk::smaa::areaTexHeight != 560) {
                Logger::err("War3AAPass: SMAA area texture size mismatch");
            } else {
                const VkExtent3D areaExtent = { uint32_t(dxvk::smaa::areaTexWidth),
                                                uint32_t(dxvk::smaa::areaTexHeight), 1u };
                if (!UploadLookupTexture(ctx, m_device, m_areaTex, m_areaLayout,
                                         areaExtent,
                                         dxvk::smaa::areaTexBytes,
                                         dxvk::smaa::areaTexSize,
                                         "War3SmaaAreaUpload")) {
                    Logger::err("War3AAPass: SMAA area texture upload failed");
                }
            }
        }

        // Search texture: 64x16, R8
        {
            DxvkImageCreateInfo searchInfo = { };
            searchInfo.type = VK_IMAGE_TYPE_2D;
            searchInfo.format = VK_FORMAT_R8_UNORM;
            searchInfo.flags = 0;
            searchInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
            searchInfo.extent = { 64, 16, 1 };
            searchInfo.numLayers = 1;
            searchInfo.mipLevels = 1;
            searchInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            searchInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            searchInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            searchInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            searchInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            m_searchTex = m_device->createImage(searchInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            m_searchLayout.reset();

            DxvkImageViewKey searchViewKey = { };
            searchViewKey.viewType = VK_IMAGE_VIEW_TYPE_2D;
            searchViewKey.format = VK_FORMAT_R8_UNORM;
            searchViewKey.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            searchViewKey.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            searchViewKey.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
            searchViewKey.mipIndex = 0;
            searchViewKey.mipCount = 1;
            searchViewKey.layerIndex = 0;
            searchViewKey.layerCount = 1;
            searchViewKey.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
            m_searchView = m_searchTex->createView(searchViewKey);

            if (!dxvk::smaa::searchTexSize ||
                dxvk::smaa::searchTexWidth != 64 ||
                dxvk::smaa::searchTexHeight != 16) {
                Logger::err("War3AAPass: SMAA search texture size mismatch");
            } else {
                const VkExtent3D searchExtent = { uint32_t(dxvk::smaa::searchTexWidth),
                                                  uint32_t(dxvk::smaa::searchTexHeight), 1u };
                if (!UploadLookupTexture(ctx, m_device, m_searchTex,
                                         m_searchLayout, searchExtent,
                                         dxvk::smaa::searchTexBytes,
                                         dxvk::smaa::searchTexSize,
                                         "War3SmaaSearchUpload")) {
                    Logger::err("War3AAPass: SMAA search texture upload failed");
                }
            }
        }

        m_lookupTablesCreated = true;
        WAR3_RENDER_LOG("DXVK War3AAPass: SMAA lookup textures created\n");
    }

    void War3AAPass::createFxaaPipeline(VkFormat format) {
        // Create pipeline layout
        std::array<DxvkDescriptorSetLayoutBinding, 1> fxaaBindings = {
            DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
        };

        m_fxaaLayout = m_device->createBuiltInPipelineLayout(
            DxvkPipelineLayoutFlag::UsesSamplerHeap,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            sizeof(FxaaPushConstants),
            fxaaBindings.size(), fxaaBindings.data());

        util::DxvkBuiltInGraphicsState state = { };
        state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
        state.fs = util::DxvkBuiltInShaderStage(war3_fxaa, nullptr);
        state.colorFormat = format;
        state.sampleCount = VK_SAMPLE_COUNT_1_BIT;

        m_fxaaPipeline = m_device->createBuiltInGraphicsPipeline(m_fxaaLayout, state);

        WAR3_RENDER_LOG("DXVK War3AAPass: FXAA pipeline created\n");
    }

    void War3AAPass::createSmaaPipelines(VkFormat format) {
        // Edge detection layout
        {
            std::array<DxvkDescriptorSetLayoutBinding, 1> edgeBindings = {
                DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
            };

            m_smaaEdgeLayout = m_device->createBuiltInPipelineLayout(
                DxvkPipelineLayoutFlag::UsesSamplerHeap,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                sizeof(SmaaEdgePushConstants),
                edgeBindings.size(), edgeBindings.data());

            util::DxvkBuiltInGraphicsState state = { };
            state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
            state.fs = util::DxvkBuiltInShaderStage(war3_smaa_edge, nullptr);
            state.colorFormat = VK_FORMAT_R8G8_UNORM;
            state.sampleCount = VK_SAMPLE_COUNT_1_BIT;

            m_smaaEdgePipeline = m_device->createBuiltInGraphicsPipeline(m_smaaEdgeLayout, state);
        }

        // Blend weight layout
        {
            std::array<DxvkDescriptorSetLayoutBinding, 3> blendBindings = {
                DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
                DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
                DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
            };

            m_smaaBlendLayout = m_device->createBuiltInPipelineLayout(
                DxvkPipelineLayoutFlag::UsesSamplerHeap,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                sizeof(SmaaBlendPushConstants),
                blendBindings.size(), blendBindings.data());

            util::DxvkBuiltInGraphicsState state = { };
            state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
            state.fs = util::DxvkBuiltInShaderStage(war3_smaa_blend, nullptr);
            state.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
            state.sampleCount = VK_SAMPLE_COUNT_1_BIT;

            m_smaaBlendPipeline = m_device->createBuiltInGraphicsPipeline(m_smaaBlendLayout, state);
        }

        // Neighbor blending layout
        {
            std::array<DxvkDescriptorSetLayoutBinding, 2> neighborBindings = {
                DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
                DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
            };

            m_smaaNeighborLayout = m_device->createBuiltInPipelineLayout(
                DxvkPipelineLayoutFlag::UsesSamplerHeap,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                sizeof(SmaaNeighborPushConstants),
                neighborBindings.size(), neighborBindings.data());

            util::DxvkBuiltInGraphicsState state = { };
            state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
            state.fs = util::DxvkBuiltInShaderStage(war3_smaa_neighbor, nullptr);
            state.colorFormat = format;
            state.sampleCount = VK_SAMPLE_COUNT_1_BIT;

            m_smaaNeighborPipeline = m_device->createBuiltInGraphicsPipeline(m_smaaNeighborLayout, state);
        }

        WAR3_RENDER_LOG("DXVK War3AAPass: SMAA pipelines created\n");
    }

    void War3AAPass::copyColorToInput(const Rc<DxvkCommandList>& ctx,
                                       const Rc<DxvkImageView>& srcView) {
        if (!m_colorCopy || !m_colorCopyView || !srcView)
            return;

        const VkImageSubresourceRange srcSubresources =
            srcView->imageSubresources();
        const VkImageLayout srcLayout =
            srcView->image()->queryLayout(srcSubresources);

        // Transition src to TRANSFER_SRC, dst copy to TRANSFER_DST
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
        barriers[0].subresourceRange = srcSubresources;

        const VkImageSubresourceRange dstSubresources =
            m_colorCopyView->imageSubresources();
        const auto dstWriteTransition = m_colorCopyLayout.plan(
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        barriers[1] = war3::render::MakeWar3OwnedImageBarrier(
            dstWriteTransition, m_colorCopy->handle(), dstSubresources);

        VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        depInfo.imageMemoryBarrierCount = 2;
        depInfo.pImageMemoryBarriers = barriers;
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
        war3::render::CommitWar3OwnedImageLayout(
            m_colorCopyLayout, dstWriteTransition, *m_colorCopy,
            dstSubresources);

        VkExtent3D srcExtent = srcView->image()->info().extent;
        VkExtent3D dstExtent = m_colorCopy->info().extent;
        VkExtent3D copyExtent = {
            std::min(srcExtent.width, dstExtent.width),
            std::min(srcExtent.height, dstExtent.height),
            1u
        };

        VkImageCopy2 copyRegion = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent = copyExtent;

        VkCopyImageInfo2 copyInfo = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
        copyInfo.srcImage = srcView->image()->handle();
        copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copyInfo.dstImage = m_colorCopy->handle();
        copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copyInfo.regionCount = 1;
        copyInfo.pRegions = &copyRegion;

        ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copyInfo);

        // Transition back: src to COLOR_ATTACHMENT_OPTIMAL, copy to SHADER_READ_ONLY
        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout     = srcLayout;

        const auto dstReadTransition = m_colorCopyLayout.plan(
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);
        barriers[1] = war3::render::MakeWar3OwnedImageBarrier(
            dstReadTransition, m_colorCopy->handle(), dstSubresources);

        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
        war3::render::CommitWar3OwnedImageLayout(
            m_colorCopyLayout, dstReadTransition, *m_colorCopy,
            dstSubresources);

        ctx->track(srcView->image(), DxvkAccess::Read);
        ctx->track(m_colorCopy, DxvkAccess::Write);
    }

    void War3AAPass::runFxaa(const Rc<DxvkCommandList>& ctx,
                              const Rc<DxvkImageView>& dstView,
                              const War3AASettings& settings) {
        if (m_fxaaPipeline == VK_NULL_HANDLE)
            return;

        // Use the destination view's extent, not cached
        VkExtent3D extent = dstView->image()->info().extent;

        // Transition destination to COLOR_ATTACHMENT_OPTIMAL
        VkImageMemoryBarrier2 dstToAttach = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        dstToAttach.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstToAttach.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        dstToAttach.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstToAttach.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        const auto dstSubresources = dstView->imageSubresources();
        const VkImageLayout dstOriginalLayout =
            dstView->image()->queryLayout(dstSubresources);
        dstToAttach.oldLayout = dstOriginalLayout;
        dstToAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        dstToAttach.image = dstView->image()->handle();
        dstToAttach.subresourceRange = dstSubresources;

        VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers = &dstToAttach;
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

        // Begin rendering to destination
        VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        colorAttachment.imageView = dstView->handle();
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        renderInfo.renderArea.offset = { 0, 0 };
        renderInfo.renderArea.extent = { extent.width, extent.height };
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttachment;

        ctx->cmdBeginRendering(&renderInfo);

        // [Restore] Bind pipeline and set viewport for FXAA
        ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipeline);
        
        VkViewport viewport = { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
        VkRect2D scissor = { { 0, 0 }, { extent.width, extent.height } };
        ctx->cmdSetViewport(1, &viewport);
        ctx->cmdSetScissor(1, &scissor);

        std::array<DxvkDescriptorWrite, 1> descriptors = { };
        descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[0].descriptor = m_colorCopyView->getDescriptor();

        FxaaPushConstants pc = { };
        pc.colorSampler = m_linearSampler->getDescriptor().samplerIndex;
        pc.invWidth = 1.0f / float(extent.width);
        pc.invHeight = 1.0f / float(extent.height);
        pc.qualitySubpix = settings.fxaaQualitySubpix;
        pc.qualityEdgeThreshold = settings.fxaaQualityEdgeThreshold;
        pc.qualityEdgeThresholdMin = settings.fxaaQualityEdgeThresholdMin;

        ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
                           m_fxaaLayout,
                           descriptors.size(),
                           descriptors.data(),
                           sizeof(pc), &pc);

        // Draw fullscreen triangle
        ctx->cmdDraw(3, 1, 0, 0);

        ctx->cmdEndRendering();

        // Transition destination back to original layout
        VkImageMemoryBarrier2 dstToOriginal = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        dstToOriginal.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstToOriginal.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        dstToOriginal.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstToOriginal.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        dstToOriginal.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        dstToOriginal.newLayout = dstOriginalLayout;
        dstToOriginal.image = dstView->image()->handle();
        dstToOriginal.subresourceRange = dstSubresources;

        VkDependencyInfo depInfo2 = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        depInfo2.imageMemoryBarrierCount = 1;
        depInfo2.pImageMemoryBarriers = &dstToOriginal;
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo2);

        ctx->track(dstView->image(), DxvkAccess::Write);
        ctx->track(m_colorCopy, DxvkAccess::Read);
        ctx->track(m_linearSampler);
    }

    void War3AAPass::runSmaa(const Rc<DxvkCommandList>& ctx,
                              const Rc<DxvkImageView>& dstView,
                              const War3AASettings& settings) {
        // Create pipelines lazily
        if (m_smaaEdgePipeline == VK_NULL_HANDLE)
            createSmaaPipelines(m_cachedFormat);

        VkExtent3D extent = dstView->image()->info().extent;
        const auto dstSubresources = dstView->imageSubresources();
        const VkImageLayout dstOriginalLayout =
            dstView->image()->queryLayout(dstSubresources);

        // 允许 ImGui 参数实时生效，避免被固定档位覆盖
        float threshold = std::clamp(settings.smaaThreshold, 0.01f, 0.5f);
        int maxSearchSteps = std::clamp(settings.smaaMaxSearchSteps, 4, 64);
        int maxSearchStepsDiag = std::clamp(settings.smaaMaxSearchStepsDiag, 0, 32);

        // Transition destination to COLOR_ATTACHMENT_OPTIMAL (for pass 3 output)
        VkImageMemoryBarrier2 dstToAttach = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        dstToAttach.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstToAttach.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        dstToAttach.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstToAttach.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        dstToAttach.oldLayout = dstOriginalLayout;
        dstToAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        dstToAttach.image = dstView->image()->handle();
        dstToAttach.subresourceRange = dstView->imageSubresources();

        VkDependencyInfo dstDepInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dstDepInfo.imageMemoryBarrierCount = 1;
        dstDepInfo.pImageMemoryBarriers = &dstToAttach;
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dstDepInfo);

        // Pass 1: Edge Detection
        {
            // Transition edges texture to attachment
            const auto subresources = m_edgesView->imageSubresources();
            const auto attachTransition = m_edgesLayout.plan(
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            VkImageMemoryBarrier2 edgesToAttach =
                war3::render::MakeWar3OwnedImageBarrier(
                    attachTransition, m_edgesTex->handle(), subresources);

            VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depInfo.imageMemoryBarrierCount = 1;
            depInfo.pImageMemoryBarriers = &edgesToAttach;
            ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
            war3::render::CommitWar3OwnedImageLayout(
                m_edgesLayout, attachTransition, *m_edgesTex, subresources);

            VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            colorAttachment.imageView = m_edgesView->handle();
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };

            VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
            renderInfo.renderArea.offset = { 0, 0 };
            renderInfo.renderArea.extent = { extent.width, extent.height };
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = 1;
            renderInfo.pColorAttachments = &colorAttachment;

            ctx->cmdBeginRendering(&renderInfo);
            ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_smaaEdgePipeline);

            VkViewport viewport = { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
            VkRect2D scissor = { { 0, 0 }, { extent.width, extent.height } };
            ctx->cmdSetViewport(1, &viewport);
            ctx->cmdSetScissor(1, &scissor);

            std::array<DxvkDescriptorWrite, 1> descriptors = { };
            descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            descriptors[0].descriptor = m_colorCopyView->getDescriptor();

            SmaaEdgePushConstants pc = { };
            pc.colorSampler = m_pointSampler->getDescriptor().samplerIndex;
            pc.invWidth = 1.0f / float(extent.width);
            pc.invHeight = 1.0f / float(extent.height);
            pc.threshold = threshold;

            ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
                               m_smaaEdgeLayout,
                               descriptors.size(),
                               descriptors.data(),
                               sizeof(pc), &pc);
            ctx->cmdDraw(3, 1, 0, 0);
            ctx->cmdEndRendering();

            // Transition edges to readable
            const auto readTransition = m_edgesLayout.plan(
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
            VkImageMemoryBarrier2 edgesToRead =
                war3::render::MakeWar3OwnedImageBarrier(
                    readTransition, m_edgesTex->handle(), subresources);

            VkDependencyInfo depInfo2 = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depInfo2.imageMemoryBarrierCount = 1;
            depInfo2.pImageMemoryBarriers = &edgesToRead;
            ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo2);
            war3::render::CommitWar3OwnedImageLayout(
                m_edgesLayout, readTransition, *m_edgesTex, subresources);
        }

        // Pass 2: Blend Weight Calculation
        {
            const auto subresources = m_blendView->imageSubresources();
            const auto attachTransition = m_blendLayout.plan(
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            VkImageMemoryBarrier2 blendToAttach =
                war3::render::MakeWar3OwnedImageBarrier(
                    attachTransition, m_blendTex->handle(), subresources);

            VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depInfo.imageMemoryBarrierCount = 1;
            depInfo.pImageMemoryBarriers = &blendToAttach;
            ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
            war3::render::CommitWar3OwnedImageLayout(
                m_blendLayout, attachTransition, *m_blendTex, subresources);

            VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            colorAttachment.imageView = m_blendView->handle();
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };

            VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
            renderInfo.renderArea.offset = { 0, 0 };
            renderInfo.renderArea.extent = { extent.width, extent.height };
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = 1;
            renderInfo.pColorAttachments = &colorAttachment;

            ctx->cmdBeginRendering(&renderInfo);
            ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_smaaBlendPipeline);

            VkViewport viewport = { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
            VkRect2D scissor = { { 0, 0 }, { extent.width, extent.height } };
            ctx->cmdSetViewport(1, &viewport);
            ctx->cmdSetScissor(1, &scissor);

            std::array<DxvkDescriptorWrite, 3> descriptors = { };
            descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            descriptors[0].descriptor = m_edgesView->getDescriptor();
            descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            descriptors[1].descriptor = m_areaView->getDescriptor();
            descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            descriptors[2].descriptor = m_searchView->getDescriptor();

            SmaaBlendPushConstants pc = { };
            // 官方 SMAA 的 blend/search 阶段对边缘贴图使用 level-zero 采样；
            // 这里改回 linear，避免搜索链因为过于硬性的 nearest 命中而断续。
            pc.edgeSampler = m_linearSampler->getDescriptor().samplerIndex;
            pc.areaSampler = m_linearSampler->getDescriptor().samplerIndex;
            pc.searchSampler = m_linearSampler->getDescriptor().samplerIndex;
            pc.invWidth = 1.0f / float(extent.width);
            pc.invHeight = 1.0f / float(extent.height);
            pc.maxSearchSteps = maxSearchSteps;
            pc.maxSearchStepsDiag = maxSearchStepsDiag;

            ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
                               m_smaaBlendLayout,
                               descriptors.size(),
                               descriptors.data(),
                               sizeof(pc), &pc);
            ctx->cmdDraw(3, 1, 0, 0);
            ctx->cmdEndRendering();

            // Transition blend to readable
            const auto readTransition = m_blendLayout.plan(
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
            VkImageMemoryBarrier2 blendToRead =
                war3::render::MakeWar3OwnedImageBarrier(
                    readTransition, m_blendTex->handle(), subresources);

            VkDependencyInfo depInfo2 = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            depInfo2.imageMemoryBarrierCount = 1;
            depInfo2.pImageMemoryBarriers = &blendToRead;
            ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo2);
            war3::render::CommitWar3OwnedImageLayout(
                m_blendLayout, readTransition, *m_blendTex, subresources);
        }

        // Pass 3: Neighborhood Blending
        {
            VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            colorAttachment.imageView = dstView->handle();
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
            renderInfo.renderArea.offset = { 0, 0 };
            renderInfo.renderArea.extent = { extent.width, extent.height };
            renderInfo.layerCount = 1;
            renderInfo.colorAttachmentCount = 1;
            renderInfo.pColorAttachments = &colorAttachment;

            ctx->cmdBeginRendering(&renderInfo);
            ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_smaaNeighborPipeline);

            VkViewport viewport = { 0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
            VkRect2D scissor = { { 0, 0 }, { extent.width, extent.height } };
            ctx->cmdSetViewport(1, &viewport);
            ctx->cmdSetScissor(1, &scissor);

            std::array<DxvkDescriptorWrite, 2> descriptors = { };
            descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            descriptors[0].descriptor = m_colorCopyView->getDescriptor();
            descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            descriptors[1].descriptor = m_blendView->getDescriptor();

            SmaaNeighborPushConstants pc = { };
            // 最后一遍颜色保持 linear；但权重贴图改回 point，避免相邻像素的权重
            // 在我们当前这条链上互相串扰，表现成“只有一条细线”的伪影。
            pc.colorSampler = m_linearSampler->getDescriptor().samplerIndex;
            pc.blendSampler = m_pointSampler->getDescriptor().samplerIndex;
            pc.invWidth = 1.0f / float(extent.width);
            pc.invHeight = 1.0f / float(extent.height);

            ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
                               m_smaaNeighborLayout,
                               descriptors.size(),
                               descriptors.data(),
                               sizeof(pc), &pc);
            ctx->cmdDraw(3, 1, 0, 0);
            ctx->cmdEndRendering();
        }

        // Transition destination back to original layout
        VkImageMemoryBarrier2 dstToOriginal = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        dstToOriginal.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstToOriginal.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        dstToOriginal.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstToOriginal.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        dstToOriginal.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        dstToOriginal.newLayout = dstOriginalLayout;
        dstToOriginal.image = dstView->image()->handle();
        dstToOriginal.subresourceRange = dstView->imageSubresources();

        VkDependencyInfo dstDepInfo2 = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dstDepInfo2.imageMemoryBarrierCount = 1;
        dstDepInfo2.pImageMemoryBarriers = &dstToOriginal;
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dstDepInfo2);

        ctx->track(dstView->image(), DxvkAccess::Write);
        ctx->track(m_colorCopy, DxvkAccess::Read);
        ctx->track(m_edgesTex, DxvkAccess::Read);
        ctx->track(m_blendTex, DxvkAccess::Read);
        ctx->track(m_areaTex, DxvkAccess::Read);
        ctx->track(m_searchTex, DxvkAccess::Read);
        ctx->track(m_linearSampler);
        ctx->track(m_pointSampler);
    }

} // namespace dxvk
