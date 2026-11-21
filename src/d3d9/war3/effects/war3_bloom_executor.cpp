#include "war3_bloom_executor.h"

#include "../../../dxvk/dxvk_access.h"
#include "../../../dxvk/dxvk_util.h"
#include "../../../util/util_error.h"

#include <war3_fullscreen_vert.h>
#include <war3_bloom.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>

namespace dxvk::war3::effects {
namespace {

// war3_bloom.frag push_block (std430).
struct BloomPushConstants {
  uint32_t sourceSampler;
  uint32_t bloomSampler;
  uint32_t mode;
  uint32_t flags;
  float invTargetX;
  float invTargetY;
  float texelX;
  float texelY;
  float threshold;
  float softKnee;
  float intensity;
  float exposure;
};
static_assert(sizeof(BloomPushConstants) == 48);
static_assert(offsetof(BloomPushConstants, invTargetX) == 16);
static_assert(offsetof(BloomPushConstants, threshold) == 32);

enum : uint32_t { Prefilter = 0, Downsample = 1, Upsample = 2, Composite = 3 };

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
      (attachment ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                  : VK_ACCESS_TRANSFER_WRITE_BIT);
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

} // namespace

BloomExecutor::BloomExecutor(Rc<DxvkDevice> device)
: m_device(std::move(device)) { }

bool BloomExecutor::ensurePipelines(VkFormat format) {
  // D3D9 sampler states of the earlier implementation: linear or point,
  // clamped, one mip level.
  const auto sampler = [&](Rc<DxvkSampler>& out, VkFilter filter) {
    if (out)
      return;
    DxvkSamplerKey key = {};
    key.setFilter(filter, filter, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    key.setUsePixelCoordinates(false);
    out = m_device->createSampler(key);
  };
  sampler(m_linearSampler, VK_FILTER_LINEAR);
  sampler(m_pointSampler, VK_FILTER_NEAREST);
  if (!m_layout) {
    const std::array<DxvkDescriptorSetLayoutBinding, 2> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT),
    };
    m_layout = m_device->createBuiltInPipelineLayout(
        DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_FRAGMENT_BIT,
        sizeof(BloomPushConstants), bindings.size(), bindings.data());
  }
  if (!m_layout || !m_linearSampler || !m_pointSampler)
    return false;
  if (m_drawPipeline && m_addPipeline && m_pipelineFormat == format)
    return true;

  util::DxvkBuiltInGraphicsState state = {};
  state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
  state.fs = util::DxvkBuiltInShaderStage(war3_bloom, nullptr);
  state.colorFormat = format;
  state.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  Rc<render::War3TrackedVkPipeline> draw;
  if (!AdoptPipeline(m_device, m_device->createBuiltInGraphicsPipeline(m_layout, state), draw))
    return false;

  // D3DRS_SRCBLEND = DESTBLEND = ONE, alpha blended the same way.
  VkPipelineColorBlendAttachmentState add = {};
  add.blendEnable = VK_TRUE;
  add.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  add.colorBlendOp = VK_BLEND_OP_ADD;
  add.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  add.alphaBlendOp = VK_BLEND_OP_ADD;
  add.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  state.cbAttachment = &add;
  Rc<render::War3TrackedVkPipeline> additive;
  if (!AdoptPipeline(m_device, m_device->createBuiltInGraphicsPipeline(m_layout, state), additive))
    return false;

  m_drawPipeline = std::move(draw);
  m_addPipeline = std::move(additive);
  m_pipelineFormat = format;
  return true;
}

bool BloomExecutor::ensureTargets(VkExtent3D extent, VkFormat copyFormat, VkFormat levelFormat) {
  const auto matches = [](const Rc<DxvkImage>& image, VkExtent3D size, VkFormat format) {
    return image && image->info().extent.width == size.width &&
           image->info().extent.height == size.height &&
           image->info().format == format;
  };
  std::array<VkExtent3D, kLevels> sizes = {};
  VkExtent3D size = { std::max(1u, extent.width), std::max(1u, extent.height), 1u };
  bool levelsMatch = true;
  for (uint32_t i = 0; i < kLevels; ++i) {
    sizes[i] = size;
    levelsMatch = levelsMatch && matches(m_levels[i], size, levelFormat);
    size = { std::max(1u, size.width / 2u), std::max(1u, size.height / 2u), 1u };
  }
  if (matches(m_sceneCopy, extent, copyFormat) && levelsMatch)
    return true;

  // Transactional replacement: earlier command lists own their allocations.
  auto sceneCopy = CreateImage2D(m_device, extent, copyFormat, false, "War3 bloom scene copy");
  auto sceneCopyView = CreateView2D(sceneCopy, false);
  if (!sceneCopy || !sceneCopyView)
    return false;
  std::array<Rc<DxvkImage>, kLevels> levels;
  std::array<Rc<DxvkImageView>, kLevels> views;
  for (uint32_t i = 0; i < kLevels; ++i) {
    levels[i] = CreateImage2D(m_device, sizes[i], levelFormat, true, "War3 bloom level");
    views[i] = CreateView2D(levels[i], true);
    if (!levels[i] || !views[i])
      return false;
  }
  m_sceneCopy = std::move(sceneCopy);
  m_sceneCopyView = std::move(sceneCopyView);
  m_sceneCopyLayout.reset();
  m_resourceBytes = ImageBytes(m_sceneCopy);
  for (uint32_t i = 0; i < kLevels; ++i) {
    m_levels[i] = std::move(levels[i]);
    m_levelViews[i] = std::move(views[i]);
    m_levelLayouts[i].reset();
    m_resourceBytes += ImageBytes(m_levels[i]);
  }
  return true;
}

