#include "d3d9_war3_volumetric_light.h"
#include "d3d9_device.h"
#include "war3/core/war3_internal_test_config.h"

#include "../dxvk/dxvk_access.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_util.h"

#include <war3_fullscreen_vert.h>
#include <war3_volumetric_light.h>

#include <algorithm>
#include <cmath>

namespace dxvk {

namespace {
struct VolumetricLightPushConstants {
  uint32_t colorSampler;
  uint32_t depthSampler;
  uint32_t shadowSampler;
  uint32_t flags;

  // x=intensity y=decay z=density w=weight
  Vector4 params0;
  // x=anisotropy(y映射) y=fadeNear z=fadeFar w=maxRayDistanceScale
  Vector4 params1;
  // x=sampleCount y=maxWorldDistance z=froxelNearDistance w=shadowStrengthScale
  Vector4 params2;
  // rgb=sunColor w=sunIntensity
  Vector4 sunColorScale;
  Vector4 viewport;
  Vector4 viewportZ;
  // x=rtW y=rtH
  Vector4 rtSize;
};
static_assert(sizeof(VolumetricLightPushConstants) <= 128,
              "VolumetricLightPushConstants exceeds common push-constant limit");

struct VolumetricCsmUniform {
  Matrix4 view;
  Matrix4 invViewProj;
  Matrix4 lightViewProj[4];
  Vector4 splitFar;
  // x=receiverBias y=invShadowRes z=cascadeCount w=pcfRadius
  Vector4 params;
  // x=cascadeBlendRange y=heightFogBase z=heightFogFalloff w=heightFogStrength
  Vector4 params2;
  // xyz=sunDir（从太阳指向地面）
  Vector4 sunDir;
  // xyz=cameraPos
  Vector4 cameraPos;
  // xyz=worldUp（与 CSM 完全一致）
  Vector4 worldUp;
};

VkImageSubresourceLayers toLayers(const VkImageSubresourceRange& range) {
  VkImageSubresourceLayers layers = {};
  layers.aspectMask = range.aspectMask;
  layers.mipLevel = range.baseMipLevel;
  layers.baseArrayLayer = range.baseArrayLayer;
  layers.layerCount = range.layerCount;
  return layers;
}
} // namespace

War3VolumetricLightPass::War3VolumetricLightPass(D3D9DeviceEx* device)
    : m_parent(device), m_device(device->GetDXVKDevice()) {
  DxvkSamplerKey linearKey = {};
  linearKey.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                      VK_SAMPLER_MIPMAP_MODE_NEAREST);
  linearKey.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  linearKey.setUsePixelCoordinates(false);
  m_linearSampler = m_device->createSampler(linearKey);

  m_layout = createPipelineLayout();

  // CSM 数据 UBO：每帧由体积光 pass 读取 shadow pass 快照后更新。
  DxvkBufferCreateInfo uboInfo = {};
  uboInfo.size = sizeof(VolumetricCsmUniform);
  uboInfo.usage =
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  uboInfo.stages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  uboInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
  m_csmUniformBuffer =
      m_device->createBuffer(uboInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

War3VolumetricLightPass::~War3VolumetricLightPass() {
  auto vk = m_device->vkd();
  for (auto& kv : m_pipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
}

const DxvkPipelineLayout* War3VolumetricLightPass::createPipelineLayout() const {
  // 绑定顺序需与 shader 一致：
  // 0=color, 1=depth, 2=shadowMap, 3=CSM UBO
  std::array<DxvkDescriptorSetLayoutBinding, 4> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
  };

  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(VolumetricLightPushConstants), bindings.size(), bindings.data());
}

VkPipeline War3VolumetricLightPass::getPipeline(const PipelineKey& key) {
  auto it = m_pipelines.find(key);
  if (it != m_pipelines.end())
    return it->second;

  VkPipeline pipeline = createPipeline(key);
  m_pipelines.insert({key, pipeline});
  return pipeline;
}

VkPipeline War3VolumetricLightPass::createPipeline(const PipelineKey& key) const {
  util::DxvkBuiltInGraphicsState state = {};
  state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
  state.fs = util::DxvkBuiltInShaderStage(war3_volumetric_light, nullptr);
  state.colorFormat = key.format;
  state.sampleCount = key.samples;

  return m_device->createBuiltInGraphicsPipeline(m_layout, state);
}

void War3VolumetricLightPass::ensureResources(VkExtent3D extent,
                                               VkFormat colorFormat,
                                               VkFormat depthFormat) {
  if (!m_colorCopy || m_cachedExtent.width != extent.width ||
      m_cachedExtent.height != extent.height || m_cachedFormat != colorFormat) {
    m_cachedExtent = extent;
    m_cachedFormat = colorFormat;

    DxvkImageCreateInfo info = {};
    info.type = VK_IMAGE_TYPE_2D;
    info.format = colorFormat;
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
    VkComponentMapping mapping = {VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);

    m_colorCopyView = m_colorCopy->createView(viewInfo);
  }

  if (!m_depthCopy || m_cachedDepthExtent.width != extent.width ||
      m_cachedDepthExtent.height != extent.height ||
      m_cachedDepthFormat != depthFormat) {
    m_cachedDepthExtent = extent;
    m_cachedDepthFormat = depthFormat;

    DxvkImageCreateInfo info = {};
    info.type = VK_IMAGE_TYPE_2D;
    info.format = depthFormat;
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
    VkComponentMapping mapping = {VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY,
                                  VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);

    m_depthCopyView = m_depthCopy->createView(viewInfo);
  }
}

void War3VolumetricLightPass::copyColor(const Rc<DxvkCommandList>& ctx,
                                         const Rc<DxvkImageView>& srcView) {
  if (!m_colorCopy || !m_colorCopyView)
    return;

  const VkImageLayout srcLayout = srcView->getLayout();

  VkImageMemoryBarrier2 barriers[2] = {};
  for (auto& b : barriers)
    b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};

  barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  barriers[0].oldLayout = srcLayout;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].image = srcView->image()->handle();
  barriers[0].subresourceRange = srcView->imageSubresources();

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
  copyRegion.srcSubresource = toLayers(srcView->imageSubresources());
  copyRegion.dstSubresource = toLayers(m_colorCopyView->imageSubresources());
  copyRegion.extent = srcView->image()->info().extent;

  VkCopyImageInfo2 copyInfo = {VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
  copyInfo.srcImage = srcView->image()->handle();
  copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  copyInfo.dstImage = m_colorCopy->handle();
  copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  copyInfo.regionCount = 1;
  copyInfo.pRegions = &copyRegion;
  ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copyInfo);

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

  ctx->track(srcView->image(), DxvkAccess::Read);
  ctx->track(m_colorCopy, DxvkAccess::Write);
}

void War3VolumetricLightPass::copyDepth(const Rc<DxvkCommandList>& ctx,
                                         const Rc<DxvkImageView>& srcView) {
  if (!m_depthCopy || !m_depthCopyView)
    return;

  const VkImageLayout srcLayout = srcView->getLayout();

  VkImageMemoryBarrier2 barriers[2] = {};
  for (auto& b : barriers)
    b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};

  barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  barriers[0].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  barriers[0].oldLayout = srcLayout;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].image = srcView->image()->handle();
  barriers[0].subresourceRange = srcView->imageSubresources();

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
  copyRegion.srcSubresource = toLayers(srcView->imageSubresources());
  copyRegion.dstSubresource = toLayers(m_depthCopyView->imageSubresources());
  copyRegion.extent = srcView->image()->info().extent;

  VkCopyImageInfo2 copyInfo = {VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
  copyInfo.srcImage = srcView->image()->handle();
  copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  copyInfo.dstImage = m_depthCopy->handle();
  copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  copyInfo.regionCount = 1;
  copyInfo.pRegions = &copyRegion;
  ctx->cmdCopyImage(DxvkCmdBuffer::ExecBuffer, &copyInfo);

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

  ctx->track(srcView->image(), DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Write);
}

