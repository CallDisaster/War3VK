#include "d3d9_war3_shadow.h"
#include "d3d9_war3_debug.h"

#include "war3/render/war3_render_objects.h"

#include "../util/util_bit.h"
#include "../dxvk/dxvk_adapter.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dxvk {
namespace {

uint64_t War3HashMatrixForShadowUpload(uint64_t hash, const Matrix4& matrix) {
  for (uint32_t row = 0; row < 4u; ++row) {
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].x));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].y));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].z));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].w));
  }
  return hash;
}

uint64_t War3ComputeShadowMatrixUploadKey(
    const War3PipelineInput& input,
    const std::vector<const War3ShadowCasterDraw*>* replayDraws) {
  const uint32_t paletteCount =
      static_cast<uint32_t>(input.scene.shadowPalettes.size());
  const uint32_t casterCount =
      replayDraws != nullptr
          ? static_cast<uint32_t>(replayDraws->size())
          : static_cast<uint32_t>(input.scene.shadowCasters.size());

  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, paletteCount);
  hash = bit::fnv1a_iter(hash, casterCount);
  hash = bit::fnv1a_iter(hash, input.scene.shadowStats.dynamicPoseSignature);

  for (uint32_t i = 0; i < paletteCount; ++i)
    hash = bit::fnv1a_iter(
        bit::fnv1a_iter(hash, input.scene.shadowPalettes[i].hash),
        input.scene.shadowPalettes[i].worldMatrices.size());

  for (uint32_t i = 0; i < casterCount; ++i) {
    const auto& draw =
        replayDraws != nullptr ? *(*replayDraws)[i] : input.scene.shadowCasters[i];
    hash = bit::fnv1a_iter(hash, uint32_t(draw.vertexBlendEnabled ? 1u : 0u));
    hash = bit::fnv1a_iter(hash, uint32_t(draw.vertexBlendIndexed ? 1u : 0u));
    hash = bit::fnv1a_iter(hash, uint32_t(draw.vertexBlendCount));
    hash = bit::fnv1a_iter(hash, draw.paletteIndex);
    hash = War3HashMatrixForShadowUpload(hash, draw.worldMatrix);
  }

  return hash;
}

}

bool War3ShadowReceiverPass::GetVolumetricShadowSnapshot(
    uint64_t expectedFrameSerial, Rc<DxvkImageView>& outShadowMapView,
    War3CsmData& outCsmData,
    uint32_t& outShadowResolution, Vector4& outSunDir,
    Vector4& outWorldUp) const {
  if (expectedFrameSerial == 0u ||
      m_shadowPublicationSettledFrameSerial != expectedFrameSerial ||
      !m_hasCompleteShadowMap || !m_shadowMapSampleView ||
      m_csmData.cascadeCount == 0u) {
    outShadowMapView = nullptr;
    outCsmData = {};
    outShadowResolution = 0u;
    outSunDir = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
    outWorldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
    return false;
  }

  outShadowMapView = m_shadowMapSampleView;
  outCsmData = m_csmData;
  // Report the actual allocated image size. Adaptive resolution may differ
  // from the configured target; using the target shifts volumetric PCF taps.
  outShadowResolution = std::max<uint32_t>(m_shadowMapResolution, 1u);
  outSunDir = m_csmData.lightDirection;
  outWorldUp = m_csmData.worldUp;
  return true;
}

bool War3ShadowReceiverPass::GetVolumetricSunShadowSnapshot(
    uint64_t expectedFrameSerial,
    War3VolumetricSunShadowSnapshot& outSnapshot) const {
  outSnapshot.depthView = nullptr;
  outSnapshot.lightViewProj[0] = Matrix4();
  outSnapshot.lightViewProj[1] = Matrix4();
  outSnapshot.lightDirection = Vector4(0.0f, 0.0f, -1.0f, 0.0f);
  outSnapshot.worldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
  outSnapshot.resolution = 0u;
  outSnapshot.cascadeCount = 1u;
  outSnapshot.frameSerial = 0u;
  outSnapshot.softRadius = 1.35f;
  outSnapshot.receiverBias = 0.006f;
  outSnapshot.radiusNear = 0.0f;
  outSnapshot.radiusFar = 0.0f;
  if (expectedFrameSerial == 0u ||
      !m_volumeSunShadowReady ||
      m_volumeSunPublishedFrameSerial != expectedFrameSerial ||
      !m_volumeSunShadowSampleView ||
      m_volumeSunShadowResolution == 0u ||
      !m_volumeSunOrthoFar.valid) {
    return false;
  }

  outSnapshot.depthView = m_volumeSunShadowSampleView;
  outSnapshot.lightViewProj[0] = m_volumeSunLightViewProj[0];
  outSnapshot.lightViewProj[1] = m_volumeSunLightViewProj[1];
  outSnapshot.lightDirection = m_volumeSunOrthoFar.lightDirection;
  outSnapshot.worldUp = m_volumeSunOrthoFar.worldUp;
  outSnapshot.resolution = m_volumeSunShadowResolution;
  outSnapshot.cascadeCount =
      (m_volumeSunOrthoNear.valid && m_volumeSunOrthoFar.valid) ? 2u : 1u;
  outSnapshot.frameSerial = m_volumeSunPublishedFrameSerial;
  outSnapshot.softRadius = m_volumeSunSoftRadius;
  outSnapshot.receiverBias = m_volumeSunReceiverBias;
  outSnapshot.radiusNear = m_volumeSunOrthoNear.valid
                               ? m_volumeSunOrthoNear.radius
                               : m_volumeSunOrthoFar.radius;
  outSnapshot.radiusFar = m_volumeSunOrthoFar.radius;
  return outSnapshot.valid();
}

void War3ShadowReceiverPass::invalidateVolumeSunShadowPublication() {
  m_volumeSunShadowReady = false;
  m_volumeSunPublishedFrameSerial = 0u;
  m_volumeSunOrthoNear = {};
  m_volumeSunOrthoFar = {};
  m_volumeSunLightViewProj[0] = {};
  m_volumeSunLightViewProj[1] = {};
}

