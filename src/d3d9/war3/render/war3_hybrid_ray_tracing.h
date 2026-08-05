#pragma once

#include "../../d3d9_war3_light.h"
#include "../../d3d9_war3_scene.h"
#include "../../d3d9_war3_settings.h"
#include "../../../dxvk/dxvk_device.h"

#include <cstdint>
#include <vector>

namespace dxvk {
class DxvkCommandList;
}

namespace dxvk::war3::render {

// Shared depth interpretation used by both the A1 compute path and the legacy
// receiver/A0 fallback. The projection determines which NDC endpoint is far;
// viewport MinZ/MaxZ only map that endpoint into raw depth.
bool InferWar3FarClearRaw(const War3WorldCameraState& camera,
                          float& clearRaw);
float War3RawDepthQuantum(VkFormat format);

struct War3ImageRegion {
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;

  bool empty() const {
    return width == 0u || height == 0u;
  }
};

// Conservative projected sphere region in the exact ceil-half-resolution
// raster contract used by A1. Invalid or camera-plane-crossing inputs return
// the full half-resolution target; only proven offscreen spheres return empty.
War3ImageRegion War3ComputeConservativeHalfResSphereRegion(
    const War3WorldCameraState& camera,
    const Vector4& lightPosition,
    VkExtent3D fullExtent);

// Conservative sphere/viewport visibility shared by contact-shadow ROI and
// other bounded point-light consumers. Returns false only when the sphere is
// provably wholly behind the camera or wholly outside the active viewport;
// invalid or camera-plane-crossing inputs remain visible (fail-soft).
bool War3SphereMayIntersectViewport(const War3WorldCameraState& camera,
                                    const Vector4& lightPosition,
                                    VkExtent3D fullExtent);

struct War3HybridRayResult {
  Rc<DxvkImageView> visibilityView;
  Rc<DxvkImageView> hizView;
  uint32_t lightLayerCount = 0u;
  uint32_t hizMipCount = 0u;
  uint64_t producedFrameSerial = 0u;
  uint64_t resourceGeneration = 0u;
  uint64_t lightGeneration = 0u;

  bool valid() const {
    return visibilityView != nullptr && hizView != nullptr &&
           lightLayerCount != 0u && hizMipCount != 0u;
  }
};

/**
 * Half-resolution hierarchical software ray tracing for point-light contact
 * shadows. The object is created lazily only after the opt-in feature becomes
 * active; the default-off path therefore creates no images or compute work.
 */
class War3HybridRayTracing final {
public:
  explicit War3HybridRayTracing(const Rc<DxvkDevice>& device);
  ~War3HybridRayTracing();

  War3HybridRayTracing(const War3HybridRayTracing&) = delete;
  War3HybridRayTracing& operator=(const War3HybridRayTracing&) = delete;

  War3HybridRayResult Run(
      const Rc<DxvkCommandList>& ctx,
      const Rc<DxvkImageView>& depthView,
      VkExtent3D fullExtent,
      const War3WorldCameraState& camera,
      const War3ShadowSettings& settings,
      const War3PointLightFrameSnapshot& lightSnapshot,
      uint32_t eligibleLightCount,
      uint64_t frameSerial);

private:
  bool ensureResources(VkExtent3D fullExtent, uint32_t lightLayerCount);
  void initializeOrAcquireResources(const Rc<DxvkCommandList>& ctx);
  void updateUniforms(const Rc<DxvkCommandList>& ctx,
                      const War3WorldCameraState& camera,
                      const War3ShadowSettings& settings,
                      const War3PointLightFrameSnapshot& lightSnapshot,
                      uint32_t lightLayerCount);

  Rc<DxvkDevice> m_device;
  const DxvkPipelineLayout* m_seedLayout = nullptr;
  const DxvkPipelineLayout* m_reduceLayout = nullptr;
  const DxvkPipelineLayout* m_contactLayout = nullptr;
  VkPipeline m_seedPipeline = VK_NULL_HANDLE;
  VkPipeline m_reducePipeline = VK_NULL_HANDLE;
  VkPipeline m_contactPipeline = VK_NULL_HANDLE;
  Rc<DxvkBuffer> m_uniformBuffer;

  Rc<DxvkImage> m_hizImage;
  Rc<DxvkImageView> m_hizSampleView;
  std::vector<Rc<DxvkImageView>> m_hizStorageViews;

  Rc<DxvkImage> m_visibilityImage;
  Rc<DxvkImageView> m_visibilitySampleView;
  Rc<DxvkImageView> m_visibilityStorageView;

  VkExtent3D m_fullExtent = {0u, 0u, 1u};
  VkExtent3D m_baseExtent = {0u, 0u, 1u};
  VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
  uint32_t m_mipCount = 0u;
  // visibility image/view 的已分配层容量；本帧 active 层数由 Run 局部值
  // 和 War3HybridRayResult::lightLayerCount 精确发布，容量只增不减。
  uint32_t m_visibilityLayerCapacity = 0u;
  uint64_t m_resourceGeneration = 0u;
  bool m_layoutInitialized = false;
};

} // namespace dxvk::war3::render
