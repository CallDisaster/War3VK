#include "war3_ssao_executor.h"

#include "../../../dxvk/dxvk_access.h"
#include "../../../dxvk/dxvk_util.h"
#include "../../../util/util_error.h"

#include <war3_fullscreen_vert.h>
#include <war3_ssao.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace dxvk::war3::effects {
namespace {

// Scalar row-major shader ABI (war3_ssao.frag push_block).
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

Rc<DxvkImageView> SampledView(const Rc<DxvkImage>& image, VkFormat format,
                              VkImageLayout layout, VkImageAspectFlags aspects) {
  DxvkImageViewKey key;
  key.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  key.format = format;
  key.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  key.layout = layout;
  key.aspects = aspects;
  key.mipIndex = 0;
  key.mipCount = 1;
  key.layerIndex = 0;
  key.layerCount = 1;
  const VkComponentMapping swizzle = {
    VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
    VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
  key.packedSwizzle = DxvkImageViewKey::packSwizzle(swizzle);
  return image->createView(key);
}

} // namespace

SsaoExecutor::SsaoExecutor(Rc<DxvkDevice> device)
: m_device(std::move(device)) { }

bool SsaoExecutor::ensurePipeline(VkFormat format) {
  if (!m_linearSampler) {
    DxvkSamplerKey key = {};
    key.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                  VK_SAMPLER_MIPMAP_MODE_NEAREST);
    key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    key.setUsePixelCoordinates(false);
    m_linearSampler = m_device->createSampler(key);
  }
  if (!m_layout) {
    const std::array<DxvkDescriptorSetLayoutBinding, 2> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
    };
    m_layout = m_device->createBuiltInPipelineLayout(
        DxvkPipelineLayoutFlag::UsesSamplerHeap,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        sizeof(SsaoPushConstants), bindings.size(), bindings.data());
  }
  if (m_pipeline && m_pipelineFormat == format)
    return true;

  util::DxvkBuiltInGraphicsState state = {};
  state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
  state.fs = util::DxvkBuiltInShaderStage(war3_ssao, nullptr);
  state.colorFormat = format;
  state.sampleCount = VK_SAMPLE_COUNT_1_BIT;

  const VkPipeline candidate =
      m_device->createBuiltInGraphicsPipeline(m_layout, state);
  if (candidate == VK_NULL_HANDLE)
    return false;
  // Old command lists retain their own pipeline owner (war3_snow_executor.cpp).
  Rc<render::War3TrackedVkPipeline> candidateOwner;
  try {
    candidateOwner = render::AdoptWar3TrackedVkPipeline(m_device, candidate);
  } catch (...) {
    m_device->vkd()->vkDestroyPipeline(m_device->vkd()->device(), candidate, nullptr);
    throw;
  }
  m_pipeline = std::move(candidateOwner);
  m_pipelineFormat = format;
  return m_pipeline != nullptr && m_linearSampler != nullptr;
}

