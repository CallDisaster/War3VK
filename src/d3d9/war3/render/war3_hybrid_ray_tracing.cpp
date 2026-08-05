#include "war3_hybrid_ray_tracing.h"

#include "../../d3d9_war3_debug.h"

#include "../../../dxvk/dxvk_access.h"
#include "../../../dxvk/dxvk_cmdlist.h"
#include "../../../dxvk/dxvk_util.h"
#include "../../../util/util_error.h"
#include "../../../util/util_matrix.h"

#include <war3_hiz_reduce.h>
#include <war3_hiz_seed.h>
#include <war3_point_contact_hiz.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace dxvk::war3::render {
namespace {

constexpr uint32_t kLocalSize = 8u;
constexpr uint32_t kMaxRayLights = 2u;

struct HybridRayUniform {
  Matrix4 view;
  Matrix4 invViewProj;
  Matrix4 proj;
  Vector4 viewport;
  Vector4 viewportZ;
  Vector4 rayParams;
  Vector4 rayParams2;
  // xyz 已由 CPU 转到 view space，w 保留 authored range。第二组仅存
  // 归一化的直射/阴影基础能量，供两灯模式分配固定 traversal 预算。
  std::array<Vector4, kMaxRayLights> lightViewPos;
  std::array<Vector4, kMaxRayLights> lightEnergy;
};

struct HybridRayPush {
  uint32_t srcMip;
  uint32_t dstMip;
  uint32_t mipCount;
  uint32_t layerCount;
  uint32_t fullWidth;
  uint32_t fullHeight;
  uint32_t baseWidth;
  uint32_t baseHeight;
  uint32_t srcWidth;
  uint32_t srcHeight;
  uint32_t dstWidth;
  uint32_t dstHeight;
};

static_assert(sizeof(HybridRayUniform) == 320u,
              "HybridRayUniform must match scalar row-major GLSL");
static_assert(sizeof(HybridRayPush) == 48u,
              "HybridRayPush must match scalar GLSL push data");

std::array<DxvkDescriptorSetLayoutBinding, 3> MakeSeedBindings() {
  return {{
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT},
  }};
}

std::array<DxvkDescriptorSetLayoutBinding, 2> MakeReduceBindings() {
  return {{
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT},
  }};
}

std::array<DxvkDescriptorSetLayoutBinding, 4> MakeContactBindings() {
  return {{
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT},
  }};
}

uint32_t ComputeMipCount(VkExtent3D extent) {
  uint32_t count = 1u;
  uint32_t dimension = std::max(extent.width, extent.height);
  while (dimension > 1u) {
    dimension >>= 1u;
    ++count;
  }
  return count;
}

VkExtent3D MipExtent(VkExtent3D base, uint32_t mip) {
  return {
      std::max(base.width >> mip, 1u),
      std::max(base.height >> mip, 1u),
      1u,
  };
}

DxvkImageViewKey MakeViewKey(VkFormat format, VkImageUsageFlags usage,
                             VkImageViewType type, uint32_t mipIndex,
                             uint32_t mipCount, uint32_t layerCount) {
  DxvkImageViewKey key = {};
  key.viewType = type;
  key.format = format;
  key.usage = usage;
  key.layout = VK_IMAGE_LAYOUT_GENERAL;
  key.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
  key.mipIndex = mipIndex;
  key.mipCount = mipCount;
  key.layerIndex = 0u;
  key.layerCount = layerCount;
  return key;
}

float DepthQuantum(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT:
      return 1.0f / 65535.0f;

    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
      return 1.0f / 16777215.0f;

    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    default:
      return 1.0e-7f;
  }
}

bool FiniteVector(const Vector4& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z) && std::isfinite(value.w);
}

War3ImageRegion FullContactRegion(VkExtent3D baseExtent) {
  return {0u, 0u, baseExtent.width, baseExtent.height};
}

