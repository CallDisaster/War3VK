#include "war3_resource_residency_census.h"

#include "../model/war3_model_resource_cache.h"
#include "../shadow/war3_shadow_runtime_contract.h"

#include "../../d3d9_mem.h"
#include "../../../util/util_env.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <string>

namespace dxvk::war3::resource_census {

struct ResourceRecord {
  uint64_t id = 0;
  ResourceRegistration registration;
  std::atomic<uint64_t> hostBackingLogicalBytes{0};
  std::atomic<uint64_t> hostMappedLogicalBytes{0};
  std::atomic<uint64_t> duplicateHostBackingLogicalBytes{0};
  std::atomic<uint64_t> lockCalls{0};
  std::atomic<uint64_t> activeLocks{0};
  std::atomic<uint64_t> readLocks{0};
  std::atomic<uint64_t> writeLocks{0};
  std::atomic<uint64_t> requestedDiscardLocks{0};
  std::atomic<uint64_t> effectiveDiscardLocks{0};
  std::atomic<uint64_t> requestedNoOverwriteLocks{0};
  std::atomic<uint64_t> effectiveNoOverwriteLocks{0};
  std::atomic<uint64_t> fullSubresourceWriteLocks{0};
  std::atomic<uint64_t> partialSubresourceWriteLocks{0};
  std::atomic<uint64_t> externalDirtyCalls{0};
  std::atomic<uint64_t> fullWriteMaskLo{0};
  std::atomic<uint64_t> fullWriteMaskHi{0};
  std::atomic<uint64_t> uploadedMaskLo{0};
  std::atomic<uint64_t> uploadedMaskHi{0};
  std::atomic<uint64_t> lastLockFrame{0};
  std::atomic<uint64_t> lastWriteFrame{0};
  std::atomic<uint64_t> lastUploadFrame{0};
  std::mutex hostBindingMutex;
  HostBackingBinding hostBinding;
};

namespace {

constexpr uint32_t kD3dLockNoOverwrite = 0x1000u;
constexpr uint32_t kD3dLockDiscard = 0x2000u;
constexpr uint32_t kD3dPoolDefault = 0u;
constexpr uint64_t kTopEntryLimit = 32u;
constexpr uint64_t kQuiescentFrames = 300u;

std::mutex g_registryMutex;
std::mutex g_gpuSkinPoolMutex;
std::vector<std::weak_ptr<ResourceRecord>> g_records;
GpuSkinPoolResidencySnapshot g_gpuSkinPoolSnapshot;
std::atomic<uint64_t> g_nextId{1u};
std::atomic<uint64_t> g_frameSerial{0u};
std::atomic<uint64_t> g_lifetimeRegistrations{0u};

bool ParseEnabled() {
  std::string value = env::getEnvVar("DXVK_WAR3_RESOURCE_CENSUS");
  std::transform(value.begin(), value.end(), value.begin(), [](char c) {
    return char(std::tolower(static_cast<unsigned char>(c)));
  });
  return value == "1" || value == "on" || value == "true" ||
         value == "yes";
}

void SetSubresourceBit(std::atomic<uint64_t>& lo,
                       std::atomic<uint64_t>& hi,
                       uint32_t subresource) noexcept {
  if (subresource < 64u)
    lo.fetch_or(uint64_t(1u) << subresource, std::memory_order_relaxed);
  else if (subresource < 128u)
    hi.fetch_or(uint64_t(1u) << (subresource - 64u),
                std::memory_order_relaxed);
}

void ClearSubresourceBit(std::atomic<uint64_t>& lo,
                         std::atomic<uint64_t>& hi,
                         uint32_t subresource) noexcept {
  if (subresource < 64u)
    lo.fetch_and(~(uint64_t(1u) << subresource),
                 std::memory_order_relaxed);
  else if (subresource < 128u)
    hi.fetch_and(~(uint64_t(1u) << (subresource - 64u)),
                 std::memory_order_relaxed);
}

bool HasEverySubresource(uint32_t count, uint64_t lo, uint64_t hi) {
  if (count == 0u || count > 128u)
    return false;
  const uint64_t expectedLo = count >= 64u
      ? ~uint64_t(0u)
      : ((uint64_t(1u) << count) - 1u);
  const uint32_t hiCount = count > 64u ? count - 64u : 0u;
  const uint64_t expectedHi = hiCount == 0u
      ? 0u
      : (hiCount >= 64u ? ~uint64_t(0u)
                        : ((uint64_t(1u) << hiCount) - 1u));
  return (lo & expectedLo) == expectedLo &&
         (hi & expectedHi) == expectedHi;
}

CandidateObservationClass ClassifyCandidate(
    const ResourceRecord& record,
    uint64_t frameSerial,
    uint64_t duplicateBytes) {
  const auto& info = record.registration;
  if (duplicateBytes == 0u)
    return CandidateObservationClass::None;
  if (info.resourceClass == ResourceClass::GpuSkinStaticMirror) {
    const bool devicePayloadAccepted = info.hasDeviceCopy &&
        HasEverySubresource(
            info.subresourceCount,
            record.uploadedMaskLo.load(std::memory_order_relaxed),
            record.uploadedMaskHi.load(std::memory_order_relaxed));
    return devicePayloadAccepted
        ? CandidateObservationClass::ShareableGpuSkinMirror
        : CandidateObservationClass::None;
  }
  if (info.dynamic ||
      record.activeLocks.load(std::memory_order_relaxed) != 0u ||
      record.readLocks.load(std::memory_order_relaxed) != 0u ||
      record.externalDirtyCalls.load(std::memory_order_relaxed) != 0u ||
      record.partialSubresourceWriteLocks.load(std::memory_order_relaxed) !=
          0u) {
    return CandidateObservationClass::None;
  }
  const bool everySubresourceFullyWritten = HasEverySubresource(
      info.subresourceCount,
      record.fullWriteMaskLo.load(std::memory_order_relaxed),
      record.fullWriteMaskHi.load(std::memory_order_relaxed));
  const bool everySubresourceUploaded = HasEverySubresource(
      info.subresourceCount,
      record.uploadedMaskLo.load(std::memory_order_relaxed),
      record.uploadedMaskHi.load(std::memory_order_relaxed));
  const uint64_t lastWrite =
      record.lastWriteFrame.load(std::memory_order_relaxed);
  const uint64_t lastUpload =
      record.lastUploadFrame.load(std::memory_order_relaxed);
  if (!everySubresourceFullyWritten || !everySubresourceUploaded ||
      frameSerial < lastWrite ||
      lastUpload < lastWrite ||
      frameSerial - lastWrite < kQuiescentFrames) {
    return CandidateObservationClass::None;
  }
  if (info.pool == kD3dPoolDefault && info.writeOnly &&
      info.deviceReadbackCapable)
    return CandidateObservationClass::QuiescentDefaultWriteOnly;
  if (info.hasDeviceCopy && info.deviceReadbackCapable)
    return CandidateObservationClass::QuiescentLazyReadbackRequired;
  return CandidateObservationClass::None;
}

struct BucketKey {
  ResourceClass resourceClass;
  uint32_t pool;
  uint32_t mapMode;
  bool dynamic;
  bool writeOnly;
};

bool SameKey(const ResourceResidencyBucket& bucket, const BucketKey& key) {
  return bucket.resourceClass == key.resourceClass &&
         bucket.pool == key.pool && bucket.mapMode == key.mapMode &&
         bucket.dynamic == key.dynamic && bucket.writeOnly == key.writeOnly;
}

struct CoherentHostBacking {
  uint64_t hostBackingLogicalBytes = 0;
  uint64_t hostMappedLogicalBytes = 0;
  uint64_t duplicateHostBackingLogicalBytes = 0;
  HostBackingBinding binding;
};

CoherentHostBacking CopyHostBacking(ResourceRecord& record) {
  std::lock_guard lock(record.hostBindingMutex);
  CoherentHostBacking result;
  result.hostBackingLogicalBytes =
      record.hostBackingLogicalBytes.load(std::memory_order_relaxed);
  result.hostMappedLogicalBytes =
      record.hostMappedLogicalBytes.load(std::memory_order_relaxed);
  result.duplicateHostBackingLogicalBytes =
      record.duplicateHostBackingLogicalBytes.load(
          std::memory_order_relaxed);
  result.binding = record.hostBinding;
  return result;
}

struct PendingHostBinding {
  uint64_t resourceId = 0;
  HostBackingBinding binding;
  CandidateObservationClass candidate =
      CandidateObservationClass::None;
};

struct BindingInterval {
  uint64_t resourceId = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
  bool mapped = false;
  CandidateObservationClass candidate =
      CandidateObservationClass::None;
};

bool SameAllocatorChunkSnapshot(
    const D3D9MemoryChunkDiagnosticSnapshot& lhs,
    const D3D9MemoryChunkDiagnosticSnapshot& rhs) noexcept {
  return lhs.chunkId == rhs.chunkId &&
      lhs.reserveBytes == rhs.reserveBytes &&
      lhs.chunkOccupiedBytes == rhs.chunkOccupiedBytes &&
      lhs.freePayloadBytes == rhs.freePayloadBytes &&
      lhs.freeRangeCount == rhs.freeRangeCount &&
      lhs.sharedMappedRefs == rhs.sharedMappedRefs &&
      lhs.sharedMappedBytes == rhs.sharedMappedBytes &&
      lhs.standaloneMappedRefs == rhs.standaloneMappedRefs &&
      lhs.standaloneMappedBytes == rhs.standaloneMappedBytes &&
      lhs.mapFailureCount == rhs.mapFailureCount &&
      lhs.unmapFailureCount == rhs.unmapFailureCount &&
      lhs.mappingStateFaultCount == rhs.mappingStateFaultCount;
}

bool SameAllocatorSnapshot(
    const D3D9MemoryAllocatorDiagnosticSnapshot& lhs,
    const D3D9MemoryAllocatorDiagnosticSnapshot& rhs) noexcept {
  if (lhs.chunkBacked != rhs.chunkBacked ||
      lhs.accountingClosure != rhs.accountingClosure ||
      lhs.mutationGenerationSaturated !=
          rhs.mutationGenerationSaturated ||
      lhs.mutationGeneration != rhs.mutationGeneration ||
      lhs.reserveBytes != rhs.reserveBytes ||
      lhs.allocatorUsedPayloadBytes != rhs.allocatorUsedPayloadBytes ||
      lhs.chunkOccupiedBytes != rhs.chunkOccupiedBytes ||
      lhs.internalFragmentationBytes != rhs.internalFragmentationBytes ||
      lhs.freePayloadBytes != rhs.freePayloadBytes ||
      lhs.sharedMappedRefs != rhs.sharedMappedRefs ||
      lhs.sharedMappedBytes != rhs.sharedMappedBytes ||
      lhs.standaloneMappedRefs != rhs.standaloneMappedRefs ||
      lhs.standaloneMappedBytes != rhs.standaloneMappedBytes ||
      lhs.mappedRefs != rhs.mappedRefs ||
      lhs.mappedBytes != rhs.mappedBytes ||
      lhs.mapFailureCount != rhs.mapFailureCount ||
      lhs.unmapFailureCount != rhs.unmapFailureCount ||
      lhs.mappingStateFaultCount != rhs.mappingStateFaultCount ||
      lhs.chunks.size() != rhs.chunks.size()) {
    return false;
  }
  for (size_t i = 0u; i < lhs.chunks.size(); ++i) {
    if (!SameAllocatorChunkSnapshot(lhs.chunks[i], rhs.chunks[i]))
      return false;
  }
  return true;
}

D3D9HostAllocatorSnapshot MakeHostAllocatorSnapshot(
    const D3D9MemoryAllocatorDiagnosticSnapshot& allocator) {
  D3D9HostAllocatorSnapshot result;
  result.allocatedBackingBytes = allocator.reserveBytes;
  result.usedPayloadBytes = allocator.allocatorUsedPayloadBytes;
  result.mappedAddressBytes = allocator.mappedBytes;
  auto& diagnostics = result.chunkDiagnostics;
  diagnostics.available = true;
  diagnostics.authority = false;
  diagnostics.chunkBacked = allocator.chunkBacked;
  diagnostics.accountingClosure = allocator.accountingClosure;
  diagnostics.mutationGeneration = allocator.mutationGeneration;
  diagnostics.mutationGenerationBegin = allocator.mutationGeneration;
  diagnostics.mutationGenerationEnd = allocator.mutationGeneration;
  diagnostics.mutationGenerationSaturated =
      allocator.mutationGenerationSaturated;
  diagnostics.reserveBytes = allocator.reserveBytes;
  diagnostics.allocatorUsedPayloadBytes =
      allocator.allocatorUsedPayloadBytes;
  diagnostics.chunkOccupiedBytes = allocator.chunkOccupiedBytes;
  diagnostics.internalFragmentationBytes =
      allocator.internalFragmentationBytes;
  diagnostics.freePayloadBytes = allocator.freePayloadBytes;
  diagnostics.sharedMappedRefs = allocator.sharedMappedRefs;
  diagnostics.sharedMappedBytes = allocator.sharedMappedBytes;
  diagnostics.standaloneMappedRefs = allocator.standaloneMappedRefs;
  diagnostics.standaloneMappedBytes = allocator.standaloneMappedBytes;
  diagnostics.mappedRefs = allocator.mappedRefs;
  diagnostics.mappedBytes = allocator.mappedBytes;
  diagnostics.mapFailureCount = allocator.mapFailureCount;
  diagnostics.unmapFailureCount = allocator.unmapFailureCount;
  diagnostics.mappingStateFaultCount =
      allocator.mappingStateFaultCount;
  diagnostics.chunks.reserve(allocator.chunks.size());
  for (const auto& source : allocator.chunks) {
    D3D9HostAllocatorSnapshot::Chunk chunk;
    chunk.chunkId = source.chunkId;
    chunk.reserveBytes = source.reserveBytes;
    chunk.chunkOccupiedBytes = source.chunkOccupiedBytes;
    chunk.freePayloadBytes = source.freePayloadBytes;
    chunk.freeRangeCount = source.freeRangeCount;
    chunk.sharedMappedRefs = source.sharedMappedRefs;
    chunk.sharedMappedBytes = source.sharedMappedBytes;
    chunk.standaloneMappedRefs = source.standaloneMappedRefs;
    chunk.standaloneMappedBytes = source.standaloneMappedBytes;
    chunk.mapFailureCount = source.mapFailureCount;
    chunk.unmapFailureCount = source.unmapFailureCount;
    chunk.mappingStateFaultCount = source.mappingStateFaultCount;
    diagnostics.chunks.push_back(chunk);
  }
  return result;
}

} // namespace

bool Enabled() noexcept {
  static const bool enabled = ParseEnabled();
  return enabled;
}

ResourceHandle Register(
    const ResourceRegistration& registration) noexcept {
  if (!Enabled())
    return {};
  try {
    auto record = std::make_shared<ResourceRecord>();
    record->id = g_nextId.fetch_add(1u, std::memory_order_relaxed);
    if (record->id == 0u)
      record->id = g_nextId.fetch_add(1u, std::memory_order_relaxed);
    record->registration = registration;
    record->hostBackingLogicalBytes.store(
        registration.hostBackingLogicalBytes, std::memory_order_relaxed);
    record->hostMappedLogicalBytes.store(
        registration.hostMappedLogicalBytes, std::memory_order_relaxed);
    record->duplicateHostBackingLogicalBytes.store(
        registration.duplicateHostBackingLogicalBytes,
        std::memory_order_relaxed);
    record->hostBinding = registration.hostBinding;
    {
      std::lock_guard lock(g_registryMutex);
      g_records.emplace_back(record);
    }
    g_lifetimeRegistrations.fetch_add(1u, std::memory_order_relaxed);
    return record;
  } catch (...) {
    return {};
  }
}

void UpdateHostBacking(const ResourceHandle& handle,
                       uint64_t hostBackingLogicalBytes,
                       uint64_t hostMappedLogicalBytes,
                       uint64_t duplicateHostBackingLogicalBytes,
                       const HostBackingBinding& hostBinding) noexcept {
  if (handle == nullptr)
    return;
  {
    std::lock_guard lock(handle->hostBindingMutex);
    // 三个数值和 numeric binding 属于一个诊断身份；必须在同一临界区
    // 发布，避免资源扫描观察到新数值配旧 binding（或反向组合）。
    handle->hostBackingLogicalBytes.store(
        hostBackingLogicalBytes, std::memory_order_relaxed);
    handle->hostMappedLogicalBytes.store(
        hostMappedLogicalBytes, std::memory_order_relaxed);
    handle->duplicateHostBackingLogicalBytes.store(
        duplicateHostBackingLogicalBytes, std::memory_order_relaxed);
    handle->hostBinding = hostBinding;
  }
}

void NoteLock(const ResourceHandle& handle,
              const ResourceLockEvent& event) noexcept {
  if (handle == nullptr)
    return;
  handle->lockCalls.fetch_add(1u, std::memory_order_relaxed);
  handle->activeLocks.fetch_add(1u, std::memory_order_relaxed);
  const uint64_t frame = g_frameSerial.load(std::memory_order_relaxed);
  handle->lastLockFrame.store(frame, std::memory_order_relaxed);
  if (event.readIntent)
    handle->readLocks.fetch_add(1u, std::memory_order_relaxed);
  if (event.writeIntent) {
    handle->writeLocks.fetch_add(1u, std::memory_order_relaxed);
    if (event.fullSubresource) {
      handle->fullSubresourceWriteLocks.fetch_add(
          1u, std::memory_order_relaxed);
      SetSubresourceBit(handle->fullWriteMaskLo, handle->fullWriteMaskHi,
                        event.subresource);
    } else {
      handle->partialSubresourceWriteLocks.fetch_add(
          1u, std::memory_order_relaxed);
    }
    handle->lastWriteFrame.store(frame, std::memory_order_relaxed);
    // 新的写入会使该子资源此前的上传证明失效。
    ClearSubresourceBit(handle->uploadedMaskLo, handle->uploadedMaskHi,
                        event.subresource);
  }
  if ((event.requestedFlags & kD3dLockDiscard) != 0u)
    handle->requestedDiscardLocks.fetch_add(1u, std::memory_order_relaxed);
  if ((event.effectiveFlags & kD3dLockDiscard) != 0u)
    handle->effectiveDiscardLocks.fetch_add(1u, std::memory_order_relaxed);
  if ((event.requestedFlags & kD3dLockNoOverwrite) != 0u)
    handle->requestedNoOverwriteLocks.fetch_add(1u,
                                                 std::memory_order_relaxed);
  if ((event.effectiveFlags & kD3dLockNoOverwrite) != 0u)
    handle->effectiveNoOverwriteLocks.fetch_add(1u,
                                                 std::memory_order_relaxed);
}

void NoteUnlock(const ResourceHandle& handle) noexcept {
  if (handle == nullptr)
    return;
  uint64_t previous = handle->activeLocks.load(std::memory_order_relaxed);
  while (previous != 0u &&
         !handle->activeLocks.compare_exchange_weak(
             previous, previous - 1u,
             std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void NoteExternalDirty(const ResourceHandle& handle) noexcept {
  if (handle == nullptr)
    return;
  handle->externalDirtyCalls.fetch_add(1u, std::memory_order_relaxed);
}

void NoteDeviceUpload(const ResourceHandle& handle,
                      uint32_t subresource) noexcept {
  if (handle == nullptr)
    return;
  SetSubresourceBit(handle->uploadedMaskLo, handle->uploadedMaskHi,
                    subresource);
  handle->lastUploadFrame.store(
      g_frameSerial.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
}

void SetFrameSerial(uint64_t frameSerial) noexcept {
  g_frameSerial.store(frameSerial, std::memory_order_relaxed);
}

void UpdateGpuSkinPoolSnapshot(
    const GpuSkinPoolResidencySnapshot& snapshot) noexcept {
  if (!Enabled())
    return;
  std::lock_guard lock(g_gpuSkinPoolMutex);
  g_gpuSkinPoolSnapshot = snapshot;
}

ResourceResidencySnapshot CaptureSnapshot(
    D3D9MemoryAllocator* allocator) {
  ResourceResidencySnapshot snapshot;
  snapshot.enabled = Enabled();
  snapshot.frameSerial = g_frameSerial.load(std::memory_order_relaxed);
  snapshot.lifetimeRegistrations = g_lifetimeRegistrations.load(
      std::memory_order_relaxed);

  if (snapshot.enabled) {
    const auto modelMemory =
        model::ShadowModelResourceCache::instance().memorySnapshot();
    snapshot.modelCache.uniqueGeosetRecords =
        modelMemory.uniqueGeosetRecords;
    snapshot.modelCache.residentGeosetRecords =
        modelMemory.residentGeosetRecords;
    snapshot.modelCache.uniqueGeosetCapacityBytes =
        modelMemory.uniqueGeosetCapacityBytes;
    snapshot.modelCache.residentGeosetCapacityBytes =
        modelMemory.residentGeosetCapacityBytes;
    snapshot.modelCache.aliasDuplicateCapacityBytes =
        modelMemory.aliasDuplicateCapacityBytes;
    snapshot.modelCache.positionsCapacityBytes =
        modelMemory.positionsCapacityBytes;
    snapshot.modelCache.normalsCapacityBytes =
        modelMemory.normalsCapacityBytes;
    snapshot.modelCache.groupSlotsCapacityBytes =
        modelMemory.groupSlotsCapacityBytes;
    snapshot.modelCache.uvCapacityBytes = modelMemory.uvCapacityBytes;
    snapshot.modelCache.primitiveCapacityBytes =
        modelMemory.primitiveCapacityBytes;
    snapshot.modelCache.indicesCapacityBytes =
        modelMemory.indicesCapacityBytes;
    snapshot.modelCache.matrixGroupsCapacityBytes =
        modelMemory.matrixGroupsCapacityBytes;
    snapshot.modelCache.matrixIndicesCapacityBytes =
        modelMemory.matrixIndicesCapacityBytes;
    snapshot.modelCache.modelPointerCapacityBytes =
        modelMemory.modelPointerCapacityBytes;
    const auto shadowStore =
        shadow::ShadowRuntimeContractCache::instance()
            .snapshotResourcesShared();
    if (shadowStore != nullptr) {
      const auto shadowMemory = shadowStore->memorySnapshot();
      snapshot.modelCache.shadowStoreRecords = shadowMemory.records;
      snapshot.modelCache.shadowStoreRecordVectorCapacityBytes =
          shadowMemory.recordVectorCapacityBytes;
      snapshot.modelCache.shadowStorePayloadCapacityBytes =
          shadowMemory.payloadCapacityBytes;
      snapshot.modelCache.shadowStoreHashContainerOverheadIncluded =
          shadowMemory.hashContainerOverheadIncluded;
    }
  } else {
    return snapshot;
  }

  {
    std::lock_guard lock(g_gpuSkinPoolMutex);
    snapshot.gpuSkinPools = g_gpuSkinPoolSnapshot;
  }

  D3D9MemoryAllocatorDiagnosticSnapshot allocatorBegin;
  bool allocatorBeginAvailable = false;
  if (allocator != nullptr) {
    try {
      allocatorBegin = allocator->CaptureDiagnosticSnapshot();
      snapshot.d3d9HostAllocator =
          MakeHostAllocatorSnapshot(allocatorBegin);
      allocatorBeginAvailable = true;
    } catch (...) {
      // 诊断快照分配失败必须 fail closed，不能影响正常渲染或资源生命周期。
    }
  }

  std::vector<ResourceHandle> live;
  {
    std::lock_guard lock(g_registryMutex);
    size_t write = 0u;
    for (size_t read = 0u; read < g_records.size(); ++read) {
      ResourceHandle record = g_records[read].lock();
      if (record == nullptr)
        continue;
      live.push_back(record);
      g_records[write++] = record;
    }
    g_records.resize(write);
  }

  snapshot.liveResources = live.size();
  snapshot.largestHostBackings.reserve(
      std::min<size_t>(live.size(), size_t(kTopEntryLimit)));
  std::vector<PendingHostBinding> pendingHostBindings;
  pendingHostBindings.reserve(live.size());
  auto& bindingCoverage =
      snapshot.d3d9HostAllocator.chunkDiagnostics.bindingCoverage;
  for (const auto& record : live) {
    const auto& info = record->registration;
    const CoherentHostBacking host = CopyHostBacking(*record);
    const uint64_t hostBytes = host.hostBackingLogicalBytes;
    const uint64_t mappedBytes = host.hostMappedLogicalBytes;
    const uint64_t duplicateBytes =
        host.duplicateHostBackingLogicalBytes;
    const uint64_t lockCalls = record->lockCalls.load(
        std::memory_order_relaxed);
    const uint64_t activeLocks = record->activeLocks.load(
        std::memory_order_relaxed);
    const uint64_t readLocks = record->readLocks.load(
        std::memory_order_relaxed);
    const uint64_t writeLocks = record->writeLocks.load(
        std::memory_order_relaxed);
    const uint64_t fullWrites = record->fullSubresourceWriteLocks.load(
        std::memory_order_relaxed);
    const uint64_t partialWrites =
        record->partialSubresourceWriteLocks.load(
            std::memory_order_relaxed);
    const uint64_t externalDirtyCalls =
        record->externalDirtyCalls.load(std::memory_order_relaxed);
    const CandidateObservationClass candidate =
        ClassifyCandidate(
            *record, snapshot.frameSerial, duplicateBytes);
    const HostBackingBinding& hostBinding = host.binding;

    if (hostBytes != 0u) {
      ++bindingCoverage.hostBackingResources;
      bindingCoverage.hostBackingLogicalBytes += hostBytes;
      switch (hostBinding.bindingClass) {
        case HostBackingBindingClass::ExactD3D9Memory:
          ++bindingCoverage.exactD3D9MemoryBindingResources;
          bindingCoverage.exactD3D9MemoryBindingLogicalBytes += hostBytes;
          ++bindingCoverage.exactBindings;
          pendingHostBindings.push_back(
              {record->id, hostBinding, candidate});
          break;
        case HostBackingBindingClass::ExternalHostAllocation:
          ++bindingCoverage.externalHostBackingResources;
          bindingCoverage.externalHostBackingLogicalBytes += hostBytes;
          break;
        case HostBackingBindingClass::UnresolvedD3D9Memory:
          ++bindingCoverage.unresolvedD3D9MemoryResources;
          bindingCoverage.unresolvedD3D9MemoryLogicalBytes += hostBytes;
          break;
        case HostBackingBindingClass::None:
        case HostBackingBindingClass::Unregistered:
          ++bindingCoverage.unregisteredHostBackingResources;
          bindingCoverage.unregisteredHostBackingLogicalBytes += hostBytes;
          break;
      }
    }

    snapshot.logicalBytes += info.logicalBytes;
    snapshot.deviceAllocationBytes += info.deviceAllocationBytes;
    snapshot.hostBackingLogicalBytes += hostBytes;
    snapshot.hostMappedLogicalBytes += mappedBytes;
    snapshot.duplicateHostBackingLogicalBytes += duplicateBytes;
    if (candidate == CandidateObservationClass::QuiescentDefaultWriteOnly ||
        candidate == CandidateObservationClass::ShareableGpuSkinMirror) {
      snapshot.observedCandidateHostBytes += duplicateBytes;
    } else if (candidate ==
               CandidateObservationClass::QuiescentLazyReadbackRequired) {
      snapshot.lazyReadbackCandidateHostBytes += duplicateBytes;
    }

    const BucketKey key = {info.resourceClass, info.pool, info.mapMode,
                           info.dynamic, info.writeOnly};
    auto bucket = std::find_if(snapshot.buckets.begin(),
        snapshot.buckets.end(), [&](const ResourceResidencyBucket& value) {
          return SameKey(value, key);
        });
    if (bucket == snapshot.buckets.end()) {
      snapshot.buckets.push_back({});
      bucket = std::prev(snapshot.buckets.end());
      bucket->resourceClass = key.resourceClass;
      bucket->pool = key.pool;
      bucket->mapMode = key.mapMode;
      bucket->dynamic = key.dynamic;
      bucket->writeOnly = key.writeOnly;
    }
    ++bucket->resources;
    bucket->logicalBytes += info.logicalBytes;
    bucket->deviceAllocationBytes += info.deviceAllocationBytes;
    bucket->hostBackingLogicalBytes += hostBytes;
    bucket->hostMappedLogicalBytes += mappedBytes;
    bucket->duplicateHostBackingLogicalBytes += duplicateBytes;
    bucket->lockCalls += lockCalls;
    bucket->activeLocks += activeLocks;
    bucket->readLocks += readLocks;
    bucket->writeLocks += writeLocks;
    bucket->requestedDiscardLocks +=
        record->requestedDiscardLocks.load(std::memory_order_relaxed);
    bucket->effectiveDiscardLocks +=
        record->effectiveDiscardLocks.load(std::memory_order_relaxed);
    bucket->requestedNoOverwriteLocks +=
        record->requestedNoOverwriteLocks.load(std::memory_order_relaxed);
    bucket->effectiveNoOverwriteLocks +=
        record->effectiveNoOverwriteLocks.load(std::memory_order_relaxed);
    bucket->fullSubresourceWriteLocks += fullWrites;
    bucket->partialSubresourceWriteLocks += partialWrites;
    bucket->externalDirtyCalls += externalDirtyCalls;
    if (lockCalls == 0u) {
      ++bucket->neverLockedResources;
      bucket->neverLockedDuplicateHostBytes += duplicateBytes;
    }
    if (candidate == CandidateObservationClass::QuiescentDefaultWriteOnly ||
        candidate == CandidateObservationClass::ShareableGpuSkinMirror) {
      ++bucket->observedCandidateResources;
      bucket->observedCandidateHostBytes += duplicateBytes;
    } else if (candidate ==
               CandidateObservationClass::QuiescentLazyReadbackRequired) {
      ++bucket->lazyReadbackCandidateResources;
      bucket->lazyReadbackCandidateHostBytes += duplicateBytes;
    }

    ResourceResidencyTopEntry top;
    top.id = record->id;
    top.resourceClass = info.resourceClass;
    top.candidateClass = candidate;
    top.pool = info.pool;
    top.usage = info.usage;
    top.mapMode = info.mapMode;
    top.subresourceCount = info.subresourceCount;
    top.logicalBytes = info.logicalBytes;
    top.deviceAllocationBytes = info.deviceAllocationBytes;
    top.hostBackingLogicalBytes = hostBytes;
    top.hostMappedLogicalBytes = mappedBytes;
    top.duplicateHostBackingLogicalBytes = duplicateBytes;
    top.lockCalls = lockCalls;
    top.activeLocks = activeLocks;
    top.readLocks = readLocks;
    top.writeLocks = writeLocks;
    top.fullSubresourceWriteLocks = fullWrites;
    top.partialSubresourceWriteLocks = partialWrites;
    top.externalDirtyCalls = externalDirtyCalls;
    top.lastLockFrame = record->lastLockFrame.load(
        std::memory_order_relaxed);
    top.lastWriteFrame = record->lastWriteFrame.load(
        std::memory_order_relaxed);
    top.lastUploadFrame = record->lastUploadFrame.load(
        std::memory_order_relaxed);
    top.hostBinding = hostBinding;
    snapshot.largestHostBackings.push_back(top);
    std::sort(snapshot.largestHostBackings.begin(),
        snapshot.largestHostBackings.end(), [](const auto& lhs,
                                               const auto& rhs) {
        if (lhs.duplicateHostBackingLogicalBytes !=
            rhs.duplicateHostBackingLogicalBytes) {
          return lhs.duplicateHostBackingLogicalBytes >
                 rhs.duplicateHostBackingLogicalBytes;
        }
        if (lhs.hostBackingLogicalBytes != rhs.hostBackingLogicalBytes)
          return lhs.hostBackingLogicalBytes > rhs.hostBackingLogicalBytes;
        return lhs.id < rhs.id;
      });
    if (snapshot.largestHostBackings.size() > kTopEntryLimit)
      snapshot.largestHostBackings.resize(kTopEntryLimit);
  }

  auto& chunkDiagnostics =
      snapshot.d3d9HostAllocator.chunkDiagnostics;
  if (allocatorBeginAvailable) {
    try {
      const D3D9MemoryAllocatorDiagnosticSnapshot allocatorEnd =
          allocator->CaptureDiagnosticSnapshot();
      chunkDiagnostics.mutationGenerationEnd =
          allocatorEnd.mutationGeneration;
      chunkDiagnostics.mutationGenerationSaturated =
          allocatorBegin.mutationGenerationSaturated ||
          allocatorEnd.mutationGenerationSaturated;
      // 代际相同仍不够：结构、映射与故障累计字段也必须逐项相同，
      // 否则候选 chunk 的回收上限一律不得发布。
      chunkDiagnostics.generationStable =
          !chunkDiagnostics.mutationGenerationSaturated &&
          SameAllocatorSnapshot(allocatorBegin, allocatorEnd);
    } catch (...) {
      chunkDiagnostics.generationStable = false;
      chunkDiagnostics.mutationGenerationEnd = 0u;
    }
  }

  std::map<uint64_t, size_t> chunkIndices;
  std::vector<std::vector<BindingInterval>> intervalsByChunk(
      chunkDiagnostics.chunks.size());
  for (size_t i = 0u; i < chunkDiagnostics.chunks.size(); ++i) {
    const uint64_t chunkId = chunkDiagnostics.chunks[i].chunkId;
    if (chunkId == 0u || !chunkIndices.emplace(chunkId, i).second)
      ++chunkDiagnostics.duplicateAllocatorChunkIdCount;
  }

  for (const PendingHostBinding& pending : pendingHostBindings) {
    const auto& binding = pending.binding;
    if (binding.chunkId == 0u || binding.alignedSliceBytes == 0u) {
      ++bindingCoverage.invalidBindingCount;
      bindingCoverage.unclassifiedAlignedSliceBytes +=
          binding.alignedSliceBytes;
      continue;
    }
    const auto chunk = chunkIndices.find(binding.chunkId);
    if (chunk == chunkIndices.end()) {
      ++bindingCoverage.missingChunkBindingCount;
      bindingCoverage.unclassifiedAlignedSliceBytes +=
          binding.alignedSliceBytes;
      continue;
    }
    auto& chunkValue = chunkDiagnostics.chunks[chunk->second];
    if (binding.offset >
            std::numeric_limits<uint64_t>::max() -
                binding.alignedSliceBytes ||
        binding.offset + binding.alignedSliceBytes >
            chunkValue.reserveBytes) {
      ++bindingCoverage.outOfBoundsBindingCount;
      bindingCoverage.unclassifiedAlignedSliceBytes +=
          binding.alignedSliceBytes;
      chunkValue.unclassifiedAlignedSliceBytes +=
          binding.alignedSliceBytes;
      continue;
    }
    intervalsByChunk[chunk->second].push_back({
        pending.resourceId,
        binding.offset,
        binding.alignedSliceBytes,
        binding.mapped,
        pending.candidate,
    });
  }

  for (size_t i = 0u; i < intervalsByChunk.size(); ++i) {
    auto& intervals = intervalsByChunk[i];
    std::sort(intervals.begin(), intervals.end(),
        [](const BindingInterval& lhs, const BindingInterval& rhs) {
          if (lhs.offset != rhs.offset)
            return lhs.offset < rhs.offset;
          if (lhs.size != rhs.size)
            return lhs.size < rhs.size;
          return lhs.resourceId < rhs.resourceId;
        });
    auto& chunk = chunkDiagnostics.chunks[i];
    uint64_t acceptedEnd = 0u;
    uint64_t acceptedOffset = 0u;
    uint64_t acceptedSize = 0u;
    bool hasAccepted = false;
    for (const BindingInterval& interval : intervals) {
      const bool duplicate = hasAccepted &&
          interval.offset == acceptedOffset &&
          interval.size == acceptedSize;
      const bool overlap = hasAccepted && interval.offset < acceptedEnd;
      if (duplicate || overlap) {
        if (duplicate)
          ++bindingCoverage.duplicateBindingCount;
        else
          ++bindingCoverage.overlapBindingCount;
        bindingCoverage.unclassifiedAlignedSliceBytes += interval.size;
        chunk.unclassifiedAlignedSliceBytes += interval.size;
        continue;
      }

      acceptedOffset = interval.offset;
      acceptedSize = interval.size;
      acceptedEnd = interval.offset + interval.size;
      hasAccepted = true;
      ++chunk.boundResources;
      chunk.boundLiveAlignedSliceBytes += interval.size;
      bindingCoverage.boundAlignedSliceBytes += interval.size;
      if (interval.mapped) {
        chunk.mappedBindingAlignedSliceBytes += interval.size;
        bindingCoverage.mappedBindingAlignedSliceBytes += interval.size;
      }
      if (interval.candidate == CandidateObservationClass::None) {
        chunk.nonCandidateAlignedSliceBytes += interval.size;
      } else {
        chunk.candidateAlignedSliceBytes += interval.size;
        if (interval.candidate ==
            CandidateObservationClass::QuiescentLazyReadbackRequired) {
          chunk.lazyReadbackCandidateAlignedSliceBytes += interval.size;
        } else {
          chunk.directCandidateAlignedSliceBytes += interval.size;
        }
      }
    }
  }

  if (chunkDiagnostics.allocatorUsedPayloadBytes >=
      bindingCoverage.boundAlignedSliceBytes) {
    bindingCoverage.unregisteredAllocatorPayloadBytes =
        chunkDiagnostics.allocatorUsedPayloadBytes -
        bindingCoverage.boundAlignedSliceBytes;
  } else {
    bindingCoverage.boundBytesExceedAllocatorUsed = true;
  }
  bindingCoverage.bindingBytesClosure =
      chunkDiagnostics.available && chunkDiagnostics.chunkBacked &&
      chunkDiagnostics.accountingClosure &&
      chunkDiagnostics.generationStable &&
      bindingCoverage.unresolvedD3D9MemoryResources == 0u &&
      bindingCoverage.missingChunkBindingCount == 0u &&
      bindingCoverage.invalidBindingCount == 0u &&
      bindingCoverage.outOfBoundsBindingCount == 0u &&
      bindingCoverage.duplicateBindingCount == 0u &&
      bindingCoverage.overlapBindingCount == 0u &&
      !bindingCoverage.boundBytesExceedAllocatorUsed &&
      bindingCoverage.unregisteredAllocatorPayloadBytes == 0u &&
      bindingCoverage.boundAlignedSliceBytes ==
          chunkDiagnostics.allocatorUsedPayloadBytes;

  const bool cleanCandidateUpperBound =
      bindingCoverage.bindingBytesClosure &&
      chunkDiagnostics.duplicateAllocatorChunkIdCount == 0u &&
      !chunkDiagnostics.mutationGenerationSaturated &&
      chunkDiagnostics.mapFailureCount == 0u &&
      chunkDiagnostics.unmapFailureCount == 0u &&
      chunkDiagnostics.mappingStateFaultCount == 0u;
  for (auto& chunk : chunkDiagnostics.chunks) {
    chunk.observedCandidateOnly = cleanCandidateUpperBound &&
        chunk.boundLiveAlignedSliceBytes != 0u &&
        chunk.candidateAlignedSliceBytes ==
            chunk.boundLiveAlignedSliceBytes &&
        chunk.nonCandidateAlignedSliceBytes == 0u &&
        chunk.unclassifiedAlignedSliceBytes == 0u;
    if (chunk.observedCandidateOnly) {
      ++chunkDiagnostics.candidateOnlyChunkCount;
      chunk.reserveReclaimUpperBoundBytes = chunk.reserveBytes;
      chunk.mappedReclaimUpperBoundBytes =
          chunk.sharedMappedBytes + chunk.standaloneMappedBytes;
      chunkDiagnostics.candidateOnlyReserveBytesUpperBound +=
          chunk.reserveReclaimUpperBoundBytes;
      chunkDiagnostics.candidateOnlyMappedBytesUpperBound +=
          chunk.mappedReclaimUpperBoundBytes;
    }
  }
  std::sort(chunkDiagnostics.chunks.begin(),
      chunkDiagnostics.chunks.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.chunkId < rhs.chunkId;
      });

  std::sort(snapshot.buckets.begin(), snapshot.buckets.end(),
      [](const auto& lhs, const auto& rhs) {
        if (lhs.resourceClass != rhs.resourceClass)
          return lhs.resourceClass < rhs.resourceClass;
        if (lhs.pool != rhs.pool)
          return lhs.pool < rhs.pool;
        if (lhs.mapMode != rhs.mapMode)
          return lhs.mapMode < rhs.mapMode;
        if (lhs.dynamic != rhs.dynamic)
          return lhs.dynamic < rhs.dynamic;
        return lhs.writeOnly < rhs.writeOnly;
      });
  return snapshot;
}

const char* ResourceClassName(ResourceClass value) noexcept {
  switch (value) {
    case ResourceClass::VertexBuffer: return "vertexBuffer";
    case ResourceClass::IndexBuffer: return "indexBuffer";
    case ResourceClass::Surface: return "surface";
    case ResourceClass::Texture2D: return "texture2D";
    case ResourceClass::CubeTexture: return "cubeTexture";
    case ResourceClass::VolumeTexture: return "volumeTexture";
    case ResourceClass::GpuSkinStaticMirror: return "gpuSkinStaticMirror";
    case ResourceClass::GpuSkinStaticAtlas: return "gpuSkinStaticAtlas";
    case ResourceClass::GpuSkinUploadPage: return "gpuSkinUploadPage";
    case ResourceClass::GpuSkinOutputPage: return "gpuSkinOutputPage";
    case ResourceClass::GpuSkinStaticUploadStaging:
      return "gpuSkinStaticUploadStaging";
  }
  return "unknown";
}

const char* CandidateObservationClassName(
    CandidateObservationClass value) noexcept {
  switch (value) {
    case CandidateObservationClass::None: return "none";
    case CandidateObservationClass::QuiescentDefaultWriteOnly:
      return "quiescentDefaultWriteOnly";
    case CandidateObservationClass::QuiescentLazyReadbackRequired:
      return "quiescentLazyReadbackRequired";
    case CandidateObservationClass::ShareableGpuSkinMirror:
      return "shareableGpuSkinMirror";
  }
  return "unknown";
}

const char* HostBackingBindingClassName(
    HostBackingBindingClass value) noexcept {
  switch (value) {
    case HostBackingBindingClass::None: return "none";
    case HostBackingBindingClass::ExactD3D9Memory:
      return "exactD3D9Memory";
    case HostBackingBindingClass::ExternalHostAllocation:
      return "externalHostAllocation";
    case HostBackingBindingClass::UnresolvedD3D9Memory:
      return "unresolvedD3D9Memory";
    case HostBackingBindingClass::Unregistered: return "unregistered";
  }
  return "unknown";
}

} // namespace dxvk::war3::resource_census