void War3ShadowReceiverPass::ensureVolumeSunShadowResources(
    uint32_t resolution) {
  resolution = std::max<uint32_t>(std::min<uint32_t>(resolution, 2048u), 256u);
  constexpr uint32_t kLayers = 2u;
  if (m_volumeSunShadowMap && m_volumeSunShadowSampleView &&
      m_volumeSunShadowLayerViews[0] && m_volumeSunShadowLayerViews[1] &&
      m_volumeSunShadowResolution == resolution &&
      m_volumeSunShadowLayers == kLayers)
    return;

  m_volumeSunShadowResolution = resolution;
  m_volumeSunShadowLayers = kLayers;
  m_volumeSunShadowMap = nullptr;
  m_volumeSunShadowSampleView = nullptr;
  m_volumeSunShadowLayerViews = {};
  invalidateVolumeSunShadowPublication();

  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_D32_SFLOAT;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = VkExtent3D{resolution, resolution, 1u};
  info.numLayers = kLayers;
  info.mipLevels = 1;
  info.usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  info.access =
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  info.debugName = "War3VolumeSunShadow";

  m_volumeSunShadowMap =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_volumeSunShadowLayout.reset();

  VkComponentMapping mapping = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

  {
    DxvkImageViewKey viewInfo;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = info.format;
    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    viewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.mipIndex = 0;
    viewInfo.mipCount = 1;
    viewInfo.layerIndex = 0;
    viewInfo.layerCount = kLayers;
    viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
    m_volumeSunShadowSampleView = m_volumeSunShadowMap->createView(viewInfo);
  }
  for (uint32_t i = 0; i < kLayers; ++i) {
    DxvkImageViewKey layerViewInfo;
    layerViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    layerViewInfo.format = info.format;
    layerViewInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    layerViewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    layerViewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
    layerViewInfo.mipIndex = 0;
    layerViewInfo.mipCount = 1;
    layerViewInfo.layerIndex = i;
    layerViewInfo.layerCount = 1;
    layerViewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
    m_volumeSunShadowLayerViews[i] =
        m_volumeSunShadowMap->createView(layerViewInfo);
  }
}

bool War3ShadowReceiverPass::GetVolumetricPointShadowSnapshot(
    uint64_t expectedLightGeneration, uint64_t expectedFrameSerial,
    War3VolumetricPointShadowSnapshot& outSnapshot) const {
  outSnapshot = {};
  if (expectedLightGeneration == 0u || expectedFrameSerial == 0u ||
      !m_pointShadowEnabled ||
      !pointShadowPublishedStateMatchesCurrentPlan() ||
      m_pointShadowCpuPlan.lightGeneration != expectedLightGeneration ||
      m_pointShadowCpuPlan.lightFrameSerial != expectedFrameSerial ||
      m_pointShadowPublishedLightGeneration != expectedLightGeneration ||
      m_pointShadowPublishedLightCount == 0u ||
      m_pointShadowResolution == 0u) {
    return false;
  }

  const Rc<DxvkSampler> pointShadowSampler =
      m_shadowSampler;
  if (!pointShadowSampler || !m_pointShadowCubeView)
    return false;

  War3VolumetricPointShadowSnapshot snapshot = {};
  snapshot.cubeView = m_pointShadowCubeView;
  snapshot.sampler = pointShadowSampler;
  snapshot.resolution = m_pointShadowResolution;
  snapshot.lightGeneration = expectedLightGeneration;
  snapshot.frameSerial = expectedFrameSerial;
  snapshot.publishedFrameSerial = m_pointShadowPublishedFrameSerial;
  snapshot.contentSignature = m_pointShadowContentSignature;
  snapshot.filterParams = m_pointShadowFilterParams;

  constexpr uint32_t kCompleteFaceMask = 0x3fu;
  const uint32_t publishedCount = std::min<uint32_t>(
      m_pointShadowPublishedLightCount,
      War3VolumetricPointShadowSnapshot::kMaxLights);
  for (uint32_t cubeLayer = 0u; cubeLayer < publishedCount; ++cubeLayer) {
    const uint32_t faceMask = m_pointShadowFaceValidMask[cubeLayer];
    if (!m_pointShadowReady[cubeLayer] ||
        (faceMask & kCompleteFaceMask) != kCompleteFaceMask ||
        m_pointShadowPublishedLightIds[cubeLayer] == 0) {
      continue;
    }

    auto& light = snapshot.lights[snapshot.lightCount++];
    light.lightId = m_pointShadowPublishedLightIds[cubeLayer];
    light.cubeLayer = cubeLayer;
    light.faceValidMask = faceMask;
    light.lightPosRange = m_pointShadowData[cubeLayer].lightPos;
    light.shadowIntensity =
        std::clamp(m_pointShadowData[cubeLayer].shadowIntensity, 0.0f, 1.0f);
    light.bias = std::max(m_pointShadowBias, 0.0f);
  }

  if (!snapshot.valid())
    return false;
  outSnapshot = std::move(snapshot);
  return true;
}

uint32_t War3ShadowReceiverPass::GetShadowSamplerIndex() const {
  if (m_shadowSampler)
    return m_shadowSampler->getDescriptor().samplerIndex;
  if (m_samplerLinear)
    return m_samplerLinear->getDescriptor().samplerIndex;
  return 0u;
}

