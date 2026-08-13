#include "d3d9_war3_volumetric_light.h"
#include "d3d9_device.h"
#include "d3d9_war3_csm.h"
#include "d3d9_war3_debug.h"
#include "d3d9_war3_light.h"
#include "d3d9_war3_shadow.h"
#include "war3/core/war3_internal_test_config.h"
#include "war3/render/war3_hybrid_ray_tracing.h"

#include "../dxvk/dxvk_access.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_util.h"
#include "../util/util_matrix.h"

#include <war3_fullscreen_vert.h>
#include <war3_volumetric_composite.h>
#include <war3_volumetric_froxel_inject.h>
#include <war3_volumetric_froxel_integrate.h>
#include <war3_volumetric_froxel_temporal.h>
#include <war3_volumetric_light.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace dxvk {

namespace {
// Author-facing/full-screen quality remains clamped to divisor >= 4. A
// clear-air frame may use an internal half-resolution target only when the
// conservative local-volume ROI fits the same ray and interaction budgets at
// the requested sample count.
constexpr uint32_t kVolumetricLocalRoiResolutionDivisor = 2u;
constexpr uint32_t kVolumetricMinResolutionDivisor = 4u;
constexpr uint32_t kVolumetricMaxResolutionDivisor = 8u;
constexpr int kVolumetricMinSamples = 4;
constexpr int kVolumetricMaxSamples = 16;
constexpr uint32_t kVolumetricMaxPointLights = 2u;
constexpr uint32_t kVolumetricMaxFogVolumes =
    War3FogVolumeFrameSnapshot::kMaxVolumes;
// Counts ray-march segments before the bounded inner loops: at most eight raw
// CSM probes plus two visibility probes for each of two point lights. This
// caps the fragment workload independently of every external setting surface,
// including stale INI/JASS values and direct mutable-settings users.
constexpr uint64_t kVolumetricRaySegmentBudget = 4'000'000ull;
// Local-volume support adds one analytical ray intersection per effect pixel,
// one medium-overlap integration per march segment, and one additional overlap
// integration for each selected point light. Keep that second dimension
// independently bounded so eight authored volumes cannot silently multiply the
// original watchdog contract into an unbounded fragment loop.
constexpr uint64_t kVolumetricFogSegmentTestBudget = 96'000'000ull;
// High keeps the lighting field coarse enough for bounded injection, while the
// final ray integration is intentionally evaluated at a finer 2D resolution.
// At 3840x2160 this admits 240x135x128 = 4,147,200 cells (~31.7 MiB/image in
// RGBA16F). Three ping-pong/current images remain below the 128 MiB quality
// budget and avoid coupling the depth terminator to a 16x16 screen tile.
constexpr uint64_t kVolumetricFroxelCellBudget = 4'500'000ull;
// Bound the expensive directional shadow interval traversal independently of
// the 3D grid build. High requests quarter-resolution integration at ordinary
// Warcraft resolutions, but automatically falls back to eighth resolution at
// 4K so the worst-case DDA contract stays below this fixed watchdog budget.
constexpr uint64_t kVolumetricFroxelTraversalWorkBudget = 350'000'000ull;
constexpr uint32_t kVolumetricFroxelMediumTileSize = 32u;
constexpr uint32_t kVolumetricFroxelMediumDepth = 64u;
constexpr uint32_t kVolumetricFroxelHighTileSize = 16u;
constexpr uint32_t kVolumetricFroxelHighDepth = 128u;
// The 3D grid and the integrated 2D effect have different jobs. Grid XY stores
// slowly varying medium/light data; the effect target owns first-surface depth
// termination and therefore needs substantially finer screen coverage near a
// giant caster silhouette. Reusing the grid tile as the effect divisor caused
// 16x16/32x32 depth blocks to erase the column as camera pitch decreased.
constexpr uint32_t kVolumetricFroxelMediumEffectDivisor = 8u;
constexpr uint32_t kVolumetricFroxelHighEffectDivisor = 4u;
constexpr uint32_t kVolumetricFroxelMediumTraversalSteps = 1024u;
constexpr uint32_t kVolumetricFroxelHighTraversalSteps = 2048u;

struct VolumetricLightPushConstants {
  uint32_t colorSampler;
  uint32_t depthSampler;
  uint32_t shadowSampler;
  uint32_t flags;

  // x=intensity y=single-scatter persistence z=density w=weight
  Vector4 params0;
  // x=anisotropy(y映射) y=fadeNear z=fadeFar w=maxRayDistanceScale
  Vector4 params1;
  // x=sampleCount y=maxWorldDistance z=froxelNearDistance w=shadowStrengthScale
  Vector4 params2;
  // rgb=sunColor w=sunIntensity
  Vector4 sunColorScale;
  Vector4 viewport;
  // x=minDepth y=maxDepth z=scene extinction mix w=unshadowed scatter fallback
  Vector4 viewportZ;
  // x=effect width y=effect height z=far clear raw w=raw depth quantum
  Vector4 rtSize;
};
static_assert(sizeof(VolumetricLightPushConstants) == 128u,
              "VolumetricLightPushConstants GLSL ABI drift");

struct VolumetricCsmUniform {
  Matrix4 view;
  Matrix4 invViewProj;
  Matrix4 lightViewProj[4];
  Vector4 splitFar;
  // x=receiverBias y=invShadowRes z=cascadeCount w=pcfRadius
  // cascadeCount: 相机 CSM 层数；若 volumeSun 启用则为 1 且 lightViewProj[0] 为体积 ortho
  Vector4 params;
  // x=cascadeBlendRange y=heightFogBase z=heightFogFalloff w=heightFogStrength
  Vector4 params2;
  // xyz=sunDir（从太阳指向地面）
  Vector4 sunDir;
  // xyz=cameraPos
  Vector4 cameraPos;
  // xyz=worldUp（与 CSM 完全一致）
  Vector4 worldUp;
  // 长期线：体积太阳 ortho。x=enabled(0/1) y=softRadius z=receiverBias w=invResolution
  // lightViewProj 与 sun 遮挡在 enabled 时由 volume sun 独占（cascadeCount=1）。
  Vector4 volumeSunParams;
};
static_assert(sizeof(VolumetricCsmUniform) == 496u,
              "VolumetricCsmUniform must match GLSL (was 480 + volumeSunParams)");

struct VolumetricPointLightUniform {
  uint32_t count = 0;
  uint32_t pointShadowSamplerIndex = 0;
  uint32_t pointShadowedLightCount = 0;
  uint32_t pad = 0;
  // x=invCubeResolution y=worldBias z=texelBiasScale w=rangeFadeStart
  Vector4 pointShadowFilter = Vector4(0.0f);
  struct {
    Vector4 pos;    // xyz=world position, w=range
    Vector4 color;  // rgb=color, w=intensity
    // x=cube layer (-1=unshadowed), y=shadow intensity
    Vector4 shadow;
  } lights[16] = {};
};

struct VolumetricFogVolumeUniform {
  uint32_t count = 0u;
  uint32_t pad0 = 0u;
  uint32_t pad1 = 0u;
  uint32_t pad2 = 0u;
  struct {
    Vector4 worldToLocal0;
    Vector4 worldToLocal1;
    Vector4 worldToLocal2;
    // x=shape (0 sphere, 1 box, 2 cylinder), y=density,
    // z=normalized edge feather, w=reserved.
    Vector4 params;
  } volumes[kVolumetricMaxFogVolumes] = {};
};

struct VolumetricCompositePushConstants {
  uint32_t colorSampler;
  uint32_t effectSampler;
  uint32_t depthSampler;
  uint32_t pad1;
  Vector4 rtSize;
};

struct VolumetricFroxelInjectPushConstants {
  uint32_t samplePhase;
  uint32_t flags;
  uint32_t gridDepth;
  uint32_t pad;
  Vector4 grid;
  Vector4 params0;
  Vector4 params1;
  Vector4 sunColorScale;
};

struct VolumetricFroxelTemporalPushConstants {
  uint32_t sampler;
  uint32_t historyValid;
  uint32_t gridDepth;
  uint32_t flags;
  Matrix4 previousViewProj;
  Vector4 grid;
  Vector4 previousCameraPos;
  Vector4 params;
};

struct VolumetricFroxelIntegratePushConstants {
  uint32_t sampler;
  uint32_t depthSampler;
  uint32_t flags;
  uint32_t gridDepth;
  Vector4 grid;
  Vector4 viewport;
  Vector4 viewportZ;
  Vector4 params;
  uint32_t shadowSampler;
  uint32_t maxTraversalSteps;
  uint32_t pad0;
  uint32_t pad1;
  Vector4 sunColorScale;
  Vector4 sunParams;
};
static_assert(sizeof(VolumetricPointLightUniform) == 800u,
              "VolumetricPointLightUniform GLSL ABI drift");
static_assert(sizeof(VolumetricFogVolumeUniform) == 528u,
              "VolumetricFogVolumeUniform GLSL ABI drift");
static_assert(sizeof(VolumetricCompositePushConstants) == 32u,
              "VolumetricCompositePushConstants GLSL ABI drift");
static_assert(sizeof(VolumetricFroxelInjectPushConstants) == 80u,
              "VolumetricFroxelInjectPushConstants GLSL ABI drift");
static_assert(sizeof(VolumetricFroxelTemporalPushConstants) == 128u,
              "VolumetricFroxelTemporalPushConstants GLSL ABI drift");
static_assert(sizeof(VolumetricFroxelIntegratePushConstants) == 128u,
              "VolumetricFroxelIntegratePushConstants GLSL ABI drift");

VkImageSubresourceLayers toLayers(const VkImageSubresourceRange& range) {
  VkImageSubresourceLayers layers = {};
  layers.aspectMask = range.aspectMask;
  layers.mipLevel = range.baseMipLevel;
  layers.baseArrayLayer = range.baseArrayLayer;
  layers.layerCount = range.layerCount;
  return layers;
}

float finiteOr(float value, float fallback) {
  return std::isfinite(value) ? value : fallback;
}

float clampFinite(float value, float lo, float hi, float fallback) {
  return std::clamp(finiteOr(value, fallback), lo, hi);
}

Vector4 SanitizeVolumetricPointPosition(const War3PointLight& light) {
  return Vector4(finiteOr(light.position.x, 0.0f),
                 finiteOr(light.position.y, 0.0f),
                 finiteOr(light.position.z, 0.0f),
                 clampFinite(light.position.w, 1.0f, 100000.0f, 1.0f));
}

war3::render::War3ImageRegion UnionRegion(
    const war3::render::War3ImageRegion& a,
    const war3::render::War3ImageRegion& b) {
  if (a.empty())
    return b;
  if (b.empty())
    return a;
  const uint32_t minX = std::min(a.x, b.x);
  const uint32_t minY = std::min(a.y, b.y);
  const uint32_t maxX = std::max(a.x + a.width, b.x + b.width);
  const uint32_t maxY = std::max(a.y + a.height, b.y + b.height);
  return {minX, minY, maxX - minX, maxY - minY};
}

VkRect2D FullRect(VkExtent3D extent) {
  return {{0, 0}, {extent.width, extent.height}};
}

bool IsFullRect(const VkRect2D& rect, VkExtent3D extent) {
  return rect.offset.x == 0 && rect.offset.y == 0 &&
         rect.extent.width == extent.width &&
         rect.extent.height == extent.height;
}

VkRect2D HalfResRegionRect(
    const war3::render::War3ImageRegion& region) {
  return {{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)},
          {region.width, region.height}};
}

