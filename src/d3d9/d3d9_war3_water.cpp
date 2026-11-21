#include "d3d9_war3_water.h"
#include "d3d9_war3_shadow.h"
#include "war3/render/war3_hybrid_ray_tracing.h"

#include "../dxvk/dxvk_util.h"

#include <war3_fullscreen_vert.h>
#include <war3_water_surface.h>

#include <algorithm>
#include <cmath>

namespace dxvk {
namespace {

struct WaterUniform {
  Matrix4 invViewProj, viewProj;
  Vector4 viewport, viewportZ, depthContract;
  Vector4 camera, sunDir, sunColor, ambient;
};
static_assert(sizeof(WaterUniform) == 240);
static_assert(offsetof(WaterUniform, camera) == 176);

struct WaterPush {
  uint32_t pointSampler, colorSampler;
  float waveTime, waveScale, reflection, specular, clarity;
};
static_assert(sizeof(WaterPush) == 28);

float setting(float value, float fallback, float maximum) {
  return std::clamp(std::isnan(value) ? fallback : value, 0.0f, maximum);
}

bool finite(const Vector4& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}

} // namespace

War3WaterSurfacePass::War3WaterSurfacePass(const Rc<DxvkDevice>& device,
                                           const War3ShadowReceiverPass* shadow)
: m_device(device), m_shadow(shadow) { }

uint64_t War3WaterSurfacePass::ResourceBytes() const {
  uint64_t bytes = 0;
  for (const auto& uniform : m_uniforms) {
    if (uniform)
      bytes += uniform->storage()->getMemoryInfo().size;
  }
  return bytes;
}

bool War3WaterSurfacePass::preparePipeline(VkFormat format, VkSampleCountFlagBits samples) {
  if (m_pipeline && m_format == format && m_samples == samples)
    return true;

  DxvkSamplerKey key = {};
  key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  key.setUsePixelCoordinates(false);
  if (!m_pointSampler) {
    key.setFilter(VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    m_pointSampler = m_device->createSampler(key);
  }
  if (!m_colorSampler) {
    key.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    m_colorSampler = m_device->createSampler(key);
  }
  if (!m_layout) {
    const DxvkDescriptorSetLayoutBinding image(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT);
    const DxvkDescriptorSetLayoutBinding uniform(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT);
    const std::array<DxvkDescriptorSetLayoutBinding, 5> bindings = {image, image, image, image, uniform};
    m_layout = m_device->createBuiltInPipelineLayout(DxvkPipelineLayoutFlag::UsesSamplerHeap,
        VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(WaterPush), bindings.size(), bindings.data());
  }
  if (!m_layout || !m_pointSampler || !m_colorSampler)
    return false;

  // Premultiplied reflection/body, additive glint. Keep the frame's original alpha.
  VkPipelineColorBlendAttachmentState blend = {};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

  util::DxvkBuiltInGraphicsState state = {};
  state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
  state.fs = util::DxvkBuiltInShaderStage(war3_water_surface, nullptr);
  state.colorFormat = format;
  state.sampleCount = samples;
  state.cbAttachment = &blend;
  VkPipeline pipeline = m_device->createBuiltInGraphicsPipeline(m_layout, state);
  if (!pipeline)
    return false;
  try {
    m_pipeline = war3::render::AdoptWar3TrackedVkPipeline(m_device, pipeline);
  } catch (...) {
    m_device->vkd()->vkDestroyPipeline(m_device->vkd()->device(), pipeline, nullptr);
    throw;
  }
  m_format = format;
  m_samples = samples;
  return true;
}

void War3WaterSurfacePass::Run(const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input) {
  if (!input.settings || !input.settings->water.surfaceEnabled || !m_shadow)
    return;
  const auto& frame = m_shadow->waterFrame();
  if (!frame.valid || !ctx || !input.colorView)
    return;

  auto* target = input.colorView->image();
  const auto extent = input.colorView->mipLevelExtent(0);
  const std::array<DxvkImageView*, 4> views = {
    frame.waterDepth.ptr(), frame.waterInfo.ptr(), frame.sceneColor.ptr(), frame.sceneDepth.ptr(),
  };
  std::array<DxvkDescriptorWrite, 5> writes = {};
  for (uint32_t i = 0; i < views.size(); ++i) {
    auto* view = views[i];
    if (!view || view->image() == target || view->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT)
      return;
    const auto size = view->mipLevelExtent(0);
    if (size.width != extent.width || size.height != extent.height)
      return;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].descriptor = view->getDescriptor(i < 2 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    if (!writes[i].descriptor)
      return;
  }

  const auto& vp = frame.viewport;
  if (!finite(vp) || vp.z < 1.0f || vp.w < 1.0f || !finite(frame.viewportZ) ||
      !finite(frame.depthContract) || frame.depthContract.y <= 0.5f)
    return;
  // Integer pixel centres follow the receiver's D3D viewport convention.
  const auto edge = [](float value, uint32_t limit) {
    return int32_t(std::clamp(std::ceil(value), 0.0f, float(limit)));
  };
  const int32_t left = edge(vp.x, extent.width), top = edge(vp.y, extent.height);
  const int32_t right = edge(vp.x + vp.z, extent.width), bottom = edge(vp.y + vp.w, extent.height);
  if (right <= left || bottom <= top)
    return;
  const VkRect2D scissor = {{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};

  const float det = determinant(frame.view);
  if (!std::isfinite(det) || std::abs(det) < 1.0e-8f)
    return;
  WaterUniform data = {frame.invViewProj, frame.proj * frame.view, vp, frame.viewportZ,
      frame.depthContract, inverse(frame.view)[3], frame.sunDir, frame.sunColor, frame.ambient};
  // The first producer publishes only the far endpoint and validity. Use the
  // receiver's own format-to-quantum function when that field is absent.
  if (data.depthContract.z <= 0.0f)
    data.depthContract.z = war3::render::War3RawDepthQuantum(frame.sceneDepth->info().format);
  if (!finite(data.camera) || !finite(data.sunDir) || !finite(data.sunColor) || !finite(data.ambient))
    return;
  for (size_t i = 0; i < 4; ++i) {
    if (!finite(data.invViewProj[i]) || !finite(data.viewProj[i]))
      return;
  }

  if (!preparePipeline(input.colorView->info().format, target->info().sampleCount))
    return;
  auto& uniform = m_uniforms[input.frameSerial % m_uniforms.size()];
  if (!uniform) {
    DxvkBufferCreateInfo info = {};
    info.size = sizeof(data);
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.debugName = "War3 water surface uniforms";
    uniform = m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }
  const auto slice = uniform->getSliceInfo(0, sizeof(data));
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[4].buffer = slice;

  const auto& water = input.settings->water;
  const War3WaterSettings defaults{};
  const float speed = setting(water.waveSpeed, defaults.waveSpeed, 3.0f);
  const auto now = std::chrono::steady_clock::now();
  const std::chrono::duration<double> elapsed = now - m_waveTick;
  m_waveTick = now;
  // Integrate seconds since creation so editing the speed (including pause)
  // keeps the existing crest positions. Every wave wraps at 256 scaled seconds.
  m_waveSeconds = std::fmod(m_waveSeconds + elapsed.count() * speed, 256.0);
  WaterPush pc = {m_pointSampler->getDescriptor().samplerIndex, m_colorSampler->getDescriptor().samplerIndex,
      float(m_waveSeconds), setting(water.waveScale, defaults.waveScale, 2.0f),
      setting(water.reflection, defaults.reflection, 1.0f), setting(water.specular, defaults.specular, 2.0f),
      setting(water.clarity, defaults.clarity, 1.0f)};

  const auto cmd = DxvkCmdBuffer::ExecBuffer;
  VkBufferMemoryBarrier2 ubo = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  ubo.srcQueueFamilyIndex = ubo.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ubo.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  ubo.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  ubo.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  ubo.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  ubo.buffer = slice.buffer;
  ubo.offset = slice.offset;
  ubo.size = sizeof(data);
  VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.bufferMemoryBarrierCount = 1;
  dep.pBufferMemoryBarriers = &ubo;
  ctx->cmdPipelineBarrier(cmd, &dep);
  ctx->cmdUpdateBuffer(cmd, slice.buffer, slice.offset, sizeof(data), &data);
  std::swap(ubo.srcStageMask, ubo.dstStageMask);
  std::swap(ubo.srcAccessMask, ubo.dstAccessMask);

  // The producer published all four sampled views for fragment reads. The
  // receiver just wrote the target. Make those pixels available to blending.
  VkImageMemoryBarrier2 color = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  color.srcQueueFamilyIndex = color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  color.image = target->handle();
  color.subresourceRange = input.colorView->imageSubresources();
  color.oldLayout = color.newLayout = target->queryLayout(color.subresourceRange) == VK_IMAGE_LAYOUT_GENERAL
      ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.srcStageMask = color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  color.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &color;
  ctx->cmdPipelineBarrier(cmd, &dep);

  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = input.colorView->handle();
  attachment.imageLayout = color.newLayout;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo render = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  render.renderArea = scissor;
  render.layerCount = 1;
  render.colorAttachmentCount = 1;
  render.pColorAttachments = &attachment;
  const VkViewport viewport = {0, 0, float(extent.width), float(extent.height), 0, 1};
  ctx->cmdBeginRendering(&render);
  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);
  ctx->cmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
  ctx->bindResources(cmd, m_layout, writes.size(), writes.data(), sizeof(pc), &pc);
  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  color.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  color.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  dep.bufferMemoryBarrierCount = 0;
  ctx->cmdPipelineBarrier(cmd, &dep);
  ctx->track(target, DxvkAccess::Write);
  for (auto* view : views)
    ctx->track(view->image(), DxvkAccess::Read);
  ctx->track(uniform, DxvkAccess::Write);
  ctx->track(m_pointSampler);
  ctx->track(m_colorSampler);
  ctx->track(m_pipeline);
}

} // namespace dxvk