bool SsaoExecutor::ensureCopies(VkExtent3D extent, VkFormat colorFormat, VkFormat depthFormat) {
  const auto matches = [&](const Rc<DxvkImage>& image, VkFormat format) {
    return image && image->info().extent.width == extent.width &&
           image->info().extent.height == extent.height &&
           image->info().format == format;
  };
  if (matches(m_colorCopy, colorFormat) && matches(m_depthCopy, depthFormat))
    return true;

  const auto create = [&](VkFormat format, VkImageLayout layout, const char* name) {
    DxvkImageCreateInfo info = {};
    info.type = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = extent;
    info.numLayers = 1;
    info.mipLevels = 1;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                  VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = layout;
    info.debugName = name;
    return m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  };
  auto color = create(colorFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "War3 SSAO color copy");
  auto depth = create(depthFormat, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, "War3 SSAO depth copy");
  if (!color || !color->storage() || !depth || !depth->storage())
    return false;
  auto colorView = SampledView(color, colorFormat,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
  auto depthView = SampledView(depth, depthFormat,
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
  if (!colorView || !depthView)
    return false;
  // Transactional replacement: earlier command lists own their allocations.
  m_resourceBytes = color->storage()->getMemoryInfo().size +
                    depth->storage()->getMemoryInfo().size;
  m_colorCopy = std::move(color);
  m_colorCopyView = std::move(colorView);
  m_colorCopyLayout.reset();
  m_depthCopy = std::move(depth);
  m_depthCopyView = std::move(depthView);
  m_depthCopyLayout.reset();
  return true;
}

SsaoStage SsaoExecutor::run(const Rc<DxvkCommandList>& ctx,
                            const EffectFrame& frame,
                            const scene::Ssao& settings,
                            EffectRecording* recording) {
  if (!ctx || !frame.colorView || !frame.depthView)
    return SsaoStage::MissingTargets;
  const auto& camera = frame.camera;
  if (!camera.valid)
    return SsaoStage::CameraUnavailable;

  auto* colorImage = frame.colorView->image();
  auto* depthImage = frame.depthView->image();
  const auto& colorInfo = colorImage->info();
  const auto& depthInfo = depthImage->info();
  // The copies are single-sampled; a multisampled target cannot be copied.
  if (colorInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT ||
      depthInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT)
    return SsaoStage::UnsupportedSamples;
  const auto colorRange = frame.colorView->imageSubresources();
  auto depthRange = frame.depthView->imageSubresources();
  const auto extent = frame.colorView->mipLevelExtent(0);
  const auto depthExtent = frame.depthView->mipLevelExtent(0);
  const auto depthFormat = lookupFormatInfo(depthInfo.format);
  if (colorImage == depthImage ||
      !(colorInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
      !(colorInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
      colorRange.layerCount != 1 || colorRange.levelCount != 1 ||
      extent.width == 0 || extent.height == 0)
    return SsaoStage::UnsupportedTarget;
  if (!depthFormat || !(depthFormat->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
      !(depthInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
      !(depthRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
      depthRange.layerCount != 1 || depthRange.levelCount != 1 ||
      !(m_device->getFormatFeatures(depthInfo.format).optimal &
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) ||
      depthExtent.width != extent.width ||
      depthExtent.height != extent.height)
    return SsaoStage::UnsupportedDepth;
  // Depth and stencil share one allocation: transition both aspects, and
  // restore the exact previous combined layout (war3_snow_executor.cpp).
  depthRange.aspectMask = depthFormat->aspectMask;
  const VkImageLayout depthOriginal = depthImage->queryLayout(depthRange);
  const VkImageLayout colorOriginal = colorImage->queryLayout(colorRange);
  if (!LayoutKnown(depthOriginal) || !LayoutKnown(colorOriginal))
    return SsaoStage::UndefinedLayout;

  const VkImageLayout colorTransfer =
      colorImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const VkImageLayout depthTransfer =
      depthImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const VkImageLayout colorAttachmentLayout =
      colorImage->pickLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  const DxvkDescriptor* colorDescriptor = nullptr;
  const DxvkDescriptor* depthDescriptor = nullptr;
  // Every allocation happens before the first command. A failure leaves the
  // frame without ambient occlusion and the command list untouched.
  try {
    if (!ensurePipeline(frame.colorView->info().format) ||
        !ensureCopies(extent, frame.colorView->info().format, depthInfo.format))
      return SsaoStage::ResourceUnavailable;
    colorDescriptor = m_colorCopyView->getDescriptor();
    depthDescriptor = m_depthCopyView->getDescriptor();
    if (!colorDescriptor || !depthDescriptor || !frame.colorView->getDescriptor())
      return SsaoStage::ResourceUnavailable;
  } catch (const DxvkError&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    return SsaoStage::ResourceUnavailable;
  } catch (const std::bad_alloc&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    return SsaoStage::ResourceUnavailable;
  }

  if (recording)
    recording->beginRecording(ctx);
  const auto colorCopyRange = m_colorCopy->getAvailableSubresources();
  const auto depthCopyRange = m_depthCopy->getAvailableSubresources();
  constexpr VkAccessFlags2 anyAccess =
      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

  // Targets to transfer sources, copies to transfer destinations.
  const auto colorWrite = m_colorCopyLayout.plan(
      m_colorCopy->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  const auto depthWrite = m_depthCopyLayout.plan(
      m_depthCopy->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  std::array<VkImageMemoryBarrier2, 4> barriers = {
    ImageBarrier(colorImage, colorRange, colorOriginal, colorTransfer,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, anyAccess,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT),
    ImageBarrier(depthImage, depthRange, depthOriginal, depthTransfer,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, anyAccess,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT),
    render::MakeWar3OwnedImageBarrier(colorWrite, m_colorCopy->handle(), colorCopyRange),
    render::MakeWar3OwnedImageBarrier(depthWrite, m_depthCopy->handle(), depthCopyRange),
  };
  VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
  dependency.imageMemoryBarrierCount = barriers.size();
  dependency.pImageMemoryBarriers = barriers.data();
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  colorImage->trackLayout(colorRange, colorTransfer);
  depthImage->trackLayout(depthRange, depthTransfer);
  render::CommitWar3OwnedImageLayout(m_colorCopyLayout, colorWrite, *m_colorCopy, colorCopyRange);
  render::CommitWar3OwnedImageLayout(m_depthCopyLayout, depthWrite, *m_depthCopy, depthCopyRange);

  const auto copy = [&](DxvkImage* source, VkImageLayout sourceLayout,
                        const VkImageSubresourceRange& sourceRange,
                        const Rc<DxvkImage>& target, VkImageLayout targetLayout,
                        VkImageAspectFlags aspect) {
    VkImageCopy2 region = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
    region.srcSubresource = { aspect, sourceRange.baseMipLevel, sourceRange.baseArrayLayer, 1 };
    region.dstSubresource = { aspect, 0, 0, 1 };
    region.extent = extent;
    VkCopyImageInfo2 info = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
    info.srcImage = source->handle();
    info.srcImageLayout = sourceLayout;
    info.dstImage = target->handle();
    info.dstImageLayout = targetLayout;
    info.regionCount = 1;
    info.pRegions = &region;
    ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &info);
  };
  copy(colorImage, colorTransfer, colorRange, m_colorCopy, colorWrite.newLayout, VK_IMAGE_ASPECT_COLOR_BIT);
  copy(depthImage, depthTransfer, depthRange, m_depthCopy, depthWrite.newLayout, VK_IMAGE_ASPECT_DEPTH_BIT);

  // Colour becomes the attachment, depth returns to where it was, and the
  // copies become readable.
  const auto colorRead = m_colorCopyLayout.plan(
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  const auto depthRead = m_depthCopyLayout.plan(
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  barriers[0] = ImageBarrier(colorImage, colorRange, colorTransfer, colorAttachmentLayout,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  barriers[1] = ImageBarrier(depthImage, depthRange, depthTransfer, depthOriginal,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, anyAccess);
  barriers[2] = render::MakeWar3OwnedImageBarrier(colorRead, m_colorCopy->handle(), colorCopyRange);
  barriers[3] = render::MakeWar3OwnedImageBarrier(depthRead, m_depthCopy->handle(), depthCopyRange);
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  colorImage->trackLayout(colorRange, colorAttachmentLayout);
  depthImage->trackLayout(depthRange, depthOriginal);
  render::CommitWar3OwnedImageLayout(m_colorCopyLayout, colorRead, *m_colorCopy, colorCopyRange);
  render::CommitWar3OwnedImageLayout(m_depthCopyLayout, depthRead, *m_depthCopy, depthCopyRange);

  VkRenderingAttachmentInfo attachment = {
    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
  attachment.imageView = frame.colorView->handle();
  attachment.imageLayout = colorAttachmentLayout;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
  rendering.renderArea.extent = { extent.width, extent.height };
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;
  ctx->cmdBeginRendering(&rendering);

  const VkViewport viewport = {
    0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f };
  const VkRect2D scissor = { { 0, 0 }, { extent.width, extent.height } };
  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);

  SsaoPushConstants pc = {};
  pc.colorSampler = m_linearSampler->getDescriptor().samplerIndex;
  pc.depthSampler = m_linearSampler->getDescriptor().samplerIndex;
  pc.radiusPx = settings.radiusPx;
  pc.strength = settings.strength;
  pc.bias = settings.bias;
  pc.power = settings.power;
  pc.invViewProj = camera.invViewProj;
  pc.viewport = Vector4(float(camera.x), float(camera.y),
                        float(camera.width), float(camera.height));
  pc.viewportZ = Vector4(camera.minZ, camera.maxZ, 0.0f, 0.0f);
  const float fadeNear = std::clamp(settings.fadeNear, 0.0f, 0.95f);
  const float fadeFar = std::clamp(settings.fadeFar, fadeNear + 0.01f, 1.0f);
  pc.fade = Vector4(fadeNear, fadeFar, 0.0f, 0.0f);

  std::array<DxvkDescriptorWrite, 2> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor = colorDescriptor;
  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = depthDescriptor;

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
      VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_layout,
                     descriptors.size(), descriptors.data(), sizeof(pc), &pc);
  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  VkImageMemoryBarrier2 restore = ImageBarrier(colorImage, colorRange,
      colorAttachmentLayout, colorOriginal,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, anyAccess);
  VkDependencyInfo restoreDependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
  restoreDependency.imageMemoryBarrierCount = 1;
  restoreDependency.pImageMemoryBarriers = &restore;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &restoreDependency);
  colorImage->trackLayout(colorRange, colorOriginal);

  // Commands hold raw handles: every owner stays alive through GPU completion.
  ctx->track(colorImage, DxvkAccess::Write);
  ctx->track(depthImage, DxvkAccess::Read);
  ctx->track(m_colorCopy, DxvkAccess::Write);
  ctx->track(m_depthCopy, DxvkAccess::Write);
  ctx->track(m_linearSampler);
  ctx->track(m_pipeline);
  return SsaoStage::CommandsRecorded;
}

} // namespace dxvk::war3::effects
