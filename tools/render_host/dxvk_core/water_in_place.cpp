#include "water_in_place.h"
#include "d3d9_fixed_state.h"
#include "../../../src/dxvk/dxvk_shader_spirv.h"
#include <war3_water_in_place_vert.h>
#include <war3_water_in_place.h>

namespace warvk::host::dxvkcore {
using namespace dxvk;
namespace {
constexpr uint32_t UniformSlot = 32, ColorSlot = 33, DepthSlot = 34;
constexpr auto WaterStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// Both variants execute the stock shader body. Inherit its descriptor arrays,
// sampler heap and push-data remapping, then describe just the additional slots.
class WaterShader final : public DxvkSpirvShader {
public:
  WaterShader(const Rc<DxvkShader>& stock, const DxvkSpirvShaderCreateInfo& info, SpirvCodeBuffer code)
  : DxvkSpirvShader(info, std::move(code)), m_layout(stock->getLayout()) {
    m_layout.addLayout(DxvkSpirvShader::getLayout());
  }
  DxvkPipelineLayoutBuilder getLayout() override { return m_layout; }
private:
  DxvkPipelineLayoutBuilder m_layout;
};

template<size_t N>
Rc<DxvkShader> makeShader(const Rc<DxvkDevice>& device, const Rc<DxvkShader>& stock,
                          const uint32_t (&code)[N], bool pixel) {
  std::array<DxvkBindingInfo, 3> bindings{};
  auto& uniform = bindings[0];
  uniform.binding = uniform.resourceIndex = UniformSlot;
  uniform.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uniform.access = VK_ACCESS_UNIFORM_READ_BIT;
  uniform.flags.set(DxvkDescriptorFlag::UniformBuffer);
  for (unsigned i = 1; i < bindings.size(); ++i) {
    auto& image = bindings[i];
    image.binding = image.resourceIndex = i == 1 ? ColorSlot : DepthSlot;
    image.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    image.viewType = VK_IMAGE_VIEW_TYPE_2D;
    image.access = VK_ACCESS_SHADER_READ_BIT;
  }
  DxvkSpirvShaderCreateInfo info;
  info.bindings = bindings.data();
  info.bindingCount = pixel ? bindings.size() : 1u;
  info.flatShadingInputs = stock->getShaderMetadata().flatShadingInputs;
  info.debugName = pixel ? "War3 in-place water FS" : "War3 in-place water VS";
  Rc<DxvkShader> shader = new WaterShader(stock, info, SpirvCodeBuffer(N, code));
  device->registerShader(shader);
  return shader;
}

Rc<DxvkImageView> snapshotView(const Rc<DxvkDevice>& device, const DxvkImageCreateInfo& source,
                               VkImageAspectFlags aspect, const char* name) {
  DxvkImageCreateInfo info;
  info.format = source.format;
  info.extent = source.extent;
  info.type = VK_IMAGE_TYPE_2D;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.numLayers = info.mipLevels = 1;
  info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  info.debugName = name;
  auto image = device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  DxvkImageViewKey key;
  key.viewType = VK_IMAGE_VIEW_TYPE_2D;
  key.format = info.format;
  key.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  key.aspects = aspect;
  key.mipCount = key.layerCount = 1;
  key.layout = info.layout;
  return image->createView(key);
}
}

void WaterInPlace::beginScene(const Rc<DxvkDevice>& device, DWORD gamePid, uint64_t completed, Stats& stats) {
  m_sampling = false;
  collectTiming(completed, stats);
  m_first = true;
  m_settingsView.open(gamePid);
  diag::DecodeWater(m_settings, m_settingsView.readWater());
  auto now = std::chrono::steady_clock::now();
  m_waveTime = std::fmod(m_waveTime + std::chrono::duration<double>(now - m_tick).count() * m_settings.waveSpeed, 256.0);
  m_tick = now;
  m_enabled = m_settings.surfaceEnabled && (m_settings.inPlace == 1 || (m_settings.inPlace == 2 &&
      device->properties().core.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));
}

void WaterInPlace::resetTargets(const Rc<DxvkContext>& ctx) {
  ctx->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, ColorSlot, nullptr);
  ctx->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, DepthSlot, nullptr);
  m_color = nullptr;
  m_depth = nullptr;
  // A resize in the middle of a scene cannot recapture its pre-water pixels.
  m_snapshotResult = Reason::Snapshot;
  m_first = false;
  endFrame();
}

