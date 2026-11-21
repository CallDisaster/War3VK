#include "war3_aa_executor.h"

#include "../../../dxvk/dxvk_access.h"
#include "../../../dxvk/dxvk_buffer.h"
#include "../../../dxvk/dxvk_util.h"
#include "../../../util/util_error.h"

#include <war3_fullscreen_vert.h>
#include <war3_fxaa.h>
#include <war3_smaa_edge.h>
#include <war3_smaa_blend.h>
#include <war3_smaa_neighbor.h>
#include "../../smaa_area_tex.inl"
#include "../../smaa_search_tex.inl"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace dxvk::war3::effects {
namespace {

// Shader ABIs (war3_fxaa.frag, war3_smaa_*.frag push_block).
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

const VkComponentMapping kIdentity = {
  VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
  VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };

Rc<DxvkImage> CreateImage2D(const Rc<DxvkDevice>& device, VkExtent3D extent,
                            VkFormat format, bool attachment, const char* name) {
  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = extent;
  info.numLayers = 1;
  info.mipLevels = 1;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
      (attachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  info.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      (attachment ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT);
  info.access = VK_ACCESS_SHADER_READ_BIT |
      (attachment ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT);
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  info.debugName = name;
  return device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

Rc<DxvkImageView> CreateView2D(const Rc<DxvkImage>& image, bool attachment) {
  if (!image)
    return nullptr;
  DxvkImageViewKey key = {};
  key.viewType = VK_IMAGE_VIEW_TYPE_2D;
  key.format = image->info().format;
  key.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
      (attachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
  key.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  key.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
  key.mipIndex = 0;
  key.mipCount = 1;
  key.layerIndex = 0;
  key.layerCount = 1;
  key.packedSwizzle = DxvkImageViewKey::packSwizzle(kIdentity);
  return image->createView(key);
}

uint64_t ImageBytes(const Rc<DxvkImage>& image) {
  return image && image->storage() ? image->storage()->getMemoryInfo().size : 0u;
}

// Adopts a new raw pipeline handle so that command lists own it. Only this
// unadopted candidate is ever destroyed here.
bool AdoptPipeline(const Rc<DxvkDevice>& device, VkPipeline candidate,
                   Rc<render::War3TrackedVkPipeline>& out) {
  if (candidate == VK_NULL_HANDLE)
    return false;
  Rc<render::War3TrackedVkPipeline> owner;
  try {
    owner = render::AdoptWar3TrackedVkPipeline(device, candidate);
  } catch (...) {
    device->vkd()->vkDestroyPipeline(device->vkd()->device(), candidate, nullptr);
    throw;
  }
  out = std::move(owner);
  return out != nullptr;
}

bool UploadLookupTexture(const Rc<DxvkCommandList>& ctx,
                         const Rc<DxvkDevice>& device,
                         const Rc<DxvkImage>& image,
                         render::War3OwnedImageLayoutState& layoutState,
                         VkExtent3D extent,
                         const void* data,
                         size_t dataSize,
                         const char* debugName) {
  if (!ctx || !device || !image)
    return false;
  if (!data || dataSize == 0)
    return false;

  DxvkBufferCreateInfo bufferInfo = {};
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
  VkImageMemoryBarrier2 barrier = render::MakeWar3OwnedImageBarrier(
      writeTransition, image->handle(), subresources);

  VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &barrier;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::InitBuffer, &depInfo);
  render::CommitWar3OwnedImageLayout(layoutState, writeTransition, *image, subresources);

  VkBufferImageCopy2 imageRegion = { VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2 };
  imageRegion.bufferOffset = uploadSlice.offset;
  imageRegion.imageExtent = extent;
  imageRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  imageRegion.imageSubresource.layerCount = 1u;

  VkCopyBufferToImageInfo2 imageCopy = { VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2 };
  imageCopy.srcBuffer = uploadSlice.buffer;
  imageCopy.dstImage = image->handle();
  imageCopy.dstImageLayout = writeTransition.newLayout;
  imageCopy.regionCount = 1;
  imageCopy.pRegions = &imageRegion;
  ctx->cmdCopyBufferToImage(DxvkCmdBuffer::InitBuffer, &imageCopy);

  const auto readTransition = layoutState.plan(
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  barrier = render::MakeWar3OwnedImageBarrier(readTransition, image->handle(), subresources);
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &barrier;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::InitBuffer, &depInfo);
  render::CommitWar3OwnedImageLayout(layoutState, readTransition, *image, subresources);

  // The copy command stores a raw VkBuffer handle. Keep the upload
  // allocation alive until this command list has completed on the GPU.
  ctx->track(uploadBuffer, DxvkAccess::Read);
  ctx->track(image, DxvkAccess::Write);
  return true;
}

} // namespace

AaExecutor::AaExecutor(Rc<DxvkDevice> device)
: m_device(std::move(device)) { }

uint64_t AaExecutor::resourceBytes() const {
  return ImageBytes(m_colorCopy) + ImageBytes(m_edgesTex) + ImageBytes(m_blendTex) +
         ImageBytes(m_areaTex) + ImageBytes(m_searchTex);
}

bool AaExecutor::ensureSamplers() {
  if (!m_linearSampler) {
    DxvkSamplerKey key = {};
    key.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    key.setUsePixelCoordinates(false);
    m_linearSampler = m_device->createSampler(key);
  }
  if (!m_pointSampler) {
    DxvkSamplerKey key = {};
    key.setFilter(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    key.setUsePixelCoordinates(false);
    m_pointSampler = m_device->createSampler(key);
  }
  return m_linearSampler != nullptr && m_pointSampler != nullptr;
}

bool AaExecutor::createSmaaLookupTextures(const Rc<DxvkCommandList>& ctx) {
  if (!ctx)
    return false;
  Rc<DxvkImage> candidateAreaTex;
  Rc<DxvkImageView> candidateAreaView;
  render::War3OwnedImageLayoutState candidateAreaLayout;
  Rc<DxvkImage> candidateSearchTex;
  Rc<DxvkImageView> candidateSearchView;
  render::War3OwnedImageLayoutState candidateSearchLayout;
  bool areaUploaded = false;
  bool searchUploaded = false;

  if (dxvk::smaa::areaTexSize && dxvk::smaa::areaTexWidth == 160 &&
      dxvk::smaa::areaTexHeight == 560) {
    const VkExtent3D areaExtent = { uint32_t(dxvk::smaa::areaTexWidth),
                                    uint32_t(dxvk::smaa::areaTexHeight), 1u };
    candidateAreaTex = CreateImage2D(m_device, areaExtent, VK_FORMAT_R8G8_UNORM,
                                     false, "War3 SMAA area");
    candidateAreaView = CreateView2D(candidateAreaTex, false);
    areaUploaded = candidateAreaView && UploadLookupTexture(
        ctx, m_device, candidateAreaTex, candidateAreaLayout, areaExtent,
        dxvk::smaa::areaTexBytes, dxvk::smaa::areaTexSize, "War3SmaaAreaUpload");
  }
  if (dxvk::smaa::searchTexSize && dxvk::smaa::searchTexWidth == 64 &&
      dxvk::smaa::searchTexHeight == 16) {
    const VkExtent3D searchExtent = { uint32_t(dxvk::smaa::searchTexWidth),
                                      uint32_t(dxvk::smaa::searchTexHeight), 1u };
    candidateSearchTex = CreateImage2D(m_device, searchExtent, VK_FORMAT_R8_UNORM,
                                       false, "War3 SMAA search");
    candidateSearchView = CreateView2D(candidateSearchTex, false);
    searchUploaded = candidateSearchView && UploadLookupTexture(
        ctx, m_device, candidateSearchTex, candidateSearchLayout, searchExtent,
        dxvk::smaa::searchTexBytes, dxvk::smaa::searchTexSize, "War3SmaaSearchUpload");
  }

  // Publication is transactional: a rejected candidate is never published and
  // the frame uses FXAA; a later frame tries again.
  if (!areaUploaded || !searchUploaded || !candidateAreaTex ||
      !candidateAreaView || !candidateSearchTex || !candidateSearchView)
    return false;

  m_areaTex = std::move(candidateAreaTex);
  m_areaView = std::move(candidateAreaView);
  m_areaLayout = candidateAreaLayout;
  m_searchTex = std::move(candidateSearchTex);
  m_searchView = std::move(candidateSearchView);
  m_searchLayout = candidateSearchLayout;
  m_lookupTablesCreated = true;
  return true;
}

bool AaExecutor::ensurePipelines(VkFormat format, bool smaa) {
  const auto layout = [&](Pass& pass, uint32_t pushBytes, uint32_t textures) {
    if (pass.layout)
      return;
    const DxvkDescriptorSetLayoutBinding binding(
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    const std::array<DxvkDescriptorSetLayoutBinding, 3> bindings = { binding, binding, binding };
    pass.layout = m_device->createBuiltInPipelineLayout(
        DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_FRAGMENT_BIT,
        pushBytes, textures, bindings.data());
  };
  const auto pipeline = [&](Pass& pass, VkFormat passFormat,
                            const util::DxvkBuiltInShaderStage& fs) {
    if (pass.pipeline && pass.format == passFormat)
      return true;
    util::DxvkBuiltInGraphicsState state = {};
    state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
    state.fs = fs;
    state.colorFormat = passFormat;
    state.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    if (!AdoptPipeline(m_device,
            m_device->createBuiltInGraphicsPipeline(pass.layout, state), pass.pipeline))
      return false;
    pass.format = passFormat;
    return true;
  };

  layout(m_fxaa, sizeof(FxaaPushConstants), 1);
  if (!m_fxaa.layout || !pipeline(m_fxaa, format,
          util::DxvkBuiltInShaderStage(war3_fxaa, nullptr)))
    return false;
  if (!smaa)
    return true;
  layout(m_smaaEdge, sizeof(SmaaEdgePushConstants), 1);
  layout(m_smaaBlend, sizeof(SmaaBlendPushConstants), 3);
  layout(m_smaaNeighbor, sizeof(SmaaNeighborPushConstants), 2);
  return m_smaaEdge.layout && m_smaaBlend.layout && m_smaaNeighbor.layout &&
      pipeline(m_smaaEdge, VK_FORMAT_R8G8_UNORM,
               util::DxvkBuiltInShaderStage(war3_smaa_edge, nullptr)) &&
      pipeline(m_smaaBlend, VK_FORMAT_R8G8B8A8_UNORM,
               util::DxvkBuiltInShaderStage(war3_smaa_blend, nullptr)) &&
      pipeline(m_smaaNeighbor, format,
               util::DxvkBuiltInShaderStage(war3_smaa_neighbor, nullptr));
}

bool AaExecutor::ensureTargets(VkExtent3D extent, VkFormat copyFormat, bool smaa) {
  const auto matches = [&](const Rc<DxvkImage>& image, VkFormat format) {
    return image && image->info().extent.width == extent.width &&
           image->info().extent.height == extent.height &&
           image->info().format == format;
  };
  if (!matches(m_colorCopy, copyFormat)) {
    // Transactional replacement: earlier command lists own their allocations.
    auto image = CreateImage2D(m_device, extent, copyFormat, false, "War3 AA color copy");
    auto view = CreateView2D(image, false);
    if (!image || !view)
      return false;
    m_colorCopy = std::move(image);
    m_colorCopyView = std::move(view);
    m_colorCopyLayout.reset();
  }
  if (!smaa)
    return true;
  if (!matches(m_edgesTex, VK_FORMAT_R8G8_UNORM)) {
    auto image = CreateImage2D(m_device, extent, VK_FORMAT_R8G8_UNORM, true, "War3 SMAA edges");
    auto view = CreateView2D(image, true);
    if (!image || !view)
      return false;
    m_edgesTex = std::move(image);
    m_edgesView = std::move(view);
    m_edgesLayout.reset();
  }
  if (!matches(m_blendTex, VK_FORMAT_R8G8B8A8_UNORM)) {
    auto image = CreateImage2D(m_device, extent, VK_FORMAT_R8G8B8A8_UNORM, true, "War3 SMAA blend");
    auto view = CreateView2D(image, true);
    if (!image || !view)
      return false;
    m_blendTex = std::move(image);
    m_blendView = std::move(view);
    m_blendLayout.reset();
  }
  return true;
}

AaStage AaExecutor::run(const Rc<DxvkCommandList>& ctx, const EffectFrame& frame,
                        const scene::Aa& settings, EffectRecording* recording) {
  if (settings.mode < scene::AaModeFxaa || settings.mode > scene::AaModeSmaaLast)
    return AaStage::Disabled;
  if (!ctx || !frame.colorView)
    return AaStage::MissingTargets;

  auto* colorImage = frame.colorView->image();
  const auto& colorInfo = colorImage->info();
  // The copy is single-sampled; a multisampled target cannot be copied.
  if (colorInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT)
    return AaStage::UnsupportedSamples;
  const auto colorRange = frame.colorView->imageSubresources();
  const auto extent = frame.colorView->mipLevelExtent(0);
  if (!(colorInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
      !(colorInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
      colorRange.layerCount != 1 || colorRange.levelCount != 1 ||
      extent.width == 0 || extent.height == 0)
    return AaStage::UnsupportedTarget;
  const VkImageLayout colorOriginal = colorImage->queryLayout(colorRange);
  if (!LayoutKnown(colorOriginal))
    return AaStage::UndefinedLayout;

  const VkFormat targetFormat = frame.colorView->info().format;
  const bool wantSmaa = settings.mode != scene::AaModeFxaa;
  const VkImageLayout colorTransfer =
      colorImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const VkImageLayout colorAttachmentLayout =
      colorImage->pickLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  bool smaa = false;
  const DxvkDescriptor* copyDescriptor = nullptr;
  std::array<const DxvkDescriptor*, 4> smaaDescriptors = {};
  // Every allocation happens before the first frame command. A failure leaves
  // the frame without AA. Only the one-time lookup upload (owned images, init
  // command buffer) may precede a later failure here.
  try {
    if (!ensureSamplers() || !ensurePipelines(targetFormat, wantSmaa) ||
        !ensureTargets(extent, colorInfo.format, wantSmaa))
      return AaStage::ResourceUnavailable;
    if (wantSmaa && !m_lookupTablesCreated)
      createSmaaLookupTextures(ctx);
    smaa = wantSmaa && m_lookupTablesCreated;
    copyDescriptor = m_colorCopyView->getDescriptor();
    if (!copyDescriptor || !frame.colorView->getDescriptor())
      return AaStage::ResourceUnavailable;
    if (smaa) {
      smaaDescriptors = { m_edgesView->getDescriptor(), m_blendView->getDescriptor(),
                          m_areaView->getDescriptor(), m_searchView->getDescriptor() };
      for (const auto* descriptor : smaaDescriptors) {
        if (!descriptor)
          return AaStage::ResourceUnavailable;
      }
      // The intermediates are attachments too; create those views now.
      if (!m_edgesView->handle() || !m_blendView->handle())
        return AaStage::ResourceUnavailable;
    }
  } catch (const DxvkError&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    return AaStage::ResourceUnavailable;
  } catch (const std::bad_alloc&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    return AaStage::ResourceUnavailable;
  }

  if (recording)
    recording->beginRecording(ctx);
  constexpr VkAccessFlags2 anyAccess =
      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  const auto copyRange = m_colorCopy->getAvailableSubresources();
  VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
  const auto barrier = [&](const VkImageMemoryBarrier2* barriers, uint32_t count) {
    dependency.imageMemoryBarrierCount = count;
    dependency.pImageMemoryBarriers = barriers;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  };

  // Target to transfer source, copy to transfer destination.
  const auto copyWrite = m_colorCopyLayout.plan(
      m_colorCopy->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  std::array<VkImageMemoryBarrier2, 2> barriers = {
    ImageBarrier(colorImage, colorRange, colorOriginal, colorTransfer,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, anyAccess,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT),
    render::MakeWar3OwnedImageBarrier(copyWrite, m_colorCopy->handle(), copyRange),
  };
  barrier(barriers.data(), barriers.size());
  colorImage->trackLayout(colorRange, colorTransfer);
  render::CommitWar3OwnedImageLayout(m_colorCopyLayout, copyWrite, *m_colorCopy, copyRange);

  VkImageCopy2 region = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
  region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT,
      colorRange.baseMipLevel, colorRange.baseArrayLayer, 1 };
  region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  region.extent = extent;
  VkCopyImageInfo2 copy = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
  copy.srcImage = colorImage->handle();
  copy.srcImageLayout = colorTransfer;
  copy.dstImage = m_colorCopy->handle();
  copy.dstImageLayout = copyWrite.newLayout;
  copy.regionCount = 1;
  copy.pRegions = &region;
  ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copy);

  // Target becomes the attachment of the last pass, the copy becomes readable.
  const auto copyRead = m_colorCopyLayout.plan(
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  barriers[0] = ImageBarrier(colorImage, colorRange, colorTransfer, colorAttachmentLayout,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  barriers[1] = render::MakeWar3OwnedImageBarrier(copyRead, m_colorCopy->handle(), copyRange);
  barrier(barriers.data(), barriers.size());
  colorImage->trackLayout(colorRange, colorAttachmentLayout);
  render::CommitWar3OwnedImageLayout(m_colorCopyLayout, copyRead, *m_colorCopy, copyRange);

  const VkViewport viewport = {
    0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
  const VkRect2D scissor = { { 0, 0 }, { extent.width, extent.height } };
  const float invWidth = 1.0f / float(extent.width);
  const float invHeight = 1.0f / float(extent.height);

  // One fullscreen triangle into view. A cleared intermediate starts at 0;
  // the target is fully overwritten.
  const auto draw = [&](const Rc<DxvkImageView>& view, VkImageLayout layout, bool clear,
                        const Pass& pass, const DxvkDescriptorWrite* descriptors,
                        uint32_t descriptorCount, const void* pc, uint32_t pcBytes) {
    VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    attachment.imageView = view->handle();
    attachment.imageLayout = layout;
    attachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    rendering.renderArea.extent = { extent.width, extent.height };
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    ctx->cmdBeginRendering(&rendering);
    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline->handle());
    ctx->cmdSetViewport(1, &viewport);
    ctx->cmdSetScissor(1, &scissor);
    ctx->bindResources(DxvkCmdBuffer::ExecBuffer, pass.layout,
                       descriptorCount, descriptors, pcBytes, pc);
    ctx->cmdDraw(3, 1, 0, 0);
    ctx->cmdEndRendering();
  };
  const auto sampled = [](const DxvkDescriptor* descriptor) {
    DxvkDescriptorWrite write = {};
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.descriptor = descriptor;
    return write;
  };
  // An owned intermediate: to attachment, drawn, then readable.
  const auto intermediate = [&](const Rc<DxvkImage>& image, const Rc<DxvkImageView>& view,
                                render::War3OwnedImageLayoutState& state, const Pass& pass,
                                const DxvkDescriptorWrite* descriptors, uint32_t descriptorCount,
                                const void* pc, uint32_t pcBytes) {
    const auto range = image->getAvailableSubresources();
    const auto attach = state.plan(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkImageMemoryBarrier2 toAttach =
        render::MakeWar3OwnedImageBarrier(attach, image->handle(), range);
    barrier(&toAttach, 1);
    render::CommitWar3OwnedImageLayout(state, attach, *image, range);
    draw(view, attach.newLayout, true, pass, descriptors, descriptorCount, pc, pcBytes);
    const auto read = state.plan(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkImageMemoryBarrier2 toRead =
        render::MakeWar3OwnedImageBarrier(read, image->handle(), range);
    barrier(&toRead, 1);
    render::CommitWar3OwnedImageLayout(state, read, *image, range);
  };

  if (!smaa) {
    const DxvkDescriptorWrite descriptors[] = { sampled(copyDescriptor) };
    FxaaPushConstants pc = {};
    pc.colorSampler = m_linearSampler->getDescriptor().samplerIndex;
    pc.invWidth = invWidth;
    pc.invHeight = invHeight;
    pc.qualitySubpix = settings.fxaaQualitySubpix;
    pc.qualityEdgeThreshold = settings.fxaaQualityEdgeThreshold;
    pc.qualityEdgeThresholdMin = settings.fxaaQualityEdgeThresholdMin;
    draw(frame.colorView, colorAttachmentLayout, false, m_fxaa, descriptors, 1, &pc, sizeof(pc));
  } else {
    // The live tuning ranges of the settings UI.
    const float threshold = std::clamp(settings.smaaThreshold, 0.01f, 0.5f);
    const int maxSearchSteps = std::clamp(settings.smaaMaxSearchSteps, 4, 64);
    const int maxSearchStepsDiag = std::clamp(settings.smaaMaxSearchStepsDiag, 0, 32);

    // Pass 1: edge detection.
    {
      const DxvkDescriptorWrite descriptors[] = { sampled(copyDescriptor) };
      SmaaEdgePushConstants pc = {};
      pc.colorSampler = m_pointSampler->getDescriptor().samplerIndex;
      pc.invWidth = invWidth;
      pc.invHeight = invHeight;
      pc.threshold = threshold;
      intermediate(m_edgesTex, m_edgesView, m_edgesLayout, m_smaaEdge,
                   descriptors, 1, &pc, sizeof(pc));
    }
    // Pass 2: blend weights. The edge texture is sampled linearly so that the
    // search does not break on hard nearest hits.
    {
      const DxvkDescriptorWrite descriptors[] = { sampled(smaaDescriptors[0]),
          sampled(smaaDescriptors[2]), sampled(smaaDescriptors[3]) };
      SmaaBlendPushConstants pc = {};
      pc.edgeSampler = m_linearSampler->getDescriptor().samplerIndex;
      pc.areaSampler = m_linearSampler->getDescriptor().samplerIndex;
      pc.searchSampler = m_linearSampler->getDescriptor().samplerIndex;
      pc.invWidth = invWidth;
      pc.invHeight = invHeight;
      pc.maxSearchSteps = maxSearchSteps;
      pc.maxSearchStepsDiag = maxSearchStepsDiag;
      intermediate(m_blendTex, m_blendView, m_blendLayout, m_smaaBlend,
                   descriptors, 3, &pc, sizeof(pc));
    }
    // Pass 3: neighbourhood blending into the target. Colour linear, weights
    // point-sampled so neighbouring weights do not bleed into each other.
    {
      const DxvkDescriptorWrite descriptors[] = { sampled(copyDescriptor),
                                                  sampled(smaaDescriptors[1]) };
      SmaaNeighborPushConstants pc = {};
      pc.colorSampler = m_linearSampler->getDescriptor().samplerIndex;
      pc.blendSampler = m_pointSampler->getDescriptor().samplerIndex;
      pc.invWidth = invWidth;
      pc.invHeight = invHeight;
      draw(frame.colorView, colorAttachmentLayout, false, m_smaaNeighbor,
           descriptors, 2, &pc, sizeof(pc));
    }
  }

  VkImageMemoryBarrier2 restore = ImageBarrier(colorImage, colorRange,
      colorAttachmentLayout, colorOriginal,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, anyAccess);
  barrier(&restore, 1);
  colorImage->trackLayout(colorRange, colorOriginal);

  // Commands hold raw handles: every owner stays alive through GPU completion.
  ctx->track(colorImage, DxvkAccess::Write);
  ctx->track(m_colorCopy, DxvkAccess::Write);
  ctx->track(m_linearSampler);
  ctx->track(m_pointSampler);
  ctx->track(m_fxaa.pipeline);
  if (smaa) {
    ctx->track(m_edgesTex, DxvkAccess::Write);
    ctx->track(m_blendTex, DxvkAccess::Write);
    ctx->track(m_areaTex, DxvkAccess::Read);
    ctx->track(m_searchTex, DxvkAccess::Read);
    ctx->track(m_smaaEdge.pipeline);
    ctx->track(m_smaaBlend.pipeline);
    ctx->track(m_smaaNeighbor.pipeline);
  }
  return smaa ? AaStage::SmaaRecorded : AaStage::FxaaRecorded;
}

} // namespace dxvk::war3::effects
