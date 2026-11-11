#include "d3d9_war3_weather.h"
#include "d3d9_device.h"
#include "war3/tools/war3_perf_monitor.h"
#include "war3/render/war3_hybrid_ray_tracing.h"

#include "../dxvk/dxvk_access.h"
#include "../dxvk/dxvk_util.h"
#include "../util/util_error.h"

#include <war3_weather_snow_vert.h>
#include <war3_weather_snow_frag.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <new>

namespace dxvk {
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

std::mutex s_diagnosticsMutex;
War3WeatherDiagnostics s_diagnostics;

void PublishDiagnostics(const War3WeatherDiagnostics& value) {
  std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
  s_diagnostics = value;
}

uint32_t Quantize(float value, float maximum, uint32_t steps) {
  return uint32_t(std::lround(std::clamp(value / maximum, 0.0f, 1.0f) *
                             float(steps)));
}

bool MatrixFinite(const Matrix4& matrix) {
  for (uint32_t row = 0; row < 4; ++row)
    for (uint32_t column = 0; column < 4; ++column)
      if (!std::isfinite(matrix[row][column]))
        return false;
  return true;
}

VkImageMemoryBarrier2 ImageBarrier(
    DxvkImage* image, const VkImageSubresourceRange& range,
    VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags2 srcStages, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStages, VkAccessFlags2 dstAccess) {
  VkImageMemoryBarrier2 result = {
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
  result.srcStageMask = srcStages;
  result.srcAccessMask = srcAccess;
  result.dstStageMask = dstStages;
  result.dstAccessMask = dstAccess;
  result.oldLayout = from;
  result.newLayout = to;
  result.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  result.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  result.image = image->handle();
  result.subresourceRange = range;
  return result;
}

} // namespace

War3WeatherDiagnostics QueryWar3WeatherDiagnostics() {
  std::lock_guard<std::mutex> lock(s_diagnosticsMutex);
  return s_diagnostics;
}

const char* War3WeatherDrawStageName(War3WeatherDrawStage stage) noexcept {
  switch (stage) {
    case War3WeatherDrawStage::Disabled: return "disabled";
    case War3WeatherDrawStage::NoSnowfall: return "no-snowfall";
    case War3WeatherDrawStage::MissingTargets: return "missing-targets";
    case War3WeatherDrawStage::EpochMismatch: return "epoch-mismatch";
    case War3WeatherDrawStage::CameraUnavailable: return "camera-unavailable";
    case War3WeatherDrawStage::CameraInvalid: return "camera-invalid";
    case War3WeatherDrawStage::UnsupportedSamples: return "unsupported-msaa";
    case War3WeatherDrawStage::UnsupportedTarget: return "unsupported-color";
    case War3WeatherDrawStage::UnsupportedDepth: return "unsupported-depth";
    case War3WeatherDrawStage::DepthCopyBudget: return "depth-copy-budget";
    case War3WeatherDrawStage::DepthCopyUnavailable: return "depth-copy-unavailable";
    case War3WeatherDrawStage::UndefinedLayout: return "undefined-layout";
    case War3WeatherDrawStage::PipelineUnavailable: return "pipeline-unavailable";
    case War3WeatherDrawStage::DuplicateFrame: return "duplicate-frame";
    case War3WeatherDrawStage::CommandsRecorded: return "commands-recorded";
    case War3WeatherDrawStage::ResourceRetryWait: return "resource-retry-wait";
    case War3WeatherDrawStage::ResourceAllocationFailed: return "resource-allocation-failed";
    case War3WeatherDrawStage::ResourceHostAllocationFailed: return "resource-host-allocation-failed";
  }
  return "unknown";
}

War3WeatherPass::War3WeatherPass(D3D9DeviceEx* parent)
: m_device(parent->GetDXVKDevice()) { }

bool War3WeatherPass::ensurePipeline(VkFormat format) {
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
  // pass teardown cannot destroy a pipeline still referenced by GPU work.
  Rc<war3::render::War3TrackedVkPipeline> candidateOwner;
  try {
    candidateOwner = war3::render::AdoptWar3TrackedVkPipeline(m_device, candidate);
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

void War3WeatherPass::clearDepthCopy() {
  m_depthCopyView = nullptr;
  m_depthCopy = nullptr;
  m_depthCopyBytes = 0;
  m_depthCopyLayout.reset();
}

bool War3WeatherPass::ensureDepthCopy(VkExtent3D extent, VkFormat format) {
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

void War3WeatherPass::copyDepth(
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
    war3::render::MakeWar3OwnedImageBarrier(write,
        m_depthCopy->handle(), copyRange),
  };
  VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
  dependency.imageMemoryBarrierCount = barriers.size();
  dependency.pImageMemoryBarriers = barriers.data();
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  sourceImage->trackLayout(sourceRange, transferRead);
  war3::render::CommitWar3OwnedImageLayout(m_depthCopyLayout, write,
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
  barriers[1] = war3::render::MakeWar3OwnedImageBarrier(read,
      m_depthCopy->handle(), copyRange);
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  sourceImage->trackLayout(sourceRange, sourceOriginal);
  war3::render::CommitWar3OwnedImageLayout(m_depthCopyLayout, read,
                                         *m_depthCopy, copyRange);
  ctx->track(sourceImage, DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Write);
}

void War3WeatherPass::Run(const Rc<DxvkCommandList>& ctx,
                         const War3PipelineInput& input) {
  const auto& weather = input.weather;
  if (!weather.enabled || weather.id == 0) {
    // An unused weather system neither creates GPU objects nor takes the
    // diagnostics lock per frame. Publish only an actual enabled->off edge.
    if (m_wasEnabled) {
      War3WeatherDiagnostics disabled;
      disabled.frozenInput = weather;
      disabled.frameSerial = input.frameSerial;
      disabled.mapEpoch = input.mapEpoch;
      disabled.deviceEpoch = input.deviceEpoch;
      disabled.id = weather.id;
      disabled.timeSeconds = weather.timeSeconds;
      PublishDiagnostics(disabled);
      m_wasEnabled = false;
      m_depthSampleView = nullptr;
      clearDepthCopy();
    }
    return;
  }
  m_wasEnabled = true;
  War3WeatherDiagnostics diagnostics;
  diagnostics.frozenInput = weather;
  diagnostics.frameSerial = input.frameSerial;
  diagnostics.mapEpoch = input.mapEpoch;
  diagnostics.deviceEpoch = input.deviceEpoch;
  diagnostics.id = weather.id;
  diagnostics.timeSeconds = weather.timeSeconds;
  diagnostics.resourceBytes = m_depthCopyBytes;
  diagnostics.worldViewport = input.scene.worldCamera.viewport;
  diagnostics.cameraFrameMatches = input.scene.worldCamera.valid &&
    input.scene.worldCamera.frameSerial == input.frameSerial;
  if (input.depthView) {
    const auto diagnosticDepthExtent = input.depthView->mipLevelExtent(0);
    diagnostics.depthWidth = diagnosticDepthExtent.width;
    diagnostics.depthHeight = diagnosticDepthExtent.height;
  }
  const auto reject = [&](War3WeatherDrawStage stage) {
    diagnostics.stage = stage;
    PublishDiagnostics(diagnostics);
  };

  if (!(weather.params.snowfall > 0.0f)) {
    reject(War3WeatherDrawStage::NoSnowfall);
    return;
  }
  if (!ctx || !input.colorView || !input.depthView) {
    reject(War3WeatherDrawStage::MissingTargets);
    return;
  }
  if (input.frameSerial == 0 || weather.frameSerial != input.frameSerial ||
      input.mapEpoch == 0 || input.deviceEpoch == 0 ||
      weather.mapEpoch != input.mapEpoch ||
      weather.deviceEpoch != input.deviceEpoch) {
    reject(War3WeatherDrawStage::EpochMismatch);
    return;
  }
  if (m_lastFrameSerial == input.frameSerial &&
      m_lastMapEpoch == input.mapEpoch &&
      m_lastDeviceEpoch == input.deviceEpoch) {
    reject(War3WeatherDrawStage::DuplicateFrame);
    return;
  }
  const auto& camera = input.scene.worldCamera;
  if (!camera.valid || camera.frameSerial != input.frameSerial) {
    reject(War3WeatherDrawStage::CameraUnavailable);
    return;
  }
  if (!MatrixFinite(camera.view) || !MatrixFinite(camera.proj) ||
      !MatrixFinite(camera.viewProj) ||
      !std::isfinite(determinant(camera.view)) ||
      std::abs(determinant(camera.view)) < 1.0e-12f) {
    reject(War3WeatherDrawStage::CameraInvalid);
    return;
  }
  const Matrix4 inverseView = inverse(camera.view);
  const Vector4 cameraH = inverseView * Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  if (!MatrixFinite(inverseView) || !std::isfinite(cameraH.w) ||
      std::abs(cameraH.w) < 1.0e-6f || !std::isfinite(cameraH.x) ||
      !std::isfinite(cameraH.y) || !std::isfinite(cameraH.z)) {
    reject(War3WeatherDrawStage::CameraInvalid);
    return;
  }

  auto* colorImage = input.colorView->image();
  auto* depthImage = input.depthView->image();
  const auto& colorInfo = colorImage->info();
  const auto& depthInfo = depthImage->info();
  if (colorInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT ||
      depthInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT) {
    reject(War3WeatherDrawStage::UnsupportedSamples);
    return;
  }
  const auto colorRange = input.colorView->imageSubresources();
  auto depthRange = input.depthView->imageSubresources();
  const auto extent = input.colorView->mipLevelExtent(0);
  const auto depthExtent = input.depthView->mipLevelExtent(0);
  const auto depthFormat = lookupFormatInfo(depthInfo.format);
  if (colorImage == depthImage ||
      !(colorInfo.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
      colorRange.layerCount != 1 || colorRange.levelCount != 1 ||
      extent.width == 0 || extent.height == 0) {
    reject(War3WeatherDrawStage::UnsupportedTarget);
    return;
  }
  if (!depthFormat || !(depthFormat->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
      !(depthInfo.usage & (VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ||
      !(depthRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
      depthRange.layerCount != 1 || depthRange.levelCount != 1 ||
      !(m_device->getFormatFeatures(depthInfo.format).optimal &
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) ||
      depthExtent.width != extent.width ||
      depthExtent.height != extent.height) {
    reject(War3WeatherDrawStage::UnsupportedDepth);
    return;
  }
  // When depth+stencil share the allocation, transition both aspects. This
  // works without separateDepthStencilLayouts and restores the exact previous
  // combined layout, including DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL.
  depthRange.aspectMask = depthFormat->aspectMask;
  const VkImageLayout depthOriginal = depthImage->queryLayout(depthRange);
  const VkImageLayout colorOriginal = colorImage->queryLayout(colorRange);
  if (depthOriginal == VK_IMAGE_LAYOUT_UNDEFINED ||
      depthOriginal == VK_IMAGE_LAYOUT_PREINITIALIZED ||
      depthOriginal == VK_IMAGE_LAYOUT_MAX_ENUM ||
      colorOriginal == VK_IMAGE_LAYOUT_UNDEFINED ||
      colorOriginal == VK_IMAGE_LAYOUT_PREINITIALIZED ||
      colorOriginal == VK_IMAGE_LAYOUT_MAX_ENUM) {
    reject(War3WeatherDrawStage::UndefinedLayout);
    return;
  }
  const auto& vp = camera.viewport;
  if (vp.Width == 0 || vp.Height == 0 || vp.X >= extent.width ||
      vp.Y >= extent.height || vp.Width > extent.width - vp.X ||
      vp.Height > extent.height - vp.Y || !std::isfinite(vp.MinZ) ||
      !std::isfinite(vp.MaxZ) || vp.MinZ < 0.0f || vp.MinZ > 1.0f ||
      vp.MaxZ < 0.0f || vp.MaxZ > 1.0f || vp.MaxZ <= vp.MinZ) {
    reject(War3WeatherDrawStage::CameraInvalid);
    return;
  }
  float farClearRaw = 0.0f;
  if (!war3::render::InferWar3FarClearRaw(camera, farClearRaw)) {
    reject(War3WeatherDrawStage::CameraInvalid);
    return;
  }
  const bool reversedDepth =
      farClearRaw < (vp.MinZ + vp.MaxZ) * 0.5f;

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
    if (minimumBytes > kMaxDepthCopyBytes) {
      reject(War3WeatherDrawStage::DepthCopyBudget);
      return;
    }
  }
  const war3::render::WeatherResourceRetry::Key resourceKey = {
      input.mapEpoch, input.deviceEpoch, extent.width, extent.height,
      uint32_t(input.colorView->info().format), uint32_t(depthInfo.format)};
  if (!m_resourceRetry.ready(resourceKey, input.frameSerial)) {
    reject(War3WeatherDrawStage::ResourceRetryWait);
    return;
  }
  const auto failPreparation = [&](War3WeatherDrawStage stage) {
    m_resourceRetry.failed(resourceKey, input.frameSerial);
    diagnostics.resourceBytes = m_depthCopyBytes;
    reject(stage);
  };
  const DxvkDescriptor* depthDescriptor = nullptr;
  // Complete optional resource preparation before any copy/barrier/rendering
  // command. Only this region has a recoverable allocation exception boundary.
  try {
    if (!ensurePipeline(input.colorView->info().format)) {
      failPreparation(War3WeatherDrawStage::PipelineUnavailable);
      return;
    }
    if (directDepth) {
      DxvkImageViewKey sampleKey = input.depthView->info();
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
      if (!ensureDepthCopy(extent, depthInfo.format)) {
        failPreparation(War3WeatherDrawStage::DepthCopyUnavailable);
        return;
      }
      m_depthSampleView = m_depthCopyView;
    }
    if (m_depthSampleView)
      depthDescriptor = m_depthSampleView->getDescriptor();
    // Attachment handle() can also lazily allocate its Vulkan view. Prepare
    // that descriptor now, before changing the input color's actual layout.
    if (!depthDescriptor || !input.colorView->getDescriptor()) {
      failPreparation(War3WeatherDrawStage::UnsupportedDepth);
      return;
    }
    m_resourceRetry.clear();
  } catch (const DxvkError&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    failPreparation(War3WeatherDrawStage::ResourceAllocationFailed);
    return;
  } catch (const std::bad_alloc&) {
    if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)
      throw;
    failPreparation(War3WeatherDrawStage::ResourceHostAllocationFailed);
    return;
  }

  auto perfScope = war3::War3PerfMonitor::instance().scope("Weather/Snow", ctx);
  if (!directDepth) {
    copyDepth(ctx, input.depthView, depthRange, depthOriginal);
    sampledDepthImage = m_depthCopy.ptr();
    sampledDepthRange = m_depthCopy->getAvailableSubresources();
    sampledDepthOriginal = m_depthCopyLayout.layout();
    readLayout = sampledDepthOriginal;
  }
  diagnostics.resourceBytes = m_depthCopyBytes;

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
  attachment.imageView = input.colorView->handle();
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
    float(vp.X) + 0.5f, float(vp.Y) + 0.5f,
    float(vp.Width), float(vp.Height),
    vp.MinZ, std::clamp(std::max(vp.MaxZ,
        vp.MinZ + (vp.MinZ < 0.5f ? 0.001f : 0.0f)), 0.0f, 1.0f) };
  const VkRect2D scissor = {
    { int32_t(vp.X), int32_t(vp.Y) }, { vp.Width, vp.Height } };
  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);
  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
      VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());

  SnowPushConstants pc = {};
  pc.viewProj = camera.viewProj;
  pc.regionXY = Vector4(weather.region.minX, weather.region.minY,
                       weather.region.maxX, weather.region.maxY);
  pc.heightTime = Vector4(weather.region.bottomZ, weather.region.topZ,
      float(weather.timeSeconds), weather.params.snowfall);
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
  pc.packedMedia = Quantize(weather.region.edgeFeather, 0.5f, 255) |
      (Quantize(weather.params.haze, 1.0f, 255) << 8) |
      (Quantize(weather.params.lowSnow, 1.0f, 255) << 16) |
      (Quantize(weather.params.gust, 1.0f, 255) << 24);

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

  m_lastFrameSerial = input.frameSerial;
  m_lastMapEpoch = input.mapEpoch;
  m_lastDeviceEpoch = input.deviceEpoch;
  diagnostics.stage = War3WeatherDrawStage::CommandsRecorded;
  diagnostics.activeParticles = uint32_t(
      float(kMaxParticles) * std::clamp(weather.params.snowfall, 0.0f, 1.0f));
  diagnostics.submitted = true;
  PublishDiagnostics(diagnostics);
}

} // namespace dxvk
