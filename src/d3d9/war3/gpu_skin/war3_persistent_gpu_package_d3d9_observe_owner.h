#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "war3_persistent_gpu_package_stage11_observe_adapter.h"
#include "war3_persistent_gpu_package_store.h"

namespace dxvk::war3::gpu_skin {

// D3D9-owned, native-bridge-independent package uploader.  This stage proves
// real atlas creation, copy submission and producer-fence completion only.
// It deliberately exposes no buffer slice or consumer authority to Main,
// CSM, point shadow or outline.
class War3PersistentGpuPackageD3D9ObserveOwner final {
public:
  using Stage11Evidence =
      War3PersistentGpuPackageStage11ObserveAdapter::Evidence;

  static constexpr bool kRuntimeObserveUploadIntegrated = true;
  static constexpr bool kObserveOnly = true;
  static constexpr bool kRequiresNativeBridge = false;
  static constexpr bool kBindsGpuResources = false;
  static constexpr bool kMutatesCanonicalDraw = false;
  static constexpr bool kPublishesConsumerAuthority = false;
  static constexpr bool kPublishesConsumerLastUseFence = false;
  static constexpr uint32_t kMaxPreparesPerFrame = 2u;

  enum class Disposition : uint8_t {
    InvalidEvidence = 0u,
    InvalidSnapshot,
    EpochRejected,
    ReadyObserved,
    MissQueued,
    Pending,
    StoreRejected,
  };

  struct ObserveResult {
    Disposition disposition = Disposition::InvalidEvidence;
    GpuSkinFallbackReason fallback = GpuSkinFallbackReason::None;
    uint64_t packageGeneration = 0u;
    uint32_t primitiveCount = 0u;
    bool ready = false;
    bool gpuBindingAllowed = false;
    bool drawMutationAllowed = false;
    bool consumerAuthorityPublished = false;
  };

  struct Submission {
    uint64_t ownerAuthority = 0u;
    uint64_t serial = 0u;
    uint64_t mapEpoch = 0u;
    uint64_t deviceEpoch = 0u;
    uint64_t frameSerial = 0u;
    uint64_t fenceValue = 0u;
    uint64_t byteCount = 0u;
    Rc<DxvkFence> fence;
    std::shared_ptr<const std::vector<GpuSkinStaticUpload>> uploads;

    explicit operator bool() const noexcept {
      return ownerAuthority != 0u && serial != 0u && mapEpoch != 0u &&
          deviceEpoch != 0u && frameSerial != 0u && fenceValue != 0u &&
          byteCount != 0u && fence != nullptr && uploads != nullptr &&
          !uploads->empty();
    }
  };

  struct Diagnostics {
    uint64_t observeCalls = 0u;
    uint64_t exactSourcesAccepted = 0u;
    uint64_t invalidEvidence = 0u;
    uint64_t invalidSnapshots = 0u;
    uint64_t epochRejects = 0u;
    uint64_t readyObservations = 0u;
    uint64_t missObservations = 0u;
    uint64_t pendingObservations = 0u;
    uint64_t storeRejects = 0u;
    uint64_t multiPrimitiveObservations = 0u;
    uint64_t submissionsBuilt = 0u;
    uint64_t submissionsCommitted = 0u;
    uint64_t submissionsRejected = 0u;
    uint64_t uploadsCommitted = 0u;
    uint64_t uploadBytesCommitted = 0u;
    uint64_t lastSubmittedFenceValue = 0u;
    uint64_t lastCompletedFenceValue = 0u;
    uint64_t currentMapEpoch = 0u;
    uint64_t currentDeviceEpoch = 0u;
    uint64_t currentFrameSerial = 0u;
    GpuSkinDiagnostics store = {};
  };

  explicit War3PersistentGpuPackageD3D9ObserveOwner(
      Rc<DxvkDevice> device,
      const GpuSkinResourceBudgets& budgets = {});
  ~War3PersistentGpuPackageD3D9ObserveOwner();

  War3PersistentGpuPackageD3D9ObserveOwner(
      const War3PersistentGpuPackageD3D9ObserveOwner&) = delete;
  War3PersistentGpuPackageD3D9ObserveOwner& operator=(
      const War3PersistentGpuPackageD3D9ObserveOwner&) = delete;

  ObserveResult observe(
      const Stage11Evidence& evidence,
      model::ShadowGeosetResourceSnapshot snapshot) noexcept;
  Submission takeSubmission() noexcept;
  bool commitSubmission(const Submission& submission) noexcept;
  void rejectSubmission(const Submission& submission) noexcept;

  void invalidateMapEpoch(uint64_t nextMapEpoch) noexcept;
  void invalidateDeviceEpoch(uint64_t nextDeviceEpoch) noexcept;
  void pollProducerCompletions() noexcept;
  Diagnostics diagnostics() const noexcept;

private:
  bool beginFrame(uint64_t mapEpoch, uint64_t deviceEpoch,
                  uint64_t frameSerial) noexcept;
  bool validEvidenceAndSnapshot(
      const Stage11Evidence& evidence,
      const model::ShadowGeosetResourceSnapshot& snapshot) const noexcept;
  bool validSubmission(const Submission& submission) const noexcept;
  static uint64_t allocateOwnerAuthority() noexcept;

  Rc<DxvkDevice> m_device;
  Rc<DxvkFence> m_producerFence;
  GpuSkinResourceBudgets m_budgets;
  GpuSkinDiagnostics m_storeDiagnostics = {};
  std::unique_ptr<War3PersistentGpuPackageStore> m_store;
  uint64_t m_ownerAuthority = 0u;
  uint64_t m_mapEpoch = 0u;
  uint64_t m_deviceEpoch = 0u;
  uint64_t m_frameSerial = 0u;
  uint64_t m_nextSubmissionSerial = 1u;
  uint64_t m_nextFenceValue = 1u;
  Submission m_openSubmission = {};
  uint32_t m_preparedThisFrame = 0u;
  Diagnostics m_diagnostics = {};
};

static_assert(
    War3PersistentGpuPackageD3D9ObserveOwner::
        kRuntimeObserveUploadIntegrated);
static_assert(War3PersistentGpuPackageD3D9ObserveOwner::kObserveOnly);
static_assert(
    !War3PersistentGpuPackageD3D9ObserveOwner::kRequiresNativeBridge);
static_assert(
    !War3PersistentGpuPackageD3D9ObserveOwner::kBindsGpuResources);
static_assert(
    !War3PersistentGpuPackageD3D9ObserveOwner::kMutatesCanonicalDraw);
static_assert(
    !War3PersistentGpuPackageD3D9ObserveOwner::
        kPublishesConsumerAuthority);
static_assert(
    !War3PersistentGpuPackageD3D9ObserveOwner::
        kPublishesConsumerLastUseFence);

}  // namespace dxvk::war3::gpu_skin
