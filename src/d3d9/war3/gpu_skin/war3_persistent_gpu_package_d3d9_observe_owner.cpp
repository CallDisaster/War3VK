#include "war3_persistent_gpu_package_d3d9_observe_owner.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace dxvk::war3::gpu_skin {

namespace {

std::atomic<uint64_t> g_nextD3D9ObserveOwnerAuthority{1u};

bool HasSliceContract(
    const DxvkBufferSlice& slice, VkBufferUsageFlags usage,
    VkPipelineStageFlags stages, VkAccessFlags access) noexcept {
  if (!slice.defined() || slice.length() == 0u || slice.buffer() == nullptr)
    return false;
  const DxvkBufferCreateInfo& info = slice.buffer()->info();
  return (info.usage & usage) == usage &&
      (info.stages & stages) == stages &&
      (info.access & access) == access &&
      slice.offset() <= info.size &&
      slice.length() <= info.size - slice.offset();
}

bool ValidObserveUpload(
    const GpuSkinStaticUpload& upload,
    uint64_t mapEpoch, uint64_t deviceEpoch) noexcept {
  if (upload.key.mapEpoch != mapEpoch ||
      upload.key.deviceEpoch != deviceEpoch ||
      upload.key.geosetData == 0u || upload.key.contentHash == 0u ||
      upload.key.immutableModelGeneration == 0u ||
      upload.key.layoutGeneration !=
          War3PersistentGpuPackageStore::kStaticPackingLayoutGeneration ||
      upload.key.reserved != 0u || upload.byteCount == 0u ||
      upload.source.length() != upload.byteCount ||
      upload.destination.length() != upload.byteCount ||
      !HasSliceContract(
          upload.source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT) ||
      !HasSliceContract(
          upload.destination, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)) {
    return false;
  }
  if (upload.source.buffer() != upload.destination.buffer())
    return true;
  const VkDeviceSize sourceEnd = upload.source.offset() + upload.byteCount;
  const VkDeviceSize destinationEnd =
      upload.destination.offset() + upload.byteCount;
  return sourceEnd <= upload.destination.offset() ||
      destinationEnd <= upload.source.offset();
}

}  // namespace