WaterInPlace::Reason WaterInPlace::snapshot(const Rc<DxvkDevice>& device, const Rc<DxvkContext>& ctx,
    const Rc<DxvkImage>& color, const Rc<DxvkImage>& depth) {
  if (!color || !depth) return Reason::Snapshot;
  const auto& c = color->info();
  const auto& d = depth->info();
  if (c.sampleCount != VK_SAMPLE_COUNT_1_BIT || d.sampleCount != VK_SAMPLE_COUNT_1_BIT) return Reason::Msaa;
  if (c.format != VK_FORMAT_B8G8R8A8_UNORM || d.format != VK_FORMAT_D32_SFLOAT ||
      c.extent.width != d.extent.width || c.extent.height != d.extent.height ||
      !(c.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(d.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    return Reason::Format;
  if (!m_color) {
    constexpr auto need = VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT |
                          VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    for (auto format : {c.format, d.format})
      if ((device->adapter()->getFormatFeatures(format).optimal & need) != need) return Reason::Format;
    auto cv = snapshotView(device, c, VK_IMAGE_ASPECT_COLOR_BIT, "War3 pre-water colour");
    auto dv = snapshotView(device, d, VK_IMAGE_ASPECT_DEPTH_BIT, "War3 pre-water depth");
    if (!cv || !dv || !cv->getDescriptor() || !dv->getDescriptor()) return Reason::Snapshot;
    ctx->initImage(cv->image(), VK_IMAGE_LAYOUT_UNDEFINED);
    ctx->initImage(dv->image(), VK_IMAGE_LAYOUT_UNDEFINED);
    m_color = std::move(cv);
    m_depth = std::move(dv);
  }
  // copyImage spills the current rendering and tracks both transitions and
  // allocation lifetimes. The next draw loads the original colour and depth.
  const VkImageSubresourceLayers rgb{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  const VkImageSubresourceLayers z{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
  ctx->copyImage(m_color->image(), rgb, {}, color, rgb, {}, c.extent);
  ctx->copyImage(m_depth->image(), z, {}, depth, z, {}, d.extent);
  return Reason::None;
}

bool WaterInPlace::fillUniform(Uniform& u, const d9::FixedFunctionState& fixed, const D3DVIEWPORT9& vp,
                               const Matrix4& stockInverseView) const {
  u.world = Matrix4(fixed.transforms[0].m);
  u.view = Matrix4(fixed.transforms[1].m);
  const auto& p = fixed.transforms[2];
  u.viewProj = Matrix4(p.m) * u.view;
  const float det = determinant(u.viewProj);
  if (!std::isfinite(det) || det == 0.0f) return false;
  u.invViewProj = inverse(u.viewProj);
  for (const auto& row : u.invViewProj.data)
    for (float v : row.data) if (!std::isfinite(v)) return false;
  // Classify depth direction inside the frustum, including infinite-far and
  // right-handed projections. Depth-vs-Z must not depend on screen X or Y.
  if (p._13 || p._23 || p._14 || p._24) return false;
  const auto viewZ = [&](float ndc) { return (p._43 - ndc * p._44) / (ndc * p._34 - p._33); };
  const float za = viewZ(0.25f), zb = viewZ(0.75f);
  const float a = std::abs(za), b = std::abs(zb);
  const float hi = std::min(1.0f, std::max(vp.MaxZ, vp.MinZ + (vp.MinZ < 0.5f ? 0.001f : 0.0f)));
  if (!std::isfinite(a) || !std::isfinite(b) || a == b || hi <= vp.MinZ) return false;
  u.viewport = Vector4(float(vp.X), float(vp.Y), float(vp.Width), float(vp.Height));
  u.depthRange = Vector4(vp.MinZ, 1.0f / (hi - vp.MinZ), a < b ? hi : vp.MinZ, a < b ? 1.0f : -1.0f);
  u.controls = Vector4(m_settings.style, m_settings.depthColor, m_settings.foam, m_settings.refraction);
  u.time = float(m_waveTime);
  u.waveScale = m_settings.waveScale;
  if (referenceStyle()) return true;

  // The fixed vertex shader already cached transpose(inverse(view)) for this
  // draw. The sign of visible view Z also covers right-handed cameras.
  const Matrix4 cameraToWorld = transpose(stockInverseView);
  const auto forward = normalize(cameraToWorld[2] * std::copysign(1.0f, a < b ? zb : za));
  if (p._34 != 0.0f) {
    const auto& eye = cameraToWorld[3];
    if (std::abs(eye.w) < 1e-7f) return false;
    u.camera = eye / eye.w;
  } else {
    u.camera = Vector4(-forward.x, -forward.y, -forward.z, 0.0f);
  }
  for (float v : u.camera.data) if (!std::isfinite(v)) return false;
  // Looking vertically has no horizontal forward; screen-up gives the glints
  // a stable bearing there as well.
  auto bearing = Vector4(forward.x, forward.y, 0, 0);
  if (lengthSqr(bearing) < 1e-8f) bearing = Vector4(cameraToWorld[1].x, cameraToWorld[1].y, 0, 0);
  bearing = normalize(bearing);
  if (!std::isfinite(bearing.x) || !std::isfinite(bearing.y)) return false;
  u.forwardX = bearing.x;
  u.forwardY = bearing.y;
  u.ambient = FixedColor(fixed.render[d9::FFIndex(d9::FFRenderStates, D3DRS_AMBIENT)]);
  if (fixed.lightCount && fixed.lights[0].Type == D3DLIGHT_DIRECTIONAL) {
    const auto& la = fixed.lights[0].Ambient;
    u.ambient = Vector4(std::max(u.ambient.x, la.r), std::max(u.ambient.y, la.g),
                        std::max(u.ambient.z, la.b), u.ambient.w);
  }
  if (fixed.render[d9::FFIndex(d9::FFRenderStates, D3DRS_LIGHTING)] && fixed.lightCount &&
      fixed.lights[0].Type == D3DLIGHT_DIRECTIONAL) {
    const auto& light = fixed.lights[0];
    const auto& d = light.Direction;
    const Vector4 toSun(-d.x, -d.y, -d.z, 0);
    const float len = length(toSun);
    if (len > 0 && std::isfinite(len)) {
      u.toSun = toSun / len;
      u.sunColor = Vector4(light.Diffuse.r, light.Diffuse.g, light.Diffuse.b, 0);
    }
  }
  u.lightControls = Vector4(m_settings.reflection, m_settings.specular, m_settings.caustics, 0);
  return true;
}

void WaterInPlace::bindUniform(const Rc<DxvkDevice>& device, const Rc<DxvkContext>& ctx, const Uniform& data) {
  if (!m_uniform) {
    DxvkBufferCreateInfo info;
    info.size = sizeof(data);
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    info.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    info.debugName = "War3 water draw constants";
    auto buffer = device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if ((buffer->memFlags() & coherent) != coherent || !buffer->mapPtr(0))
      throw DxvkError("Water uniform mapping unavailable");
    std::memcpy(buffer->mapPtr(0), &data, sizeof(data));
    m_uniform = std::move(buffer);
  } else if (std::memcmp(&data, &m_lastUniform, sizeof(data))) {
    auto storage = m_uniform->allocateStorage();
    if (!storage || !storage->mapPtr()) throw DxvkError("Water uniform rename unavailable");
    std::memcpy(storage->mapPtr(), &data, sizeof(data));
    ctx->invalidateBuffer(m_uniform, std::move(storage));
  }
  m_lastUniform = data;
  ctx->bindUniformBuffer(WaterStages, UniformSlot, DxvkBufferSlice(m_uniform));
}

WaterInPlace::Reason WaterInPlace::bind(const Rc<DxvkDevice>& device, const Rc<DxvkContext>& ctx,
    const Rc<DxvkImage>& color, const Rc<DxvkImage>& depth, const d9::FixedFunctionState* fixed,
    const D3DVIEWPORT9& vp, const Matrix4& stockInverseView,
    const Rc<DxvkShader>& stockVS, const Rc<DxvkShader>& stockFS, Stats& stats) {
  if (m_first) {
    m_first = false;
    m_snapshotResult = Reason::Snapshot;
    try { m_snapshotResult = snapshot(device, ctx, color, depth); }
    catch (...) { return m_snapshotResult; }
    if (m_snapshotResult == Reason::None) ++stats.snapshots;
  }
  // Even an unsupported first draw consumed the snapshot opportunity: never
  // capture again behind water that has already blended into the scene.
  if (m_snapshotResult != Reason::None) return m_snapshotResult;
  if (!fixed) return Reason::DrawState;
  Uniform data{};
  if (!fillUniform(data, *fixed, vp, stockInverseView)) return Reason::Projection;
  try {
    if (!m_vs) m_vs = makeShader(device, stockVS, war3_water_in_place_vert, false);
    if (!m_fs) m_fs = makeShader(device, stockFS, war3_water_in_place, true);
  } catch (...) { return Reason::Pipeline; }
  try {
    bindUniform(device, ctx, data);
    ctx->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, ColorSlot, Rc<DxvkImageView>(m_color));
    ctx->bindResourceImageView(VK_SHADER_STAGE_FRAGMENT_BIT, DepthSlot, Rc<DxvkImageView>(m_depth));
    ctx->bindShader<VK_SHADER_STAGE_VERTEX_BIT>(Rc<DxvkShader>(m_vs));
    ctx->bindShader<VK_SHADER_STAGE_FRAGMENT_BIT>(Rc<DxvkShader>(m_fs));
  } catch (...) { return Reason::Resources; }
  return Reason::None;
}

void WaterInPlace::finishDraw(Reason reason, Stats& stats) {
  if (reason == Reason::None) {
    ++stats.draws;
    m_drewThisFrame = true;
  } else {
    ++stats.fallbacks[size_t(reason)];
    if (m_sampling) m_partial = true;
  }
}

WaterInPlace::Timestamps::Timestamps(const Rc<DxvkDevice>& device) : vk(device->vkd()) {
  VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = 2 * MaxTimedDraws;
  if (vk->vkCreateQueryPool(vk->device(), &info, nullptr, &pool) != VK_SUCCESS) pool = VK_NULL_HANDLE;
}

WaterInPlace::Timestamps::~Timestamps() {
  if (pool) vk->vkDestroyQueryPool(vk->device(), pool, nullptr);
}

void WaterInPlace::beginTiming(const Rc<DxvkDevice>& device, const Rc<DxvkCommandList>& list, uint64_t submission) {
  try {
    if (m_first) {
      if (!m_clockChecked) {
        m_clockChecked = true;
        uint32_t count = 0;
        auto query = device->vki()->vkGetPhysicalDeviceQueueFamilyProperties;
        query(device->adapter()->handle(), &count, nullptr);
        std::vector<VkQueueFamilyProperties> props(count);
        query(device->adapter()->handle(), &count, props.data());
        const auto family = device->queues().graphics.queueFamily;
        const unsigned bits = family < count ? props[family].timestampValidBits : 0;
        if (bits && bits <= 64) m_tickMask = UINT64_MAX >> (64 - bits);
        m_periodUs = device->properties().core.properties.limits.timestampPeriod / 1000.0;
        if (m_tickMask && device->features().vk12.hostQueryReset) m_timestamps = new Timestamps(device);
      }
      m_sampling = !(m_waterScenes++ % 32) && !m_submission && m_timestamps && m_timestamps->pool;
      if (m_sampling) {
        m_partial = false;
        m_timestamps->vk->vkResetQueryPool(device->handle(), m_timestamps->pool, 0, 2 * MaxTimedDraws);
      }
    }
    if (!m_sampling) return;
    if (m_stampCount == MaxTimedDraws) { m_partial = true; m_sampling = false; return; }
    if (m_submission != submission) list->track(m_timestamps);
    m_submission = submission;
    list->cmdWriteTimestamp(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            m_timestamps->pool, 2 * m_stampCount);
  } catch (...) { m_partial = true; m_sampling = false; }
}

void WaterInPlace::endTiming(const Rc<DxvkCommandList>& list) {
  if (!m_sampling) return;
  list->cmdWriteTimestamp(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          m_timestamps->pool, 2 * m_stampCount + 1);
  ++m_stampCount;
}

void WaterInPlace::collectTiming(uint64_t completed, Stats& stats) {
  if (!m_submission || completed < m_submission) return;
  if (m_stampCount && !m_partial) {
    std::array<uint64_t, 2 * MaxTimedDraws> stamps;
    const auto result = m_timestamps->vk->vkGetQueryPoolResults(m_timestamps->vk->device(),
        m_timestamps->pool, 0, 2 * m_stampCount, sizeof(stamps), stamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY) return;
    if (result == VK_SUCCESS) {
      uint64_t ticks = 0;
      for (uint32_t i = 0; i < 2 * m_stampCount; i += 2)
        ticks += (stamps[i + 1] - stamps[i]) & m_tickMask;
      double us = double(ticks) * m_periodUs;
      ++stats.gpuSamples;
      stats.gpuTotalUs += us;
      stats.gpuMaxUs = std::max(stats.gpuMaxUs, us);
    }
  }
  m_sampling = false;
  m_submission = m_stampCount = 0;
}

}