VkRect2D EffectRegionToCompositeRect(
    const war3::render::War3ImageRegion& region,
    VkExtent3D effectExtent, VkExtent3D fullExtent) {
  if (region.empty())
    return {};
  if (region.x == 0u && region.y == 0u &&
      region.width == effectExtent.width &&
      region.height == effectExtent.height)
    return FullRect(fullExtent);

  // The composite reconstructs a depth-guided 2x2 low-resolution footprint.
  // Expand by one effect texel, map with the actual odd-size extent ratio,
  // then add one full-resolution pixel for pixel-centre rounding.
  const uint32_t effectLoX = region.x > 0u ? region.x - 1u : 0u;
  const uint32_t effectLoY = region.y > 0u ? region.y - 1u : 0u;
  const uint32_t effectHiX = std::min(
      effectExtent.width, region.x + region.width + 1u);
  const uint32_t effectHiY = std::min(
      effectExtent.height, region.y + region.height + 1u);
  uint32_t fullLoX = static_cast<uint32_t>(
      uint64_t(effectLoX) * fullExtent.width / effectExtent.width);
  uint32_t fullLoY = static_cast<uint32_t>(
      uint64_t(effectLoY) * fullExtent.height / effectExtent.height);
  uint32_t fullHiX = static_cast<uint32_t>(
      (uint64_t(effectHiX) * fullExtent.width + effectExtent.width - 1u) /
      effectExtent.width);
  uint32_t fullHiY = static_cast<uint32_t>(
      (uint64_t(effectHiY) * fullExtent.height + effectExtent.height - 1u) /
      effectExtent.height);
  fullLoX = fullLoX > 0u ? fullLoX - 1u : 0u;
  fullLoY = fullLoY > 0u ? fullLoY - 1u : 0u;
  fullHiX = std::min(fullExtent.width, fullHiX + 1u);
  fullHiY = std::min(fullExtent.height, fullHiY + 1u);
  return {{static_cast<int32_t>(fullLoX), static_cast<int32_t>(fullLoY)},
          {fullHiX - fullLoX, fullHiY - fullLoY}};
}

double VolumetricPointRelevance(const War3PointLight& light,
                                const Vector4& cameraPos) {
  if (!std::isfinite(cameraPos.x) || !std::isfinite(cameraPos.y) ||
      !std::isfinite(cameraPos.z) || !std::isfinite(light.position.x) ||
      !std::isfinite(light.position.y) ||
      !std::isfinite(light.position.z))
    return 0.0;

  // Mirror the values that are actually uploaded below. maxRGB preserves
  // saturated red/blue emitters while preventing a black high-intensity light
  // from occupying the limited volumetric budget.
  const double range = double(
      clampFinite(light.position.w, 1.0f, 100000.0f, 1.0f));
  const double intensity = double(
      clampFinite(light.color.w, 0.0f, 64.0f, 0.0f));
  const double colorPeak = double(std::max({
      clampFinite(light.color.x, 0.0f, 64.0f, 0.0f),
      clampFinite(light.color.y, 0.0f, 64.0f, 0.0f),
      clampFinite(light.color.z, 0.0f, 64.0f, 0.0f)}));
  const double dx = double(light.position.x) - double(cameraPos.x);
  const double dy = double(light.position.y) - double(cameraPos.y);
  const double dz = double(light.position.z) - double(cameraPos.z);
  const double distSq = dx * dx + dy * dy + dz * dz;
  const double score =
      intensity * colorPeak * range * range / (1.0 + distSq);
  return std::isfinite(score) && score >= 0.0 ? score : 0.0;
}

struct VolumetricPointSelection {
  std::array<uint32_t, War3PointLightFrameSnapshot::kMaxLights>
      sourceIndices = {};
  uint32_t count = 0u;
};

VolumetricPointSelection SelectVolumetricPointLights(
    const War3PointLightFrameSnapshot& snapshot,
    const Vector4& cameraPos,
    const War3WorldCameraState& camera,
    VkExtent3D fullExtent,
    const War3VolumetricLightSettings& settings) {
  VolumetricPointSelection result = {};
  if (!snapshot.hasAny || snapshot.count == 0u ||
      settings.maxPointLights == 0u)
    return result;

  const uint32_t budget = std::min<uint32_t>(
      settings.maxPointLights, kVolumetricMaxPointLights);
  const uint32_t available = std::min<uint32_t>(
      snapshot.count, War3PointLightFrameSnapshot::kMaxLights);
  const bool froxel =
      settings.quality == War3VolumetricQuality::FroxelMedium ||
      settings.quality == War3VolumetricQuality::FroxelHigh;
  const double maxWorldDistance = froxel
      ? double(clampFinite(settings.froxelFar, 1000.0f, 10000.0f, 10000.0f))
      : std::max(
            double(clampFinite(settings.sunDistance, 100.0f, 6000.0f,
                               1200.0f)) *
                double(clampFinite(settings.maxRayDistance, 0.05f, 2.0f,
                                   0.68f)),
            25.0);

  struct Candidate {
    uint32_t sourceIndex = 0u;
    double score = 0.0;
  };
  std::array<Candidate, War3PointLightFrameSnapshot::kMaxLights> candidates = {};
  uint32_t candidateCount = 0u;

  for (uint32_t i = 0u; i < available; ++i) {
    const War3PointLight& light = snapshot.lights[i];
    const Vector4 boundedPosition = SanitizeVolumetricPointPosition(light);
    const float range = boundedPosition.w;

    const double dx = double(boundedPosition.x) - double(cameraPos.x);
    const double dy = double(boundedPosition.y) - double(cameraPos.y);
    const double dz = double(boundedPosition.z) - double(cameraPos.z);
    const double centerDistanceSq = dx * dx + dy * dy + dz * dz;
    const double reachableCenterDistance = maxWorldDistance + double(range);
    const double conservativeReach = reachableCenterDistance +
        std::max(1.0e-4, reachableCenterDistance * 1.0e-6);
    if (!std::isfinite(centerDistanceSq) ||
        centerDistanceSq > conservativeReach * conservativeReach)
      continue;

    // Reject only spheres that the shared contact-shadow projection proves
    // cannot intersect any forward viewport ray. Camera-plane crossings and
    // invalid projections deliberately remain candidates.
    if (!war3::render::War3SphereMayIntersectViewport(
            camera, boundedPosition, fullExtent))
      continue;

    Candidate& candidate = candidates[candidateCount++];
    candidate.sourceIndex = i;
    candidate.score = VolumetricPointRelevance(light, cameraPos);
  }

  result.count = std::min(candidateCount, budget);
  if (result.count == 0u)
    return result;

  if (candidateCount > budget) {
    const auto ranksBefore = [](const Candidate& a, const Candidate& b) {
      if (a.score != b.score)
        return a.score > b.score;
      return a.sourceIndex < b.sourceIndex;
    };
    // Fixed-size insertion sort: deterministic, allocation-free, and the
    // canonical source index is an explicit stable tie breaker.
    for (uint32_t i = 1u; i < candidateCount; ++i) {
      const Candidate key = candidates[i];
      uint32_t j = i;
      while (j > 0u && ranksBefore(key, candidates[j - 1u])) {
        candidates[j] = candidates[j - 1u];
        --j;
      }
      candidates[j] = key;
    }
  }

  for (uint32_t i = 0u; i < result.count; ++i)
    result.sourceIndices[i] = candidates[i].sourceIndex;
  return result;
}

War3FogVolumeFrameSnapshot SelectVolumetricFogVolumes(
    const War3FogVolumeFrameSnapshot& snapshot,
    const War3WorldCameraState& camera, VkExtent3D fullExtent) {
  War3FogVolumeFrameSnapshot selected = {};
  selected.generation = snapshot.generation;
  selected.frameSerial = snapshot.frameSerial;
  const uint32_t available = std::min<uint32_t>(
      snapshot.count, War3FogVolumeFrameSnapshot::kMaxVolumes);
  for (uint32_t i = 0u; i < available; ++i) {
    const War3FogVolumeFrameEntry& volume = snapshot.volumes[i];
    if (!std::isfinite(volume.boundingSphere.x) ||
        !std::isfinite(volume.boundingSphere.y) ||
        !std::isfinite(volume.boundingSphere.z) ||
        !std::isfinite(volume.boundingSphere.w) ||
        volume.boundingSphere.w <= 0.0f)
      continue;
    // The shared projection helper rejects only a sphere proven not to touch
    // any forward viewport ray. Camera-plane crossings remain conservative.
    if (!war3::render::War3SphereMayIntersectViewport(
            camera, volume.boundingSphere, fullExtent))
      continue;
    selected.volumes[selected.count++] = volume;
  }
  selected.hasAny = selected.count > 0u;
  return selected;
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
  m_compositeLayout = createCompositePipelineLayout();
  m_froxelInjectLayout = createFroxelInjectLayout();
  m_froxelTemporalLayout = createFroxelTemporalLayout();
  m_froxelIntegrateLayout = createFroxelIntegrateLayout();
  m_froxelInjectPipeline = m_device->createBuiltInComputePipeline(
      m_froxelInjectLayout,
      util::DxvkBuiltInShaderStage(war3_volumetric_froxel_inject, nullptr));
  m_froxelTemporalPipeline = m_device->createBuiltInComputePipeline(
      m_froxelTemporalLayout,
      util::DxvkBuiltInShaderStage(war3_volumetric_froxel_temporal, nullptr));
  m_froxelIntegratePipeline = m_device->createBuiltInComputePipeline(
      m_froxelIntegrateLayout,
      util::DxvkBuiltInShaderStage(war3_volumetric_froxel_integrate, nullptr));

  // CSM 数据 UBO：每帧由体积光 pass 读取 shadow pass 快照后更新。
  DxvkBufferCreateInfo uboInfo = {};
  uboInfo.size = sizeof(VolumetricCsmUniform);
  uboInfo.usage =
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  uboInfo.stages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  uboInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
  uboInfo.debugName = "War3VolumetricCsmUBO";

  DxvkBufferCreateInfo lightInfo = {};
  lightInfo.size = sizeof(VolumetricPointLightUniform);
  lightInfo.usage =
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  lightInfo.stages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  lightInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
  lightInfo.debugName = "War3VolumetricPointLightUBO";

  DxvkBufferCreateInfo fogVolumeInfo = {};
  fogVolumeInfo.size = sizeof(VolumetricFogVolumeUniform);
  fogVolumeInfo.usage =
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  fogVolumeInfo.stages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  fogVolumeInfo.access =
      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
  fogVolumeInfo.debugName = "War3VolumetricFogVolumeUBO";

  // Ring buffers remove the cross-frame WAR hazard on these per-frame-updated
  // UBOs. kUboRingSlots > D3D9 MaxFrameLatency (20) guarantees the slot chosen
  // by input.frameSerial % kUboRingSlots was last used by a frame the GPU has
  // already retired, so the per-frame update needs no pre-barrier drain.
  for (uint32_t slot = 0u; slot < kUboRingSlots; ++slot) {
    m_csmUniformBuffers[slot] =
        m_device->createBuffer(uboInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    m_lightBuffers[slot] =
        m_device->createBuffer(lightInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    m_fogVolumeBuffers[slot] = m_device->createBuffer(
        fogVolumeInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }
}

War3VolumetricLightPass::~War3VolumetricLightPass() {
  auto vk = m_device->vkd();
  if (m_froxelInjectPipeline != VK_NULL_HANDLE)
    vk->vkDestroyPipeline(vk->device(), m_froxelInjectPipeline, nullptr);
  if (m_froxelTemporalPipeline != VK_NULL_HANDLE)
    vk->vkDestroyPipeline(vk->device(), m_froxelTemporalPipeline, nullptr);
  if (m_froxelIntegratePipeline != VK_NULL_HANDLE)
    vk->vkDestroyPipeline(vk->device(), m_froxelIntegratePipeline, nullptr);
  for (auto& kv : m_pipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
  for (auto& kv : m_compositePipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
}

const DxvkPipelineLayout* War3VolumetricLightPass::createPipelineLayout() const {
  // 绑定顺序需与 shader 一致：
  // 0=color, 1=depth, 2=shadowMap, 3=CSM UBO, 4=point lights,
  // 5=point-shadow cube array, 6=local fog volumes.
  std::array<DxvkDescriptorSetLayoutBinding, 7> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
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

const DxvkPipelineLayout*
War3VolumetricLightPass::createCompositePipelineLayout() const {
  std::array<DxvkDescriptorSetLayoutBinding, 3> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
  };

  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(VolumetricCompositePushConstants), bindings.size(),
      bindings.data());
}

const DxvkPipelineLayout*
War3VolumetricLightPass::createFroxelInjectLayout() const {
  std::array<DxvkDescriptorSetLayoutBinding, 6> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
  };
  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_COMPUTE_BIT,
      sizeof(VolumetricFroxelInjectPushConstants), bindings.size(),
      bindings.data());
}

const DxvkPipelineLayout*
War3VolumetricLightPass::createFroxelTemporalLayout() const {
  std::array<DxvkDescriptorSetLayoutBinding, 4> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
  };
  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_COMPUTE_BIT,
      sizeof(VolumetricFroxelTemporalPushConstants), bindings.size(),
      bindings.data());
}