void War3ShadowReceiverPass::ensureCopyResources(VkExtent3D extent,
                                                 VkFormat format) {
  if (m_colorCopy && m_cachedExtent.width == extent.width &&
      m_cachedExtent.height == extent.height && m_cachedFormat == format) {
    return;
  }

  m_cachedExtent = extent;
  m_cachedFormat = format;

  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.flags = 0;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = extent;
  info.numLayers = 1;
  info.mipLevels = 1;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.stages =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                VK_ACCESS_TRANSFER_WRITE_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  m_colorCopy =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_colorCopyLayout.reset();

  DxvkImageViewKey viewInfo;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  viewInfo.format = format;
  viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  viewInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.mipIndex = 0;
  viewInfo.mipCount = 1;
  viewInfo.layerIndex = 0;
  viewInfo.layerCount = 1;
  VkComponentMapping mapping = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);

  m_colorCopyView = m_colorCopy->createView(viewInfo);
}

void War3ShadowReceiverPass::ensureDepthCopyResources(VkExtent3D extent,
                                                      VkFormat format) {
  if (m_depthCopy && m_depthCopyView && m_depthCopyView2D &&
      m_cachedDepthExtent.width == extent.width &&
      m_cachedDepthExtent.height == extent.height &&
      m_cachedDepthFormat == format) {
    return;
  }

  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.flags = 0;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = extent;
  info.numLayers = 1;
  info.mipLevels = 1;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT;
  info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                VK_ACCESS_TRANSFER_WRITE_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

  Rc<DxvkImage> newDepthCopy =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  DxvkImageViewKey viewInfo;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  viewInfo.format = format;
  viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  viewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  viewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
  viewInfo.mipIndex = 0;
  viewInfo.mipCount = 1;
  viewInfo.layerIndex = 0;
  viewInfo.layerCount = 1;
  VkComponentMapping mapping = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);

  Rc<DxvkImageView> newDepthCopyView = newDepthCopy->createView(viewInfo);
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  Rc<DxvkImageView> newDepthCopyView2D = newDepthCopy->createView(viewInfo);

  // Publish transactionally: replace views while the old image is still
  // alive, then replace the backing image and its cache key.
  m_depthCopyView = std::move(newDepthCopyView);
  m_depthCopyView2D = std::move(newDepthCopyView2D);
  m_depthCopy = std::move(newDepthCopy);
  m_depthCopyLayout.reset();
  m_cachedDepthExtent = extent;
  m_cachedDepthFormat = format;
}

void War3ShadowReceiverPass::ensureMotionVectorResources(VkExtent3D extent) {
  if (m_motionVectorImage && m_mvCachedExtent.width == extent.width &&
      m_mvCachedExtent.height == extent.height) {
    return;
  }

  m_mvCachedExtent = extent;
  m_hasPrevFrameData = false;
  m_shadowHistoryValid = false;
  m_shadowTaaWasActiveLastFrame = false;

  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R16G16_SFLOAT;
  info.flags = 0;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = extent;
  info.numLayers = 1;
  info.mipLevels = 1;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  info.access =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  m_motionVectorImage =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_motionVectorLayout.reset();

  DxvkImageViewKey viewInfo = {};
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = info.format;
  viewInfo.usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  viewInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.mipIndex = 0;
  viewInfo.mipCount = 1;
  viewInfo.layerIndex = 0;
  viewInfo.layerCount = 1;
  VkComponentMapping mapping = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);

  m_motionVectorView = m_motionVectorImage->createView(viewInfo);
}

void War3ShadowReceiverPass::ensureShadowTaaResources(VkExtent3D extent) {
  const bool sameExtent =
      (m_shadowCurrent && m_shadowCurrentExtent.width == extent.width &&
       m_shadowCurrentExtent.height == extent.height && m_shadowHistory[0] &&
       m_shadowHistoryExtent.width == extent.width &&
       m_shadowHistoryExtent.height == extent.height);
  if (sameExtent)
    return;

  m_shadowCurrentExtent = extent;
  m_shadowHistoryExtent = extent;
  m_shadowHistoryIndex = 0;
  m_shadowHistoryValid = false;
  m_hasPrevFrameData = false;
  m_shadowTaaWasActiveLastFrame = false;
  m_shadowTaaHistoryContractValid = false;
  if (++m_shadowTaaResourceGeneration == 0u)
    ++m_shadowTaaResourceGeneration;

  // 当前帧阴影因子（未滤波）
  {
    DxvkImageCreateInfo info = {};
    info.type = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8_UNORM;
    info.flags = 0;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = extent;
    info.numLayers = 1;
    info.mipLevels = 1;
    info.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    m_shadowCurrent =
        m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    m_shadowCurrentLayout.reset();

    DxvkImageViewKey viewInfo = {};
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = info.format;
    viewInfo.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.mipIndex = 0;
    viewInfo.mipCount = 1;
    viewInfo.layerIndex = 0;
    viewInfo.layerCount = 1;
    VkComponentMapping mapping = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
    m_shadowCurrentView = m_shadowCurrent->createView(viewInfo);
  }

  // History v2 (ping-pong): R stores visibility and G stores normalized
  // linear receiver depth.  Keeping depth beside visibility lets the temporal
  // resolve reject disocclusions instead of smearing stale shadow coverage
  // across newly visible geometry.
  for (uint32_t i = 0; i < 2; i++) {
    DxvkImageCreateInfo info = {};
    info.type = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R16G16_SFLOAT;
    info.flags = 0;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = extent;
    info.numLayers = 1;
    info.mipLevels = 1;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    info.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_GENERAL;

    m_shadowHistory[i] =
        m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    m_shadowHistoryLayouts[i].reset();

    VkComponentMapping mapping = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

    DxvkImageViewKey sampleView = {};
    sampleView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    sampleView.format = info.format;
    sampleView.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    sampleView.layout = VK_IMAGE_LAYOUT_GENERAL;
    sampleView.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    sampleView.mipIndex = 0;
    sampleView.mipCount = 1;
    sampleView.layerIndex = 0;
    sampleView.layerCount = 1;
    sampleView.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
    m_shadowHistoryView[i] = m_shadowHistory[i]->createView(sampleView);

    DxvkImageViewKey storageView = sampleView;
    storageView.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    storageView.layout = VK_IMAGE_LAYOUT_GENERAL;
    m_shadowHistoryStorageView[i] = m_shadowHistory[i]->createView(storageView);
  }
}

