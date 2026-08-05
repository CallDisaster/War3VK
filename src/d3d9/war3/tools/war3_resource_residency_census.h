#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace dxvk {
class D3D9MemoryAllocator;
}

namespace dxvk::war3::resource_census {

// 本模块仅用于诊断。任何分类都不能授权驱逐；它们只标记观察结果，后续的
// Seal-and-Evict 契约必须独立证明这些结果后才能采取行动。
enum class ResourceClass : uint32_t {
  VertexBuffer = 0,
  IndexBuffer = 1,
  Surface = 2,
  Texture2D = 3,
  CubeTexture = 4,
  VolumeTexture = 5,
  GpuSkinStaticMirror = 6,
  GpuSkinStaticAtlas = 7,
  GpuSkinUploadPage = 8,
  GpuSkinOutputPage = 9,
  GpuSkinStaticUploadStaging = 10,
};

enum class CandidateObservationClass : uint32_t {
  None = 0,
  QuiescentDefaultWriteOnly = 1,
  QuiescentLazyReadbackRequired = 2,
  ShareableGpuSkinMirror = 3,
};

// host backing 的来源必须显式分类；只有 ExactD3D9Memory 才能参与 chunk 闭合。
enum class HostBackingBindingClass : uint32_t {
  None = 0,
  ExactD3D9Memory = 1,
  ExternalHostAllocation = 2,
  UnresolvedD3D9Memory = 3,
  Unregistered = 4,
};

struct HostBackingBinding {
  HostBackingBindingClass bindingClass =
      HostBackingBindingClass::None;
  uint64_t chunkId = 0;
  uint64_t offset = 0;
  uint64_t alignedSliceBytes = 0;
  bool mapped = false;
};

struct ResourceRegistration {
  ResourceClass resourceClass = ResourceClass::VertexBuffer;
  uint32_t pool = 0;
  uint32_t usage = 0;
  uint32_t mapMode = 0;
  uint32_t subresourceCount = 1;
  uint64_t logicalBytes = 0;
  // 分配切片的大小，不代表独占的物理显存或内存大小。
  uint64_t deviceAllocationBytes = 0;
  // 主机端载荷的逻辑大小。文件映射可能已经预留或提交后备内存，但未在
  // 32 位进程地址空间中保留持久视图。
  uint64_t hostBackingLogicalBytes = 0;
  uint64_t hostMappedLogicalBytes = 0;
  // 在设备图像或缓冲区副本之外额外存在的主机端载荷。
  uint64_t duplicateHostBackingLogicalBytes = 0;
  bool dynamic = false;
  bool writeOnly = false;
  bool hasDeviceCopy = false;
  bool deviceReadbackCapable = false;
  HostBackingBinding hostBinding;
};

struct ResourceLockEvent {
  uint64_t byteOffset = 0;
  uint64_t byteLength = 0;
  uint32_t requestedFlags = 0;
  uint32_t effectiveFlags = 0;
  uint32_t subresource = 0;
  bool readIntent = false;
  bool writeIntent = false;
  bool fullSubresource = false;
};

struct ResourceResidencyBucket {
  ResourceClass resourceClass = ResourceClass::VertexBuffer;
  uint32_t pool = 0;
  uint32_t mapMode = 0;
  bool dynamic = false;
  bool writeOnly = false;
  uint64_t resources = 0;
  uint64_t logicalBytes = 0;
  uint64_t deviceAllocationBytes = 0;
  uint64_t hostBackingLogicalBytes = 0;
  uint64_t hostMappedLogicalBytes = 0;
  uint64_t duplicateHostBackingLogicalBytes = 0;
  uint64_t lockCalls = 0;
  uint64_t activeLocks = 0;
  uint64_t readLocks = 0;
  uint64_t writeLocks = 0;
  uint64_t requestedDiscardLocks = 0;
  uint64_t effectiveDiscardLocks = 0;
  uint64_t requestedNoOverwriteLocks = 0;
  uint64_t effectiveNoOverwriteLocks = 0;
  uint64_t fullSubresourceWriteLocks = 0;
  uint64_t partialSubresourceWriteLocks = 0;
  uint64_t externalDirtyCalls = 0;
  uint64_t neverLockedResources = 0;
  uint64_t neverLockedDuplicateHostBytes = 0;
  uint64_t observedCandidateResources = 0;
  uint64_t observedCandidateHostBytes = 0;
  uint64_t lazyReadbackCandidateResources = 0;
  uint64_t lazyReadbackCandidateHostBytes = 0;
};

struct ResourceResidencyTopEntry {
  uint64_t id = 0;
  ResourceClass resourceClass = ResourceClass::VertexBuffer;
  CandidateObservationClass candidateClass =
      CandidateObservationClass::None;
  uint32_t pool = 0;
  uint32_t usage = 0;
  uint32_t mapMode = 0;
  uint32_t subresourceCount = 0;
  uint64_t logicalBytes = 0;
  uint64_t deviceAllocationBytes = 0;
  uint64_t hostBackingLogicalBytes = 0;
  uint64_t hostMappedLogicalBytes = 0;
  uint64_t duplicateHostBackingLogicalBytes = 0;
  uint64_t lockCalls = 0;
  uint64_t activeLocks = 0;
  uint64_t readLocks = 0;
  uint64_t writeLocks = 0;
  uint64_t fullSubresourceWriteLocks = 0;
  uint64_t partialSubresourceWriteLocks = 0;
  uint64_t externalDirtyCalls = 0;
  uint64_t lastLockFrame = 0;
  uint64_t lastWriteFrame = 0;
  uint64_t lastUploadFrame = 0;
  HostBackingBinding hostBinding;
};

struct ModelCacheResidencySnapshot {
  uint64_t uniqueGeosetRecords = 0;
  uint64_t residentGeosetRecords = 0;
  uint64_t uniqueGeosetCapacityBytes = 0;
  uint64_t residentGeosetCapacityBytes = 0;
  uint64_t aliasDuplicateCapacityBytes = 0;
  uint64_t positionsCapacityBytes = 0;
  uint64_t normalsCapacityBytes = 0;
  uint64_t groupSlotsCapacityBytes = 0;
  uint64_t uvCapacityBytes = 0;
  uint64_t primitiveCapacityBytes = 0;
  uint64_t indicesCapacityBytes = 0;
  uint64_t matrixGroupsCapacityBytes = 0;
  uint64_t matrixIndicesCapacityBytes = 0;
  uint64_t modelPointerCapacityBytes = 0;
  uint64_t hashContainerOverheadIncluded = 0;
  uint64_t shadowStoreRecords = 0;
  uint64_t shadowStoreRecordVectorCapacityBytes = 0;
  uint64_t shadowStorePayloadCapacityBytes = 0;
  uint64_t shadowStoreHashContainerOverheadIncluded = 0;
};

struct D3D9HostAllocatorSnapshot {
  uint64_t allocatedBackingBytes = 0;
  uint64_t usedPayloadBytes = 0;
  uint64_t mappedAddressBytes = 0;
  struct BindingCoverage {
    uint64_t hostBackingResources = 0;
    uint64_t hostBackingLogicalBytes = 0;
    uint64_t exactD3D9MemoryBindingResources = 0;
    uint64_t exactD3D9MemoryBindingLogicalBytes = 0;
    uint64_t exactBindings = 0;
    uint64_t boundAlignedSliceBytes = 0;
    uint64_t mappedBindingAlignedSliceBytes = 0;
    uint64_t externalHostBackingResources = 0;
    uint64_t externalHostBackingLogicalBytes = 0;
    uint64_t unresolvedD3D9MemoryResources = 0;
    uint64_t unresolvedD3D9MemoryLogicalBytes = 0;
    uint64_t unregisteredHostBackingResources = 0;
    uint64_t unregisteredHostBackingLogicalBytes = 0;
    uint64_t missingChunkBindingCount = 0;
    uint64_t invalidBindingCount = 0;
    uint64_t outOfBoundsBindingCount = 0;
    uint64_t duplicateBindingCount = 0;
    uint64_t overlapBindingCount = 0;
    uint64_t unclassifiedAlignedSliceBytes = 0;
    uint64_t unregisteredAllocatorPayloadBytes = 0;
    bool boundBytesExceedAllocatorUsed = false;
    bool bindingBytesClosure = false;
  };