const DxvkPipelineLayout*
War3VolumetricLightPass::createFroxelIntegrateLayout() const {
  std::array<DxvkDescriptorSetLayoutBinding, 5> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT),
  };
  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_COMPUTE_BIT,
      sizeof(VolumetricFroxelIntegratePushConstants), bindings.size(),
      bindings.data());
}

VkPipeline War3VolumetricLightPass::getPipeline(const PipelineKey& key) {
  auto it = m_pipelines.find(key);
  if (it != m_pipelines.end())
    return it->second;

  VkPipeline pipeline = createPipeline(key);
  m_pipelines.insert({key, pipeline});
  return pipeline;
}

VkPipeline War3VolumetricLightPass::getCompositePipeline(
    const PipelineKey& key) {
  auto it = m_compositePipelines.find(key);
  if (it != m_compositePipelines.end())
    return it->second;

  VkPipeline pipeline = createCompositePipeline(key);
  m_compositePipelines.insert({key, pipeline});
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

VkPipeline War3VolumetricLightPass::createCompositePipeline(
    const PipelineKey& key) const {
  util::DxvkBuiltInGraphicsState state = {};
  state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
  state.fs = util::DxvkBuiltInShaderStage(war3_volumetric_composite, nullptr);
  state.colorFormat = key.format;
  state.sampleCount = key.samples;

  return m_device->createBuiltInGraphicsPipeline(m_compositeLayout, state);
}

void War3VolumetricLightPass::ensureResources(VkExtent3D extent,
                                               VkFormat colorFormat,
                                               VkFormat depthFormat,
                                               uint32_t resolutionDivisor,
                                               bool storageEffect) {
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

  const uint32_t divisor = std::clamp<uint32_t>(
      resolutionDivisor, kVolumetricLocalRoiResolutionDivisor,
      kVolumetricFroxelMediumTileSize);
  VkExtent3D effectExtent = {
      std::max<uint32_t>(1u, (extent.width + divisor - 1u) / divisor),
      std::max<uint32_t>(1u, (extent.height + divisor - 1u) / divisor),
      1u};
  constexpr VkFormat effectFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  if (!m_effectImage || m_cachedEffectExtent.width != effectExtent.width ||
      m_cachedEffectExtent.height != effectExtent.height ||
      m_cachedEffectFormat != effectFormat ||
      m_effectStorageEnabled != storageEffect) {
    m_cachedEffectExtent = effectExtent;
    m_cachedEffectFormat = effectFormat;
    m_effectStorageEnabled = storageEffect;

    DxvkImageCreateInfo info = {};
    info.type = VK_IMAGE_TYPE_2D;
    info.format = effectFormat;
    info.flags = 0;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = effectExtent;
    info.numLayers = 1;
    info.mipLevels = 1;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT;
    if (storageEffect)
      info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    info.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (storageEffect)
      info.stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    info.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_SHADER_READ_BIT;
    if (storageEffect)
      info.access |= VK_ACCESS_SHADER_WRITE_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    m_effectImage =
        m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    DxvkImageViewKey viewInfo;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = effectFormat;
    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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

    m_effectView = m_effectImage->createView(viewInfo);

    m_effectStorageView = nullptr;
    if (storageEffect) {
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
      viewInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
      m_effectStorageView = m_effectImage->createView(viewInfo);
    }
  }
}

void War3VolumetricLightPass::invalidateFroxelHistory() {
  m_froxelHistoryValid = false;
  m_froxelHistoryFrameSerial = 0u;
  m_froxelHistoryMapEpoch = 0u;
  m_froxelHistoryDeviceEpoch = 0u;
  m_froxelHistoryNear = 0.0f;
  m_froxelHistoryFar = 0.0f;
}

bool War3VolumetricLightPass::ensureFroxelResources(
    VkExtent3D fullExtent, War3VolumetricQuality quality) {
  const bool high = quality == War3VolumetricQuality::FroxelHigh;
  if (!high && quality != War3VolumetricQuality::FroxelMedium)
    return false;
  const uint32_t tileSize = high ? kVolumetricFroxelHighTileSize
                                 : kVolumetricFroxelMediumTileSize;
  const uint32_t depth = high ? kVolumetricFroxelHighDepth
                              : kVolumetricFroxelMediumDepth;
  const VkExtent3D grid = {
      std::max(1u, (fullExtent.width + tileSize - 1u) / tileSize),
      std::max(1u, (fullExtent.height + tileSize - 1u) / tileSize), depth};
  const uint64_t cellCount = uint64_t(grid.width) * uint64_t(grid.height) *
                             uint64_t(grid.depth);
  if (cellCount == 0u || cellCount > kVolumetricFroxelCellBudget)
    return false;
  const uint32_t max3dDimension =
      m_device->properties().core.properties.limits.maxImageDimension3D;
  if (grid.width > max3dDimension || grid.height > max3dDimension ||
      grid.depth > max3dDimension)
    return false;
  constexpr VkFormatFeatureFlags2 requiredFeatures =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
  if ((m_device->getFormatFeatures(VK_FORMAT_R16G16B16A16_SFLOAT).optimal &
       requiredFeatures) != requiredFeatures)
    return false;

  if (m_froxelCurrentImage && m_froxelGridExtent.width == grid.width &&
      m_froxelGridExtent.height == grid.height &&
      m_froxelGridExtent.depth == grid.depth &&
      m_froxelResourceQuality == quality)
    return true;

  DxvkImageCreateInfo info = {};
  info.type = VK_IMAGE_TYPE_3D;
  info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  info.flags = 0;
  info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  info.extent = grid;
  info.numLayers = 1u;
  info.mipLevels = 1u;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.layout = VK_IMAGE_LAYOUT_GENERAL;
  info.debugName = "War3VolumetricFroxel";

  DxvkImageViewKey viewInfo = {};
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
  viewInfo.format = info.format;
  viewInfo.usage = info.usage;
  viewInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
  viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.mipIndex = 0u;
  viewInfo.mipCount = 1u;
  viewInfo.layerIndex = 0u;
  viewInfo.layerCount = 1u;

  m_froxelCurrentImage =
      m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_froxelCurrentView = m_froxelCurrentImage->createView(viewInfo);
  for (uint32_t i = 0u; i < m_froxelHistoryImages.size(); ++i) {
    m_froxelHistoryImages[i] =
        m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    m_froxelHistoryViews[i] =
        m_froxelHistoryImages[i]->createView(viewInfo);
  }
  m_froxelGridExtent = grid;
  m_froxelResourceQuality = quality;
  m_froxelHistoryIndex = 0u;
  invalidateFroxelHistory();
  return m_froxelCurrentView && m_froxelHistoryViews[0] &&
         m_froxelHistoryViews[1];
}

void War3VolumetricLightPass::ensurePointShadowFallbackResources(
    const Rc<DxvkCommandList>& ctx) {
  if (!m_pointShadowFallbackCube || !m_pointShadowFallbackCubeView) {
    DxvkImageCreateInfo info = {};
    info.type = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R32_SFLOAT;
    info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    info.extent = VkExtent3D{1u, 1u, 1u};
    info.numLayers = 6u;
    info.mipLevels = 1u;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.stages =
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.debugName = "War3VolumetricPointShadowFallbackCube";

    Rc<DxvkImage> newCube =
        m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    DxvkImageViewKey viewInfo;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    viewInfo.format = info.format;
    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    viewInfo.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.mipIndex = 0u;
    viewInfo.mipCount = 1u;
    viewInfo.layerIndex = 0u;
    viewInfo.layerCount = 6u;
    const VkComponentMapping mapping = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.packedSwizzle = DxvkImageViewKey::packSwizzle(mapping);
    Rc<DxvkImageView> newView = newCube->createView(viewInfo);

    m_pointShadowFallbackCube = std::move(newCube);
    m_pointShadowFallbackCubeView = std::move(newView);
    m_pointShadowFallbackReady = false;
  }

  if (m_pointShadowFallbackReady)
    return;

  ctx->track(m_pointShadowFallbackCube, DxvkAccess::Write);
  const VkImageSubresourceRange range = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 6u};
  VkImageMemoryBarrier2 toClear = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  toClear.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  toClear.srcAccessMask = 0u;
  toClear.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  toClear.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toClear.image = m_pointShadowFallbackCube->handle();
  toClear.subresourceRange = range;
  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.imageMemoryBarrierCount = 1u;
  depInfo.pImageMemoryBarriers = &toClear;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  VkClearColorValue clear = {};
  clear.float32[0] = 1.0f;
  clear.float32[1] = 1.0f;
  clear.float32[2] = 1.0f;
  clear.float32[3] = 1.0f;
  ctx->cmdClearColorImage(DxvkCmdBuffer::ExecBuffer,
                          m_pointShadowFallbackCube->handle(),
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          &clear, 1u, &range);

  VkImageMemoryBarrier2 toRead = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  toRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.image = m_pointShadowFallbackCube->handle();
  toRead.subresourceRange = range;
  depInfo.pImageMemoryBarriers = &toRead;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  m_pointShadowFallbackReady = true;
}

void War3VolumetricLightPass::copyColor(const Rc<DxvkCommandList>& ctx,
                                         const Rc<DxvkImageView>& srcView) {
  if (!srcView || !m_colorCopy || !m_colorCopyView)
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

  barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
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
  barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].newLayout = srcLayout;

  barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  ctx->track(srcView->image(), DxvkAccess::Read);
  ctx->track(m_colorCopy, DxvkAccess::Write);
}

void War3VolumetricLightPass::copyDepth(const Rc<DxvkCommandList>& ctx,
                                         const Rc<DxvkImageView>& srcView) {
  if (!srcView || !m_depthCopy || !m_depthCopyView)
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

  barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
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
  barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  ctx->track(srcView->image(), DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Write);
}

