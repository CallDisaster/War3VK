#include "d3d9_war3_shadow.h"
#include "d3d9_war3_debug.h"

#include "war3/render/war3_render_objects.h"

#include <algorithm>
#include <cstring>

namespace dxvk {
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
  if (m_depthCopy && m_cachedDepthExtent.width == extent.width &&
      m_cachedDepthExtent.height == extent.height &&
      m_cachedDepthFormat == format) {
    return;
  }

  m_cachedDepthExtent = extent;
  m_cachedDepthFormat = format;

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
  info.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

  m_depthCopy =
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

  m_depthCopyView = m_depthCopy->createView(viewInfo);
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

  DxvkImageViewKey viewInfo = {};
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = info.format;
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

    DxvkImageViewKey viewInfo = {};
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = info.format;
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
    m_shadowCurrentView = m_shadowCurrent->createView(viewInfo);
  }

  // 历史（Ping-Pong）：采样使用 GENERAL，写入使用 STORAGE_IMAGE(GENERAL)
  for (uint32_t i = 0; i < 2; i++) {
    DxvkImageCreateInfo info = {};
    info.type = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8_UNORM;
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
  viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  viewInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.mipIndex = 0;
  viewInfo.mipCount = 1;
  viewInfo.layerIndex = 0;
  viewInfo.layerCount = 1;
  viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);

  m_outlineMaskVisibleView = m_outlineMaskVisible->createView(viewInfo);
  m_outlineMaskAllView = m_outlineMaskAll->createView(viewInfo);
}