void War3ShadowReceiverPass::ensureOutlineMaskResources(VkExtent3D extent) {
  if (m_outlineMaskVisible && m_outlineMaskAll &&
      m_outlineMaskExtent.width == extent.width &&
      m_outlineMaskExtent.height == extent.height) {
    return;
  }

  m_outlineMaskExtent = extent;

  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R8_UNORM;
  info.flags = 0;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = extent;
  info.numLayers = 1;
  info.mipLevels = 1;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  info.access =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  m_outlineMaskVisible =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_outlineMaskAll =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  VkComponentMapping mapping = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

  DxvkImageViewKey viewInfo = {};
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R8_UNORM;
  // The exact same view is first bound as an MRT color attachment and then
  // sampled by the edge pass. Restricting it to SAMPLED makes the attachment
  // use invalid even though the underlying image advertises both usages.
  viewInfo.usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  viewInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.mipIndex = 0;
  viewInfo.mipCount = 1;
  viewInfo.layerIndex = 0;
  viewInfo.layerCount = 1;
  viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);

  m_outlineMaskVisibleView = m_outlineMaskVisible->createView(viewInfo);
  m_outlineMaskAllView = m_outlineMaskAll->createView(viewInfo);
  // Vulkan images are born UNDEFINED. DxvkImageViewKey::layout is descriptor
  // metadata, not a query for the image's current layout.
  m_outlineMaskLayoutState =
      war3::render::War3OutlineMaskLayoutState::Undefined;
}

