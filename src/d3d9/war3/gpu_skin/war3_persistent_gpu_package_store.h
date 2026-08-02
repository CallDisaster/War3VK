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
  static constexpr uint32_t kStaticPackingLayoutGeneration =
      kPersistentGpuPackageStaticLayoutGeneration;
  static constexpr bool kD3D9SharedOwnerEnabled = false;
  static constexpr bool kRequiresNativeBridge = false;
  // A submitted package upload owns both sides of the copy until its producer
  // fence completes. Epoch invalidation may drop lookup state, but it must not
  // discard this retirement queue or release the destination atlas early.
  static constexpr bool kProducerRetirementSurvivesEpochClear = true;
  // P0 reset still clears active atlas lookup state, while submitted copy
  // retirements now survive the epoch. The legacy manager remains the only
  // owner and retains War3GpuSkinResources through consumer completion; a
  // future shared D3D9 owner must additionally publish an exact last-use fence
  // for Main/CSM/point/outline before this gate can be enabled.
  static constexpr bool kCrossEpochRetirementSafe = false;
  // Cache generation alone cannot prove current Game.dll memory because the
  // legacy cache is not map-scoped and has ready-pointer early outs.
  static constexpr bool kCurrentStageSourceAuthorityIntegrated = false;
  static constexpr bool kStaticHintCurrentGenerationAuthorityIntegrated =
      true;
  // This is a capability declaration, not a runtime thread guard. The public
  // GpuSkinStaticUpload still exposes key/slices, and the interval from Store
  // creation through manager EmitCs recording has no sealed transaction token
  // or final exact revalidation. P2 must close both before publication.
  static constexpr bool kRecordingThreadOwnershipIntegrated = false;
  // P1 freezes immutable content only. Legacy Ready still means the upload was
  // accepted for submission; it is not producer-fence completion authority.
  static constexpr bool kProducerCompletionAuthorityIntegrated = false;
  // GPU slice/usage checks run in production validation, but the isolated
  // CPU runnable intentionally constructs no DxvkDevice/DxvkBuffer.
  static constexpr bool kFrozenGpuSliceRunnableIntegrated = false;

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
      if (*it != nullptr && (*it)->fence != nullptr &&
          getFenceValue((*it)->fence) >= (*it)->retireValue) {
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
    PersistentGpuPackageImmutableProof immutableProof;
    std::vector<PersistentGpuPackagePrimitiveProof> primitiveProofs;
  };

  struct RetiredStaticUpload {
    GpuSkinStaticResourceKey key;
    DxvkBufferSlice source;
    DxvkBufferSlice destination;
    Rc<DxvkFence> fence;
    uint64_t retireValue = 0;
    resource_census::ResourceHandle residencyCensus;
    resource_census::ResourceHandle destinationResidencyCensus;
  };

  GpuSkinStaticResourceKey makeKey(
      const model::ShadowGeosetResourceRecord& record,
      uint32_t layoutGeneration) const;
  std::shared_ptr<GpuSkinStaticResource> createStaticResource(
      const QueuedStaticMiss& miss);
  bool ownsFrozenPayload(
      const GpuSkinStaticResource& resource) const noexcept;
  void recordFallback(GpuSkinFallbackReason reason);
  void clearEpochResources();
  void deferOutstandingProducerRetirements() noexcept;

  Rc<DxvkDevice> m_device;
  GpuSkinResourceBudgets m_budgets;
  VkDeviceSize m_storageBufferOffsetAlignment = 16u;
  GpuSkinDiagnostics& m_diagnostics;
  uint64_t m_mapEpoch = 0u;
  uint64_t m_deviceEpoch = 0u;
  uint64_t m_instanceAuthority = 0u;
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
  std::deque<std::unique_ptr<RetiredStaticUpload>> m_retiredStaticUploads;
};

static_assert(!War3PersistentGpuPackageStore::kD3D9SharedOwnerEnabled);
static_assert(!War3PersistentGpuPackageStore::kRequiresNativeBridge);
static_assert(
    War3PersistentGpuPackageStore::kProducerRetirementSurvivesEpochClear);
static_assert(!War3PersistentGpuPackageStore::kCrossEpochRetirementSafe);
static_assert(!War3PersistentGpuPackageStore::
    kCurrentStageSourceAuthorityIntegrated);
static_assert(War3PersistentGpuPackageStore::
    kStaticHintCurrentGenerationAuthorityIntegrated);
static_assert(!War3PersistentGpuPackageStore::
    kRecordingThreadOwnershipIntegrated);
static_assert(!War3PersistentGpuPackageStore::
    kProducerCompletionAuthorityIntegrated);
static_assert(!War3PersistentGpuPackageStore::
    kFrozenGpuSliceRunnableIntegrated);

}  // namespace dxvk::war3::gpu_skin