// Project a conservative AABB around the exact view-space sphere consumed by
// the contact shader: both light and receiver use (world * view).xyz, then the
// authored world-unit range is compared directly in that space. A ratio of
// affine functions over a convex box reaches its screen-space extrema at a
// vertex as long as clip W does not cross zero. Crossing the camera plane
// therefore deliberately falls back to the complete target; a box wholly
// behind that plane cannot contain a visible receiver.
War3ImageRegion ComputeContactRegion(const War3WorldCameraState& camera,
                                     const Vector4& lightPosition,
                                     VkExtent3D fullExtent,
                                     VkExtent3D baseExtent) {
  const War3ImageRegion full = FullContactRegion(baseExtent);
  if (!FiniteVector(lightPosition) || !fullExtent.width ||
      !fullExtent.height || !baseExtent.width || !baseExtent.height ||
      !camera.viewport.Width || !camera.viewport.Height)
    return full;

  const float radius = std::max(lightPosition.w, 1.0f);
  const Vector4 centerViewH =
      camera.view * Vector4(lightPosition.x, lightPosition.y,
                            lightPosition.z, 1.0f);
  if (!FiniteVector(centerViewH) || !std::isfinite(radius))
    return full;

  const Vector4 centerView(centerViewH.x, centerViewH.y, centerViewH.z, 1.0f);
  const Vector4 extent(radius, radius, radius, 0.0f);

  const Matrix4 invProj = inverse(camera.proj);
  const Vector4 endpoint0 = invProj * Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  const Vector4 endpoint1 = invProj * Vector4(0.0f, 0.0f, 1.0f, 1.0f);
  const Vector4 insideView = invProj * Vector4(0.0f, 0.0f, 0.5f, 1.0f);
  const Vector4 insideClip = camera.proj * insideView;
  if (!FiniteVector(endpoint0) || !FiniteVector(endpoint1) ||
      !FiniteVector(insideView) || !FiniteVector(insideClip) ||
      std::abs(insideClip.w) <= 1.0e-7f)
    return full;
  const auto endpointDistance = [](const Vector4& endpoint) {
    return std::abs(endpoint.w) <= 1.0e-7f
        ? std::numeric_limits<float>::infinity()
        : std::abs(endpoint.z / endpoint.w);
  };
  const float endpointDistance0 = endpointDistance(endpoint0);
  const float endpointDistance1 = endpointDistance(endpoint1);
  const bool finiteEndpoint0 = std::isfinite(endpointDistance0);
  const bool finiteEndpoint1 = std::isfinite(endpointDistance1);
  if ((!finiteEndpoint0 && !finiteEndpoint1) ||
      (finiteEndpoint0 && finiteEndpoint1 &&
       std::abs(endpointDistance0 - endpointDistance1) <=
          1.0e-5f *
              std::max({endpointDistance0, endpointDistance1, 1.0f})))
    return full;
  const bool nearIsOne = endpointDistance1 < endpointDistance0;
  const float frontWSign = std::copysign(1.0f, insideClip.w);

  constexpr float kMinClipW = 1.0e-5f;
  bool allFront = true;
  bool allBehind = true;
  double minScreenX = std::numeric_limits<double>::infinity();
  double minScreenY = std::numeric_limits<double>::infinity();
  double maxScreenX = -std::numeric_limits<double>::infinity();
  double maxScreenY = -std::numeric_limits<double>::infinity();

  for (uint32_t corner = 0u; corner < 8u; ++corner) {
    const Vector4 viewCorner(
        centerView.x + ((corner & 1u) ? extent.x : -extent.x),
        centerView.y + ((corner & 2u) ? extent.y : -extent.y),
        centerView.z + ((corner & 4u) ? extent.z : -extent.z), 1.0f);
    const Vector4 clip = camera.proj * viewCorner;
    if (!FiniteVector(clip))
      return full;

    const float signedW = clip.w * frontWSign;
    allFront = allFront && signedW > kMinClipW;
    allBehind = allBehind && signedW < -kMinClipW;
    if (signedW <= kMinClipW)
      continue;

    const double invW = 1.0 / double(clip.w);
    const double ndcX = double(clip.x) * invW;
    const double ndcY = double(clip.y) * invW;
    const double ndcZ = double(clip.z) * invW;
    if (!std::isfinite(ndcZ) ||
        (!nearIsOne && ndcZ < -1.0e-5) ||
        (nearIsOne && ndcZ > 1.0 + 1.0e-5))
      return full;
    const double screenX = double(camera.viewport.X) +
        (0.5 * ndcX + 0.5) * double(camera.viewport.Width);
    const double screenY = double(camera.viewport.Y) +
        (0.5 - 0.5 * ndcY) * double(camera.viewport.Height);
    if (!std::isfinite(screenX) || !std::isfinite(screenY))
      return full;
    minScreenX = std::min(minScreenX, screenX);
    minScreenY = std::min(minScreenY, screenY);
    maxScreenX = std::max(maxScreenX, screenX);
    maxScreenY = std::max(maxScreenY, screenY);
  }

  if (allBehind)
    return {};
  if (!allFront)
    return full;

  const double viewportMinX = std::clamp(
      double(camera.viewport.X), 0.0, double(fullExtent.width));
  const double viewportMinY = std::clamp(
      double(camera.viewport.Y), 0.0, double(fullExtent.height));
  const double viewportMaxX = std::clamp(
      double(camera.viewport.X) + double(camera.viewport.Width), 0.0,
      double(fullExtent.width));
  const double viewportMaxY = std::clamp(
      double(camera.viewport.Y) + double(camera.viewport.Height), 0.0,
      double(fullExtent.height));
  if (maxScreenX < viewportMinX || minScreenX > viewportMaxX ||
      maxScreenY < viewportMinY || minScreenY > viewportMaxY)
    return {};

  const double clippedMinX = std::max(minScreenX, viewportMinX);
  const double clippedMinY = std::max(minScreenY, viewportMinY);
  const double clippedMaxX = std::min(maxScreenX, viewportMaxX);
  const double clippedMaxY = std::min(maxScreenY, viewportMaxY);

  // The extra two half-resolution texels absorb integer-pixel centre
  // conventions, floating-point projection error, and the receiver's normal
  // neighbourhood without ever under-covering an in-range receiver.
  constexpr int64_t kGuardTexels = 2;
  int64_t minX = int64_t(std::floor(clippedMinX * 0.5)) - kGuardTexels;
  int64_t minY = int64_t(std::floor(clippedMinY * 0.5)) - kGuardTexels;
  int64_t maxX = int64_t(std::floor(clippedMaxX * 0.5)) + 1 + kGuardTexels;
  int64_t maxY = int64_t(std::floor(clippedMaxY * 0.5)) + 1 + kGuardTexels;
  minX = std::clamp<int64_t>(minX, 0, int64_t(baseExtent.width));
  minY = std::clamp<int64_t>(minY, 0, int64_t(baseExtent.height));
  maxX = std::clamp<int64_t>(maxX, 0, int64_t(baseExtent.width));
  maxY = std::clamp<int64_t>(maxY, 0, int64_t(baseExtent.height));
  if (minX >= maxX || minY >= maxY)
    return {};

  return {uint32_t(minX), uint32_t(minY), uint32_t(maxX - minX),
          uint32_t(maxY - minY)};
}