bool War3ShadowReceiverPass::ensureShadowResources(uint32_t cascadeCount,
                                                   uint32_t resolution) {
  constexpr uint64_t kCsmFallbackReserveBytes = 512ull * 1024ull * 1024ull;
  cascadeCount = std::min<uint32_t>(std::max<uint32_t>(cascadeCount, 1u), 4u);
  resolution = std::max<uint32_t>(resolution, 256u);
  m_csmRequestedResolution = resolution;

  const auto publishDiagnostics = [&]() {
    CsmResolutionDiagnostics diagnostics = {};
    diagnostics.requestedResolution = m_csmRequestedResolution;
    diagnostics.effectiveResolution = m_csmEffectiveResolution;
    diagnostics.fallbackReason =
        static_cast<uint32_t>(m_csmResolutionFallbackReason);
    diagnostics.fallbackLatched =
        m_csmResolutionFallbackLatched ? 1u : 0u;
    diagnostics.memoryBudgetSupported =
        m_csmMemoryBudgetSupported ? 1u : 0u;
    diagnostics.memoryBudgetBytes = m_csmMemoryBudgetBytes;
    diagnostics.memoryAvailableBytes = m_csmMemoryAvailableBytes;
    diagnostics.resourceGeneration = m_shadowMapResourceGeneration;
    diagnostics.resourceRebuildCount = m_csmResourceRebuildCount;
    PublishCsmResolutionDiagnostics(diagnostics);
  };

  if (m_shadowMap && m_shadowCasterMask &&
      m_shadowMapSampleView && m_shadowCasterMaskSampleView &&
      m_shadowMapResolution == resolution &&
      m_shadowMapLayers == cascadeCount) {
    m_csmEffectiveResolution = m_shadowMapResolution;
    publishDiagnostics();
    return true;
  }

  uint32_t candidateResolution =
      m_csmResolutionFallbackLatched && resolution > 2048u
          ? 2048u
          : resolution;

  // VK_EXT_memory_budget is the only preflight authority. Without it we try
  // 4096 and only latch a fallback after an actual allocation failure.
  m_csmMemoryBudgetSupported = m_device->features().extMemoryBudget != 0u;
  m_csmMemoryBudgetBytes = 0u;
  m_csmMemoryAvailableBytes = 0u;
  if (!m_csmResolutionFallbackLatched && candidateResolution > 2048u &&
      m_csmMemoryBudgetSupported) {
    const DxvkAdapterMemoryInfo memoryInfo =
        m_device->adapter()->getMemoryHeapInfo();
    for (uint32_t i = 0u; i < memoryInfo.heapCount; ++i) {
      const auto& heap = memoryInfo.heaps[i];
      if ((heap.heapFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0u)
        continue;
      const uint64_t available =
          heap.memoryBudget > heap.memoryAllocated
              ? heap.memoryBudget - heap.memoryAllocated
              : 0u;
      if (available >= m_csmMemoryAvailableBytes) {
        m_csmMemoryBudgetBytes = heap.memoryBudget;
        m_csmMemoryAvailableBytes = available;
      }
    }
    const uint64_t candidateBytes =
        uint64_t(candidateResolution) * uint64_t(candidateResolution) *
        uint64_t(cascadeCount) * 5ull; // D32 + R8
    if (m_csmMemoryAvailableBytes <
        candidateBytes + kCsmFallbackReserveBytes) {
      m_csmResolutionFallbackLatched = true;
      m_csmResolutionFallbackReason =
          CsmResolutionFallbackReason::MemoryBudget;
      candidateResolution = 2048u;
    }
  }

  // A session-latched 2048 fallback still receives the original 4096 request
  // every frame. Compare against the resolved candidate as well as the user
  // request so the fallback stays observable without recreating identical
  // 2048 resources on every pass.
  if (m_shadowMap && m_shadowCasterMask &&
      m_shadowMapSampleView && m_shadowCasterMaskSampleView &&
      m_shadowMapResolution == candidateResolution &&
      m_shadowMapLayers == cascadeCount) {
    m_csmEffectiveResolution = m_shadowMapResolution;
    publishDiagnostics();
    return true;
  }

  struct ShadowResourceCandidate {
    Rc<DxvkImage> shadowMap;
    Rc<DxvkImageView> shadowMapSampleView;
    decltype(m_shadowMapLayerViews) shadowMapLayerViews = {};
    Rc<DxvkImage> shadowCasterMask;
    Rc<DxvkImageView> shadowCasterMaskSampleView;
    decltype(m_shadowCasterMaskLayerViews) shadowCasterMaskLayerViews = {};
  };

  const auto createCandidate = [&](uint32_t requestedCandidateResolution,
                                   ShadowResourceCandidate& candidate) {
    DxvkImageCreateInfo info = {};
    info.type = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = VkExtent3D{requestedCandidateResolution,
                             requestedCandidateResolution, 1u};
    info.numLayers = cascadeCount;
    info.mipLevels = 1;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT;
    info.stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_SHADER_READ_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    DxvkImageCreateInfo maskInfo = {};
    maskInfo.type = VK_IMAGE_TYPE_2D;
    maskInfo.format = VK_FORMAT_R8_UNORM;
    maskInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    maskInfo.extent = info.extent;
    maskInfo.numLayers = cascadeCount;
    maskInfo.mipLevels = 1;
    maskInfo.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    maskInfo.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    maskInfo.access =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    maskInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    maskInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    maskInfo.debugName = "War3ShadowCasterMask";

    try {
      Rc<DxvkImage> candidateShadowMap = m_device->createImage(
          info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      Rc<DxvkImage> candidateShadowCasterMask = m_device->createImage(
          maskInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!candidateShadowMap || !candidateShadowMap->storage() ||
          !candidateShadowCasterMask ||
          !candidateShadowCasterMask->storage()) {
        return false;
      }
      candidate.shadowMap = std::move(candidateShadowMap);
      candidate.shadowCasterMask = std::move(candidateShadowCasterMask);

      const VkComponentMapping mapping = {
          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
      DxvkImageViewKey viewInfo;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      viewInfo.format = info.format;
      viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      viewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      viewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
      viewInfo.mipIndex = 0;
      viewInfo.mipCount = 1;
      viewInfo.layerIndex = 0;
      viewInfo.layerCount = cascadeCount;
      viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
      candidate.shadowMapSampleView = candidate.shadowMap->createView(viewInfo);

      DxvkImageViewKey maskViewInfo;
      maskViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      maskViewInfo.format = maskInfo.format;
      maskViewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      maskViewInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      maskViewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
      maskViewInfo.mipIndex = 0;
      maskViewInfo.mipCount = 1;
      maskViewInfo.layerIndex = 0;
      maskViewInfo.layerCount = cascadeCount;
      maskViewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
      candidate.shadowCasterMaskSampleView =
          candidate.shadowCasterMask->createView(maskViewInfo);

      for (uint32_t i = 0u; i < cascadeCount; ++i) {
        DxvkImageViewKey layerViewInfo;
        layerViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        layerViewInfo.format = info.format;
        layerViewInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        layerViewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        layerViewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
        layerViewInfo.mipIndex = 0;
        layerViewInfo.mipCount = 1;
        layerViewInfo.layerIndex = i;
        layerViewInfo.layerCount = 1;
        layerViewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
        candidate.shadowMapLayerViews[i] =
            candidate.shadowMap->createView(layerViewInfo);

        DxvkImageViewKey maskLayerViewInfo;
        maskLayerViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        maskLayerViewInfo.format = maskInfo.format;
        maskLayerViewInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        maskLayerViewInfo.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        maskLayerViewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        maskLayerViewInfo.mipIndex = 0;
        maskLayerViewInfo.mipCount = 1;
        maskLayerViewInfo.layerIndex = i;
        maskLayerViewInfo.layerCount = 1;
        maskLayerViewInfo.packedSwizzle =
            DxvkImageViewKey::packSwizzle(mapping);
        candidate.shadowCasterMaskLayerViews[i] =
            candidate.shadowCasterMask->createView(maskLayerViewInfo);
      }
    } catch (const DxvkError& error) {
      const std::string message = error.message();
      if (m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST ||
          message.find("VK_ERROR_DEVICE_LOST") != std::string::npos ||
          message.find(": -4") != std::string::npos) {
        throw;
      }
      return false;
    }

    if (!candidate.shadowMapSampleView ||
        !candidate.shadowCasterMaskSampleView)
      return false;
    for (uint32_t i = 0u; i < cascadeCount; ++i) {
      if (!candidate.shadowMapLayerViews[i] ||
          !candidate.shadowCasterMaskLayerViews[i])
        return false;
    }
    return true;
  };

  ShadowResourceCandidate candidate = {};
  if (!createCandidate(candidateResolution, candidate)) {
    if (candidateResolution > 2048u) {
      m_csmResolutionFallbackLatched = true;
      m_csmResolutionFallbackReason =
          CsmResolutionFallbackReason::AllocationFailure;
      candidateResolution = 2048u;
      candidate = {};
    }
    if (!createCandidate(candidateResolution, candidate)) {
      m_csmEffectiveResolution = m_shadowMapResolution;
      publishDiagnostics();
      return false;
    }
  }

  // Transactional commit: old resources remain alive until every candidate
  // image and view exists. Only the successful swap changes generations.
  std::swap(m_shadowMap, candidate.shadowMap);
  std::swap(m_shadowMapSampleView, candidate.shadowMapSampleView);
  std::swap(m_shadowMapLayerViews, candidate.shadowMapLayerViews);
  std::swap(m_shadowCasterMask, candidate.shadowCasterMask);
  std::swap(m_shadowCasterMaskSampleView,
            candidate.shadowCasterMaskSampleView);
  std::swap(m_shadowCasterMaskLayerViews,
            candidate.shadowCasterMaskLayerViews);
  m_shadowMapLayout.reset();
  m_shadowCasterMaskLayout.reset();

  m_shadowMapResolution = candidateResolution;
  m_shadowMapLayers = cascadeCount;
  m_csmEffectiveResolution = candidateResolution;

  // Replay path is retired; no D3D9 depth texture/surface is recreated.
  m_shadowTexture = nullptr;
  m_shadowSurface = nullptr;
  m_hasCompleteShadowMap = false;
  m_lastShadowMapCasterCount = 0;
  m_transientEmptyReplayHoldFramesRemaining = 0;
  m_recentSemanticDynamicHoldFramesRemaining = 0;
  m_shadowAdaptiveFrameIndex = 0;
  m_shadowHistoryValid = false;
  m_shadowTaaWasActiveLastFrame = false;
  m_shadowTaaHistoryContractValid = false;
  if (++m_shadowMapResourceGeneration == 0u)
    ++m_shadowMapResourceGeneration;
  ++m_csmResourceRebuildCount;
  publishDiagnostics();
  return true;
}

DxvkResourceBufferInfo War3ShadowReceiverPass::ensureShadowMatrixBuffer(
    const Rc<DxvkCommandList> &ctx, const War3PipelineInput &input,
    const std::vector<const War3ShadowCasterDraw*> *replayDraws) {
  // RenderObjectRegistry 的帧号是单调递增的，可用于“同一帧多次调用只上传一次”
  // （ShadowMap / OutlineMask / UnitOutline 可能都会用到同一份矩阵数据）。
  const uint64_t frameNumber =
      dxvk::war3::render::RenderObjectRegistry::instance().getFrameNumber();

  const uint32_t paletteCount =
      static_cast<uint32_t>(input.scene.shadowPalettes.size());
  const uint32_t casterCount =
      replayDraws != nullptr
          ? static_cast<uint32_t>(replayDraws->size())
          : static_cast<uint32_t>(input.scene.shadowCasters.size());

  // 在矩阵 SSBO 末尾追加“每个 draw 的 worldMatrix”，用于非骨骼物体的 GPU 端 MVP
  // 计算。 约定：objectMatrixIndex = objectBase + drawIndex
  const uint32_t objectBase = paletteCount * 256u;
  const uint64_t sceneKey =
      War3ComputeShadowMatrixUploadKey(input, replayDraws);

  if (m_shadowMatrixUploadedFrame == frameNumber &&
      m_shadowMatrixBufferInfo.buffer != VK_NULL_HANDLE &&
      m_shadowMatrixObjectBase == objectBase &&
      m_shadowMatrixSceneKey == sceneKey) {
    // 同一帧多次使用时，仍然需要对当前命令列表做资源追踪
    ctx->track(m_vertexBlendPaletteBuffer, DxvkAccess::Read);
    return m_shadowMatrixBufferInfo;
  }

  const VkDeviceSize matrixCount =
      (std::max)(VkDeviceSize(1u),
                 VkDeviceSize(objectBase) + VkDeviceSize(casterCount));
  const VkDeviceSize requiredBytes = matrixCount * sizeof(Matrix4);
  const VkDeviceSize align = 256u;
  const VkDeviceSize requiredStride =
      (requiredBytes + (align - 1u)) & ~(align - 1u);

  // Allocation renaming pool: plain Rc ownership keeps slots resident, while
  // DxvkAccess::Read is released only after the command list has completed on
  // the GPU. Never write a host-visible slot while that access is still live.
  size_t selectedSlot = m_shadowMatrixUploadSlots.size();
  size_t replaceableSlot = m_shadowMatrixUploadSlots.size();
  VkDeviceSize selectedCapacity = ~VkDeviceSize(0u);
  for (size_t i = 0u; i < m_shadowMatrixUploadSlots.size(); ++i) {
    const auto& slot = m_shadowMatrixUploadSlots[i];
    if (!slot.buffer || slot.buffer->isInUse(DxvkAccess::Read))
      continue;

    if (slot.capacity >= requiredStride) {
      if (slot.capacity < selectedCapacity) {
        selectedSlot = i;
        selectedCapacity = slot.capacity;
      }
    } else if (replaceableSlot == m_shadowMatrixUploadSlots.size()) {
      replaceableSlot = i;
    }
  }

  if (selectedSlot == m_shadowMatrixUploadSlots.size()) {
    if (replaceableSlot == m_shadowMatrixUploadSlots.size() &&
        m_shadowMatrixUploadSlots.size() >=
            kShadowMatrixUploadPoolLimit) {
      // GPU completion has not released any suitable backing. Missing one
      // shadow frame is preferable to overwriting matrices still in flight.
      return {};
    }

    DxvkBufferCreateInfo bufInfo = {};
    bufInfo.size = requiredStride;
    bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufInfo.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    bufInfo.access = VK_ACCESS_SHADER_READ_BIT;
    bufInfo.debugName = "War3ShadowMatricesSSBO";

    ShadowMatrixUploadSlot newSlot = {};
    newSlot.buffer = m_device->createBuffer(
        bufInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!newSlot.buffer)
      return {};
    newSlot.mapPtr = newSlot.buffer->mapPtr(0u);
    newSlot.capacity = requiredStride;
    if (newSlot.mapPtr == nullptr)
      return {};

    if (replaceableSlot != m_shadowMatrixUploadSlots.size()) {
      m_shadowMatrixUploadSlots[replaceableSlot] = std::move(newSlot);
      selectedSlot = replaceableSlot;
    } else {
      m_shadowMatrixUploadSlots.push_back(std::move(newSlot));
      selectedSlot = m_shadowMatrixUploadSlots.size() - 1u;
    }
  }

  auto& uploadSlot = m_shadowMatrixUploadSlots[selectedSlot];
  m_vertexBlendPaletteBuffer = uploadSlot.buffer;
  m_vertexBlendPaletteMapPtr = uploadSlot.mapPtr;
  m_vertexBlendPaletteCapacity = uploadSlot.capacity;
  if (!m_vertexBlendPaletteBuffer || m_vertexBlendPaletteMapPtr == nullptr ||
      m_vertexBlendPaletteCapacity < requiredBytes)
    return {};

  ++m_shadowMatrixUploadSerial;
  const VkDeviceSize baseOffset = 0u;
  m_paletteBaseMatrixIndex = 0u;

  Matrix4 *dst = reinterpret_cast<Matrix4 *>(
      reinterpret_cast<uint8_t *>(m_vertexBlendPaletteMapPtr) + baseOffset);

  if (paletteCount == 0 && casterCount == 0) {
    dst[0] = Matrix4();
  } else {
    for (uint32_t i = 0; i < paletteCount; i++) {
      const auto& palette = input.scene.shadowPalettes[i];
      dxvk::war3::render::War3ExpandShadowPaletteForUpload(
          dst + VkDeviceSize(i) *
                    dxvk::war3::render::kWar3ShadowPaletteMatrixCapacity,
          palette.worldMatrices.data(),
          dxvk::war3::render::War3BoundShadowPaletteMatrixCount(
              palette.worldMatrices.size()));
    }

    for (uint32_t i = 0; i < casterCount; i++) {
      dst[VkDeviceSize(objectBase) + VkDeviceSize(i)] =
          replayDraws != nullptr ? (*replayDraws)[i]->worldMatrix
                                 : input.scene.shadowCasters[i].worldMatrix;
    }
  }

  auto slice =
      m_vertexBlendPaletteBuffer->getSliceInfo(baseOffset, requiredBytes);

  VkBufferMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  barrier.buffer = slice.buffer;
  barrier.offset = slice.offset;
  barrier.size = slice.size;

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.bufferMemoryBarrierCount = 1;
  depInfo.pBufferMemoryBarriers = &barrier;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  ctx->track(m_vertexBlendPaletteBuffer, DxvkAccess::Read);

  m_shadowMatrixBufferInfo = slice;
  m_shadowMatrixUploadedFrame = frameNumber;
  m_shadowMatrixObjectBase = objectBase;
  m_shadowMatrixSceneKey = sceneKey;

  return slice;
}

void War3ShadowReceiverPass::ensurePointShadowNeutralResources() {
  if (m_pointShadowNeutralCube && m_pointShadowNeutralCubeView)
    return;

  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_D32_SFLOAT;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = VkExtent3D{1u, 1u, 1u};
  info.numLayers = 6u;
  info.mipLevels = 1u;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.stages =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  info.debugName = "War3PointShadowNeutralCube";

  m_pointShadowNeutralCube =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_pointShadowNeutralLayout.reset();

  DxvkImageViewKey viewInfo;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
  viewInfo.format = info.format;
  viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  viewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  viewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
  viewInfo.mipIndex = 0u;
  viewInfo.mipCount = 1u;
  viewInfo.layerIndex = 0u;
  viewInfo.layerCount = 6u;
  const VkComponentMapping mapping = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
  m_pointShadowNeutralCubeView =
      m_pointShadowNeutralCube->createView(viewInfo);
}

// Point-light cube-shadow resources are sized to the configured shadow-light
// capacity. The old fixed 4-light allocation made a single 1024 cube cost the
// memory of four lights (96 MiB) and prevented a practical 2048 ultra mode.
void War3ShadowReceiverPass::ensurePointShadowResources(
    uint32_t resolution, uint32_t capacityLights) {
  resolution = std::clamp<uint32_t>(resolution, 128u, 2048u);
  capacityLights =
      std::clamp<uint32_t>(capacityLights, 1u, kMaxPointShadowLights);
  if (m_pointShadowCube && m_pointShadowCubeView &&
      m_pointShadowResolution == resolution &&
      m_pointShadowCapacityLights == capacityLights)
    return;

  const uint32_t layerCount = capacityLights * 6u;

  // 创建 CubeArray 深度纹理
  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_D32_SFLOAT;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = VkExtent3D{resolution, resolution, 1u};
  info.numLayers = layerCount;
  info.mipLevels = 1;
  info.usage =
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  info.access =
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  info.debugName = "War3PointShadowCubeArray";

  // Build the replacement transactionally. If allocation/view creation throws,
  // the last-good cube remains owned by the pass instead of being cleared first.
  Rc<DxvkImage> newCube =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  VkComponentMapping mapping = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

  DxvkImageViewKey viewInfo;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
  viewInfo.format = info.format;
  viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  viewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  viewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
  viewInfo.mipIndex = 0;
  viewInfo.mipCount = 1;
  viewInfo.layerIndex = 0;
  viewInfo.layerCount = layerCount;
  viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
  Rc<DxvkImageView> newCubeView = newCube->createView(viewInfo);

  std::array<Rc<DxvkImageView>, kMaxPointShadowLights * 6u> newFaceViews = {};
  for (uint32_t i = 0; i < layerCount; i++) {
    DxvkImageViewKey faceViewInfo;
    faceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    faceViewInfo.format = info.format;
    faceViewInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    faceViewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    faceViewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
    faceViewInfo.mipIndex = 0;
    faceViewInfo.mipCount = 1;
    faceViewInfo.layerIndex = i; // 每个点光源占连续 6 个 cube face layer
    faceViewInfo.layerCount = 1;
    faceViewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
    newFaceViews[i] = newCube->createView(faceViewInfo);
  }

  m_pointShadowCube = newCube;
  m_pointShadowCubeView = newCubeView;
  m_pointShadowFaceViews = newFaceViews;
  m_pointShadowResolution = resolution;
  m_pointShadowCapacityLights = capacityLights;
  for (auto& layout : m_pointShadowFaceLayouts)
    layout.reset();
  m_pointShadowResourceGeneration =
      m_pointShadowResourceGeneration == std::numeric_limits<uint64_t>::max()
          ? 0u
          : m_pointShadowResourceGeneration + 1u;
  // A recreated image contains no valid face data. Keeping the old validity
  // mask would let a low face-budget update sample untouched faces from the
  // new (empty) cube as if they were temporal history.
  invalidatePointShadowPublishedState();

  const uint64_t bytes = uint64_t(resolution) * uint64_t(resolution) * 4ull *
                         uint64_t(layerCount);
  WAR3_RENDER_LOG(
      "DXVK War3Shadow: Point Shadow CubeArray created (%ux%u x %u layers, "
      "capacity=%u, %.1f MiB D32)\n",
      resolution, resolution, layerCount, capacityLights,
      static_cast<double>(bytes) / (1024.0 * 1024.0));
}

void War3ShadowReceiverPass::copyColor(const Rc<DxvkCommandList> &ctx,
                                       const Rc<DxvkImageView> &dstView) {
  if (!m_colorCopy)
    return;

  const VkImageSubresourceRange srcSubresources =
      dstView->imageSubresources();
  const VkImageLayout srcLayout =
      dstView->image()->queryLayout(srcSubresources);

  // Transition src to TRANSFER_SRC, dst copy to TRANSFER_DST
  VkImageMemoryBarrier2 barriers[2] = {};
  for (auto &b : barriers)
    b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};

  barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  barriers[0].oldLayout = srcLayout;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].image = dstView->image()->handle();
  barriers[0].subresourceRange = srcSubresources;

  const VkImageSubresourceRange dstSubresources =
      m_colorCopyView->imageSubresources();
  const auto dstWriteTransition = m_colorCopyLayout.plan(
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      VK_ACCESS_2_TRANSFER_WRITE_BIT);
  barriers[1] = war3::render::MakeWar3OwnedImageBarrier(
      dstWriteTransition, m_colorCopy->handle(), dstSubresources);

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.imageMemoryBarrierCount = 2;
  depInfo.pImageMemoryBarriers = barriers;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  war3::render::CommitWar3OwnedImageLayout(
      m_colorCopyLayout, dstWriteTransition, *m_colorCopy, dstSubresources);

  VkImageCopy2 copyRegion = {VK_STRUCTURE_TYPE_IMAGE_COPY_2};
  copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copyRegion.srcSubresource.layerCount = 1;
  copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copyRegion.dstSubresource.layerCount = 1;
  copyRegion.extent = dstView->image()->info().extent;

  VkCopyImageInfo2 copyInfo = {VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
  copyInfo.srcImage = dstView->image()->handle();
  copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  copyInfo.dstImage = m_colorCopy->handle();
  copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  copyInfo.regionCount = 1;
  copyInfo.pRegions = &copyRegion;

  ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copyInfo);

  // Transition back: src to COLOR_ATTACHMENT_OPTIMAL, copy to SHADER_READ_ONLY
  barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].newLayout = srcLayout;

  const auto dstReadTransition = m_colorCopyLayout.plan(
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_SHADER_READ_BIT);
  barriers[1] = war3::render::MakeWar3OwnedImageBarrier(
      dstReadTransition, m_colorCopy->handle(), dstSubresources);

  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  war3::render::CommitWar3OwnedImageLayout(
      m_colorCopyLayout, dstReadTransition, *m_colorCopy, dstSubresources);

  ctx->track(dstView->image(), DxvkAccess::Read);
  ctx->track(m_colorCopy, DxvkAccess::Write);
}

