#pragma once

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "war3_gpu_skin_resources.h"

namespace dxvk::war3::gpu_skin {

// Owns only immutable, generation-qualified model packages and their one-shot
// upload retirement.  The live renderer does not construct this object
// independently yet: War3GpuSkinResources remains its sole P0 owner and keeps
// the existing manager-disabled construction boundary unchanged.
class War3PersistentGpuPackageStore final {
public:
  static constexpr bool kD3D9SharedOwnerEnabled = false;
  static constexpr bool kRequiresNativeBridge = false;
  // The inherited P0 reset path still clears the atlas and producer-retirement
  // records together. The legacy manager is safe because it retains the old
  // War3GpuSkinResources owner while work is in flight, but a future shared
  // D3D9 owner must first add explicit upload/use-fence generation retirement.
  static constexpr bool kCrossEpochRetirementSafe = false;

  War3PersistentGpuPackageStore(
      Rc<DxvkDevice> device,
      const GpuSkinResourceBudgets& budgets,
      VkDeviceSize storageBufferOffsetAlignment,
      GpuSkinDiagnostics& diagnostics);
  ~War3PersistentGpuPackageStore();

  War3PersistentGpuPackageStore(
      const War3PersistentGpuPackageStore&) = delete;
  War3PersistentGpuPackageStore& operator=(
      const War3PersistentGpuPackageStore&) = delete;

  bool beginFrame(uint64_t mapEpoch, uint64_t deviceEpoch,
                  uint64_t frameTag);
  void invalidateMapEpoch(uint64_t nextMapEpoch);
  void invalidateDevice(Rc<DxvkDevice> device, uint64_t nextDeviceEpoch,
                        VkDeviceSize storageBufferOffsetAlignment);
  void reset();

  GpuSkinStaticLookup findOrQueueStatic(
      model::ShadowGeosetResourceSnapshot record,
      uint32_t layoutGeneration);
  GpuSkinStaticLookup probeStatic(
      const model::ShadowGeosetResourceStamp& stamp,
      uint32_t layoutGeneration);
  uint32_t prepareQueuedStaticResources(uint32_t maxRecords);
  std::vector<GpuSkinStaticUpload> takeStaticUploads();
  bool retireStaticUpload(const GpuSkinStaticUpload& upload,
                          Rc<DxvkFence> fence, uint64_t value);
  DxvkBufferSlice staticAtlasSlice() const;

  bool hasRetiredUploads() const;
  // Keep the caller's stack-bound fence query as a zero-allocation template.
  // The call is synchronous, so no callback or reference escapes this method.
  template <typename FenceValueQuery>
  void pollRetired(const FenceValueQuery& getFenceValue) {
    for (auto it = m_retiredStaticUploads.begin();
         it != m_retiredStaticUploads.end();) {
      if (it->fence != nullptr &&
          getFenceValue(it->fence) >= it->retireValue) {
        ++m_diagnostics.staticUploadRetirementsReclaimed;
        it = m_retiredStaticUploads.erase(it);
      } else {
        ++it;
      }
    }
  }
  void fillResidencySnapshot(
      resource_census::GpuSkinPoolResidencySnapshot& snapshot) const;

private:
  struct QueuedStaticMiss {
    GpuSkinStaticResourceKey key;
    model::ShadowGeosetResourceSnapshot record;
  };

  struct RetiredStaticUpload {
    GpuSkinStaticResourceKey key;
    DxvkBufferSlice source;
    Rc<DxvkFence> fence;
    uint64_t retireValue = 0;
    resource_census::ResourceHandle residencyCensus;
  };

  GpuSkinStaticResourceKey makeKey(
      const model::ShadowGeosetResourceRecord& record,
      uint32_t layoutGeneration) const;
  std::shared_ptr<GpuSkinStaticResource> createStaticResource(
      const QueuedStaticMiss& miss);
  void recordFallback(GpuSkinFallbackReason reason);
  void clearEpochResources();

  Rc<DxvkDevice> m_device;
  GpuSkinResourceBudgets m_budgets;
  VkDeviceSize m_storageBufferOffsetAlignment = 16u;
  GpuSkinDiagnostics& m_diagnostics;
  uint64_t m_mapEpoch = 0u;
  uint64_t m_deviceEpoch = 0u;
  uint64_t m_nextStaticPackageGeneration = 1u;
  VkDeviceSize m_staticBytes = 0u;
  VkDeviceSize m_staticCursor = 0u;
  uint64_t m_queuedStaticMissHostBytes = 0u;
  uint64_t m_peakQueuedStaticMissRecords = 0u;
  uint64_t m_peakQueuedStaticMissHostBytes = 0u;
  std::unordered_map<GpuSkinStaticResourceKey,
      std::shared_ptr<GpuSkinStaticResource>, GpuSkinStaticResourceKeyHash>
      m_staticResources;
  std::deque<QueuedStaticMiss> m_staticMisses;
  std::vector<GpuSkinStaticUpload> m_readyStaticUploads;
  Rc<DxvkBuffer> m_staticAtlas;
  resource_census::ResourceHandle m_staticAtlasCensus;
  std::deque<RetiredStaticUpload> m_retiredStaticUploads;
};

static_assert(!War3PersistentGpuPackageStore::kD3D9SharedOwnerEnabled);
static_assert(!War3PersistentGpuPackageStore::kRequiresNativeBridge);
static_assert(!War3PersistentGpuPackageStore::kCrossEpochRetirementSafe);

}  // namespace dxvk::war3::gpu_skin