  struct Chunk {
    uint64_t chunkId = 0;
    uint64_t reserveBytes = 0;
    uint64_t chunkOccupiedBytes = 0;
    uint64_t freePayloadBytes = 0;
    uint64_t freeRangeCount = 0;
    uint64_t sharedMappedRefs = 0;
    uint64_t sharedMappedBytes = 0;
    uint64_t standaloneMappedRefs = 0;
    uint64_t standaloneMappedBytes = 0;
    uint64_t mapFailureCount = 0;
    uint64_t unmapFailureCount = 0;
    uint64_t mappingStateFaultCount = 0;
    uint64_t boundResources = 0;
    uint64_t boundLiveAlignedSliceBytes = 0;
    uint64_t candidateAlignedSliceBytes = 0;
    uint64_t directCandidateAlignedSliceBytes = 0;
    uint64_t lazyReadbackCandidateAlignedSliceBytes = 0;
    uint64_t nonCandidateAlignedSliceBytes = 0;
    uint64_t unclassifiedAlignedSliceBytes = 0;
    uint64_t mappedBindingAlignedSliceBytes = 0;
    bool observedCandidateOnly = false;
    uint64_t reserveReclaimUpperBoundBytes = 0;
    uint64_t mappedReclaimUpperBoundBytes = 0;
  };