bool War3VolumetricLightPass::drawVolumetricLight(
    const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input,
    const War3PointLightFrameSnapshot& pointLights,
    const War3FogVolumeFrameSnapshot& fogVolumes,
    const std::array<uint32_t, War3PointLightFrameSnapshot::kMaxLights>&
        selectedPointIndices,
    uint32_t selectedPointCount, const Vector4& cameraPos,
    float farClearRaw, float rawDepthQuantum, bool farIsOne,
    const VkRect2D& effectScissor,
    uint32_t& outPointShadowedLightCount) {
  outPointShadowedLightCount = 0u;
  if (!m_layout || !m_linearSampler || !m_colorCopyView || !m_depthCopyView ||
      !m_effectView || !m_csmUniformBuffers[0] || !m_lightBuffers[0] ||
      !m_fogVolumeBuffers[0] || !input.settings)
    return false;
  if (!input.scene.worldCamera.valid)
    return false;

  // This frame's UBO ring slot. frameSerial is monotonic per frame and
  // kUboRingSlots > D3D9 MaxFrameLatency, so the reused slot's GPU reads have
  // long since retired — no cross-frame WAR pre-barrier is needed.
  const uint32_t uboSlot =
      static_cast<uint32_t>(input.frameSerial % kUboRingSlots);

  const auto& settings = input.settings->postFx.volumetricLight;

  const Vector4 frameSunColor = input.lighting != nullptr
      ? input.lighting->sunColor
      : input.settings->sun.color;
  const Vector4 frameSunDirection = input.lighting != nullptr
      ? input.lighting->sunDirection
      : input.settings->sun.direction;

  const Vector4 sanitizedSunColor =
      Vector4(clampFinite(frameSunColor.x, 0.0f, 64.0f, 0.0f),
              clampFinite(frameSunColor.y, 0.0f, 64.0f, 0.0f),
              clampFinite(frameSunColor.z, 0.0f, 64.0f, 0.0f),
              0.0f);
  const float sunColorPeak = std::max(
      {sanitizedSunColor.x, sanitizedSunColor.y, sanitizedSunColor.z});

  const float minSunIntensity =
      clampFinite(settings.minSunIntensity, 0.0f, 1.0f, 0.08f);
  const float configuredSunIntensity =
      clampFinite(input.settings->sun.enabled ? input.settings->sun.intensity
                                              : 0.0f,
                  0.0f, 64.0f, 0.0f);
  const float requestedSunIntensity =
      configuredSunIntensity >= minSunIntensity ? configuredSunIntensity
                                                : 0.0f;
  // 只有强度、没有颜色并不构成发光源。黑色/非法太阳既不能通过无源消光
  // 压暗底图，也不能在没有点光体积时强制执行全屏方向光路径。
  const float requestedSunRadiancePeak =
      requestedSunIntensity * sunColorPeak;
  const bool wantsSunVolume = requestedSunRadiancePeak > 1e-6f;
  const bool hasPointVolumeCandidate = selectedPointCount > 0u;

  auto* shadowPass =
      m_parent ? m_parent->GetWar3ShadowReceiverPass() : nullptr;

  Rc<DxvkImageView> shadowMapView = nullptr;
  War3CsmData csmData = {};
  uint32_t shadowResolution = 0u;
  Vector4 csmSunDir = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
  Vector4 csmWorldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
  const bool hasCsmSnapshot =
      shadowPass && shadowPass->GetVolumetricShadowSnapshot(
                        input.frameSerial, shadowMapView, csmData,
                        shadowResolution, csmSunDir, csmWorldUp);

  War3VolumetricSunShadowSnapshot volumeSun = {};
  const bool wantVolumeSun =
      settings.volumeSunShadowEnabled && wantsSunVolume;
  const bool hasVolumeSunSnapshot =
      wantVolumeSun && shadowPass &&
      shadowPass->GetVolumetricSunShadowSnapshot(input.frameSerial, volumeSun) &&
      volumeSun.valid();
  const bool useVolumeSunPrimary = hasVolumeSunSnapshot;
  const bool allowCsmFallback =
      settings.volumeSunShadowFallbackToCsm || !settings.volumeSunShadowEnabled;
  // 太阳体积遮挡：优先体积 ortho；否则按合同回退相机 CSM 或抑制太阳。
  const bool hasDirectionalShadowSource =
      useVolumeSunPrimary || (allowCsmFallback && hasCsmSnapshot);

  if (!hasDirectionalShadowSource && !hasCsmSnapshot) {
    // CSM is a dependency of directional occlusion only. A valid point-light
    // volume must keep running when the caller requires CSM but the sun
    // snapshot is temporarily unavailable; in that case the sun contribution
    // is suppressed below instead of fabricating an unshadowed shaft.
    if (settings.requireCsmSnapshot && wantsSunVolume &&
        !hasPointVolumeCandidate)
      return false;

    // The shader statically binds a texture2DArray shadow slot. Reuse the
    // already-copied depth view as a legal placeholder and upload cascadeCount
    // zero; computeShadowVisibility then never samples it. This makes the UI's
    // "require CSM" switch a real contract instead of a silent no-op.
    shadowMapView = m_depthCopyView;
    shadowResolution = 1u;
    csmData = {};
    csmSunDir = frameSunDirection;
    csmWorldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
    if (wantsSunVolume) {
      if (settings.requireCsmSnapshot) {
        static bool s_loggedSunSuppressedWithoutCsm = false;
        if (!s_loggedSunSuppressedWithoutCsm) {
          s_loggedSunSuppressedWithoutCsm = true;
          WAR3_RENDER_LOG(
              "DXVK War3Volumetric: no volume-sun/CSM snapshot; suppressing sun "
              "scattering while retaining independent point-light volume\n");
        }
      } else {
        static bool s_loggedOptionalCsmFallback = false;
        if (!s_loggedOptionalCsmFallback) {
          s_loggedOptionalCsmFallback = true;
          WAR3_RENDER_LOG(
              "DXVK War3Volumetric: no volume-sun/CSM snapshot; using bounded "
              "unshadowed sun-scattering fallback\n");
        }
      }
    }
  } else if (useVolumeSunPrimary) {
    shadowMapView = volumeSun.depthView;
    shadowResolution = volumeSun.resolution;
    csmSunDir = volumeSun.lightDirection;
    csmWorldUp = volumeSun.worldUp;
  } else if (!hasCsmSnapshot) {
    shadowMapView = m_depthCopyView;
    shadowResolution = 1u;
    csmData = {};
    csmSunDir = frameSunDirection;
    csmWorldUp = Vector4(0.0f, 0.0f, 1.0f, 0.0f);
  }

  const float sunIntensity =
      wantsSunVolume &&
              (hasDirectionalShadowSource || !settings.requireCsmSnapshot)
          ? requestedSunIntensity
          : 0.0f;

  // ShadowReceiver publishes cube contents only after the full generation,
  // frame, content signature and six-face validity tuple is complete. Volume
  // uses stable light ids from this immutable snapshot because its relevance
  // top-K order intentionally differs from the canonical cube-layer order.
  War3VolumetricPointShadowSnapshot pointShadowSnapshot = {};
  const bool hasPointShadowSnapshot =
      hasPointVolumeCandidate && shadowPass &&
      shadowPass->GetVolumetricPointShadowSnapshot(
          pointLights.generation, pointLights.frameSerial,
          pointShadowSnapshot);

  Rc<DxvkImageView> pointShadowSampleView;
  Rc<DxvkSampler> pointShadowSampler;
  if (hasPointShadowSnapshot) {
    pointShadowSampleView = pointShadowSnapshot.cubeView;
    pointShadowSampler = pointShadowSnapshot.sampler;
  } else {
    // The shader statically declares textureCubeArray. Bind a dimension-correct
    // fail-lit resource even when point shadows are disabled or stale; never
    // substitute a 2D/2D-array CSM view.
    ensurePointShadowFallbackResources(ctx);
    pointShadowSampleView = m_pointShadowFallbackCubeView;
    pointShadowSampler = m_linearSampler;
  }
  if (!pointShadowSampleView || !pointShadowSampler)
    return false;

  auto effectImage = m_effectView->image();

  PipelineKey key = {};
  key.format = effectImage->info().format;
  key.samples = VK_SAMPLE_COUNT_1_BIT;

  VkPipeline pipeline = getPipeline(key);
  if (pipeline == VK_NULL_HANDLE)
    return false;

  VkExtent3D effectExtent = effectImage->info().extent;

  {
    VkImageMemoryBarrier2 toWrite = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toWrite.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toWrite.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toWrite.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toWrite.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toWrite.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toWrite.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toWrite.image = effectImage->handle();
    toWrite.subresourceRange = m_effectView->imageSubresources();

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &toWrite;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  VkRenderingAttachmentInfo attachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = m_effectView->handle();
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea.offset = {0, 0};
  renderInfo.renderArea.extent = {effectExtent.width, effectExtent.height};
  renderInfo.layerCount = 1u;
  renderInfo.colorAttachmentCount = 1u;
  renderInfo.pColorAttachments = &attachment;

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = float(effectExtent.width);
  viewport.height = float(effectExtent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &effectScissor);

  // 体积太阳主路径：1–2 层固定半径 ortho（近锐/远盖），不按相机 split。
  // 回退路径：保留相机 CSM 多级联合同。
  const uint32_t volumeLayers = useVolumeSunPrimary
      ? std::min<uint32_t>(std::max<uint32_t>(volumeSun.cascadeCount, 1u), 2u)
      : 0u;
  const uint32_t cascadeCount = useVolumeSunPrimary
      ? volumeLayers
      : (hasCsmSnapshot
             ? std::min<uint32_t>(std::max<uint32_t>(csmData.cascadeCount, 1u),
                                  4u)
             : 0u);
  const uint32_t safeCascadeCount = std::max<uint32_t>(cascadeCount, 1u);
  VolumetricCsmUniform csmUbo = {};
  csmUbo.view = input.scene.worldCamera.view;
  csmUbo.invViewProj = input.scene.worldCamera.invViewProj;
  if (useVolumeSunPrimary) {
    csmUbo.lightViewProj[0] = volumeSun.lightViewProj[0];
    csmUbo.lightViewProj[1] = (volumeLayers > 1u)
                                  ? volumeSun.lightViewProj[1]
                                  : volumeSun.lightViewProj[0];
    csmUbo.lightViewProj[2] = csmUbo.lightViewProj[1];
    csmUbo.lightViewProj[3] = csmUbo.lightViewProj[1];
  } else {
    for (uint32_t i = 0; i < 4; i++) {
      const uint32_t idx =
          (i < safeCascadeCount) ? i : (safeCascadeCount - 1u);
      csmUbo.lightViewProj[i] = csmData.cascades[idx].lightViewProj;
    }
  }
  const float fallbackSplit = std::max(settings.sunDistance, 1.0f);
  // volume-sun 路径不使用 split 选层（shader 专用分支）；填极大值防误用。
  const float split0 = useVolumeSunPrimary
                           ? 1.0e9f
                           : (cascadeCount > 0u ? csmData.cascades[0].splitFar
                                                : fallbackSplit);
  const float split1 = useVolumeSunPrimary
                           ? split0
                           : ((cascadeCount > 1u) ? csmData.cascades[1].splitFar
                                                  : split0);
  const float split2 = useVolumeSunPrimary
                           ? split0
                           : ((cascadeCount > 2u) ? csmData.cascades[2].splitFar
                                                  : split1);
  const float split3 = useVolumeSunPrimary
                           ? split0
                           : ((cascadeCount > 3u) ? csmData.cascades[3].splitFar
                                                  : split2);
  csmUbo.splitFar = Vector4(split0, split1, split2, split3);

  // 使用 CSM 快照中的光向，避免与 CSM 内部符号修正不一致。
  Vector4 sunDir = csmSunDir;
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

  csmUbo.cameraPos = cameraPos;
  // 使用 CSM 快照中的 worldUp，避免相机俯仰触发 Y/Z 轴切换。
  Vector4 worldUp = csmWorldUp;
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

  const float receiverBias = useVolumeSunPrimary
      ? clampFinite(volumeSun.receiverBias, 0.0f, 0.01f, 0.0001f)
      : clampFinite(input.settings->shadows.receiverBias, 0.0f, 0.05f, 0.004f);
  const float invShadowRes =
      1.0f / float(std::max<uint32_t>(shadowResolution, 1u));
  const float pcfRadius = useVolumeSunPrimary
      ? clampFinite(volumeSun.softRadius, 0.0f, 8.0f, 1.35f)
      : clampFinite(input.settings->shadows.pcfRadius, 0.0f, 8.0f, 0.85f);
  // 体积太阳单层无需相机 cascade blend；回退 CSM 时保留原 blend。
  const float cascadeBlendRange = useVolumeSunPrimary
      ? 0.0f
      : clampFinite(input.settings->shadows.cascadeBlendRange, 0.0f, 2000.0f,
                    120.0f);
  const float heightFogBase = finiteOr(settings.heightFogBase, 0.0f);
  const float heightFogFalloff =
      clampFinite(settings.heightFogFalloff, 0.00001f, 0.05f, 0.0012f);
  const float heightFogStrength = settings.heightFogEnabled
      ? clampFinite(settings.heightFogStrength, 0.0f, 2.0f, 0.20f)
      : 0.0f;
  csmUbo.params =
      Vector4(receiverBias, invShadowRes, float(cascadeCount), pcfRadius);
  csmUbo.params2 = Vector4(cascadeBlendRange, heightFogBase, heightFogFalloff,
                           heightFogStrength);
  csmUbo.volumeSunParams = Vector4(
      useVolumeSunPrimary ? 1.0f : 0.0f, pcfRadius, receiverBias, invShadowRes);

  auto csmInfo = m_csmUniformBuffers[uboSlot]->getSliceInfo(
      0u, sizeof(VolumetricCsmUniform));

  VolumetricPointLightUniform lightUbo = {};
  lightUbo.pointShadowSamplerIndex =
      pointShadowSampler->getDescriptor().samplerIndex;
  if (hasPointShadowSnapshot) {
    const float invResolution =
        1.0f / float(std::max(pointShadowSnapshot.resolution, 1u));
    const float worldBias = pointShadowSnapshot.lightCount > 0u
        ? std::max(0.0f, finiteOr(
              pointShadowSnapshot.lights[0].bias, 0.05f))
        : 0.05f;
    lightUbo.pointShadowFilter = Vector4(
        invResolution, worldBias,
        clampFinite(pointShadowSnapshot.filterParams.z, 0.0f, 1.0f, 0.50f),
        clampFinite(pointShadowSnapshot.filterParams.w, 0.50f, 0.98f,
                    0.78f));
  }
  // Run() already selected the exact immutable frame snapshot before any
  // resource/copy work. Reuse it here: no second manager lookup, no consumer
  // ordering drift, and defensive bounds checks fail soft to fewer lights.
  if (hasPointVolumeCandidate) {
    const uint32_t available = std::min<uint32_t>(
        pointLights.count, War3PointLightFrameSnapshot::kMaxLights);
    const uint32_t requested = std::min<uint32_t>(
        selectedPointCount, kVolumetricMaxPointLights);
    for (uint32_t i = 0u; i < requested; ++i) {
      const uint32_t sourceIndex = selectedPointIndices[i];
      if (sourceIndex >= available)
        continue;
      const War3PointLight& sourceLight = pointLights.lights[sourceIndex];
      const Vector4 srcPos = SanitizeVolumetricPointPosition(sourceLight);
      const Vector4& srcColor = sourceLight.color;
      const uint32_t dst = lightUbo.count++;
      lightUbo.lights[dst].pos = srcPos;
      lightUbo.lights[dst].color =
          Vector4(clampFinite(srcColor.x, 0.0f, 64.0f, 0.0f),
                  clampFinite(srcColor.y, 0.0f, 64.0f, 0.0f),
                  clampFinite(srcColor.z, 0.0f, 64.0f, 0.0f),
                  clampFinite(srcColor.w, 0.0f, 64.0f, 0.0f));
      lightUbo.lights[dst].shadow = Vector4(-1.0f, 0.0f, 0.0f, 0.0f);

      // One cube lookup per overlapped march segment is enough to stop the
      // dominant through-wall leak. Cap this to the two most relevant selected
      // lights; copying the surface receiver's 16-tap PCF here would multiply
      // into hundreds of millions of samples at high quality.
      if (hasPointShadowSnapshot &&
          lightUbo.pointShadowedLightCount < 2u) {
        for (uint32_t shadowIndex = 0u;
             shadowIndex < pointShadowSnapshot.lightCount;
             ++shadowIndex) {
          const auto& shadowLight = pointShadowSnapshot.lights[shadowIndex];
          if (shadowLight.lightId != sourceLight.id)
            continue;
          const float shadowIntensity =
              clampFinite(shadowLight.shadowIntensity, 0.0f, 1.0f, 0.0f);
          if (shadowIntensity <= 1.0e-4f)
            break;
          lightUbo.lights[dst].shadow =
              Vector4(float(shadowLight.cubeLayer), shadowIntensity,
                      0.0f, 0.0f);
          ++lightUbo.pointShadowedLightCount;
          break;
        }
      }
    }
  }
  outPointShadowedLightCount = lightUbo.pointShadowedLightCount;

  auto lightInfo = m_lightBuffers[uboSlot]->getSliceInfo(
      0u, sizeof(VolumetricPointLightUniform));

  VolumetricFogVolumeUniform fogVolumeUbo = {};
  fogVolumeUbo.count = std::min<uint32_t>(
      fogVolumes.count, kVolumetricMaxFogVolumes);
  for (uint32_t i = 0u; i < fogVolumeUbo.count; ++i) {
    const War3FogVolumeFrameEntry& source = fogVolumes.volumes[i];
    fogVolumeUbo.volumes[i].worldToLocal0 = source.worldToLocal0;
    fogVolumeUbo.volumes[i].worldToLocal1 = source.worldToLocal1;
    fogVolumeUbo.volumes[i].worldToLocal2 = source.worldToLocal2;
    fogVolumeUbo.volumes[i].params = source.params;
  }
  auto fogVolumeInfo = m_fogVolumeBuffers[uboSlot]->getSliceInfo(
      0u, sizeof(VolumetricFogVolumeUniform));

  // Ring-slice UBOs: this slot was last written kUboRingSlots frames ago
  // (> D3D9 MaxFrameLatency), so no in-flight frame is still reading it. The
  // pre-update barrier therefore uses no source scope (NONE) instead of the
  // former FRAGMENT_SHADER->TRANSFER cross-frame WAR drain; the post-update
  // barrier below still makes the write visible to this frame's uniform reads.
  std::array<VkBufferMemoryBarrier2, 3> bufferBarriers = {};
  bufferBarriers[0] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  bufferBarriers[0].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  bufferBarriers[0].srcAccessMask = VK_ACCESS_2_NONE;
  bufferBarriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  bufferBarriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  bufferBarriers[0].buffer = csmInfo.buffer;
  bufferBarriers[0].offset = csmInfo.offset;
  bufferBarriers[0].size = sizeof(VolumetricCsmUniform);

  bufferBarriers[1] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  bufferBarriers[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  bufferBarriers[1].srcAccessMask = VK_ACCESS_2_NONE;
  bufferBarriers[1].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  bufferBarriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  bufferBarriers[1].buffer = lightInfo.buffer;
  bufferBarriers[1].offset = lightInfo.offset;
  bufferBarriers[1].size = sizeof(VolumetricPointLightUniform);

  bufferBarriers[2] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  bufferBarriers[2].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  bufferBarriers[2].srcAccessMask = VK_ACCESS_2_NONE;
  bufferBarriers[2].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  bufferBarriers[2].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  bufferBarriers[2].buffer = fogVolumeInfo.buffer;
  bufferBarriers[2].offset = fogVolumeInfo.offset;
  bufferBarriers[2].size = sizeof(VolumetricFogVolumeUniform);

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.bufferMemoryBarrierCount =
      static_cast<uint32_t>(bufferBarriers.size());
  depInfo.pBufferMemoryBarriers = bufferBarriers.data();
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, csmInfo.buffer, csmInfo.offset,
                       sizeof(VolumetricCsmUniform), &csmUbo);
  ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, lightInfo.buffer,
                       lightInfo.offset, sizeof(lightUbo), &lightUbo);
  ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, fogVolumeInfo.buffer,
                       fogVolumeInfo.offset, sizeof(fogVolumeUbo),
                       &fogVolumeUbo);

  for (auto &barrier : bufferBarriers) {
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  }
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

  std::array<DxvkDescriptorWrite, 7> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor = m_colorCopyView->getDescriptor();
  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = m_depthCopyView->getDescriptor();
  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[2].descriptor = shadowMapView->getDescriptor();
  descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[3].descriptor = nullptr;
  descriptors[3].buffer = csmInfo;
  descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[4].descriptor = nullptr;
  descriptors[4].buffer = lightInfo;
  descriptors[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[5].descriptor = pointShadowSampleView->getDescriptor();
  descriptors[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[6].descriptor = nullptr;
  descriptors[6].buffer = fogVolumeInfo;

  VolumetricLightPushConstants pc = {};
  pc.colorSampler = m_linearSampler->getDescriptor().samplerIndex;
  pc.depthSampler = m_linearSampler->getDescriptor().samplerIndex;
  pc.shadowSampler = hasCsmSnapshot && shadowPass
      ? shadowPass->GetShadowSamplerIndex()
      : m_linearSampler->getDescriptor().samplerIndex;

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
  if (farIsOne)
    pc.flags |= 0x20u;

  pc.params0 =
      Vector4(clampFinite(settings.intensity, 0.0f, 4.0f, 0.0f),
              clampFinite(settings.decay, 0.70f, 0.999f, 0.945f),
              settings.globalMediumEnabled
                  ? clampFinite(settings.density, 0.0f, 2.0f, 0.0f)
                  : 0.0f,
              clampFinite(settings.weight, 0.0f, 3.0f, 1.0f));
  const float fadeNear =
      clampFinite(settings.fadeNear, 0.0f, 0.95f, 0.05f);
  const float fadeFar = clampFinite(settings.fadeFar, fadeNear + 0.01f, 1.0f,
                                    std::max(fadeNear + 0.01f, 0.78f));
  pc.params1 =
      Vector4(clampFinite(settings.skyThreshold, 0.55f, 0.99f, 0.72f),
              fadeNear, fadeFar,
              clampFinite(settings.maxRayDistance, 0.05f, 2.0f, 0.68f));
  const float shadowStrengthScale =
      input.settings->shadows.enabled
          ? clampFinite(input.settings->shadows.strength, 0.0f, 1.0f, 1.0f)
          : 0.0f;
  // Adaptive quality is subordinate to the hard ray-segment budget below.
  // Raising an external setting can never restore the old TDR-prone loop.
  int effectiveSamples = std::clamp(
      settings.sampleCount, kVolumetricMinSamples, kVolumetricMaxSamples);
  if (settings.adaptiveSampleCount) {
    const float intensity01 = std::clamp(pc.params0.x / 0.35f, 0.55f, 1.0f);
    const float source01 = sunIntensity > 1e-6f
        ? std::clamp(sunIntensity, 0.45f, 1.0f)
        : (lightUbo.count > 0u ? 1.0f : 0.55f);
    // Shadow strength controls radiometric contrast, not geometric sampling
    // frequency. Scaling samples by it made a perfectly valid .60 shadow
    // setting turn a requested 16-step unit silhouette into only ~12 probes.
    // The independent ray-segment budget below remains the hard TDR backstop.
    const float quality = std::min(intensity01, source01);
    const int adaptive =
        static_cast<int>(std::lround(float(effectiveSamples) * quality));
    const int adaptiveFloor = std::min(12, effectiveSamples);
    effectiveSamples =
        std::clamp(adaptive, adaptiveFloor, effectiveSamples);
  }
  // Only scissored fragments are submitted. Charging the complete backing
  // image would force a small half-resolution local ROI down to four samples
  // even though the watchdog work outside the ROI does not exist.
  const uint64_t effectPixelCount =
      uint64_t(effectScissor.extent.width) *
      uint64_t(effectScissor.extent.height);
  const uint64_t budgetedSamples = effectPixelCount != 0u
      ? kVolumetricRaySegmentBudget / effectPixelCount
      : uint64_t(kVolumetricMinSamples);
  effectiveSamples = std::min(
      effectiveSamples,
      std::clamp<int>(
          static_cast<int>(std::min<uint64_t>(
              budgetedSamples, uint64_t(kVolumetricMaxSamples))),
          kVolumetricMinSamples, kVolumetricMaxSamples));
  if (fogVolumeUbo.count > 0u && effectPixelCount > 0u) {
    const uint64_t fogInteractionMultiplier =
        1u + uint64_t(lightUbo.count);
    const uint64_t denominator =
        effectPixelCount * uint64_t(fogVolumeUbo.count) *
        fogInteractionMultiplier;
    const uint64_t fogBudgetedSamples = denominator > 0u
        ? kVolumetricFogSegmentTestBudget / denominator
        : uint64_t(kVolumetricMinSamples);
    effectiveSamples = std::min(
        effectiveSamples,
        std::clamp<int>(
            static_cast<int>(std::min<uint64_t>(
                fogBudgetedSamples, uint64_t(kVolumetricMaxSamples))),
            kVolumetricMinSamples, kVolumetricMaxSamples));
  }
  pc.params2 =
      Vector4(float(effectiveSamples),
              clampFinite(settings.sunDistance, 100.0f, 6000.0f, 1200.0f),
              clampFinite(settings.froxelNear, 0.1f, 500.0f, 20.0f),
              shadowStrengthScale);
  pc.sunColorScale =
      Vector4(sanitizedSunColor.x, sanitizedSunColor.y,
              sanitizedSunColor.z, std::max(0.0f, sunIntensity));
  pc.viewport = Vector4(float(input.scene.worldCamera.viewport.X),
                        float(input.scene.worldCamera.viewport.Y),
                        float(input.scene.worldCamera.viewport.Width),
                        float(input.scene.worldCamera.viewport.Height));
  pc.viewportZ = Vector4(
      input.scene.worldCamera.viewport.MinZ,
      input.scene.worldCamera.viewport.MaxZ,
      clampFinite(settings.extinctionStrength, 0.0f, 1.0f, 0.18f),
      clampFinite(settings.unshadowedScattering, 0.0f, 1.0f, 0.22f));
  pc.rtSize = Vector4(float(effectExtent.width), float(effectExtent.height),
                      farClearRaw, rawDepthQuantum);

  // Volumetric Lighting 2.0. Run owns the independent effect-resolution and
  // traversal budgets. A storage-capable effect view is therefore the exact
  // admission proof; do not recompute a fixed divisor here because High can
  // conservatively degrade from 1/4 to 1/8 at very large render targets.
  const bool wantsFroxel =
      settings.quality == War3VolumetricQuality::FroxelMedium ||
      settings.quality == War3VolumetricQuality::FroxelHigh;
  const VkExtent3D fullExtent = m_colorCopy->info().extent;
  const bool froxelExtentAdmitted = wantsFroxel && m_effectStorageView;

  if (froxelExtentAdmitted &&
      ensureFroxelResources(fullExtent, settings.quality) &&
      m_froxelInjectLayout && m_froxelTemporalLayout &&
      m_froxelIntegrateLayout &&
      m_froxelInjectPipeline != VK_NULL_HANDLE &&
      m_froxelTemporalPipeline != VK_NULL_HANDLE &&
      m_froxelIntegratePipeline != VK_NULL_HANDLE &&
      m_effectStorageView) {
    // Every XY column shares this exact radial distribution. Scene depth is
    // only an integration terminator; it must never slide or rescale the grid.
    const float froxelFar = clampFinite(
        settings.froxelFar, 1000.0f, 10000.0f, 10000.0f);
    const float froxelNear = std::clamp(
        clampFinite(settings.froxelNear, 0.1f, 500.0f, 20.0f), 0.1f,
        std::max(0.1f, froxelFar - 1.0f));

    // A single conservative compute dependency covers ping-pong reuse:
    // previous temporal/integration reads complete before current and history
    // targets are overwritten. All 3D images stay in GENERAL.
    VkMemoryBarrier2 computeReuse = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    computeReuse.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    computeReuse.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                                 VK_ACCESS_2_SHADER_WRITE_BIT;
    computeReuse.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    computeReuse.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo computeReuseInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    computeReuseInfo.memoryBarrierCount = 1u;
    computeReuseInfo.pMemoryBarriers = &computeReuse;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &computeReuseInfo);

    std::array<DxvkDescriptorWrite, 6> injectDescriptors = {};
    injectDescriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    injectDescriptors[0].descriptor = m_froxelCurrentView->getDescriptor();
    injectDescriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    injectDescriptors[1].descriptor = shadowMapView->getDescriptor();
    injectDescriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    injectDescriptors[2].buffer = csmInfo;
    injectDescriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    injectDescriptors[3].buffer = lightInfo;
    injectDescriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    injectDescriptors[4].descriptor = pointShadowSampleView->getDescriptor();
    injectDescriptors[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    injectDescriptors[5].buffer = fogVolumeInfo;

    VolumetricFroxelInjectPushConstants injectPc = {};
    injectPc.samplePhase = static_cast<uint32_t>(input.frameSerial & 7u);
    injectPc.flags = pc.flags;
    injectPc.gridDepth = m_froxelGridExtent.depth;
    injectPc.grid = Vector4(float(m_froxelGridExtent.width),
                            float(m_froxelGridExtent.height), froxelNear,
                            froxelFar);
    const float decayN = std::clamp(
        (clampFinite(settings.decay, 0.70f, 0.999f, 0.95f) - 0.70f) /
            0.299f,
        0.0f, 1.0f);
    injectPc.params0 = Vector4(
        clampFinite(settings.intensity, 0.0f, 4.0f, 0.0f),
        0.72f + (0.96f - 0.72f) * decayN,
        settings.globalMediumEnabled
            ? clampFinite(settings.density, 0.0f, 2.0f, 0.0f)
            : 0.0f,
        clampFinite(settings.weight, 0.0f, 3.0f, 1.0f));
    injectPc.params1 = Vector4(
        shadowStrengthScale,
        settings.quality == War3VolumetricQuality::FroxelHigh ? 4.0f : 2.0f,
        clampFinite(settings.weight, 0.0f, 3.0f, 1.0f), 0.0f);
    injectPc.sunColorScale = pc.sunColorScale;

    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                         VK_PIPELINE_BIND_POINT_COMPUTE,
                         m_froxelInjectPipeline);
    ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_froxelInjectLayout,
                       injectDescriptors.size(), injectDescriptors.data(),
                       sizeof(injectPc), &injectPc);
    ctx->cmdDispatch(DxvkCmdBuffer::ExecBuffer,
                     (m_froxelGridExtent.width + 3u) / 4u,
                     (m_froxelGridExtent.height + 3u) / 4u,
                     (m_froxelGridExtent.depth + 3u) / 4u);

    VkMemoryBarrier2 injectionReady = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    injectionReady.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    injectionReady.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    injectionReady.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    injectionReady.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo injectionReadyInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    injectionReadyInfo.memoryBarrierCount = 1u;
    injectionReadyInfo.pMemoryBarriers = &injectionReady;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &injectionReadyInfo);

    const float cameraDeltaX = cameraPos.x - m_froxelHistoryCameraPos.x;
    const float cameraDeltaY = cameraPos.y - m_froxelHistoryCameraPos.y;
    const float cameraDeltaZ = cameraPos.z - m_froxelHistoryCameraPos.z;
    const float cameraDelta = std::sqrt(cameraDeltaX * cameraDeltaX +
                                        cameraDeltaY * cameraDeltaY +
                                        cameraDeltaZ * cameraDeltaZ);
    const bool consecutiveFrame = input.frameSerial != 0u &&
        m_froxelHistoryFrameSerial != 0u &&
        input.frameSerial == m_froxelHistoryFrameSerial + 1u;
    const bool sameDistribution =
        std::abs(froxelNear - m_froxelHistoryNear) <= 1.0e-3f &&
        std::abs(froxelFar - m_froxelHistoryFar) <=
            std::max(1.0e-3f, froxelFar * 1.0e-5f);
    const float historyCameraLimit = std::min(512.0f, froxelFar * 0.10f);
    const bool historyReadable = settings.froxelTemporalEnabled &&
        m_froxelHistoryValid && input.mapEpoch != 0u &&
        input.deviceEpoch != 0u && input.mapEpoch == m_froxelHistoryMapEpoch &&
        input.deviceEpoch == m_froxelHistoryDeviceEpoch && consecutiveFrame &&
        sameDistribution && std::isfinite(cameraDelta) &&
        cameraDelta <= historyCameraLimit;
    const uint32_t historyReadIndex = m_froxelHistoryIndex;
    const uint32_t historyWriteIndex = (historyReadIndex + 1u) % 2u;

    std::array<DxvkDescriptorWrite, 4> temporalDescriptors = {};
    temporalDescriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    temporalDescriptors[0].descriptor = m_froxelCurrentView->getDescriptor();
    temporalDescriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    temporalDescriptors[1].descriptor =
        m_froxelHistoryViews[historyReadIndex]->getDescriptor();
    temporalDescriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    temporalDescriptors[2].descriptor =
        m_froxelHistoryViews[historyWriteIndex]->getDescriptor();
    temporalDescriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    temporalDescriptors[3].buffer = csmInfo;

    VolumetricFroxelTemporalPushConstants temporalPc = {};
    temporalPc.sampler = m_linearSampler->getDescriptor().samplerIndex;
    temporalPc.historyValid = historyReadable ? 1u : 0u;
    temporalPc.gridDepth = m_froxelGridExtent.depth;
    temporalPc.flags = pc.flags;
    temporalPc.previousViewProj = historyReadable
        ? m_froxelHistoryViewProj
        : input.scene.worldCamera.viewProj;
    temporalPc.grid = injectPc.grid;
    temporalPc.previousCameraPos = historyReadable
        ? m_froxelHistoryCameraPos
        : cameraPos;
    temporalPc.params = Vector4(
        clampFinite(settings.froxelHistoryWeight, 0.0f, 0.98f, 0.88f),
        clampFinite(settings.froxelVarianceGamma, 0.5f, 4.0f, 1.5f),
        clampFinite(settings.froxelReactiveScale, 0.0f, 4.0f, 1.25f), 0.0f);

    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                         VK_PIPELINE_BIND_POINT_COMPUTE,
                         m_froxelTemporalPipeline);
    ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_froxelTemporalLayout,
                       temporalDescriptors.size(), temporalDescriptors.data(),
                       sizeof(temporalPc), &temporalPc);
    ctx->cmdDispatch(DxvkCmdBuffer::ExecBuffer,
                     (m_froxelGridExtent.width + 3u) / 4u,
                     (m_froxelGridExtent.height + 3u) / 4u,
                     (m_froxelGridExtent.depth + 3u) / 4u);

    VkMemoryBarrier2 temporalReady = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    temporalReady.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    temporalReady.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    temporalReady.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    temporalReady.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo temporalReadyInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    temporalReadyInfo.memoryBarrierCount = 1u;
    temporalReadyInfo.pMemoryBarriers = &temporalReady;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &temporalReadyInfo);

    VkImageMemoryBarrier2 effectToWrite = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    effectToWrite.srcStageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    effectToWrite.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    effectToWrite.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    effectToWrite.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    effectToWrite.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    effectToWrite.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    effectToWrite.image = m_effectImage->handle();
    effectToWrite.subresourceRange = m_effectView->imageSubresources();
    VkDependencyInfo effectToWriteInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    effectToWriteInfo.imageMemoryBarrierCount = 1u;
    effectToWriteInfo.pImageMemoryBarriers = &effectToWrite;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &effectToWriteInfo);

    std::array<DxvkDescriptorWrite, 5> integrateDescriptors = {};
    integrateDescriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    integrateDescriptors[0].descriptor =
        m_froxelHistoryViews[historyWriteIndex]->getDescriptor();
    integrateDescriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    integrateDescriptors[1].descriptor = m_depthCopyView->getDescriptor();
    integrateDescriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    integrateDescriptors[2].descriptor = m_effectStorageView->getDescriptor();
    integrateDescriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    integrateDescriptors[3].buffer = csmInfo;
    integrateDescriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    integrateDescriptors[4].descriptor = shadowMapView->getDescriptor();

    VolumetricFroxelIntegratePushConstants integratePc = {};
    integratePc.sampler = m_linearSampler->getDescriptor().samplerIndex;
    integratePc.depthSampler = m_linearSampler->getDescriptor().samplerIndex;
    integratePc.flags = pc.flags;
    integratePc.gridDepth = m_froxelGridExtent.depth;
    integratePc.grid = injectPc.grid;
    integratePc.viewport = pc.viewport;
    integratePc.viewportZ = Vector4(pc.viewportZ.x, pc.viewportZ.y,
                                    farClearRaw, rawDepthQuantum);
    integratePc.params = Vector4(
        float(effectExtent.width), float(effectExtent.height),
        clampFinite(settings.extinctionStrength, 0.0f, 1.0f, 0.05f),
        injectPc.params0.y);
    integratePc.shadowSampler = pc.shadowSampler;
    // One budget is shared by the entire camera ray, not restarted for every
    // Z slice. The shader walks actual shadow-map texel/depth-crossing
    // intervals and only enters a bounded dense fallback after this budget.
    // Low-pitch rays can cross hundreds of volume-sun texels before reaching
    // the caster column. The previous 128/256 cap exhausted early and silently
    // fell back to one midpoint per Z slice, recreating the stacked-caster
    // artifact and making the column shrink as its interval moved farther down
    // the ray. The integration shader now consumes this budget directly while
    // accumulating texel/depth-crossing sub-intervals.
    integratePc.maxTraversalSteps =
        settings.quality == War3VolumetricQuality::FroxelHigh
            ? kVolumetricFroxelHighTraversalSteps
            : kVolumetricFroxelMediumTraversalSteps;
    integratePc.sunColorScale = Vector4(
        pc.sunColorScale.x, pc.sunColorScale.y, pc.sunColorScale.z,
        pc.sunColorScale.w * injectPc.params0.x);
    integratePc.sunParams = Vector4(
        clampFinite((settings.skyThreshold - 0.55f) * 0.50f, 0.0f, 0.22f,
                    0.08f),
        shadowStrengthScale,
        settings.requireCsmSnapshot
            ? 0.0f
            : clampFinite(settings.unshadowedScattering, 0.0f, 1.0f, 0.03f),
        clampFinite(settings.weight, 0.0f, 3.0f, 1.0f));

    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                         VK_PIPELINE_BIND_POINT_COMPUTE,
                         m_froxelIntegratePipeline);
    ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_froxelIntegrateLayout,
                       integrateDescriptors.size(), integrateDescriptors.data(),
                       sizeof(integratePc), &integratePc);
    ctx->cmdDispatch(DxvkCmdBuffer::ExecBuffer,
                     (effectExtent.width + 7u) / 8u,
                     (effectExtent.height + 7u) / 8u, 1u);

    VkImageMemoryBarrier2 effectToRead = effectToWrite;
    effectToRead.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    effectToRead.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    effectToRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    effectToRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    effectToRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    effectToRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    effectToWriteInfo.pImageMemoryBarriers = &effectToRead;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &effectToWriteInfo);

    m_froxelHistoryIndex = historyWriteIndex;
    m_froxelHistoryValid = settings.froxelTemporalEnabled &&
        input.frameSerial != 0u && input.mapEpoch != 0u &&
        input.deviceEpoch != 0u;
    m_froxelHistoryFrameSerial = input.frameSerial;
    m_froxelHistoryMapEpoch = input.mapEpoch;
    m_froxelHistoryDeviceEpoch = input.deviceEpoch;
    m_froxelHistoryNear = froxelNear;
    m_froxelHistoryFar = froxelFar;
    m_froxelHistoryViewProj = input.scene.worldCamera.viewProj;
    m_froxelHistoryCameraPos = cameraPos;

    ctx->track(m_froxelCurrentImage, DxvkAccess::Write);
    ctx->track(m_froxelHistoryImages[historyReadIndex], DxvkAccess::Read);
    ctx->track(m_froxelHistoryImages[historyWriteIndex], DxvkAccess::Write);
    ctx->track(m_effectImage, DxvkAccess::Write);
    ctx->track(m_depthCopy, DxvkAccess::Read);
    ctx->track(shadowMapView->image(), DxvkAccess::Read);
    ctx->track(pointShadowSampleView->image(), DxvkAccess::Read);
    ctx->track(m_csmUniformBuffers[uboSlot], DxvkAccess::Write);
    ctx->track(m_lightBuffers[uboSlot], DxvkAccess::Write);
    ctx->track(m_fogVolumeBuffers[uboSlot], DxvkAccess::Write);
    ctx->track(m_linearSampler);
    ctx->track(pointShadowSampler);

    static uint32_t s_froxelSubmitLogs = 0u;
    if (s_froxelSubmitLogs++ < 16u ||
        (s_froxelSubmitLogs % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK War3Volumetric: froxel submitted backend=%u grid=%ux%ux%u "
          "effect=%ux%u commonNear=%.1f commonFar=%.1f history=%d volumeSun=%d "
          "cascades=%u shadowSteps=%u points=%u pointShadows=%u "
          "fogVolumes=%u\n",
          static_cast<uint32_t>(settings.quality),
          m_froxelGridExtent.width, m_froxelGridExtent.height,
          m_froxelGridExtent.depth, effectExtent.width, effectExtent.height,
          double(froxelNear), double(froxelFar),
          historyReadable ? 1 : 0, csmUbo.volumeSunParams.x > 0.5f ? 1 : 0,
          static_cast<uint32_t>(std::max(csmUbo.params.z, 0.0f)),
          integratePc.maxTraversalSteps, lightUbo.count,
          lightUbo.pointShadowedLightCount, fogVolumeUbo.count);
    }
    return true;
  }

  if (!wantsFroxel)
    invalidateFroxelHistory();

  ctx->cmdBeginRendering(&renderInfo);
  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                       pipeline);
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_layout, descriptors.size(),
                     descriptors.data(), sizeof(pc), &pc);
  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  {
    VkImageMemoryBarrier2 toRead = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toRead.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toRead.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.image = effectImage->handle();
    toRead.subresourceRange = m_effectView->imageSubresources();

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &toRead;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  ctx->track(m_effectImage, DxvkAccess::Write);
  ctx->track(m_colorCopy, DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Read);
  ctx->track(shadowMapView->image(), DxvkAccess::Read);
  ctx->track(pointShadowSampleView->image(), DxvkAccess::Read);
  ctx->track(m_csmUniformBuffers[uboSlot], DxvkAccess::Write);
  ctx->track(m_lightBuffers[uboSlot], DxvkAccess::Write);
  ctx->track(m_fogVolumeBuffers[uboSlot], DxvkAccess::Write);
  ctx->track(m_linearSampler);
  ctx->track(pointShadowSampler);
  static bool s_loggedVolumetricDraw = false;
  if (!s_loggedVolumetricDraw) {
    s_loggedVolumetricDraw = true;
    WAR3_RENDER_LOG(
        "DXVK War3Volumetric: draw active effect=%ux%u csm=%d cascades=%u "
        "points=%u pointShadows=%u fogVolumes=%u samples=%d "
        "intensity=%.2f density=%.2f "
        "extinction=%.2f\n",
        effectExtent.width, effectExtent.height, hasCsmSnapshot ? 1 : 0,
        cascadeCount, lightUbo.count, lightUbo.pointShadowedLightCount,
        fogVolumeUbo.count, effectiveSamples,
        static_cast<double>(pc.params0.x),
        static_cast<double>(pc.params0.z),
        static_cast<double>(pc.viewportZ.z));
  }
  return true;
}