// If the actual D3D clear value is not tracked, infer the projection's far
// endpoint. MinZ/MaxZ are viewport transforms and may themselves be reversed,
// so this decision must be made in NDC/view space first.
bool InferFarClearRaw(const War3WorldCameraState& camera, float& clearRaw) {
  const Matrix4 invProj = inverse(camera.proj);
  const Vector4 endpoint0 = invProj * Vector4(0.0f, 0.0f, 0.0f, 1.0f);
  const Vector4 endpoint1 = invProj * Vector4(0.0f, 0.0f, 1.0f, 1.0f);
  if (!FiniteVector(endpoint0) || !FiniteVector(endpoint1))
    return false;

  const auto endpointDistance = [](const Vector4& endpoint) {
    if (std::abs(endpoint.w) <= 1.0e-7f)
      return std::numeric_limits<float>::infinity();
    return std::abs(endpoint.z / endpoint.w);
  };

  const float distance0 = endpointDistance(endpoint0);
  const float distance1 = endpointDistance(endpoint1);
  const bool finite0 = std::isfinite(distance0);
  const bool finite1 = std::isfinite(distance1);
  if (!finite0 && !finite1)
    return false;

  if (finite0 && finite1 &&
      std::abs(distance0 - distance1) <=
          1.0e-5f * std::max({distance0, distance1, 1.0f}))
    return false;

  const bool farIsOne = distance1 > distance0;
  clearRaw = farIsOne ? camera.viewport.MaxZ : camera.viewport.MinZ;
  return std::isfinite(clearRaw);
}

void DestroyPipeline(const Rc<DxvkDevice>& device, VkPipeline& pipeline) {
  if (pipeline == VK_NULL_HANDLE)
    return;
  device->vkd()->vkDestroyPipeline(device->vkd()->device(), pipeline,
                                   nullptr);
  pipeline = VK_NULL_HANDLE;
}

} // namespace

bool InferWar3FarClearRaw(const War3WorldCameraState& camera,
                          float& clearRaw) {
  return InferFarClearRaw(camera, clearRaw);
}

float War3RawDepthQuantum(VkFormat format) {
  return DepthQuantum(format);
}

War3ImageRegion War3ComputeConservativeHalfResSphereRegion(
    const War3WorldCameraState& camera, const Vector4& lightPosition,
    VkExtent3D fullExtent) {
  const VkExtent3D baseExtent = {
      std::max(1u, (fullExtent.width + 1u) / 2u),
      std::max(1u, (fullExtent.height + 1u) / 2u), 1u};
  if (!camera.valid)
    return FullContactRegion(baseExtent);
  return ComputeContactRegion(camera, lightPosition, fullExtent, baseExtent);
}

bool War3SphereMayIntersectViewport(const War3WorldCameraState& camera,
                                    const Vector4& lightPosition,
                                    VkExtent3D fullExtent) {
  if (!camera.valid)
    return true;
  // ComputeContactRegion is conservative by construction. Its empty decision
  // is made before half-resolution scaling, so reusing the exact A1 extent
  // contract also gives volume selection the same camera-plane fail-soft rule.
  return !War3ComputeConservativeHalfResSphereRegion(
              camera, lightPosition, fullExtent).empty();
}