void War3ShadowReceiverPass::copyDepth(const Rc<DxvkCommandList> &ctx,
                                       const Rc<DxvkImageView> &srcDepthView) {
  if (!m_depthCopy || !m_depthCopyView || !srcDepthView)
    return;

  const VkImageSubresourceRange srcSubresources =
      srcDepthView->imageSubresources();
  const VkImageLayout srcLayout =
      srcDepthView->image()->queryLayout(srcSubresources);

  // Transition src depth to TRANSFER_SRC, dst copy to TRANSFER_DST
  VkImageMemoryBarrier2 barriers[2] = {};
  for (auto &b : barriers)
    b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};

  barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  barriers[0].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  barriers[0].oldLayout = srcLayout;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].image = srcDepthView->image()->handle();
  barriers[0].subresourceRange = srcSubresources;

  const VkImageSubresourceRange dstSubresources =
      m_depthCopyView->imageSubresources();
  const auto dstWriteTransition = m_depthCopyLayout.plan(
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      VK_ACCESS_2_TRANSFER_WRITE_BIT);
  barriers[1] = war3::render::MakeWar3OwnedImageBarrier(
      dstWriteTransition, m_depthCopy->handle(), dstSubresources);

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.imageMemoryBarrierCount = 2;
  depInfo.pImageMemoryBarriers = barriers;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  war3::render::CommitWar3OwnedImageLayout(
      m_depthCopyLayout, dstWriteTransition, *m_depthCopy, dstSubresources);

  VkImageCopy2 copyRegion = {VK_STRUCTURE_TYPE_IMAGE_COPY_2};
  copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  copyRegion.srcSubresource.layerCount = 1;
  copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  copyRegion.dstSubresource.layerCount = 1;
  copyRegion.extent = srcDepthView->image()->info().extent;

  VkCopyImageInfo2 copyInfo = {VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
  copyInfo.srcImage = srcDepthView->image()->handle();
  copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  copyInfo.dstImage = m_depthCopy->handle();
  copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  copyInfo.regionCount = 1;
  copyInfo.pRegions = &copyRegion;

  ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copyInfo);

  // Transition back: src to DEPTH_STENCIL_ATTACHMENT_OPTIMAL, copy to
  // DEPTH_STENCIL_READ_ONLY_OPTIMAL
  barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].newLayout = srcLayout;

  const auto dstReadTransition = m_depthCopyLayout.plan(
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_SHADER_READ_BIT);
  barriers[1] = war3::render::MakeWar3OwnedImageBarrier(
      dstReadTransition, m_depthCopy->handle(), dstSubresources);

  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  war3::render::CommitWar3OwnedImageLayout(
      m_depthCopyLayout, dstReadTransition, *m_depthCopy, dstSubresources);

  ctx->track(srcDepthView->image(), DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Write);
}


} // namespace dxvk