bool War3VolumetricLightPass::compositeVolumetricLight(
    const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input,
    const VkRect2D& compositeScissor) {
  if (!m_compositeLayout || !m_linearSampler || !m_colorCopyView ||
      !m_effectView || !m_depthCopyView || !input.colorView)
    return false;

  auto colorImage = input.colorView->image();
  PipelineKey key = {};
  key.format = colorImage->info().format;
  key.samples = colorImage->info().sampleCount;

  VkPipeline pipeline = getCompositePipeline(key);
  if (pipeline == VK_NULL_HANDLE)
    return false;

  VkExtent3D extent = colorImage->info().extent;
  VkExtent3D effectExtent = m_effectView->image()->info().extent;

  VkRenderingAttachmentInfo attachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = input.colorView->handle();
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = IsFullRect(compositeScissor, extent)
      ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
      : VK_ATTACHMENT_LOAD_OP_LOAD;
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

  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &compositeScissor);

  std::array<DxvkDescriptorWrite, 3> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor = m_colorCopyView->getDescriptor();
  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = m_effectView->getDescriptor();
  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[2].descriptor = m_depthCopyView->getDescriptor();

  VolumetricCompositePushConstants pc = {};
  pc.colorSampler = m_linearSampler->getDescriptor().samplerIndex;
  pc.effectSampler = m_linearSampler->getDescriptor().samplerIndex;
  pc.depthSampler = m_linearSampler->getDescriptor().samplerIndex;
  pc.rtSize = Vector4(float(extent.width), float(extent.height),
                      float(effectExtent.width), float(effectExtent.height));

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                       pipeline);
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_compositeLayout,
                     descriptors.size(), descriptors.data(), sizeof(pc), &pc);
  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  ctx->track(input.colorView->image(), DxvkAccess::Write);
  ctx->track(m_colorCopy, DxvkAccess::Read);
  ctx->track(m_effectImage, DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Read);
  ctx->track(m_linearSampler);
  return true;
}

