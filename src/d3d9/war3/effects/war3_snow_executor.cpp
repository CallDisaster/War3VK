#include "war3_snow_executor.h"

#include "../../../dxvk/dxvk_access.h"
#include "../../../dxvk/dxvk_util.h"
#include "../../../util/util_error.h"

#include <war3_weather_snow_vert.h>
#include <war3_weather_snow_frag.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <new>

namespace dxvk::war3::effects {
namespace {

// Scalar row-major shader ABI, within Vulkan's guaranteed 128-byte limit.
// Packed author parameters are display-only: gameplay must not consume them.
struct SnowPushConstants {
  Matrix4 viewProj;
  Vector4 regionXY; // minX, minY, maxX, maxY
  Vector4 heightTime; // bottomZ, topZ, timeSeconds, snowfall
  Vector4 windAnchor; // bounded integrated displacement XY, camera XY
  uint32_t seed;
  uint32_t depthSampler;
  float cameraZ;
  uint32_t packedMedia; // feather:8, haze:8, lowSnow:8, gust:8
};
static_assert(sizeof(SnowPushConstants) == 128);
static_assert(offsetof(SnowPushConstants, regionXY) == 64);
static_assert(offsetof(SnowPushConstants, heightTime) == 80);
static_assert(offsetof(SnowPushConstants, windAnchor) == 96);
static_assert(offsetof(SnowPushConstants, seed) == 112);

uint32_t Quantize(float value, float maximum, uint32_t steps) {
  return uint32_t(std::lround(std::clamp(value / maximum, 0.0f, 1.0f) *
                             float(steps)));
}

} // namespace

SnowExecutor::SnowExecutor(Rc<DxvkDevice> device)
: m_device(std::move(device)) { }

bool SnowExecutor::ensurePipeline(VkFormat format) {
  if (!m_depthSampler) {
    DxvkSamplerKey key = {};
    key.setFilter(VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                  VK_SAMPLER_MIPMAP_MODE_NEAREST);
    key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    key.setUsePixelCoordinates(false);
    m_depthSampler = m_device->createSampler(key);
  }
  if (!m_layout) {
    const DxvkDescriptorSetLayoutBinding binding(
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    m_layout = m_device->createBuiltInPipelineLayout(
        DxvkPipelineLayoutFlag::UsesSamplerHeap,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        sizeof(SnowPushConstants), 1, &binding);
  }
  if (m_pipeline && m_pipelineFormat == format)
    return true;

  VkPipelineColorBlendAttachmentState blend = {};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  // Preserve the target's authored alpha channel.
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

  util::DxvkBuiltInGraphicsState state = {};
  state.vs = util::DxvkBuiltInShaderStage(war3_weather_snow_vert, nullptr);
  state.fs = util::DxvkBuiltInShaderStage(war3_weather_snow_frag, nullptr);
  state.colorFormat = format;
  state.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  state.cbAttachment = &blend;

  const VkPipeline candidate =
      m_device->createBuiltInGraphicsPipeline(m_layout, state);
  if (candidate == VK_NULL_HANDLE)
    return false;
  // Old command lists retain their own pipeline owner. Format changes and
  // executor teardown cannot destroy a pipeline still referenced by GPU work.
  Rc<render::War3TrackedVkPipeline> candidateOwner;
  try {
    candidateOwner = render::AdoptWar3TrackedVkPipeline(m_device, candidate);
  } catch (...) {
    // Adoption can fail allocating its CPU owner/function. It has not returned
    // an owner and no command references this new raw handle; release only
    // this unadopted candidate, never the previous tracked pipeline.
    m_device->vkd()->vkDestroyPipeline(m_device->vkd()->device(), candidate, nullptr);
    throw;
  }
  m_pipeline = std::move(candidateOwner);
  m_pipelineFormat = format;
  return m_pipeline != nullptr && m_depthSampler != nullptr;
}

void SnowExecutor::clearDepthCopy() {
  m_depthCopyView = nullptr;
  m_depthCopy = nullptr;
  m_depthCopyBytes = 0;
  m_depthCopyLayout.reset();
}

void SnowExecutor::release() {
  m_depthSampleView = nullptr;
  clearDepthCopy();
}

bool SnowExecutor::ensureDepthCopy(VkExtent3D extent, VkFormat format) {
  if (m_depthCopy && m_depthCopy->info().extent.width == extent.width &&
      m_depthCopy->info().extent.height == extent.height &&
      m_depthCopy->info().format == format)
    return true;

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
  info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  info.debugName = "War3 weather depth sample";
  auto candidate = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!candidate || !candidate->storage())
    return false;
  const uint64_t bytes = candidate->storage()->getMemoryInfo().size;
  if (bytes > kMaxDepthCopyBytes)
    return false;

  DxvkImageViewKey key;
  key.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  key.format = format;
  key.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  key.layout = info.layout;
  key.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
  key.mipIndex = 0;
  key.mipCount = 1;
  key.layerIndex = 0;
  key.layerCount = 1;
  const VkComponentMapping swizzle = {
    VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
    VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
  key.packedSwizzle = DxvkImageViewKey::packSwizzle(swizzle);
  auto candidateView = candidate->createView(key);
  if (!candidateView)
    return false;
  // Transactional replacement. Previous command lists own their allocations;
  // no assumed "N frames" retirement is used on resize/format changes.
  m_depthCopy = std::move(candidate);
  m_depthCopyView = std::move(candidateView);
  m_depthCopyBytes = bytes;
  m_depthCopyLayout.reset();
  return true;
}

void SnowExecutor::copyDepth(
    const Rc<DxvkCommandList>& ctx, const Rc<DxvkImageView>& source,
    const VkImageSubresourceRange& sourceRange,
    VkImageLayout sourceOriginal) {
  auto* sourceImage = source->image();
  const VkImageLayout transferRead =
      sourceImage->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  const VkImageLayout transferWrite =
      m_depthCopy->pickLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const auto copyRange = m_depthCopy->getAvailableSubresources();
  // In particular, every reused allocation has an explicit previous fragment
  // read -> transfer-write dependency. Holding an Rc or using a ring slot is
  // not a substitute for this GPU access dependency.
  const auto write = m_depthCopyLayout.plan(transferWrite,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  std::array<VkImageMemoryBarrier2, 2> barriers = {
    ImageBarrier(sourceImage, sourceRange, sourceOriginal, transferRead,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT),
    render::MakeWar3OwnedImageBarrier(write,
        m_depthCopy->handle(), copyRange),
  };
  VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
  dependency.imageMemoryBarrierCount = barriers.size();
  dependency.pImageMemoryBarriers = barriers.data();
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  sourceImage->trackLayout(sourceRange, transferRead);
  render::CommitWar3OwnedImageLayout(m_depthCopyLayout, write,
                                     *m_depthCopy, copyRange);

  VkImageCopy2 region = { VK_STRUCTURE_TYPE_IMAGE_COPY_2 };
  region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT,
    sourceRange.baseMipLevel, sourceRange.baseArrayLayer, 1 };
  region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
  region.extent = m_depthCopy->info().extent;
  VkCopyImageInfo2 copy = { VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2 };
  copy.srcImage = sourceImage->handle();
  copy.srcImageLayout = transferRead;
  copy.dstImage = m_depthCopy->handle();
  copy.dstImageLayout = transferWrite;
  copy.regionCount = 1;
  copy.pRegions = &region;
  ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copy);

  const auto read = m_depthCopyLayout.plan(
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  barriers[0] = ImageBarrier(sourceImage, sourceRange, transferRead,
      sourceOriginal, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
  barriers[1] = render::MakeWar3OwnedImageBarrier(read,
      m_depthCopy->handle(), copyRange);
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  sourceImage->trackLayout(sourceRange, sourceOriginal);
  render::CommitWar3OwnedImageLayout(m_depthCopyLayout, read,
                                     *m_depthCopy, copyRange);
  ctx->track(sourceImage, DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Write);
}

SnowResult SnowExecutor::run(const Rc<DxvkCommandList>& ctx,
                             const EffectFrame& frame,
                             const SnowFrame& input,
                             EffectRecording* recording) {
  const auto& weather = input.snow;
  SnowResult result;
  result.resourceBytes = m_depthCopyBytes;
  if (frame.depthView) {
    const auto diagnosticDepthExtent = frame.depthView->mipLevelExtent(0);
    result.depthWidth = diagnosticDepthExtent.width;
    result.depthHeight = diagnosticDepthExtent.height;
  }
  const auto reject = [&](SnowStage stage) {
    result.stage = stage;
    return result;
  };

  if (!(weather.snowfall > 0.0f))
    return reject(SnowStage::NoSnowfall);
  if (!ctx || !frame.colorView || !frame.depthView)
    return reject(SnowStage::MissingTargets);
  if (frame.frameSerial == 0 || input.frameSerial != frame.frameSerial ||
      frame.mapEpoch == 0 || frame.deviceEpoch == 0 ||
      input.mapEpoch != frame.mapEpoch ||
      input.deviceEpoch != frame.deviceEpoch)
    return reject(SnowStage::EpochMismatch);
  if (m_lastFrameSerial == frame.frameSerial &&
      m_lastMapEpoch == frame.mapEpoch &&
      m_lastDeviceEpoch == frame.deviceEpoch)
    return reject(SnowStage::DuplicateFrame);
  const auto& camera = frame.camera;
  if (!camera.valid || camera.frameSerial != frame.frameSerial)
    return reject(SnowStage::CameraUnavailable);
  if (!MatrixFinite(camera.view) || !MatrixFinite(camera.proj) ||
      !MatrixFinite(camera.viewProj) ||
      !std::isfinite(determinant(camera.view)) ||
      std::abs(determinant(camera.view)) < 1.0e-12f)
    return reject(SnowStage::CameraInvalid);
  const Matrix4 inverseView = inverse(camera.view);
  const Vector4 cameraH = inverseView * Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  if (!MatrixFinite(inverseView) || !std::isfinite(cameraH.w) ||
      std::abs(cameraH.w) < 1.0e-6f || !std::isfinite(cameraH.x) ||
      !std::isfinite(cameraH.y) || !std::isfinite(cameraH.z))
    return reject(SnowStage::CameraInvalid);

  auto* colorImage = frame.colorView->image();
  auto* depthImage = frame.depthView->image();
  const auto& colorInfo = colorImage->info();
  const auto& depthInfo = depthImage->info();
  if (colorInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT ||
      depthInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT)
    return reject(SnowStage::UnsupportedSamples);
  const auto colorRange = frame.colorView->imageSubresources();
  auto depthRange = frame.depthView->imageSubresources();
  const auto extent = frame.colorView->mipLevelExtent(0);
  const auto depthExtent = frame.depthView->mipLevelExtent(0);
  const auto depthFormat = lookupFormatInfo(depthInfo.format);
  if (colorImage == depthImage ||
      !(colorInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
      colorRange.layerCount != 1 || colorRange.levelCount != 1 ||
      extent.width == 0 || extent.height == 0)
    return reject(SnowStage::UnsupportedTarget);
  if (!depthFormat || !(depthFormat->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
      !(depthInfo.usage & (VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ||
      !(depthRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
      depthRange.layerCount != 1 || depthRange.levelCount != 1 ||
      !(m_device->getFormatFeatures(depthInfo.format).optimal &
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) ||
      depthExtent.width != extent.width ||
      depthExtent.height != extent.height)
    return reject(SnowStage::UnsupportedDepth);
  // When depth+stencil share the allocation, transition both aspects. This
  // works without separateDepthStencilLayouts and restores the exact previous
  // combined layout, including DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL.
  depthRange.aspectMask = depthFormat->aspectMask;
  const VkImageLayout depthOriginal = depthImage->queryLayout(depthRange);
  const VkImageLayout colorOriginal = colorImage->queryLayout(colorRange);
  if (!LayoutKnown(depthOriginal) || !LayoutKnown(colorOriginal))
    return reject(SnowStage::UndefinedLayout);
  if (camera.width == 0 || camera.height == 0 || camera.x >= extent.width ||
      camera.y >= extent.height || camera.width > extent.width - camera.x ||
      camera.height > extent.height - camera.y || !std::isfinite(camera.minZ) ||
      !std::isfinite(camera.maxZ) || camera.minZ < 0.0f || camera.minZ > 1.0f ||
      camera.maxZ < 0.0f || camera.maxZ > 1.0f || camera.maxZ <= camera.minZ)
    return reject(SnowStage::CameraInvalid);
  float farClearRaw = 0.0f;
  if (!InferFarClearRaw(camera.proj, camera.minZ, camera.maxZ, farClearRaw))
    return reject(SnowStage::CameraInvalid);
  const bool reversedDepth =
      farClearRaw < (camera.minZ + camera.maxZ) * 0.5f;

  const VkImageLayout colorAttachmentLayout =
      colorImage->pickLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  const bool directDepth = (depthInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
  auto* sampledDepthImage = depthImage;
  auto sampledDepthRange = depthRange;
  VkImageLayout sampledDepthOriginal = depthOriginal;
  VkImageLayout readLayout =
      depthImage->pickLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  if (!directDepth) {
    const uint64_t minimumBytes = uint64_t(extent.width) * extent.height *
                                  depthFormat->elementSize;
    if (minimumBytes > kMaxDepthCopyBytes)
      return reject(SnowStage::DepthCopyBudget);
  }
  const render::WeatherResourceRetry::Key resourceKey = {
      frame.mapEpoch, frame.deviceEpoch, extent.width, extent.height,
      uint32_t(frame.colorView->info().format), uint32_t(depthInfo.format)};
  if (!m_resourceRetry.ready(resourceKey, frame.frameSerial))
    return reject(SnowStage::ResourceRetryWait);
  const auto failPreparation = [&](SnowStage stage) {
    m_resourceRetry.failed(resourceKey, frame.frameSerial);
    result.resourceBytes = m_depthCopyBytes;
    return reject(stage);
  };
  const DxvkDescriptor* depthDescriptor = nullptr;
  // Complete optional resource preparation before any copy/barrier/rendering
  // command. Only this region has a recoverable allocation exception boundary.
  try {
    if (!ensurePipeline(frame.colorView->info().format))
      return failPreparation(SnowStage::PipelineUnavailable);
    if (directDepth) {
      DxvkImageViewKey sampleKey = frame.depthView->info();
      sampleKey.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      sampleKey.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      sampleKey.layout = readLayout;
      sampleKey.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
      const VkComponentMapping swizzle = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
      sampleKey.packedSwizzle = DxvkImageViewKey::packSwizzle(swizzle);
      m_depthSampleView = depthImage->createView(sampleKey);
      clearDepthCopy();
    } else {
      if (!ensureDepthCopy(extent, depthInfo.format))
        return failPreparation(SnowStage::DepthCopyUnavailable);
      m_depthSampleView = m_depthCopyView;
    }
    if (m_depthSampleView)
      depthDescriptor = m_depthSampleView->getDescriptor();
    // Attachment handle() can also lazily allocate its Vulkan view. Prepare
    // that descriptor now, before changing the input color's actual layout.
    if (!depthDescriptor || !frame.colorView->getDescriptor())
      return failPreparation(SnowStage::UnsupportedDepth);
    m_resourceRetry.clear();
  } catch (const DxvkError&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    return failPreparation(SnowStage::ResourceAllocationFailed);
  } catch (const std::bad_alloc&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    return failPreparation(SnowStage::ResourceHostAllocationFailed);
  }

  if (recording)
    recording->beginRecording(ctx);
  if (!directDepth) {
    copyDepth(ctx, frame.depthView, depthRange, depthOriginal);
    sampledDepthImage = m_depthCopy.ptr();
    sampledDepthRange = m_depthCopy->getAvailableSubresources();
    sampledDepthOriginal = m_depthCopyLayout.layout();
    readLayout = sampledDepthOriginal;
  }
  result.resourceBytes = m_depthCopyBytes;

  std::array<VkImageMemoryBarrier2, 2> barriers = {
    ImageBarrier(sampledDepthImage, sampledDepthRange,
        sampledDepthOriginal, readLayout,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT),
    ImageBarrier(colorImage, colorRange, colorOriginal,
        colorAttachmentLayout,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT),
  };
  VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
  dependency.imageMemoryBarrierCount = barriers.size();
  dependency.pImageMemoryBarriers = barriers.data();
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  sampledDepthImage->trackLayout(sampledDepthRange, readLayout);
  colorImage->trackLayout(colorRange, colorAttachmentLayout);

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
    float(camera.x) + 0.5f, float(camera.y) + 0.5f,
    float(camera.width), float(camera.height),
    camera.minZ, std::clamp(std::max(camera.maxZ,
        camera.minZ + (camera.minZ < 0.5f ? 0.001f : 0.0f)), 0.0f, 1.0f) };
  const VkRect2D scissor = {
    { int32_t(camera.x), int32_t(camera.y) }, { camera.width, camera.height } };
  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);
  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
      VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());

  SnowPushConstants pc = {};
  pc.viewProj = camera.viewProj;
  pc.regionXY = Vector4(weather.minX, weather.minY,
                       weather.maxX, weather.maxY);
  pc.heightTime = Vector4(weather.bottomZ, weather.topZ,
      float(weather.timeSeconds), weather.snowfall);
  // The spatial pattern repeats only every 8192 * 96 world units. Reducing
  // accumulated displacement by this period keeps float precision bounded;
  // hashes wrap by the same 8192-cell period, preserving particle identity.
  constexpr double kDisplacementPeriod = 8192.0 * 96.0;
  pc.windAnchor = Vector4(
      float(std::fmod(weather.windDisplacementX, kDisplacementPeriod)),
      float(std::fmod(weather.windDisplacementY, kDisplacementPeriod)),
      cameraH.x / cameraH.w, cameraH.y / cameraH.w);
  pc.seed = weather.seed;
  // Sampler heap indices are uint16_t. Bit 31 carries exact depth polarity.
  pc.depthSampler = m_depthSampler->getDescriptor().samplerIndex |
      (reversedDepth ? 0x80000000u : 0u);
  pc.cameraZ = cameraH.z / cameraH.w;
  pc.packedMedia = Quantize(weather.edgeFeather, 0.5f, 255) |
      (Quantize(weather.haze, 1.0f, 255) << 8) |
      (Quantize(weather.lowSnow, 1.0f, 255) << 16) |
      (Quantize(weather.gust, 1.0f, 255) << 24);

  DxvkDescriptorWrite descriptor = {};
  descriptor.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor.descriptor = depthDescriptor;
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_layout, 1, &descriptor,
                     sizeof(pc), &pc);
  ctx->cmdDraw(6, kMaxParticles, 0, 0);
  ctx->cmdEndRendering();

  barriers[0] = ImageBarrier(sampledDepthImage, sampledDepthRange,
      readLayout, sampledDepthOriginal,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
  barriers[1] = ImageBarrier(colorImage, colorRange,
      colorAttachmentLayout, colorOriginal,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  sampledDepthImage->trackLayout(sampledDepthRange, sampledDepthOriginal);
  colorImage->trackLayout(colorRange, colorOriginal);
  // Commands contain raw handles/descriptors. All actual owners are retained
  // through GPU completion; replacing cached views does not release backing.
  ctx->track(depthImage, DxvkAccess::Read);
  ctx->track(sampledDepthImage, DxvkAccess::Read);
  ctx->track(colorImage, DxvkAccess::Write);
  ctx->track(m_depthSampler);
  ctx->track(m_pipeline);

  m_lastFrameSerial = frame.frameSerial;
  m_lastMapEpoch = frame.mapEpoch;
  m_lastDeviceEpoch = frame.deviceEpoch;
  result.stage = SnowStage::CommandsRecorded;
  result.activeParticles = uint32_t(
      float(kMaxParticles) * std::clamp(weather.snowfall, 0.0f, 1.0f));
  result.submitted = true;
  return result;
}

} // namespace dxvk::war3::effects