BloomStage BloomExecutor::run(const Rc<DxvkCommandList>& ctx, const EffectFrame& frame,
                              const scene::Bloom& settings, EffectRecording* recording) {
  if (!ctx || !frame.colorView)
    return BloomStage::MissingTargets;

  auto* colorImage = frame.colorView->image();
  const auto& colorInfo = colorImage->info();
  if (colorInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT)
    return BloomStage::UnsupportedSamples;
  const auto colorRange = frame.colorView->imageSubresources();
  const auto extent = frame.colorView->mipLevelExtent(0);
  if (!(colorInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
      !(colorInfo.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
      colorRange.layerCount != 1 || colorRange.levelCount != 1 ||
      extent.width == 0 || extent.height == 0)
    return BloomStage::UnsupportedTarget;
  const VkImageLayout colorOriginal = colorImage->queryLayout(colorRange);
  if (!LayoutKnown(colorOriginal))
    return BloomStage::UndefinedLayout;

  const VkFormat targetFormat = frame.colorView->info().format;
  const VkImageLayout colorTransfer =
      colorImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const VkImageLayout colorAttachmentLayout =
      colorImage->pickLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  const DxvkDescriptor* sceneDescriptor = nullptr;
  std::array<const DxvkDescriptor*, kLevels> levelDescriptors = {};
  // Every allocation happens before the first command. A failure leaves the
  // frame as it is and the command list untouched.
  try {
    if (!ensurePipelines(targetFormat) ||
        !ensureTargets(extent, colorInfo.format, targetFormat))
      return BloomStage::ResourceUnavailable;
    sceneDescriptor = m_sceneCopyView->getDescriptor();
    if (!sceneDescriptor || !frame.colorView->getDescriptor())
      return BloomStage::ResourceUnavailable;
    for (uint32_t i = 0; i < kLevels; ++i) {
      levelDescriptors[i] = m_levelViews[i]->getDescriptor();
      if (!levelDescriptors[i] || !m_levelViews[i]->handle())
        return BloomStage::ResourceUnavailable;
    }
  } catch (const DxvkError&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    return BloomStage::ResourceUnavailable;
  } catch (const std::bad_alloc&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    return BloomStage::ResourceUnavailable;
  }

  if (recording)
    recording->beginRecording(ctx);
  constexpr VkAccessFlags2 anyAccess =
      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
  const auto barrier = [&](const VkImageMemoryBarrier2* barriers, uint32_t count) {
    dependency.imageMemoryBarrierCount = count;
    dependency.pImageMemoryBarriers = barriers;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  };

  // The scene copy: the source of the prefilter and the base of the composite.
  const auto copyRange = m_sceneCopy->getAvailableSubresources();
  const auto copyWrite = m_sceneCopyLayout.plan(
      m_sceneCopy->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  std::array<VkImageMemoryBarrier2, 2> barriers = {
    ImageBarrier(colorImage, colorRange, colorOriginal, colorTransfer,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, anyAccess,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT),
    render::MakeWar3OwnedImageBarrier(copyWrite, m_sceneCopy->handle(), copyRange),
  };
  barrier(barriers.data(), barriers.size());
  colorImage->trackLayout(colorRange, colorTransfer);
  render::CommitWar3OwnedImageLayout(m_sceneCopyLayout, copyWrite, *m_sceneCopy, copyRange);

  VkImageCopy2 region = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
  region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT,
      colorRange.baseMipLevel, colorRange.baseArrayLayer, 1 };
  region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  region.extent = extent;
  VkCopyImageInfo2 copy = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
  copy.srcImage = colorImage->handle();
  copy.srcImageLayout = colorTransfer;
  copy.dstImage = m_sceneCopy->handle();
  copy.dstImageLayout = copyWrite.newLayout;
  copy.regionCount = 1;
  copy.pRegions = &region;
  ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copy);

  const auto copyRead = m_sceneCopyLayout.plan(
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  barriers[0] = ImageBarrier(colorImage, colorRange, colorTransfer, colorAttachmentLayout,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  barriers[1] = render::MakeWar3OwnedImageBarrier(copyRead, m_sceneCopy->handle(), copyRange);
  barrier(barriers.data(), barriers.size());
  colorImage->trackLayout(colorRange, colorAttachmentLayout);
  render::CommitWar3OwnedImageLayout(m_sceneCopyLayout, copyRead, *m_sceneCopy, copyRange);

  BloomPushConstants pc = {};
  // The sRGB decode and encode always run: the D3D9 implementation's shader
  // (the HLSL embedded in war3_post_process.cpp, not shaders/war3_postfx.hlsl)
  // converted unconditionally and never read useSrgb. Same pixels as before.
  pc.flags = (settings.acesToneMap ? 1u : 0u) | 2u;
  pc.threshold = settings.threshold;
  pc.softKnee = settings.softKnee;
  pc.intensity = settings.intensity;
  pc.exposure = settings.exposure;

  const auto extentOf = [](const Rc<DxvkImage>& image) { return image->info().extent; };
  // One fullscreen triangle into a target of the given size.
  const auto draw = [&](const Rc<DxvkImageView>& view, VkImageLayout layout, VkExtent3D size,
                        bool additive, const DxvkDescriptor* source, const DxvkDescriptor* bloom,
                        uint32_t sourceSampler) {
    VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    attachment.imageView = view->handle();
    attachment.imageLayout = layout;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    rendering.renderArea.extent = { size.width, size.height };
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    ctx->cmdBeginRendering(&rendering);
    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        (additive ? m_addPipeline : m_drawPipeline)->handle());
    const VkViewport viewport = {
      0.0f, 0.0f, float(size.width), float(size.height), 0.0f, 1.0f };
    const VkRect2D scissor = { { 0, 0 }, { size.width, size.height } };
    ctx->cmdSetViewport(1, &viewport);
    ctx->cmdSetScissor(1, &scissor);
    std::array<DxvkDescriptorWrite, 2> descriptors = {};
    descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptors[0].descriptor = source;
    descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptors[1].descriptor = bloom;
    pc.sourceSampler = sourceSampler;
    pc.bloomSampler = m_linearSampler->getDescriptor().samplerIndex;
    pc.invTargetX = 1.0f / float(size.width);
    pc.invTargetY = 1.0f / float(size.height);
    ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_layout,
                       descriptors.size(), descriptors.data(), sizeof(pc), &pc);
    ctx->cmdDraw(3, 1, 0, 0);
    ctx->cmdEndRendering();
  };
  // A bloom level: to attachment (contents kept, the upsample adds to them),
  // drawn, then readable.
  const auto level = [&](uint32_t i, uint32_t mode, const DxvkDescriptor* source,
                         VkExtent3D sourceSize) {
    const auto range = m_levels[i]->getAvailableSubresources();
    const auto attach = m_levelLayouts[i].plan(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkImageMemoryBarrier2 toAttach =
        render::MakeWar3OwnedImageBarrier(attach, m_levels[i]->handle(), range);
    barrier(&toAttach, 1);
    render::CommitWar3OwnedImageLayout(m_levelLayouts[i], attach, *m_levels[i], range);
    pc.mode = mode;
    pc.texelX = 1.0f / float(sourceSize.width);
    pc.texelY = 1.0f / float(sourceSize.height);
    draw(m_levelViews[i], attach.newLayout, extentOf(m_levels[i]), mode == Upsample,
         source, source, m_linearSampler->getDescriptor().samplerIndex);
    const auto read = m_levelLayouts[i].plan(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkImageMemoryBarrier2 toRead =
        render::MakeWar3OwnedImageBarrier(read, m_levels[i]->handle(), range);
    barrier(&toRead, 1);
    render::CommitWar3OwnedImageLayout(m_levelLayouts[i], read, *m_levels[i], range);
  };

  level(0, Prefilter, sceneDescriptor, extent);
  for (uint32_t i = 1; i < kLevels; ++i)
    level(i, Downsample, levelDescriptors[i - 1], extentOf(m_levels[i - 1]));
  for (uint32_t i = kLevels - 1; i-- > 0; )
    level(i, Upsample, levelDescriptors[i + 1], extentOf(m_levels[i + 1]));

  // Composite over the whole target: scene point-sampled, bloom linear.
  pc.mode = Composite;
  pc.texelX = pc.texelY = 1.0f;
  draw(frame.colorView, colorAttachmentLayout, extent, false, sceneDescriptor,
       levelDescriptors[0], m_pointSampler->getDescriptor().samplerIndex);

  VkImageMemoryBarrier2 restore = ImageBarrier(colorImage, colorRange,
      colorAttachmentLayout, colorOriginal,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, anyAccess);
  barrier(&restore, 1);
  colorImage->trackLayout(colorRange, colorOriginal);

  // Commands hold raw handles: every owner stays alive through GPU completion.
  ctx->track(colorImage, DxvkAccess::Write);
  ctx->track(m_sceneCopy, DxvkAccess::Write);
  for (const auto& image : m_levels)
    ctx->track(image, DxvkAccess::Write);
  ctx->track(m_linearSampler);
  ctx->track(m_pointSampler);
  ctx->track(m_drawPipeline);
  ctx->track(m_addPipeline);
  return BloomStage::CommandsRecorded;
}

} // namespace dxvk::war3::effects