void War3VolumetricLightPass::drawVolumetricLight(
    const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) {
  if (!m_layout || !m_linearSampler || !m_colorCopyView || !m_depthCopyView ||
      !m_csmUniformBuffer || !input.settings)
    return;
  if (!input.scene.worldCamera.valid)
    return;

  auto* shadowPass =
      m_parent ? m_parent->GetWar3ShadowReceiverPass() : nullptr;
  if (!shadowPass)
    return;

  Rc<DxvkImageView> shadowMapView = nullptr;
  War3CsmData csmData = {};
  uint32_t shadowResolution = 0u;
  if (!shadowPass->GetVolumetricShadowSnapshot(shadowMapView, csmData,
                                               shadowResolution)) {
    return;
  }

  const auto& settings = input.settings->postFx.volumetricLight;
  auto colorImage = input.colorView->image();

  PipelineKey key = {};
  key.format = colorImage->info().format;
  key.samples = colorImage->info().sampleCount;

  VkPipeline pipeline = getPipeline(key);
  if (pipeline == VK_NULL_HANDLE)
    return;

  VkExtent3D extent = colorImage->info().extent;

  VkRenderingAttachmentInfo attachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = input.colorView->handle();
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea.offset = {0, 0};
  renderInfo.renderArea.extent = {extent.width, extent.height};
  renderInfo.layerCount = 1u;
  renderInfo.colorAttachmentCount = 1u;
  renderInfo.pColorAttachments = &attachment;
  ctx->cmdBeginRendering(&renderInfo);

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = float(extent.width);
  viewport.height = float(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor = {};
  scissor.offset = {0, 0};
  scissor.extent = {extent.width, extent.height};

  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);

  const uint32_t cascadeCount =
      std::min<uint32_t>(std::max<uint32_t>(csmData.cascadeCount, 1u), 4u);
  VolumetricCsmUniform csmUbo = {};
  csmUbo.view = input.scene.worldCamera.view;
  csmUbo.invViewProj = input.scene.worldCamera.invViewProj;
  for (uint32_t i = 0; i < 4; i++) {
    const uint32_t idx = (i < cascadeCount) ? i : (cascadeCount - 1u);
    csmUbo.lightViewProj[i] = csmData.cascades[idx].lightViewProj;
  }
  const float split0 = csmData.cascades[0].splitFar;
  const float split1 = (cascadeCount > 1u) ? csmData.cascades[1].splitFar : split0;
  const float split2 = (cascadeCount > 2u) ? csmData.cascades[2].splitFar : split1;
  const float split3 = (cascadeCount > 3u) ? csmData.cascades[3].splitFar : split2;
  csmUbo.splitFar = Vector4(split0, split1, split2, split3);

  // 使用 CSM 快照中的光向，避免与 CSM 内部符号修正不一致。
  Vector4 sunDir = csmData.lightDir;
  const float sunLen2 =
      sunDir.x * sunDir.x + sunDir.y * sunDir.y + sunDir.z * sunDir.z;
  if (sunLen2 > 1e-8f) {
    const float invLen = 1.0f / std::sqrt(sunLen2);
    sunDir.x *= invLen;
    sunDir.y *= invLen;
    sunDir.z *= invLen;
  } else {
    sunDir = Vector4(-0.3f, -0.2f, -1.0f, 0.0f);
  }
  csmUbo.sunDir = Vector4(sunDir.x, sunDir.y, sunDir.z, 0.0f);

  const Matrix4 invView = inverse(input.scene.worldCamera.view);
  Vector4 cameraPos = invView[3];
  cameraPos.w = 1.0f;
  csmUbo.cameraPos = cameraPos;
  // 使用 CSM 快照中的 worldUp，避免相机俯仰触发 Y/Z 轴切换。
  Vector4 worldUp = csmData.worldUp;
  const float upLenSq =
      worldUp.x * worldUp.x + worldUp.y * worldUp.y + worldUp.z * worldUp.z;
  if (upLenSq > 1e-8f) {
    const float invLen = 1.0f / std::sqrt(upLenSq);
    worldUp.x *= invLen;
    worldUp.y *= invLen;
    worldUp.z *= invLen;
  } else {
    worldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
  }
  csmUbo.worldUp = worldUp;

  const float receiverBias = std::max(0.0f, input.settings->shadows.receiverBias);
  const float invShadowRes =
      1.0f / float(std::max<uint32_t>(shadowResolution, 1u));
  const float pcfRadius = std::max(0.0f, input.settings->shadows.pcfRadius);
  const float cascadeBlendRange =
      std::max(0.0f, input.settings->shadows.cascadeBlendRange);
  const float heightFogBase = settings.heightFogBase;
  const float heightFogFalloff = std::max(0.00001f, settings.heightFogFalloff);
  const float heightFogStrength = std::max(0.0f, settings.heightFogStrength);
  csmUbo.params =
      Vector4(receiverBias, invShadowRes, float(cascadeCount), pcfRadius);
  csmUbo.params2 = Vector4(cascadeBlendRange, heightFogBase, heightFogFalloff,
                           heightFogStrength);

  auto csmInfo =
      m_csmUniformBuffer->getSliceInfo(0u, sizeof(VolumetricCsmUniform));
  ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, csmInfo.buffer, csmInfo.offset,
                       sizeof(VolumetricCsmUniform), &csmUbo);

  VkBufferMemoryBarrier2 csmBarrier = {
      VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  csmBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  csmBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  csmBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  csmBarrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  csmBarrier.buffer = csmInfo.buffer;
  csmBarrier.offset = csmInfo.offset;
  csmBarrier.size = sizeof(VolumetricCsmUniform);

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.bufferMemoryBarrierCount = 1;
  depInfo.pBufferMemoryBarriers = &csmBarrier;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  std::array<DxvkDescriptorWrite, 4> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor = m_colorCopyView->getDescriptor();
  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = m_depthCopyView->getDescriptor();
  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[2].descriptor = shadowMapView->getDescriptor();
  descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[3].descriptor = nullptr;
  descriptors[3].buffer = csmInfo;

  Vector4 sunColor = input.settings->sun.color;
  sunColor.w = 0.0f;
  const float sunIntensity =
      std::max(0.0f, input.settings->sun.enabled ? input.settings->sun.intensity
                                                  : 1.0f);

  VolumetricLightPushConstants pc = {};
  pc.colorSampler = m_linearSampler->getDescriptor().samplerIndex;
  pc.depthSampler = m_linearSampler->getDescriptor().samplerIndex;
  pc.shadowSampler = shadowPass->GetShadowSamplerIndex();

  if (dxvk::war3::internal::kVolumetricLightDebugFlipUvX)
    pc.flags |= 0x1u;
  if (dxvk::war3::internal::kVolumetricLightDebugFlipUvY)
    pc.flags |= 0x2u;
  if (dxvk::war3::internal::kVolumetricLightDebugFlipSunRaySign)
    pc.flags |= 0x4u;
  if (dxvk::war3::internal::kVolumetricLightDebugDisableNearFade)
    pc.flags |= 0x8u;
  if (dxvk::war3::internal::kVolumetricLightDebugFlipDepthDelta)
    pc.flags |= 0x10u;

  pc.params0 = Vector4(std::max(0.0f, settings.intensity),
                       std::clamp(settings.decay, 0.70f, 0.999f),
                       std::max(0.0f, settings.density),
                       std::max(0.0f, settings.weight));
  const float fadeNear = std::clamp(settings.fadeNear, 0.0f, 0.95f);
  const float fadeFar = std::clamp(settings.fadeFar, fadeNear + 0.01f, 1.0f);
  pc.params1 = Vector4(std::clamp(settings.skyThreshold, 0.55f, 0.99f), fadeNear,
                       fadeFar, std::max(0.05f, settings.maxRayDistance));
  const float shadowStrengthScale =
      input.settings->shadows.enabled
          ? std::clamp(input.settings->shadows.strength, 0.05f, 1.5f)
          : 1.0f;
  pc.params2 =
      Vector4(float(std::clamp(settings.sampleCount, 4, 96)),
              std::max(100.0f, settings.sunDistance),
              std::max(0.1f, settings.froxelNear), shadowStrengthScale);
  pc.sunColorScale =
      Vector4(sunColor.x, sunColor.y, sunColor.z, std::max(0.0f, sunIntensity));
  pc.viewport = Vector4(float(input.scene.worldCamera.viewport.X),
                        float(input.scene.worldCamera.viewport.Y),
                        float(input.scene.worldCamera.viewport.Width),
                        float(input.scene.worldCamera.viewport.Height));
  pc.viewportZ = Vector4(input.scene.worldCamera.viewport.MinZ,
                         input.scene.worldCamera.viewport.MaxZ, 0.0f, 0.0f);
  pc.rtSize = Vector4(float(extent.width), float(extent.height), 0.0f, 0.0f);

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                       pipeline);
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_layout, descriptors.size(),
                     descriptors.data(), sizeof(pc), &pc);
  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  ctx->track(input.colorView->image(), DxvkAccess::Write);
  ctx->track(m_colorCopy, DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Read);
  ctx->track(shadowMapView->image(), DxvkAccess::Read);
  ctx->track(m_csmUniformBuffer, DxvkAccess::Read);
  ctx->track(m_linearSampler);
}

void War3VolumetricLightPass::Run(const Rc<DxvkCommandList>& ctx,
                                  const War3PipelineInput& input) {
  if (!input.settings)
    return;
  if (!input.settings->postFx.volumetricLight.enabled)
    return;
  if (!input.colorView || !input.depthView)
    return;

  const auto& settings = input.settings->postFx.volumetricLight;
  if (settings.intensity <= 0.0f || settings.sampleCount <= 0)
    return;

  VkExtent3D extent = input.colorView->image()->info().extent;
  ensureResources(extent, input.colorView->image()->info().format,
                  input.depthView->image()->info().format);

  copyColor(ctx, input.colorView);
  copyDepth(ctx, input.depthView);
  drawVolumetricLight(ctx, input);
}

} // namespace dxvk