  struct ChunkDiagnostics {
    bool available = false;
    bool authority = false;
    bool chunkBacked = false;
    bool accountingClosure = false;
    bool mutationGenerationSaturated = false;
    bool generationStable = false;
    uint64_t mutationGeneration = 0;
    uint64_t mutationGenerationBegin = 0;
    uint64_t mutationGenerationEnd = 0;
    uint64_t reserveBytes = 0;
    uint64_t allocatorUsedPayloadBytes = 0;
    uint64_t chunkOccupiedBytes = 0;
    uint64_t internalFragmentationBytes = 0;
    uint64_t freePayloadBytes = 0;
    uint64_t sharedMappedRefs = 0;
    uint64_t sharedMappedBytes = 0;
    uint64_t standaloneMappedRefs = 0;
    uint64_t standaloneMappedBytes = 0;
    uint64_t mappedRefs = 0;
    uint64_t mappedBytes = 0;
    uint64_t mapFailureCount = 0;
    uint64_t unmapFailureCount = 0;
    uint64_t mappingStateFaultCount = 0;
    uint64_t duplicateAllocatorChunkIdCount = 0;
    uint64_t candidateOnlyChunkCount = 0;
    uint64_t candidateOnlyReserveBytesUpperBound = 0;
    uint64_t candidateOnlyMappedBytesUpperBound = 0;
    BindingCoverage bindingCoverage;
    std::vector<Chunk> chunks;
  } chunkDiagnostics;
};

struct GpuSkinPoolResidencySnapshot {
  uint64_t staticAtlasReservedBytes = 0;
  uint64_t staticAtlasUsedBytes = 0;
  uint64_t staticResourceRecords = 0;
  uint64_t staticReadyRecords = 0;
  uint64_t staticPendingRecords = 0;
  uint64_t staticSubmittedRecords = 0;
  uint64_t staticInvalidRecords = 0;
  uint64_t queuedStaticMissRecords = 0;
  uint64_t queuedStaticMissHostBytes = 0;
  uint64_t peakQueuedStaticMissRecords = 0;
  uint64_t peakQueuedStaticMissHostBytes = 0;
  uint64_t readyStaticUploadCount = 0;
  uint64_t readyStaticUploadBytes = 0;
  uint64_t retiredStaticUploadCount = 0;
  uint64_t retiredStaticUploadBytes = 0;
  uint64_t uploadResidentBytes = 0;
  uint64_t uploadActivePages = 0;
  uint64_t uploadActiveCapacityBytes = 0;
  uint64_t uploadActiveUsedBytes = 0;
  uint64_t uploadPendingPages = 0;
  uint64_t uploadPendingCapacityBytes = 0;
  uint64_t uploadPendingUsedBytes = 0;
  uint64_t uploadRetiredPages = 0;
  uint64_t uploadRetiredCapacityBytes = 0;
  uint64_t uploadRetiredUsedBytes = 0;
  uint64_t uploadIdlePages = 0;
  uint64_t uploadIdleCapacityBytes = 0;
  uint64_t outputResidentBytes = 0;
  uint64_t outputPages = 0;
  uint64_t outputCapacityBytes = 0;
  uint64_t outputCursorBytes = 0;
  uint64_t outputOutstandingSlices = 0;
  uint64_t outputActiveLeases = 0;
  uint64_t outputRetiredLeases = 0;
};

struct ResourceResidencySnapshot {
  bool enabled = false;
  uint64_t frameSerial = 0;
  uint64_t lifetimeRegistrations = 0;
  uint64_t liveResources = 0;
  uint64_t logicalBytes = 0;
  uint64_t deviceAllocationBytes = 0;
  uint64_t hostBackingLogicalBytes = 0;
  uint64_t hostMappedLogicalBytes = 0;
  uint64_t duplicateHostBackingLogicalBytes = 0;
  uint64_t observedCandidateHostBytes = 0;
  uint64_t lazyReadbackCandidateHostBytes = 0;
  D3D9HostAllocatorSnapshot d3d9HostAllocator;
  GpuSkinPoolResidencySnapshot gpuSkinPools;
  std::vector<ResourceResidencyBucket> buckets;
  std::vector<ResourceResidencyTopEntry> largestHostBackings;
  ModelCacheResidencySnapshot modelCache;
};

struct ResourceRecord;
using ResourceHandle = std::shared_ptr<ResourceRecord>;

bool Enabled() noexcept;
ResourceHandle Register(const ResourceRegistration& registration) noexcept;
void UpdateHostBacking(const ResourceHandle& handle,
                       uint64_t hostBackingLogicalBytes,
                       uint64_t hostMappedLogicalBytes,
                       uint64_t duplicateHostBackingLogicalBytes,
                       const HostBackingBinding& hostBinding = {}) noexcept;
void NoteLock(const ResourceHandle& handle,
              const ResourceLockEvent& event) noexcept;
void NoteUnlock(const ResourceHandle& handle) noexcept;
void NoteExternalDirty(const ResourceHandle& handle) noexcept;
void NoteDeviceUpload(const ResourceHandle& handle,
                      uint32_t subresource) noexcept;
void SetFrameSerial(uint64_t frameSerial) noexcept;
void UpdateGpuSkinPoolSnapshot(
    const GpuSkinPoolResidencySnapshot& snapshot) noexcept;
ResourceResidencySnapshot CaptureSnapshot(
    D3D9MemoryAllocator* allocator);

const char* ResourceClassName(ResourceClass value) noexcept;
const char* CandidateObservationClassName(
    CandidateObservationClass value) noexcept;
const char* HostBackingBindingClassName(
    HostBackingBindingClass value) noexcept;

} // namespace dxvk::war3::resource_census