War3HybridRayTracing::War3HybridRayTracing(const Rc<DxvkDevice>& device)
    : m_device(device) {
  const auto seedBindings = MakeSeedBindings();
  const auto reduceBindings = MakeReduceBindings();
  const auto contactBindings = MakeContactBindings();

  m_seedLayout = m_device->createBuiltInPipelineLayout(
      0u, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(HybridRayPush),
      seedBindings.size(), seedBindings.data());
  m_reduceLayout = m_device->createBuiltInPipelineLayout(
      0u, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(HybridRayPush),
      reduceBindings.size(), reduceBindings.data());
  m_contactLayout = m_device->createBuiltInPipelineLayout(
      0u, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(HybridRayPush),
      contactBindings.size(), contactBindings.data());

  try {
    m_seedPipeline = m_device->createBuiltInComputePipeline(
        m_seedLayout,
        util::DxvkBuiltInShaderStage(war3_hiz_seed, nullptr));
    m_reducePipeline = m_device->createBuiltInComputePipeline(
        m_reduceLayout,
        util::DxvkBuiltInShaderStage(war3_hiz_reduce, nullptr));
    m_contactPipeline = m_device->createBuiltInComputePipeline(
        m_contactLayout,
        util::DxvkBuiltInShaderStage(war3_point_contact_hiz, nullptr));

    DxvkBufferCreateInfo info = {};
    info.size = sizeof(HybridRayUniform);
    info.usage =
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    info.debugName = "War3HybridRayUniform";
    m_uniformBuffer =
        m_device->createBuffer(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (m_seedPipeline == VK_NULL_HANDLE ||
        m_reducePipeline == VK_NULL_HANDLE ||
        m_contactPipeline == VK_NULL_HANDLE || !m_uniformBuffer) {
      throw DxvkError(std::string(
          "War3HybridRay: compute pipeline or uniform allocation returned "
          "null"));
    }
  } catch (...) {
    DestroyPipeline(m_device, m_contactPipeline);
    DestroyPipeline(m_device, m_reducePipeline);
    DestroyPipeline(m_device, m_seedPipeline);
    throw;
  }
}

War3HybridRayTracing::~War3HybridRayTracing() {
  DestroyPipeline(m_device, m_contactPipeline);
  DestroyPipeline(m_device, m_reducePipeline);
  DestroyPipeline(m_device, m_seedPipeline);
}

bool War3HybridRayTracing::ensureResources(VkExtent3D fullExtent,
                                           uint32_t lightLayerCount) {
  lightLayerCount = std::clamp(lightLayerCount, 1u, kMaxRayLights);
  const VkExtent3D baseExtent = {
      std::max((fullExtent.width + 1u) / 2u, 1u),
      std::max((fullExtent.height + 1u) / 2u, 1u),
      1u,
  };
  const uint32_t mipCount = ComputeMipCount(baseExtent);

  if (m_hizImage && m_visibilityImage &&
      m_fullExtent.width == fullExtent.width &&
      m_fullExtent.height == fullExtent.height &&
      m_visibilityLayerCapacity >= lightLayerCount &&
      m_mipCount == mipCount)
    return true;

  // 灯数在 1/2 之间波动时只允许容量增长，不能每帧来回重建 image/view。
  // 冷启动单灯仍只分配一层；真正出现第二盏 eligible 灯后才扩为两层。
  const uint32_t allocationLayerCount = std::max(
      lightLayerCount, m_visibilityLayerCapacity);

  constexpr VkFormatFeatureFlags2 requiredHiz =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
  constexpr VkFormatFeatureFlags2 requiredVisibility =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
  const VkFormatFeatureFlags2 hizFeatures =
      m_device->getFormatFeatures(VK_FORMAT_R32G32_SFLOAT).optimal;
  const VkFormatFeatureFlags2 visibilityFeatures =
      m_device->getFormatFeatures(VK_FORMAT_R16G16_SFLOAT).optimal;
  if ((hizFeatures & requiredHiz) != requiredHiz ||
      (visibilityFeatures & requiredVisibility) != requiredVisibility) {
    static bool s_loggedUnsupportedFormats = false;
    if (!std::exchange(s_loggedUnsupportedFormats, true)) {
      WAR3_RENDER_LOG(
          "DXVK War3HybridRay: sampled/storage image formats unsupported; "
          "falling back to A0 contact rays\n");
    }
    return false;
  }

  // Build a complete replacement set before publishing any of it. This keeps
  // the previous generation intact if allocation or view creation throws.
  DxvkImageCreateInfo hizInfo = {};
  hizInfo.type = VK_IMAGE_TYPE_2D;
  hizInfo.format = VK_FORMAT_R32G32_SFLOAT;
  hizInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  hizInfo.extent = baseExtent;
  hizInfo.numLayers = 1u;
  hizInfo.mipLevels = mipCount;
  hizInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  hizInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  hizInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  hizInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  hizInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
  hizInfo.debugName = "War3HybridRayHiZ";
  Rc<DxvkImage> newHiz =
      m_device->createImage(hizInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  Rc<DxvkImageView> newHizSample = newHiz->createView(MakeViewKey(
      hizInfo.format, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_VIEW_TYPE_2D, 0u,
      mipCount, 1u));
  std::vector<Rc<DxvkImageView>> newHizStorage;
  newHizStorage.reserve(mipCount);
  for (uint32_t mip = 0u; mip < mipCount; ++mip) {
    newHizStorage.push_back(newHiz->createView(MakeViewKey(
        hizInfo.format, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_VIEW_TYPE_2D,
        mip, 1u, 1u)));
  }

  DxvkImageCreateInfo visibilityInfo = {};
  visibilityInfo.type = VK_IMAGE_TYPE_2D;
  visibilityInfo.format = VK_FORMAT_R16G16_SFLOAT;
  visibilityInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
  visibilityInfo.extent = baseExtent;
  visibilityInfo.numLayers = allocationLayerCount;
  visibilityInfo.mipLevels = 1u;
  visibilityInfo.usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  visibilityInfo.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_TRANSFER_BIT;
  visibilityInfo.access =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
      VK_ACCESS_TRANSFER_WRITE_BIT;
  visibilityInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  visibilityInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
  visibilityInfo.debugName = "War3HybridRayVisibility";
  Rc<DxvkImage> newVisibility = m_device->createImage(
      visibilityInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  Rc<DxvkImageView> newVisibilitySample = newVisibility->createView(
      MakeViewKey(visibilityInfo.format, VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 1u,
                  allocationLayerCount));
  Rc<DxvkImageView> newVisibilityStorage = newVisibility->createView(
      MakeViewKey(visibilityInfo.format, VK_IMAGE_USAGE_STORAGE_BIT,
                  VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0u, 1u,
                  allocationLayerCount));

  // Views do not own a separate image lifetime. Drop/replace every old view
  // while the old image member is still alive, then publish the new image.
  m_hizSampleView = std::move(newHizSample);
  m_hizStorageViews = std::move(newHizStorage);
  m_hizImage = std::move(newHiz);
  m_visibilitySampleView = std::move(newVisibilitySample);
  m_visibilityStorageView = std::move(newVisibilityStorage);
  m_visibilityImage = std::move(newVisibility);
  m_fullExtent = fullExtent;
  m_baseExtent = baseExtent;
  m_mipCount = mipCount;
  m_visibilityLayerCapacity = allocationLayerCount;
  ++m_resourceGeneration;
  m_layoutInitialized = false;

  uint64_t hizTexels = 0u;
  for (uint32_t mip = 0u; mip < mipCount; ++mip) {
    const VkExtent3D mipExtent = MipExtent(baseExtent, mip);
    hizTexels += uint64_t(mipExtent.width) * uint64_t(mipExtent.height);
  }
  const uint64_t totalBytes =
      hizTexels * 8u + uint64_t(baseExtent.width) *
      uint64_t(baseExtent.height) * uint64_t(allocationLayerCount) * 4u;
  WAR3_RENDER_LOG(
      "DXVK War3HybridRay: resources full=%ux%u half=%ux%u mips=%u "
      "layers=%u generation=%llu bytes=%llu\n",
      fullExtent.width, fullExtent.height, baseExtent.width,
      baseExtent.height, mipCount, allocationLayerCount,
      static_cast<unsigned long long>(m_resourceGeneration),
      static_cast<unsigned long long>(totalBytes));
  return true;
}

void War3HybridRayTracing::initializeOrAcquireResources(
    const Rc<DxvkCommandList>& ctx) {
  std::array<VkImageMemoryBarrier2, 2> barriers = {};
  for (auto& barrier : barriers) {
    barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout = m_layoutInitialized ? VK_IMAGE_LAYOUT_GENERAL
                                            : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcStageMask = m_layoutInitialized
        ? (VK_PIPELINE_STAGE_2_TRANSFER_BIT |
           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
        : VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = m_layoutInitialized
        ? (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT |
           VK_ACCESS_2_SHADER_WRITE_BIT)
        : 0u;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0u;
    barrier.subresourceRange.baseArrayLayer = 0u;
  }

  barriers[0].image = m_hizImage->handle();
  barriers[0].subresourceRange.levelCount = m_mipCount;
  barriers[0].subresourceRange.layerCount = 1u;
  barriers[1].image = m_visibilityImage->handle();
  barriers[1].subresourceRange.levelCount = 1u;
  barriers[1].subresourceRange.layerCount = m_visibilityLayerCapacity;
  barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;

  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = barriers.size();
  dependency.pImageMemoryBarriers = barriers.data();
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  m_layoutInitialized = true;
}

void War3HybridRayTracing::updateUniforms(
    const Rc<DxvkCommandList>& ctx, const War3WorldCameraState& camera,
    const War3ShadowSettings& settings,
    const War3PointLightFrameSnapshot& lightSnapshot,
    uint32_t lightLayerCount) {
  HybridRayUniform data = {};
  data.view = camera.view;
  data.invViewProj = camera.invViewProj;
  data.proj = camera.proj;
  data.viewport = Vector4(
      float(camera.viewport.X), float(camera.viewport.Y),
      float(camera.viewport.Width), float(camera.viewport.Height));

  float clearRaw = 0.0f;
  const bool clearKnown = InferWar3FarClearRaw(camera, clearRaw);
  data.viewportZ = Vector4(camera.viewport.MinZ, camera.viewport.MaxZ,
                           clearRaw, clearKnown ? 1.0f : 0.0f);
  data.rayParams = Vector4(
      std::clamp(settings.pointRayShadowMaxDistance, 32.0f, 2400.0f),
      std::clamp(settings.pointRayShadowThickness, 1.0f, 160.0f),
      std::clamp(settings.pointRayShadowStartOffset, 1.0f, 96.0f),
      float(std::clamp<uint32_t>(settings.pointRayShadowHiZMaxVisits,
                                 8u, 64u)));
  data.rayParams2 =
      Vector4(War3RawDepthQuantum(m_depthFormat), 0.0f, 0.0f, 0.0f);

  // canonical shadow prefix 已过滤无效/黑色/零强度灯。这里仍使用 double
  // 计算基础能量并归一化，避免极端脚本参数在 shader 内溢出。归一化只参与
  // 多灯预算份额，不改变 receiver 的实际光照或 authored shadow strength。
  std::array<double, kMaxRayLights> lightEnergy = {};
  double maxLightEnergy = 0.0;
  for (uint32_t i = 0u; i < lightLayerCount; ++i) {
    const War3PointLight& source = lightSnapshot.lights[i];
    const Vector4 viewPosition = camera.view * Vector4(
        source.position.x, source.position.y, source.position.z, 1.0f);
    data.lightViewPos[i] = Vector4(
        viewPosition.x, viewPosition.y, viewPosition.z,
        std::max(source.position.w, 1.0f));

    const double colorPeak = std::max(
        {double(source.color.x), double(source.color.y),
         double(source.color.z), 0.0});
    const double intensity = std::max(double(source.color.w), 0.0);
    const double shadowStrength = std::clamp(
        double(source.params.x), 0.0, 1.0);
    const double energy = colorPeak * intensity * shadowStrength;
    lightEnergy[i] = std::isfinite(energy) && energy > 0.0 ? energy : 0.0;
    maxLightEnergy = std::max(maxLightEnergy, lightEnergy[i]);
  }
  if (maxLightEnergy > 0.0) {
    for (uint32_t i = 0u; i < lightLayerCount; ++i) {
      const double normalizedEnergy = std::clamp(
          lightEnergy[i] / maxLightEnergy, 0.0, 1.0);
      // 极端脚本值也不能让一个合法弱灯在 float 转换时下溢成“无灯”。
      // 当强灯不覆盖当前 receiver 时，弱灯仍应独占完整预算。
      data.lightEnergy[i].x = lightEnergy[i] > 0.0
          ? float(std::max(normalizedEnergy, 1.0e-20))
          : 0.0f;
    }
  }

  const DxvkResourceBufferInfo buffer =
      m_uniformBuffer->getSliceInfo(0u, sizeof(data));
  VkBufferMemoryBarrier2 barrier = {
      VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer.buffer;
  barrier.offset = buffer.offset;
  barrier.size = sizeof(data);
  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.bufferMemoryBarrierCount = 1u;
  dependency.pBufferMemoryBarriers = &barrier;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, buffer.buffer, buffer.offset,
                       sizeof(data), &data);
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
}

War3HybridRayResult War3HybridRayTracing::Run(
    const Rc<DxvkCommandList>& ctx, const Rc<DxvkImageView>& depthView,
    VkExtent3D fullExtent, const War3WorldCameraState& camera,
    const War3ShadowSettings& settings,
    const War3PointLightFrameSnapshot& lightSnapshot,
    uint32_t eligibleLightCount, uint64_t frameSerial) {
  War3HybridRayResult result = {};
  const uint32_t lightLayerCount = std::min<uint32_t>(
      {eligibleLightCount, lightSnapshot.shadowCount, kMaxRayLights});
  if (!ctx || !depthView || !camera.valid || !lightLayerCount ||
      !fullExtent.width || !fullExtent.height ||
      depthView->image()->info().sampleCount != VK_SAMPLE_COUNT_1_BIT ||
      m_seedPipeline == VK_NULL_HANDLE ||
      m_reducePipeline == VK_NULL_HANDLE ||
      m_contactPipeline == VK_NULL_HANDLE || !m_uniformBuffer)
    return result;

  float inferredClearRaw = 0.0f;
  if (!InferWar3FarClearRaw(camera, inferredClearRaw))
    return result;
  (void)inferredClearRaw;

  // Compute the conservative light ROIs before allocating, clearing, or
  // building Hi-Z. If every eligible light sphere is outside the viewport,
  // publishing no A1 result is the correct fail-soft outcome: the receiver
  // keeps its cube-shadow/A0 visibility and no cached A1 view is published.
  std::array<War3ImageRegion, kMaxRayLights> contactRegions = {};
  uint32_t nonEmptyRegionCount = 0u;
  for (uint32_t lightIndex = 0u; lightIndex < lightLayerCount;
       ++lightIndex) {
    contactRegions[lightIndex] =
        War3ComputeConservativeHalfResSphereRegion(
            camera, lightSnapshot.lights[lightIndex].position, fullExtent);
    nonEmptyRegionCount += !contactRegions[lightIndex].empty();
  }
  if (!nonEmptyRegionCount)
    return result;

  try {
    if (!ensureResources(fullExtent, lightLayerCount))
      return result;
  } catch (const DxvkError& e) {
    static uint32_t s_resourceFailureLogs = 0u;
    if (s_resourceFailureLogs++ < 8u ||
        (s_resourceFailureLogs % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK War3HybridRay: resource creation failed; falling back to "
          "A0 (%s)\n",
          e.message().c_str());
    }
    return result;
  } catch (...) {
    static bool s_loggedUnknownResourceFailure = false;
    if (!std::exchange(s_loggedUnknownResourceFailure, true)) {
      WAR3_RENDER_LOG(
          "DXVK War3HybridRay: unexpected resource creation failure; "
          "falling back to A0\n");
    }
    return result;
  }

  m_depthFormat = depthView->image()->info().format;

  // Retain every resource before the first command is recorded. The caller
  // deliberately catches A1 failures and keeps the feature fail-soft; early
  // tracking also keeps a partially recorded command list lifetime-safe if a
  // later bind or dispatch operation throws.
  ctx->track(depthView->image(), DxvkAccess::Read);
  ctx->track(m_hizImage, DxvkAccess::Write);
  ctx->track(m_visibilityImage, DxvkAccess::Write);
  ctx->track(m_uniformBuffer, DxvkAccess::Write);

  initializeOrAcquireResources(ctx);
  updateUniforms(ctx, camera, settings, lightSnapshot, lightLayerCount);

  // ROI dispatches do not touch pixels outside a light's projected sphere.
  // Clear every layer to the fail-soft contract first: visibility=1 and
  // confidence=0 leaves the receiver's cube-shadow/direct-light result intact.
  VkClearColorValue visibilityClear = {};
  visibilityClear.float32[0] = 1.0f;
  const VkImageSubresourceRange visibilityRange = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, lightLayerCount};
  ctx->cmdClearColorImage(DxvkCmdBuffer::ExecBuffer,
                          m_visibilityImage->handle(),
                          VK_IMAGE_LAYOUT_GENERAL, &visibilityClear, 1u,
                          &visibilityRange);

  HybridRayPush push = {};
  push.mipCount = m_mipCount;
  push.layerCount = lightLayerCount;
  push.fullWidth = m_fullExtent.width;
  push.fullHeight = m_fullExtent.height;
  push.baseWidth = m_baseExtent.width;
  push.baseHeight = m_baseExtent.height;
  push.srcWidth = m_fullExtent.width;
  push.srcHeight = m_fullExtent.height;
  push.dstWidth = m_baseExtent.width;
  push.dstHeight = m_baseExtent.height;

  const DxvkResourceBufferInfo uniform =
      m_uniformBuffer->getSliceInfo(0u, sizeof(HybridRayUniform));
  std::array<DxvkDescriptorWrite, 3> seedDescriptors = {};
  seedDescriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  seedDescriptors[0].descriptor = depthView->getDescriptor();
  seedDescriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  seedDescriptors[1].descriptor = m_hizStorageViews[0]->getDescriptor();
  seedDescriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  seedDescriptors[2].buffer = uniform;
  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                       VK_PIPELINE_BIND_POINT_COMPUTE, m_seedPipeline);
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_seedLayout,
                     seedDescriptors.size(), seedDescriptors.data(),
                     sizeof(push), &push);
  ctx->cmdDispatch(DxvkCmdBuffer::ExecBuffer,
                   (m_baseExtent.width + kLocalSize - 1u) / kLocalSize,
                   (m_baseExtent.height + kLocalSize - 1u) / kLocalSize, 1u);

  const auto makeMipReadable = [&](uint32_t mip,
                                   VkPipelineStageFlags2 dstStages) {
    VkImageMemoryBarrier2 barrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = dstStages;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image = m_hizImage->handle();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1u, 0u, 1u};
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1u;
    dependency.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &dependency);
  };
  makeMipReadable(0u, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

  for (uint32_t dstMip = 1u; dstMip < m_mipCount; ++dstMip) {
    const uint32_t srcMip = dstMip - 1u;
    const VkExtent3D srcExtent = MipExtent(m_baseExtent, srcMip);
    const VkExtent3D dstExtent = MipExtent(m_baseExtent, dstMip);
    push.srcMip = srcMip;
    push.dstMip = dstMip;
    push.srcWidth = srcExtent.width;
    push.srcHeight = srcExtent.height;
    push.dstWidth = dstExtent.width;
    push.dstHeight = dstExtent.height;

    std::array<DxvkDescriptorWrite, 2> reduceDescriptors = {};
    reduceDescriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    reduceDescriptors[0].descriptor =
        m_hizStorageViews[srcMip]->getDescriptor();
    reduceDescriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    reduceDescriptors[1].descriptor =
        m_hizStorageViews[dstMip]->getDescriptor();
    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                         VK_PIPELINE_BIND_POINT_COMPUTE, m_reducePipeline);
    ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_reduceLayout,
                       reduceDescriptors.size(), reduceDescriptors.data(),
                       sizeof(push), &push);
    ctx->cmdDispatch(DxvkCmdBuffer::ExecBuffer,
                     (dstExtent.width + kLocalSize - 1u) / kLocalSize,
                     (dstExtent.height + kLocalSize - 1u) / kLocalSize, 1u);
    makeMipReadable(dstMip, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
  }

  VkImageMemoryBarrier2 visibilityClearReady = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  visibilityClearReady.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  visibilityClearReady.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  visibilityClearReady.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  visibilityClearReady.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  visibilityClearReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  visibilityClearReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  visibilityClearReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  visibilityClearReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  visibilityClearReady.image = m_visibilityImage->handle();
  visibilityClearReady.subresourceRange = visibilityRange;
  VkDependencyInfo visibilityClearDependency = {
      VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  visibilityClearDependency.imageMemoryBarrierCount = 1u;
  visibilityClearDependency.pImageMemoryBarriers = &visibilityClearReady;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer,
                          &visibilityClearDependency);

  push.srcMip = 0u;
  push.dstMip = 0u;
  push.srcWidth = m_baseExtent.width;
  push.srcHeight = m_baseExtent.height;
  push.dstWidth = m_baseExtent.width;
  push.dstHeight = m_baseExtent.height;
  std::array<DxvkDescriptorWrite, 4> contactDescriptors = {};
  contactDescriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  contactDescriptors[0].descriptor = depthView->getDescriptor();
  contactDescriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  contactDescriptors[1].descriptor = m_hizSampleView->getDescriptor();
  contactDescriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  contactDescriptors[2].descriptor =
      m_visibilityStorageView->getDescriptor();
  contactDescriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  contactDescriptors[3].buffer = uniform;
  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                       VK_PIPELINE_BIND_POINT_COMPUTE, m_contactPipeline);
  uint64_t scheduledTexels = 0u;
  uint32_t emptyRegions = 0u;
  for (uint32_t lightIndex = 0u; lightIndex < lightLayerCount;
       ++lightIndex) {
    const War3ImageRegion& region = contactRegions[lightIndex];
    if (region.empty()) {
      ++emptyRegions;
      continue;
    }

    // The contact shader does not consume mip indices. Reuse them for the
    // layer and pack x/y/width/height into the existing dispatch quartet so
    // this optimization keeps the original 48-byte push ABI.
    push.srcMip = lightIndex;
    push.dstMip = 0u;
    push.srcWidth = region.x;
    push.srcHeight = region.y;
    push.dstWidth = region.width;
    push.dstHeight = region.height;
    ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_contactLayout,
                       contactDescriptors.size(), contactDescriptors.data(),
                       sizeof(push), &push);
    ctx->cmdDispatch(DxvkCmdBuffer::ExecBuffer,
                     (region.width + kLocalSize - 1u) / kLocalSize,
                     (region.height + kLocalSize - 1u) / kLocalSize, 1u);
    scheduledTexels += uint64_t(region.width) * uint64_t(region.height);
  }

  static uint32_t s_roiLogCount = 0u;
  if (s_roiLogCount < 8u || (frameSerial % 600u) == 0u) {
    ++s_roiLogCount;
    const uint64_t fullTexels = uint64_t(m_baseExtent.width) *
        uint64_t(m_baseExtent.height) * uint64_t(lightLayerCount);
    WAR3_RENDER_LOG(
        "DXVK War3HybridRay: contact ROI scheduled=%llu full=%llu "
        "emptyLayers=%u layers=%u\n",
        static_cast<unsigned long long>(scheduledTexels),
        static_cast<unsigned long long>(fullTexels), emptyRegions,
        lightLayerCount);
  }

  VkImageMemoryBarrier2 visibilityReadable = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  visibilityReadable.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  visibilityReadable.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                     VK_ACCESS_2_SHADER_WRITE_BIT;
  visibilityReadable.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  visibilityReadable.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  visibilityReadable.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  visibilityReadable.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  visibilityReadable.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  visibilityReadable.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  visibilityReadable.image = m_visibilityImage->handle();
  visibilityReadable.subresourceRange = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, lightLayerCount};
  VkDependencyInfo visibilityDependency = {
      VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  visibilityDependency.imageMemoryBarrierCount = 1u;
  visibilityDependency.pImageMemoryBarriers = &visibilityReadable;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &visibilityDependency);

  result.visibilityView = m_visibilitySampleView;
  result.hizView = m_hizSampleView;
  result.lightLayerCount = lightLayerCount;
  result.hizMipCount = m_mipCount;
  result.producedFrameSerial = frameSerial;
  result.resourceGeneration = m_resourceGeneration;
  result.lightGeneration = lightSnapshot.generation;
  return result;
}

} // namespace dxvk::war3::render