void War3VolumetricLightPass::Run(const Rc<DxvkCommandList>& ctx,
                                  const War3PipelineInput& input) {
  // 关闭路径必须极早返回：不分配资源、不 copy color/depth、不影响 CSM。
  if (!input.settings)
    return;
  if (!input.settings->postFx.volumetricLight.enabled)
    return;
  if (!input.colorView || !input.depthView)
    return;
  if (!input.scene.worldCamera.valid)
    return;

  const auto& settings = input.settings->postFx.volumetricLight;
  const uint32_t activeBackend = static_cast<uint32_t>(settings.quality);
  static int32_t s_lastLoggedBackend = -1;
  if (s_lastLoggedBackend != static_cast<int32_t>(activeBackend)) {
    s_lastLoggedBackend = static_cast<int32_t>(activeBackend);
    WAR3_RENDER_LOG(
        "DXVK War3Volumetric: active backend=%u (%s)\n",
        activeBackend,
        settings.quality == War3VolumetricQuality::FroxelHigh
            ? "froxel-high"
            : settings.quality == War3VolumetricQuality::FroxelMedium
            ? "froxel-medium"
            : "legacy-raymarch");
  }
  const float safeIntensity = finiteOr(settings.intensity, 0.0f);
  const float safeDensity =
      clampFinite(settings.density, 0.0f, 2.0f, 0.0f);
  const bool hasConfiguredGlobalMedium =
      settings.globalMediumEnabled && safeDensity > 1e-6f;
  const bool localMediumRequested =
      War3FogVolumeManager::Instance().HasActiveVolumes();
  // Global density zero now means clear air outside authored volumes, not an
  // unconditional pass kill. Keep the atomic manager probe before any image
  // allocation or snapshot lock.
  if (safeIntensity <= 1e-6f ||
      (!hasConfiguredGlobalMedium && !localMediumRequested) ||
      settings.sampleCount <= 0)
    return;

  const auto& colorInfo = input.colorView->image()->info();
  const auto& depthInfo = input.depthView->image()->info();
  const VkExtent3D extent = colorInfo.extent;
  if (extent.width == 0u || extent.height == 0u ||
      depthInfo.extent.width == 0u || depthInfo.extent.height == 0u)
    return;
  if (depthInfo.extent.width != extent.width ||
      depthInfo.extent.height != extent.height)
    return;
  // 当前 shader 使用 sampler2DArray，copy 目标也是单采样图像。直接 copy
  // 多采样源在 Vulkan 中非法，因此在实现显式 resolve 前安全跳过。
  if (colorInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT ||
      depthInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT)
    return;

  // 夜间 / 太阳过弱时跳过：体积光几乎不可见却仍会 full-screen ray march。
  const float sunIntensity = clampFinite(
      input.settings->sun.enabled ? input.settings->sun.intensity : 0.0f,
      0.0f, 64.0f, 0.0f);
  const Vector4 frameSunColor = input.lighting != nullptr
      ? input.lighting->sunColor
      : input.settings->sun.color;
  const float sunColorPeak = std::max(
      {clampFinite(frameSunColor.x, 0.0f, 64.0f, 0.0f),
       clampFinite(frameSunColor.y, 0.0f, 64.0f, 0.0f),
       clampFinite(frameSunColor.z, 0.0f, 64.0f, 0.0f)});
  const float minSunIntensity =
      clampFinite(settings.minSunIntensity, 0.0f, 1.0f, 0.08f);
  const bool pointFeatureRequested = settings.includePointLights &&
      settings.maxPointLights > 0u &&
      input.settings->shadows.pointLightsEnabled &&
      War3LightManager::Instance().HasActiveLights();
  const bool hasSunVolume = sunIntensity >= minSunIntensity &&
                            sunIntensity * sunColorPeak > 1e-6f;
  // Keep the no-lock atomic gate before depth algebra. A true value is only a
  // request to inspect the canonical snapshot; it is not proof that any
  // finite, energetic or view-relevant point light exists.
  if (!hasSunVolume && !pointFeatureRequested)
    return;

  // Reuse the same projection-derived depth contract as the Hi-Z contact-ray
  // and shadow-receiver paths. Viewport MinZ/MaxZ may be reversed, so keeping
  // their signed delta is required for a valid world-position reconstruction.
  const float minZ = input.scene.worldCamera.viewport.MinZ;
  const float maxZ = input.scene.worldCamera.viewport.MaxZ;
  const float depthDelta = maxZ - minZ;
  float farClearRaw = 0.0f;
  if (!std::isfinite(minZ) || !std::isfinite(maxZ) ||
      std::abs(depthDelta) <= 1.0e-6f ||
      !war3::render::InferWar3FarClearRaw(input.scene.worldCamera,
                                         farClearRaw))
    return;

  const float farN = (farClearRaw - minZ) / depthDelta;
  const float rawDepthQuantum =
      war3::render::War3RawDepthQuantum(depthInfo.format);
  if (!std::isfinite(farClearRaw) || !std::isfinite(farN) ||
      farN < -1.0e-4f || farN > 1.0001f ||
      !std::isfinite(rawDepthQuantum) || rawDepthQuantum <= 0.0f)
    return;
  const bool farIsOne = farN > 0.5f;

  const Matrix4 invView = inverse(input.scene.worldCamera.view);
  const Vector4 cameraPos(
      invView[3].x, invView[3].y, invView[3].z, 1.0f);
  if (!std::isfinite(cameraPos.x) || !std::isfinite(cameraPos.y) ||
      !std::isfinite(cameraPos.z))
    return;

  War3FogVolumeFrameSnapshot fogVolumes = {};
  if (localMediumRequested) {
    fogVolumes = SelectVolumetricFogVolumes(
        War3FogVolumeManager::Instance().GetFrameSnapshot(input.frameSerial),
        input.scene.worldCamera, extent);
  }
  const bool hasLocalMedium = fogVolumes.hasAny && fogVolumes.count > 0u;
  if (!hasConfiguredGlobalMedium && !hasLocalMedium)
    return;

  War3PointLightFrameSnapshot pointLights = {};
  VolumetricPointSelection pointSelection = {};
  if (pointFeatureRequested) {
    pointLights = War3LightManager::Instance().GetFrameSnapshot(
        input.frameSerial, cameraPos);
    pointSelection = SelectVolumetricPointLights(
        pointLights, cameraPos, input.scene.worldCamera, extent, settings);
  }
  const bool hasPointVolume = pointSelection.count > 0u;
  // This is the exact pre-copy source gate. Zero-energy/invalid lights are
  // filtered by the canonical snapshot, while unreachable or provably
  // offscreen spheres are filtered locally without mutating shared order.
  if (!hasSunVolume && !hasPointVolume)
    return;

  // CSM 只约束太阳散射；缺失时 sun fail-soft，而独立点光体积仍可继续。
  if (settings.requireCsmSnapshot && hasSunVolume) {
    auto* shadowPass =
        m_parent ? m_parent->GetWar3ShadowReceiverPass() : nullptr;
    if (!shadowPass && !hasPointVolume) {
      static bool s_loggedMissingShadowPass = false;
      if (!s_loggedMissingShadowPass) {
        s_loggedMissingShadowPass = true;
        WAR3_RENDER_LOG(
            "DXVK War3Volumetric: requireCsmSnapshot is enabled but the "
            "ShadowReceiver pass is unavailable\n");
      }
      return;
    }
    Rc<DxvkImageView> probeView = nullptr;
    War3CsmData probeCsm = {};
    uint32_t probeRes = 0u;
    Vector4 probeSun = Vector4(0.0f);
    Vector4 probeUp = Vector4(0.0f);
    const bool hasProbeSnapshot =
        shadowPass && shadowPass->GetVolumetricShadowSnapshot(
                          input.frameSerial, probeView, probeCsm, probeRes,
                          probeSun, probeUp);
    if (!hasProbeSnapshot) {
      static uint32_t s_missingCsmLogs = 0u;
      if (s_missingCsmLogs++ < 8u || (s_missingCsmLogs % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3Volumetric: sun is waiting for a complete CSM "
            "snapshot%s\n",
            hasPointVolume ? "; point-light volume remains active" : "");
      }
      if (!hasPointVolume)
        return;
    }
  }

  const bool froxelRequested =
      settings.quality == War3VolumetricQuality::FroxelMedium ||
      settings.quality == War3VolumetricQuality::FroxelHigh;
  constexpr VkFormatFeatureFlags2 requiredFroxelFormatFeatures =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
  const VkFormatFeatureFlags2 froxelFormatFeatures =
      m_device->getFormatFeatures(VK_FORMAT_R16G16B16A16_SFLOAT).optimal;
  bool froxelAdmitted = froxelRequested &&
      (froxelFormatFeatures & requiredFroxelFormatFeatures) ==
          requiredFroxelFormatFeatures;
  if (froxelRequested && !froxelAdmitted) {
    static bool s_loggedUnsupportedFroxelFormat = false;
    if (!std::exchange(s_loggedUnsupportedFroxelFormat, true)) {
      WAR3_RENDER_LOG(
          "DXVK War3Volumetric: RGBA16F sampled/storage/attachment format "
          "unsupported; falling back to legacy raymarch\n");
    }
  }
  uint32_t resolutionDivisor = froxelAdmitted
      ? (settings.quality == War3VolumetricQuality::FroxelHigh
             ? kVolumetricFroxelHighEffectDivisor
             : kVolumetricFroxelMediumEffectDivisor)
      : std::clamp<uint32_t>(settings.resolutionDivisor,
                             kVolumetricMinResolutionDivisor,
                             kVolumetricMaxResolutionDivisor);
  const auto effectExtentForDivisor = [&](uint32_t divisor) {
    return VkExtent3D{
        static_cast<uint32_t>(std::max<uint64_t>(
            1u, (uint64_t(extent.width) + divisor - 1u) / divisor)),
        static_cast<uint32_t>(std::max<uint64_t>(
            1u, (uint64_t(extent.height) + divisor - 1u) / divisor)),
        1u};
  };

  if (froxelAdmitted) {
    const uint64_t maxTraversalSteps =
        settings.quality == War3VolumetricQuality::FroxelHigh
            ? kVolumetricFroxelHighTraversalSteps
            : kVolumetricFroxelMediumTraversalSteps;
    VkExtent3D candidate = effectExtentForDivisor(resolutionDivisor);
    while (resolutionDivisor < kVolumetricMaxResolutionDivisor &&
           uint64_t(candidate.width) * uint64_t(candidate.height) >
               kVolumetricFroxelTraversalWorkBudget / maxTraversalSteps) {
      resolutionDivisor = std::min(
          kVolumetricMaxResolutionDivisor, resolutionDivisor * 2u);
      candidate = effectExtentForDivisor(resolutionDivisor);
    }
  }
  VkExtent3D effectExtent = {};
  const auto minimumRaySegments = [](const VkExtent3D& value) {
    return uint64_t(value.width) * uint64_t(value.height) *
        uint64_t(kVolumetricMinSamples);
  };
  const auto minimumFogSegmentTests = [&](const VkExtent3D& value) {
    return uint64_t(value.width) * uint64_t(value.height) *
        uint64_t(kVolumetricMinSamples) * uint64_t(fogVolumes.count) *
        (1u + uint64_t(pointSelection.count));
  };
  VkRect2D effectScissor = {};
  VkRect2D compositeScissor = FullRect(extent);

  // In clear air, every non-zero fragment is confined to the union of authored
  // local media. Give a distant/small volume twice the linear resolution, but
  // only when its conservative ceil-half ROI fits both watchdog budgets at the
  // *requested* sample count. Large/on-camera volumes retain the established
  // quarter-resolution full-screen path instead of trading away longitudinal
  // samples for spatial resolution.
  bool useLocalHalfResRoi = false;
  if (!froxelAdmitted && !hasConfiguredGlobalMedium && hasLocalMedium) {
    war3::render::War3ImageRegion localRegion = {};
    for (uint32_t i = 0u; i < fogVolumes.count; ++i) {
      localRegion = UnionRegion(
          localRegion,
          war3::render::War3ComputeConservativeHalfResSphereRegion(
              input.scene.worldCamera, fogVolumes.volumes[i].boundingSphere,
              extent));
    }
    if (localRegion.empty())
      return;

    const uint64_t roiPixels =
        uint64_t(localRegion.width) * uint64_t(localRegion.height);
    const uint64_t requestedSamples = uint64_t(std::clamp(
        settings.sampleCount, kVolumetricMinSamples, kVolumetricMaxSamples));
    const uint64_t fogInteractionScale =
        requestedSamples * uint64_t(fogVolumes.count) *
        (1u + uint64_t(pointSelection.count));
    const bool rayBudgetFits = requestedSamples > 0u &&
        roiPixels <= kVolumetricRaySegmentBudget / requestedSamples;
    const bool fogBudgetFits = fogInteractionScale == 0u ||
        roiPixels <= kVolumetricFogSegmentTestBudget / fogInteractionScale;
    if (rayBudgetFits && fogBudgetFits) {
      useLocalHalfResRoi = true;
      resolutionDivisor = kVolumetricLocalRoiResolutionDivisor;
      effectExtent = effectExtentForDivisor(resolutionDivisor);
      effectScissor = HalfResRegionRect(localRegion);
      compositeScissor = EffectRegionToCompositeRect(
          localRegion, effectExtent, extent);
    }
  }

  if (froxelAdmitted) {
    effectExtent = effectExtentForDivisor(resolutionDivisor);
    const uint64_t maxTraversalSteps =
        settings.quality == War3VolumetricQuality::FroxelHigh
            ? kVolumetricFroxelHighTraversalSteps
            : kVolumetricFroxelMediumTraversalSteps;
    const uint64_t traversalWork = uint64_t(effectExtent.width) *
        uint64_t(effectExtent.height) * maxTraversalSteps;
    const uint32_t froxelTile =
        settings.quality == War3VolumetricQuality::FroxelHigh
            ? kVolumetricFroxelHighTileSize
            : kVolumetricFroxelMediumTileSize;
    const uint32_t froxelDepth =
        settings.quality == War3VolumetricQuality::FroxelHigh
            ? kVolumetricFroxelHighDepth
            : kVolumetricFroxelMediumDepth;
    const uint32_t froxelWidth = std::max(
        1u, (extent.width + froxelTile - 1u) / froxelTile);
    const uint32_t froxelHeight = std::max(
        1u, (extent.height + froxelTile - 1u) / froxelTile);
    const uint64_t froxelCells = uint64_t(froxelWidth) *
        uint64_t(froxelHeight) * uint64_t(froxelDepth);
    const uint32_t max3dDimension =
        m_device->properties().core.properties.limits.maxImageDimension3D;
    if (froxelCells == 0u || froxelCells > kVolumetricFroxelCellBudget ||
        traversalWork > kVolumetricFroxelTraversalWorkBudget ||
        froxelWidth > max3dDimension ||
        froxelHeight > max3dDimension ||
        froxelDepth > max3dDimension) {
      static uint32_t s_froxelBudgetFallbackLogs = 0u;
      if (s_froxelBudgetFallbackLogs++ < 8u ||
          (s_froxelBudgetFallbackLogs % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3Volumetric: froxel grid/effect rejected by "
            "cell/traversal/dimension budget; "
            "falling back to legacy raymarch\n");
      }
      resolutionDivisor = std::clamp<uint32_t>(
          settings.resolutionDivisor, kVolumetricMinResolutionDivisor,
          kVolumetricMaxResolutionDivisor);
      effectExtent = effectExtentForDivisor(resolutionDivisor);
      froxelAdmitted = false;
    }
  }
  if (froxelAdmitted) {
    effectScissor = FullRect(effectExtent);
  } else if (!useLocalHalfResRoi) {
    effectExtent = effectExtentForDivisor(resolutionDivisor);
    while ((minimumRaySegments(effectExtent) > kVolumetricRaySegmentBudget ||
            minimumFogSegmentTests(effectExtent) >
                kVolumetricFogSegmentTestBudget) &&
           resolutionDivisor < kVolumetricMaxResolutionDivisor) {
      effectExtent = effectExtentForDivisor(++resolutionDivisor);
    }
    // The fragment shader deliberately keeps a four-sample quality floor. If
    // even divisor=8 cannot honor the watchdog budget, skip this optional pass
    // instead of silently exceeding the advertised hard limit.
    if (minimumRaySegments(effectExtent) > kVolumetricRaySegmentBudget ||
        minimumFogSegmentTests(effectExtent) >
            kVolumetricFogSegmentTestBudget)
      return;
    effectScissor = FullRect(effectExtent);
  }

  ensureResources(extent, colorInfo.format, depthInfo.format,
                  resolutionDivisor, froxelAdmitted);

  copyColor(ctx, input.colorView);
  copyDepth(ctx, input.depthView);
  uint32_t pointShadowedLightCount = 0u;
  const bool effectSubmitted = drawVolumetricLight(
      ctx, input, pointLights, fogVolumes, pointSelection.sourceIndices,
      pointSelection.count, cameraPos, farClearRaw, rawDepthQuantum,
      farIsOne, effectScissor, pointShadowedLightCount);
  if (effectSubmitted &&
      compositeVolumetricLight(ctx, input, compositeScissor)) {
    // Exact execution evidence: this is emitted only after both the low-res
    // scattering draw and the full-res composite were recorded successfully.
    // Keep it periodic so a long DBWIN capture cannot evict the only marker.
    static uint32_t s_volumetricSubmitLogs = 0u;
    const uint32_t submitLog = s_volumetricSubmitLogs++;
    if (submitLog < 16u || (submitLog % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK War3Volumetric: composite submitted frame=%llu points=%u "
          "pointShadows=%u fogVolumes=%u\n",
          static_cast<unsigned long long>(input.frameSerial),
          pointSelection.count, pointShadowedLightCount, fogVolumes.count);
    }
  }
}

} // namespace dxvk