void War3ShadowReceiverPass::ensureShadowResources(uint32_t cascadeCount,
                                                   uint32_t resolution) {
  cascadeCount = std::min<uint32_t>(std::max<uint32_t>(cascadeCount, 1u), 4u);
  resolution = std::max<uint32_t>(resolution, 256u);

  if (m_shadowMap && m_shadowMapResolution == resolution &&
      m_shadowMapLayers == cascadeCount)
    return;

  m_shadowMapResolution = resolution;
  m_shadowMapLayers = cascadeCount;

  // Replay 路径已弃用：不再创建 D3D9 depth texture/surface
  m_shadowTexture = nullptr;
  m_shadowSurface = nullptr;

  m_shadowMap = nullptr;
  m_shadowMapSampleView = nullptr;
  m_shadowMapLayerViews = {};
  m_hasCompleteShadowMap = false;
  m_lastShadowMapCasterCount = 0;
  m_shadowAdaptiveFrameIndex = 0;

  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_D32_SFLOAT;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = VkExtent3D{resolution, resolution, 1u};
  info.numLayers = cascadeCount;
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

  m_shadowMap =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

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
    viewInfo.layerCount = cascadeCount;
    viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
    m_shadowMapSampleView = m_shadowMap->createView(viewInfo);
  }

  for (uint32_t i = 0; i < cascadeCount; i++) {
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
    m_shadowMapLayerViews[i] = m_shadowMap->createView(layerViewInfo);
  }
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

  if (m_shadowMatrixUploadedFrame == frameNumber &&
      m_shadowMatrixBufferInfo.buffer != VK_NULL_HANDLE &&
      m_shadowMatrixObjectBase == objectBase) {
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

  if (!m_vertexBlendPaletteBuffer || m_paletteStride < requiredStride) {
    m_paletteStride = requiredStride;
    m_vertexBlendPaletteCapacity = m_paletteStride * kPaletteRingCount;

    DxvkBufferCreateInfo bufInfo = {};
    bufInfo.size = m_vertexBlendPaletteCapacity;
    bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufInfo.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    bufInfo.access = VK_ACCESS_SHADER_READ_BIT;
    bufInfo.debugName = "War3ShadowMatricesSSBO";

    m_vertexBlendPaletteBuffer = m_device->createBuffer(
        bufInfo, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    m_vertexBlendPaletteMapPtr = m_vertexBlendPaletteBuffer->mapPtr(0u);
  }

  if (!m_vertexBlendPaletteBuffer || m_vertexBlendPaletteMapPtr == nullptr)
    return {};

  const uint32_t ringIndex = uint32_t(input.frameIndex % kPaletteRingCount);
  const VkDeviceSize baseOffset = VkDeviceSize(ringIndex) * m_paletteStride;
  m_paletteBaseMatrixIndex = uint32_t(baseOffset / sizeof(Matrix4));

  Matrix4 *dst = reinterpret_cast<Matrix4 *>(
      reinterpret_cast<uint8_t *>(m_vertexBlendPaletteMapPtr) + baseOffset);

  if (paletteCount == 0 && casterCount == 0) {
    dst[0] = Matrix4();
  } else {
    for (uint32_t i = 0; i < paletteCount; i++) {
      std::memcpy(dst + VkDeviceSize(i) * 256u,
                  input.scene.shadowPalettes[i].worldMatrices.data(),
                  sizeof(Matrix4) * 256u);
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

  return slice;
}

// [NEW] Point Light Cube Shadow Map 资源创建
void War3ShadowReceiverPass::ensurePointShadowResources() {
  if (m_pointShadowCube)
    return; // 已创建，直接返回

  constexpr uint32_t resolution = kPointShadowResolution;

  // 创建 Cube 深度纹理
  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_D32_SFLOAT;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = VkExtent3D{resolution, resolution, 1u};
  info.numLayers = 6; // Cube 有 6 个 layer
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
  info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; // 重要：标记为 Cube 兼容

  m_pointShadowCube =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  VkComponentMapping mapping = {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

  // 创建 Cube 采样视图
  {
    DxvkImageViewKey viewInfo;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = info.format;
    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    viewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.mipIndex = 0;
    viewInfo.mipCount = 1;
    viewInfo.layerIndex = 0;
    viewInfo.layerCount = 6;
    viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
    m_pointShadowCubeView = m_pointShadowCube->createView(viewInfo);
  }

  // 创建 6 个面的渲染视图（用于 Depth Attachment）
  for (uint32_t i = 0; i < 6; i++) {
    DxvkImageViewKey faceViewInfo;
    faceViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    faceViewInfo.format = info.format;
    faceViewInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    faceViewInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    faceViewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
    faceViewInfo.mipIndex = 0;
    faceViewInfo.mipCount = 1;
    faceViewInfo.layerIndex = i; // 每个面一个 layer
    faceViewInfo.layerCount = 1;
    faceViewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
    m_pointShadowFaceViews[i] = m_pointShadowCube->createView(faceViewInfo);
  }

  WAR3_RENDER_LOG(
      "DXVK War3Shadow: Point Shadow Cube created (%ux%u x 6 faces)\n",
      resolution, resolution);
}

void War3ShadowReceiverPass::copyColor(const Rc<DxvkCommandList> &ctx,
                                       const Rc<DxvkImageView> &dstView) {
  if (!m_colorCopy)
    return;

  const VkImageLayout srcLayout = dstView->getLayout();

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
  barriers[0].subresourceRange = dstView->imageSubresources();

  barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].image = m_colorCopy->handle();
  barriers[1].subresourceRange = m_colorCopyView->imageSubresources();

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.imageMemoryBarrierCount = 2;
  depInfo.pImageMemoryBarriers = barriers;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

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

  barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  ctx->track(dstView->image(), DxvkAccess::Read);
  ctx->track(m_colorCopy, DxvkAccess::Write);
}

void War3ShadowReceiverPass::copyDepth(const Rc<DxvkCommandList> &ctx,
                                       const Rc<DxvkImageView> &srcDepthView) {
  if (!m_depthCopy || !m_depthCopyView || !srcDepthView)
    return;

  const VkImageLayout srcLayout = srcDepthView->getLayout();

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
  barriers[0].subresourceRange = srcDepthView->imageSubresources();

  barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barriers[1].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].image = m_depthCopy->handle();
  barriers[1].subresourceRange = m_depthCopyView->imageSubresources();

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.imageMemoryBarrierCount = 2;
  depInfo.pImageMemoryBarriers = barriers;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

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

  barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  ctx->track(srcDepthView->image(), DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Write);
}


} // namespace dxvk