uint64_t War3PersistentGpuPackageD3D9ObserveOwner::
allocateOwnerAuthority() noexcept {
  uint64_t current = g_nextD3D9ObserveOwnerAuthority.load(
      std::memory_order_relaxed);
  while (current != 0u &&
         current != (std::numeric_limits<uint64_t>::max)()) {
    if (g_nextD3D9ObserveOwnerAuthority.compare_exchange_weak(
            current, current + 1u, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return current;
    }
  }
  return 0u;
}

War3PersistentGpuPackageD3D9ObserveOwner::
War3PersistentGpuPackageD3D9ObserveOwner(
    Rc<DxvkDevice> device, const GpuSkinResourceBudgets& budgets)
: m_device(std::move(device)), m_budgets(budgets),
  m_ownerAuthority(allocateOwnerAuthority()) {
  if (m_device == nullptr || m_ownerAuthority == 0u)
    return;

  const VkDeviceSize alignment = (std::max)(VkDeviceSize{16u},
      m_device->properties().core.properties.limits
          .minStorageBufferOffsetAlignment);
  DxvkFenceCreateInfo fenceInfo = {};
  fenceInfo.initialValue = 0u;
  m_producerFence = m_device->createFence(fenceInfo);
  if (m_producerFence == nullptr)
    return;
  m_store = std::make_unique<War3PersistentGpuPackageStore>(
      m_device, m_budgets, alignment, m_storeDiagnostics);
}

War3PersistentGpuPackageD3D9ObserveOwner::
~War3PersistentGpuPackageD3D9ObserveOwner() {
  pollProducerCompletions();
}

bool War3PersistentGpuPackageD3D9ObserveOwner::validEvidenceAndSnapshot(
    const Stage11Evidence& evidence,
    const model::ShadowGeosetResourceSnapshot& snapshot) const noexcept {
  using Adapter = War3PersistentGpuPackageStage11ObserveAdapter;
  return evidence.requestedMode == Adapter::Mode::Observe &&
      evidence.effectiveMode == Adapter::Mode::Observe &&
      evidence.disposition == Adapter::Disposition::RecordedCurrentMapSource &&
      evidence.frameSerial != 0u && evidence.policyRevision != 0u &&
      evidence.mapEpoch != 0u && evidence.deviceEpoch != 0u &&
      evidence.sourceGeneration != 0u &&
      evidence.immutableModelGeneration != 0u &&
      evidence.immutableContentHash != 0u &&
      evidence.meshPayloadIdentity != 0u &&
      evidence.geosetVertexCount != 0u &&
      evidence.provesCurrentGameMemory && !evidence.packageReady &&
      !evidence.fullyEquivalent && !evidence.drawMutationAllowed &&
      !evidence.gpuBindingAllowed && !evidence.commandRecordingAllowed &&
      !evidence.packagePublicationAllowed &&
      evidence.eligibleConsumerMask == 0u &&
      evidence.wouldUseConsumerMask == 0u &&
      evidence.actualConsumerMask == 0u && snapshot != nullptr &&
      reinterpret_cast<uintptr_t>(snapshot->geosetDataPtr) ==
          evidence.meshPayloadIdentity &&
      snapshot->mapEpoch == evidence.mapEpoch &&
      snapshot->contentHash == evidence.immutableContentHash &&
      snapshot->immutableModelGeneration ==
          evidence.immutableModelGeneration &&
      snapshot->vertexCount == evidence.geosetVertexCount &&
      snapshot->immutableCaptureStatus ==
          model::ShadowGeosetImmutableCaptureStatus::Complete &&
      snapshot->readyForShadowConsumer();
}

bool War3PersistentGpuPackageD3D9ObserveOwner::beginFrame(
    uint64_t mapEpoch, uint64_t deviceEpoch,
    uint64_t frameSerial) noexcept {
  if (m_store == nullptr || m_producerFence == nullptr ||
      m_ownerAuthority == 0u || mapEpoch == 0u || deviceEpoch == 0u ||
      frameSerial == 0u) {
    return false;
  }
  try {
    if (m_deviceEpoch != deviceEpoch) {
      m_store->invalidateDevice(
          m_device, deviceEpoch,
          (std::max)(VkDeviceSize{16u},
              m_device->properties().core.properties.limits
                  .minStorageBufferOffsetAlignment));
      m_deviceEpoch = deviceEpoch;
      m_mapEpoch = 0u;
    }
    if (m_mapEpoch != mapEpoch) {
      m_store->invalidateMapEpoch(mapEpoch);
      m_mapEpoch = mapEpoch;
    }
    if (m_frameSerial != frameSerial) {
      pollProducerCompletions();
      m_frameSerial = frameSerial;
      m_preparedThisFrame = 0u;
    }
    return m_store->beginFrame(mapEpoch, deviceEpoch, frameSerial);
  } catch (...) {
    return false;
  }
}

War3PersistentGpuPackageD3D9ObserveOwner::ObserveResult
War3PersistentGpuPackageD3D9ObserveOwner::observe(
    const Stage11Evidence& evidence,
    model::ShadowGeosetResourceSnapshot snapshot) noexcept {
  ++m_diagnostics.observeCalls;
  ObserveResult result = {};
  if (!validEvidenceAndSnapshot(evidence, snapshot)) {
    result.disposition = Disposition::InvalidEvidence;
    ++m_diagnostics.invalidEvidence;
    return result;
  }
  if (!beginFrame(
          evidence.mapEpoch, evidence.deviceEpoch, evidence.frameSerial)) {
    result.disposition = Disposition::EpochRejected;
    result.fallback = GpuSkinFallbackReason::InvalidEpoch;
    ++m_diagnostics.epochRejects;
    return result;
  }

  const model::ShadowGeosetResourceSnapshot currentSnapshot =
      model::ShadowModelResourceCache::instance()
          .findGeosetSnapshotByData(snapshot->geosetDataPtr);
  if (currentSnapshot == nullptr || currentSnapshot.get() != snapshot.get() ||
      currentSnapshot->mapEpoch != evidence.mapEpoch ||
      currentSnapshot->contentHash != evidence.immutableContentHash ||
      currentSnapshot->immutableModelGeneration !=
          evidence.immutableModelGeneration) {
    result.disposition = Disposition::InvalidSnapshot;
    result.fallback = GpuSkinFallbackReason::StaticResourceInvalid;
    ++m_diagnostics.invalidSnapshots;
    return result;
  }

  ++m_diagnostics.exactSourcesAccepted;
  result.primitiveCount = currentSnapshot->primitiveCount;
  if (result.primitiveCount > 1u)
    ++m_diagnostics.multiPrimitiveObservations;

  try {
    const GpuSkinStaticLookup lookup = m_store->findOrQueueStatic(
        currentSnapshot,
        War3PersistentGpuPackageStore::kStaticPackingLayoutGeneration);
    result.fallback = lookup.fallback;
    if (lookup) {
      result.disposition = Disposition::ReadyObserved;
      result.ready = true;
      result.packageGeneration =
          lookup.resource->packageProof.packageGeneration;
      ++m_diagnostics.readyObservations;
    } else if (lookup.fallback ==
                   GpuSkinFallbackReason::StaticResourceMiss) {
      result.disposition = Disposition::MissQueued;
      ++m_diagnostics.missObservations;
    } else if (lookup.fallback ==
                   GpuSkinFallbackReason::StaticResourcePending) {
      result.disposition = Disposition::Pending;
      ++m_diagnostics.pendingObservations;
    } else {
      result.disposition = Disposition::StoreRejected;
      ++m_diagnostics.storeRejects;
    }

    if (m_preparedThisFrame < kMaxPreparesPerFrame) {
      const uint32_t remaining =
          kMaxPreparesPerFrame - m_preparedThisFrame;
      m_preparedThisFrame +=
          m_store->prepareQueuedStaticResources(remaining);
    }
  } catch (...) {
    result = {};
    result.disposition = Disposition::StoreRejected;
    result.fallback = GpuSkinFallbackReason::StaticResourceInvalid;
    ++m_diagnostics.storeRejects;
  }
  return result;
}

War3PersistentGpuPackageD3D9ObserveOwner::Submission
War3PersistentGpuPackageD3D9ObserveOwner::takeSubmission() noexcept {
  if (m_store == nullptr || m_producerFence == nullptr ||
      static_cast<bool>(m_openSubmission) || m_mapEpoch == 0u ||
      m_deviceEpoch == 0u || m_frameSerial == 0u) {
    return {};
  }

  try {
    // Allocate the command-shared vector before removing uploads from Store.
    // The subsequent vector move assignment is noexcept with the same
    // allocator, so allocation failure cannot strand the ready queue.
    auto owned = std::make_shared<std::vector<GpuSkinStaticUpload>>();
    *owned = m_store->takeStaticUploads();
    if (owned->empty())
      return {};

    uint64_t byteCount = 0u;
    for (size_t i = 0u; i < owned->size(); ++i) {
      const GpuSkinStaticUpload& upload = (*owned)[i];
      if (!ValidObserveUpload(upload, m_mapEpoch, m_deviceEpoch) ||
          upload.byteCount >
              (std::numeric_limits<uint64_t>::max)() - byteCount) {
        ++m_diagnostics.submissionsRejected;
        return {};
      }
      for (size_t earlier = 0u; earlier < i; ++earlier) {
        if ((*owned)[earlier].key == upload.key) {
          ++m_diagnostics.submissionsRejected;
          return {};
        }
      }
      byteCount += upload.byteCount;
    }
    if (m_nextSubmissionSerial == 0u ||
        m_nextSubmissionSerial ==
            (std::numeric_limits<uint64_t>::max)() ||
        m_nextFenceValue == 0u ||
        m_nextFenceValue == (std::numeric_limits<uint64_t>::max)()) {
      ++m_diagnostics.submissionsRejected;
      return {};
    }

    Submission submission = {};
    submission.ownerAuthority = m_ownerAuthority;
    submission.serial = m_nextSubmissionSerial++;
    submission.mapEpoch = m_mapEpoch;
    submission.deviceEpoch = m_deviceEpoch;
    submission.frameSerial = m_frameSerial;
    submission.fenceValue = m_nextFenceValue++;
    submission.byteCount = byteCount;
    submission.fence = m_producerFence;
    submission.uploads = std::move(owned);
    m_openSubmission = submission;
    ++m_diagnostics.submissionsBuilt;
    return submission;
  } catch (...) {
    ++m_diagnostics.submissionsRejected;
    return {};
  }
}

bool War3PersistentGpuPackageD3D9ObserveOwner::validSubmission(
    const Submission& submission) const noexcept {
  return static_cast<bool>(submission) &&
      static_cast<bool>(m_openSubmission) &&
      submission.ownerAuthority == m_ownerAuthority &&
      submission.ownerAuthority == m_openSubmission.ownerAuthority &&
      submission.serial == m_openSubmission.serial &&
      submission.mapEpoch == m_openSubmission.mapEpoch &&
      submission.deviceEpoch == m_openSubmission.deviceEpoch &&
      submission.frameSerial == m_openSubmission.frameSerial &&
      submission.fenceValue == m_openSubmission.fenceValue &&
      submission.byteCount == m_openSubmission.byteCount &&
      submission.fence == m_producerFence &&
      submission.fence == m_openSubmission.fence &&
      submission.uploads.get() == m_openSubmission.uploads.get() &&
      submission.mapEpoch == m_mapEpoch &&
      submission.deviceEpoch == m_deviceEpoch;
}

bool War3PersistentGpuPackageD3D9ObserveOwner::commitSubmission(
    const Submission& submission) noexcept {
  if (!validSubmission(submission)) {
    ++m_diagnostics.submissionsRejected;
    return false;
  }
  bool committed = false;
  try {
    committed = m_store->retireStaticUploads(
        *submission.uploads, submission.fence, submission.fenceValue);
  } catch (...) {
    committed = false;
  }
  if (committed) {
    ++m_diagnostics.submissionsCommitted;
    m_diagnostics.uploadsCommitted += submission.uploads->size();
    m_diagnostics.uploadBytesCommitted += submission.byteCount;
    m_diagnostics.lastSubmittedFenceValue = submission.fenceValue;
  } else {
    ++m_diagnostics.submissionsRejected;
  }
  m_openSubmission = {};
  return committed;
}

void War3PersistentGpuPackageD3D9ObserveOwner::rejectSubmission(
    const Submission& submission) noexcept {
  if (!validSubmission(submission))
    return;
  ++m_diagnostics.submissionsRejected;
  m_openSubmission = {};
}

void War3PersistentGpuPackageD3D9ObserveOwner::invalidateMapEpoch(
    uint64_t nextMapEpoch) noexcept {
  if (m_store == nullptr || nextMapEpoch == 0u)
    return;
  try {
    m_store->invalidateMapEpoch(nextMapEpoch);
    m_mapEpoch = nextMapEpoch;
    m_frameSerial = 0u;
    m_preparedThisFrame = 0u;
    m_openSubmission = {};
  } catch (...) {
  }
}

void War3PersistentGpuPackageD3D9ObserveOwner::invalidateDeviceEpoch(
    uint64_t nextDeviceEpoch) noexcept {
  if (m_store == nullptr || m_device == nullptr || nextDeviceEpoch == 0u)
    return;
  try {
    const VkDeviceSize alignment = (std::max)(VkDeviceSize{16u},
        m_device->properties().core.properties.limits
            .minStorageBufferOffsetAlignment);
    m_store->invalidateDevice(m_device, nextDeviceEpoch, alignment);
    m_deviceEpoch = nextDeviceEpoch;
    m_mapEpoch = 0u;
    m_frameSerial = 0u;
    m_preparedThisFrame = 0u;
    m_openSubmission = {};
  } catch (...) {
  }
}

void War3PersistentGpuPackageD3D9ObserveOwner::
pollProducerCompletions() noexcept {
  if (m_store == nullptr || m_producerFence == nullptr)
    return;
  try {
    m_store->pollRetired([](const Rc<DxvkFence>& fence) {
      return fence != nullptr ? fence->getValue() : uint64_t{0u};
    });
    m_diagnostics.lastCompletedFenceValue = m_producerFence->getValue();
  } catch (...) {
  }
}

War3PersistentGpuPackageD3D9ObserveOwner::Diagnostics
War3PersistentGpuPackageD3D9ObserveOwner::diagnostics() const noexcept {
  Diagnostics result = m_diagnostics;
  result.currentMapEpoch = m_mapEpoch;
  result.currentDeviceEpoch = m_deviceEpoch;
  result.currentFrameSerial = m_frameSerial;
  result.store = m_storeDiagnostics;
  return result;
}

}  // namespace dxvk::war3::gpu_skin
