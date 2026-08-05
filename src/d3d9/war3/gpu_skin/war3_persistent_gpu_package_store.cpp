#include "war3_persistent_gpu_package_store.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace dxvk::war3::gpu_skin {

namespace {

constexpr VkDeviceSize kMinimumStorageBufferOffsetAlignment = 16u;

std::atomic<uint64_t> g_nextStoreInstanceAuthority{1u};

uint64_t AllocateStoreInstanceAuthority() noexcept {
  uint64_t current = g_nextStoreInstanceAuthority.load(
      std::memory_order_relaxed);
  while (current != 0u &&
         current != std::numeric_limits<uint64_t>::max()) {
    if (g_nextStoreInstanceAuthority.compare_exchange_weak(
            current, current + 1u, std::memory_order_relaxed,
            std::memory_order_relaxed))
      return current;
  }
  return 0u;
}

bool TryAlignUp(VkDeviceSize value, VkDeviceSize alignment,
                VkDeviceSize& result) {
  if (alignment == 0u)
    return false;

  const VkDeviceSize remainder = value % alignment;
  if (remainder == 0u) {
    result = value;
    return true;
  }

  const VkDeviceSize padding = alignment - remainder;
  if (value > std::numeric_limits<VkDeviceSize>::max() - padding)
    return false;

  result = value + padding;
  return true;
}

resource_census::ResourceHandle RegisterStaticMirror(
    VkDeviceSize deviceBytes, bool hasDeviceCopy) {
  resource_census::ResourceRegistration census = {};
  census.resourceClass =
      resource_census::ResourceClass::GpuSkinStaticMirror;
  census.pool = 0xFFFFFFFFu;
  census.logicalBytes = deviceBytes;
  // ShadowModelResourceCache already owns and counts the immutable CPU data.
  // This package retains shared ownership without counting it a second time.
  census.hostBackingLogicalBytes = 0u;
  census.hostMappedLogicalBytes = 0u;
  census.duplicateHostBackingLogicalBytes = 0u;
  census.writeOnly = true;
  census.hasDeviceCopy = hasDeviceCopy;
  return resource_census::Register(census);
}

resource_census::ResourceHandle RegisterGpuBuffer(
    resource_census::ResourceClass resourceClass,
    const Rc<DxvkBuffer>& buffer, VkDeviceSize logicalBytes,
    bool hostMapped, bool duplicateHostBacking) {
  if (buffer == nullptr)
    return {};
  const Rc<DxvkResourceAllocation> storage = buffer->storage();
  const uint64_t allocationBytes = storage != nullptr
      ? uint64_t(storage->getMemoryInfo().size)
      : uint64_t(logicalBytes);
  resource_census::ResourceRegistration census = {};
  census.resourceClass = resourceClass;
  census.pool = 0xFFFFFFFFu;
  census.logicalBytes = logicalBytes;
  census.deviceAllocationBytes = allocationBytes;
  census.hostBackingLogicalBytes = hostMapped ? allocationBytes : 0u;
  census.hostMappedLogicalBytes = hostMapped ? allocationBytes : 0u;
  census.duplicateHostBackingLogicalBytes =
      hostMapped && duplicateHostBacking ? allocationBytes : 0u;
  census.hasDeviceCopy = true;
  return resource_census::Register(census);
}

DxvkBufferCreateInfo StaticBufferInfo(VkDeviceSize size, const char* name) {
  DxvkBufferCreateInfo info = {};
  info.size = size;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT;
  info.access = VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                VK_ACCESS_INDEX_READ_BIT |
                VK_ACCESS_TRANSFER_WRITE_BIT;
  info.debugName = name;
  return info;
}

DxvkBufferCreateInfo StaticUploadBufferInfo(
    VkDeviceSize size, const char* name) {
  DxvkBufferCreateInfo info = {};
  info.size = size;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  info.stages = VK_PIPELINE_STAGE_HOST_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  info.access = VK_ACCESS_HOST_WRITE_BIT |
                VK_ACCESS_TRANSFER_READ_BIT |
                VK_ACCESS_SHADER_READ_BIT;
  info.debugName = name;
  return info;
}

bool SameStaticLayout(
    const GpuSkinStaticSourceLayout& lhs,
    const GpuSkinStaticSourceLayout& rhs) noexcept {
  return lhs.positionOffset == rhs.positionOffset &&
      lhs.normalOffset == rhs.normalOffset &&
      lhs.groupSlotOffset == rhs.groupSlotOffset &&
      lhs.texcoord0Offset == rhs.texcoord0Offset &&
      lhs.texcoord1Offset == rhs.texcoord1Offset &&
      lhs.byteSize == rhs.byteSize;
}

bool SameBufferSlice(
    const DxvkBufferSlice& lhs, const DxvkBufferSlice& rhs) noexcept {
  return lhs.defined() == rhs.defined() &&
      (!lhs.defined() ||
       (lhs.buffer() == rhs.buffer() && lhs.offset() == rhs.offset() &&
        lhs.length() == rhs.length()));
}

bool SamePrimitiveVector(
    const std::vector<GpuSkinStaticPrimitiveProof>& lhs,
    const std::vector<GpuSkinStaticPrimitiveProof>& rhs) noexcept {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0u; i < lhs.size(); ++i) {
    if (!SameGpuSkinStaticPrimitiveProof(lhs[i], rhs[i]))
      return false;
  }
  return true;
}

bool SameDerivedPrimitiveVector(
    const std::vector<PersistentGpuPackagePrimitiveProof>& derived,
    const std::vector<GpuSkinStaticPrimitiveProof>& frozen) noexcept {
  if (derived.size() != frozen.size())
    return false;
  for (size_t i = 0u; i < derived.size(); ++i) {
    const auto& lhs = derived[i];
    const auto& rhs = frozen[i];
    if (lhs.indexContentHash != rhs.indexContentHash ||
        lhs.ordinal != rhs.ordinal ||
        lhs.primitiveTypeOrMaterialSlot !=
            rhs.primitiveTypeOrMaterialSlot ||
        lhs.firstIndex != rhs.firstIndex ||
        lhs.indexCount != rhs.indexCount ||
        lhs.minVertex != rhs.minVertex ||
        lhs.maxVertex != rhs.maxVertex) {
      return false;
    }
  }
  return true;
}

bool ValidateFrozenMirrorsAndGpuRanges(
    const GpuSkinStaticResource& resource) noexcept {
  const auto& frozen = resource.frozenPayload;
  if (frozen == nullptr || frozen->storeInstanceAuthority() == 0u ||
      frozen->snapshotIdentity() == nullptr || frozen->record() == nullptr ||
      frozen->snapshotIdentity() != frozen->record().get() ||
      resource.record == nullptr || resource.record.get() !=
          frozen->snapshotIdentity() ||
      !(resource.key == frozen->key()) ||
      !SameGpuSkinStaticPackageProof(
          resource.packageProof, frozen->packageProof()) ||
      !SamePrimitiveVector(
          resource.primitiveProofs, frozen->primitiveProofs()) ||
      resource.indexContentHash != frozen->indexContentHash() ||
      resource.maxVertexGroupSlot != frozen->maxVertexGroupSlot() ||
      !SameStaticLayout(resource.sourceLayout, frozen->sourceLayout()) ||
      !SameBufferSlice(resource.packageSlice, frozen->packageSlice()) ||
      !SameBufferSlice(resource.staticSource, frozen->staticSource()) ||
      !SameBufferSlice(resource.indexSource, frozen->indexSource()) ||
      resource.indexType != frozen->indexType() ||
      resource.indexCount != frozen->indexCount() ||
      resource.allocatedBytes != frozen->allocatedBytes()) {
    return false;
  }

  const auto& expected = frozen->packageProof();
  const auto& immutable = frozen->immutableProof();
  const auto& record = *frozen->record();
  if (resource.key.reserved != 0u || expected.mapEpoch == 0u ||
      expected.deviceEpoch == 0u || expected.packageGeneration == 0u ||
      expected.geosetData == 0u || expected.contentHash == 0u ||
      expected.immutableModelGeneration == 0u ||
      expected.layoutGeneration !=
          kPersistentGpuPackageStaticLayoutGeneration ||
      resource.key.mapEpoch != expected.mapEpoch ||
      resource.key.deviceEpoch != expected.deviceEpoch ||
      resource.key.geosetData != expected.geosetData ||
      resource.key.contentHash != expected.contentHash ||
      resource.key.immutableModelGeneration !=
          expected.immutableModelGeneration ||
      resource.key.layoutGeneration != expected.layoutGeneration ||
      record.geosetDataPtr != reinterpret_cast<void*>(expected.geosetData) ||
      record.contentHash != expected.contentHash ||
      record.immutableModelGeneration !=
          expected.immutableModelGeneration ||
      immutable.immutableModelGeneration !=
          expected.immutableModelGeneration ||
      immutable.contentHash != expected.contentHash ||
      immutable.positionContentHash != expected.positionContentHash ||
      immutable.normalContentHash != expected.normalContentHash ||
      immutable.vertexGroupContentHash !=
          expected.vertexGroupContentHash ||
      immutable.uv0ContentHash != expected.uv0ContentHash ||
      immutable.uv1ContentHash != expected.uv1ContentHash ||
      immutable.indexContentHash != expected.indexContentHash ||
      immutable.primitiveProofHash != expected.primitiveProofHash ||
      immutable.localBoundsHash != expected.localBoundsHash ||
      immutable.layoutGeneration != expected.layoutGeneration ||
      immutable.vertexCount != expected.vertexCount ||
      immutable.indexCount != expected.indexCount ||
      immutable.uvLayerCount != expected.uvLayerCount ||
      immutable.primitiveProofCount != expected.primitiveProofCount ||
      immutable.localMinX != expected.localMinX ||
      immutable.localMinY != expected.localMinY ||
      immutable.localMinZ != expected.localMinZ ||
      immutable.localMaxX != expected.localMaxX ||
      immutable.localMaxY != expected.localMaxY ||
      immutable.localMaxZ != expected.localMaxZ ||
      expected.indexType != VK_INDEX_TYPE_UINT16 ||
      expected.staticByteLength != immutable.staticByteSize ||
      uint64_t(expected.indexByteLength) !=
          uint64_t(expected.indexCount) * sizeof(uint16_t)) {
    return false;
  }

  const GpuSkinStaticSourceLayout immutableLayout = {
      immutable.positionOffset, immutable.normalOffset,
      immutable.groupSlotOffset, immutable.texcoord0Offset,
      immutable.texcoord1Offset, immutable.staticByteSize};
  if (!SameStaticLayout(resource.sourceLayout, immutableLayout) ||
      resource.maxVertexGroupSlot != immutable.maxVertexGroupSlot ||
      !resource.packageSlice.defined() ||
      !resource.staticSource.defined() || !resource.indexSource.defined() ||
      resource.packageSlice.buffer() == nullptr ||
      resource.staticSource.buffer() != resource.packageSlice.buffer() ||
      resource.indexSource.buffer() != resource.packageSlice.buffer() ||
      resource.packageSlice.length() != resource.allocatedBytes ||
      resource.staticSource.offset() != expected.staticByteOffset ||
      resource.staticSource.length() != expected.staticByteLength ||
      resource.indexSource.offset() != expected.indexByteOffset ||
      resource.indexSource.length() != expected.indexByteLength ||
      resource.packageSlice.offset() != resource.staticSource.offset()) {
    return false;
  }

  const uint64_t packageOffset = resource.packageSlice.offset();
  const uint64_t packageLength = resource.packageSlice.length();
  const uint64_t staticOffset = resource.staticSource.offset();
  const uint64_t staticLength = resource.staticSource.length();
  const uint64_t indexOffset = resource.indexSource.offset();
  const uint64_t indexLength = resource.indexSource.length();
  const DxvkBufferCreateInfo& info = resource.packageSlice.buffer()->info();
  if (packageOffset > info.size || packageLength > info.size - packageOffset ||
      staticOffset < packageOffset || indexOffset < packageOffset ||
      staticLength > packageLength || indexLength > packageLength ||
      staticOffset - packageOffset > packageLength - staticLength ||
      indexOffset - packageOffset > packageLength - indexLength ||
      staticOffset > std::numeric_limits<uint64_t>::max() - staticLength ||
      indexOffset < staticOffset + staticLength ||
      indexOffset - packageOffset + indexLength != packageLength ||
      indexOffset % alignof(uint16_t) != 0u) {
    return false;
  }

  constexpr VkBufferUsageFlags kRequiredUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  constexpr VkPipelineStageFlags kRequiredStages =
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT;
  constexpr VkAccessFlags kRequiredAccess =
      VK_ACCESS_SHADER_READ_BIT |
      VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
      VK_ACCESS_INDEX_READ_BIT |
      VK_ACCESS_TRANSFER_WRITE_BIT;
  return (info.usage & kRequiredUsage) == kRequiredUsage &&
      (info.stages & kRequiredStages) == kRequiredStages &&
      (info.access & kRequiredAccess) == kRequiredAccess;
}

}  // namespace

bool ValidateGpuSkinStaticPackage(
    const GpuSkinStaticResource& resource) noexcept {
  return resource.state == GpuSkinStaticResourceState::Ready &&
      ValidateFrozenMirrorsAndGpuRanges(resource);
}

bool ValidateGpuSkinStaticFrozenPayload(
    const GpuSkinStaticResource& resource) noexcept {
  if (resource.state == GpuSkinStaticResourceState::Invalid ||
      !ValidateFrozenMirrorsAndGpuRanges(resource) ||
      resource.frozenPayload == nullptr ||
      resource.frozenPayload->record() == nullptr) {
    return false;
  }

  PersistentGpuPackageImmutableProof recomputed = {};
  std::vector<PersistentGpuPackagePrimitiveProof> primitives;
  if (!BuildPersistentGpuPackageImmutableProof(
          *resource.frozenPayload->record(), recomputed, primitives) ||
      !SamePersistentGpuPackageImmutableProof(
          recomputed, resource.frozenPayload->immutableProof()) ||
      !SameDerivedPrimitiveVector(
          primitives, resource.frozenPayload->primitiveProofs())) {
    return false;
  }
  return true;
}

War3PersistentGpuPackageStore::War3PersistentGpuPackageStore(
    Rc<DxvkDevice> device,
    const GpuSkinResourceBudgets& budgets,
    VkDeviceSize storageBufferOffsetAlignment,
    GpuSkinDiagnostics& diagnostics)
: m_device(std::move(device)),
  m_budgets(budgets),
  m_storageBufferOffsetAlignment(std::max(
      storageBufferOffsetAlignment, kMinimumStorageBufferOffsetAlignment)),
  m_diagnostics(diagnostics),
  m_instanceAuthority(AllocateStoreInstanceAuthority()) {
}

War3PersistentGpuPackageStore::~War3PersistentGpuPackageStore() {
  reset();
  deferOutstandingProducerRetirements();
}

bool War3PersistentGpuPackageStore::beginFrame(
    uint64_t mapEpoch, uint64_t deviceEpoch, uint64_t frameTag) {
  if (m_device == nullptr || mapEpoch == 0u || deviceEpoch == 0u ||
      frameTag == 0u)
    return false;
  if ((m_mapEpoch != 0u && m_mapEpoch != mapEpoch) ||
      (m_deviceEpoch != 0u && m_deviceEpoch != deviceEpoch))
    return false;
  m_mapEpoch = mapEpoch;
  m_deviceEpoch = deviceEpoch;
  return true;
}

void War3PersistentGpuPackageStore::invalidateMapEpoch(
    uint64_t nextMapEpoch) {
  clearEpochResources();
  m_mapEpoch = nextMapEpoch;
}

void War3PersistentGpuPackageStore::invalidateDevice(
    Rc<DxvkDevice> device, uint64_t nextDeviceEpoch,
    VkDeviceSize storageBufferOffsetAlignment) {
  clearEpochResources();
  m_device = std::move(device);
  m_storageBufferOffsetAlignment = std::max(
      storageBufferOffsetAlignment, kMinimumStorageBufferOffsetAlignment);
  m_deviceEpoch = nextDeviceEpoch;
}

void War3PersistentGpuPackageStore::reset() {
  clearEpochResources();
  m_mapEpoch = 0u;
  m_deviceEpoch = 0u;
}

GpuSkinStaticResourceKey War3PersistentGpuPackageStore::makeKey(
    const model::ShadowGeosetResourceRecord& record,
    uint32_t layoutGeneration) const {
  GpuSkinStaticResourceKey key;
  key.mapEpoch = m_mapEpoch;
  key.deviceEpoch = m_deviceEpoch;
  key.geosetData = reinterpret_cast<uintptr_t>(record.geosetDataPtr);
  key.contentHash = record.contentHash;
  key.immutableModelGeneration = record.immutableModelGeneration;
  key.layoutGeneration = kStaticPackingLayoutGeneration;
  if (layoutGeneration != kStaticPackingLayoutGeneration)
    return {};
  return key;
}

GpuSkinStaticLookup War3PersistentGpuPackageStore::findOrQueueStatic(
    model::ShadowGeosetResourceSnapshot record,
    uint32_t layoutGeneration) {
  if (record == nullptr || m_instanceAuthority == 0u ||
      layoutGeneration != kStaticPackingLayoutGeneration) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }
  const model::ShadowGeosetResourceRecord& snapshot = *record;
  const model::ShadowGeosetResourceSnapshot cacheSnapshot =
      model::ShadowModelResourceCache::instance().findGeosetSnapshotByData(
          snapshot.geosetDataPtr);
  if (cacheSnapshot == nullptr || cacheSnapshot.get() != record.get() ||
      snapshot.geosetDataPtr == nullptr || snapshot.contentHash == 0u ||
      snapshot.immutableModelGeneration == 0u ||
      snapshot.immutableCaptureStatus !=
          model::ShadowGeosetImmutableCaptureStatus::Complete ||
      snapshot.vertexCount == 0u) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }
  if (m_device == nullptr) {
    recordFallback(GpuSkinFallbackReason::DeviceLost);
    return {nullptr, GpuSkinFallbackReason::DeviceLost};
  }
  if (m_mapEpoch == 0u || m_deviceEpoch == 0u) {
    recordFallback(GpuSkinFallbackReason::InvalidEpoch);
    return {nullptr, GpuSkinFallbackReason::InvalidEpoch};
  }
  const GpuSkinStaticResourceKey key = makeKey(snapshot, layoutGeneration);
  const auto ready = m_staticResources.find(key);
  if (ready != m_staticResources.end()) {
    if (ready->second != nullptr &&
        ready->second->state == GpuSkinStaticResourceState::Ready) {
      const bool exactLayout = ownsFrozenPayload(*ready->second) &&
          ready->second->readyValidationAuthority == m_instanceAuthority &&
          ready->second->record != nullptr &&
          ready->second->record->contentHash == snapshot.contentHash &&
          ready->second->record->immutableModelGeneration ==
              snapshot.immutableModelGeneration &&
          ready->second->record->vertexCount == snapshot.vertexCount;
      if (!exactLayout) {
        recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
        return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
      }
      ++m_diagnostics.staticCacheHits;
      return {ready->second, GpuSkinFallbackReason::None};
    }
    if (ready->second != nullptr &&
        ready->second->state == GpuSkinStaticResourceState::Invalid) {
      recordFallback(GpuSkinFallbackReason::StaticBudgetExhausted);
      return {nullptr, GpuSkinFallbackReason::StaticBudgetExhausted};
    }
    recordFallback(GpuSkinFallbackReason::StaticResourcePending);
    return {nullptr, GpuSkinFallbackReason::StaticResourcePending};
  }

  const auto queued = std::find_if(
      m_staticMisses.begin(), m_staticMisses.end(),
      [&key](const QueuedStaticMiss& miss) { return miss.key == key; });
  if (queued != m_staticMisses.end()) {
    recordFallback(GpuSkinFallbackReason::StaticResourcePending);
    return {nullptr, GpuSkinFallbackReason::StaticResourcePending};
  }
  if (m_staticMisses.size() >= m_budgets.missQueueCapacity) {
    recordFallback(GpuSkinFallbackReason::MissQueueFull);
    return {nullptr, GpuSkinFallbackReason::MissQueueFull};
  }

  PersistentGpuPackageImmutableProof immutableProof = {};
  std::vector<PersistentGpuPackagePrimitiveProof> primitiveProofs;
  if (!BuildPersistentGpuPackageImmutableProof(
          snapshot, immutableProof, primitiveProofs)) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }

  ++m_diagnostics.staticCacheMisses;
  m_staticMisses.push_back(QueuedStaticMiss{
      key, std::move(record), immutableProof, std::move(primitiveProofs)});
  m_peakQueuedStaticMissRecords = std::max<uint64_t>(
      m_peakQueuedStaticMissRecords, m_staticMisses.size());
  m_peakQueuedStaticMissHostBytes = std::max(
      m_peakQueuedStaticMissHostBytes, m_queuedStaticMissHostBytes);
  recordFallback(GpuSkinFallbackReason::StaticResourceMiss);
  return {nullptr, GpuSkinFallbackReason::StaticResourceMiss};
}

GpuSkinStaticLookup War3PersistentGpuPackageStore::probeStatic(
    const model::ShadowGeosetResourceStamp& stamp,
    uint32_t layoutGeneration) {
  if (m_device == nullptr) {
    recordFallback(GpuSkinFallbackReason::DeviceLost);
    return {nullptr, GpuSkinFallbackReason::DeviceLost};
  }
  if (m_mapEpoch == 0u || m_deviceEpoch == 0u) {
    recordFallback(GpuSkinFallbackReason::InvalidEpoch);
    return {nullptr, GpuSkinFallbackReason::InvalidEpoch};
  }
  if (layoutGeneration != kStaticPackingLayoutGeneration ||
      m_instanceAuthority == 0u || stamp.geosetDataPtr == nullptr ||
      stamp.contentHash == 0u || stamp.immutableModelGeneration == 0u ||
      stamp.immutableCaptureStatus !=
          model::ShadowGeosetImmutableCaptureStatus::Complete ||
      stamp.vertexCount == 0u) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }

  const model::ShadowGeosetResourceSnapshot currentSnapshot =
      model::ShadowModelResourceCache::instance().findGeosetSnapshotByData(
          stamp.geosetDataPtr);
  if (currentSnapshot == nullptr ||
      currentSnapshot->geosetDataPtr != stamp.geosetDataPtr ||
      currentSnapshot->contentHash != stamp.contentHash ||
      currentSnapshot->immutableModelGeneration !=
          stamp.immutableModelGeneration ||
      currentSnapshot->immutableCaptureStatus !=
          model::ShadowGeosetImmutableCaptureStatus::Complete ||
      currentSnapshot->vertexCount != stamp.vertexCount) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }

  GpuSkinStaticResourceKey key;
  key.mapEpoch = m_mapEpoch;
  key.deviceEpoch = m_deviceEpoch;
  key.geosetData = reinterpret_cast<uintptr_t>(stamp.geosetDataPtr);
  key.contentHash = stamp.contentHash;
  key.immutableModelGeneration = stamp.immutableModelGeneration;
  key.layoutGeneration = kStaticPackingLayoutGeneration;
  const auto found = m_staticResources.find(key);
  if (found == m_staticResources.end()) {
    const auto queued = std::find_if(
        m_staticMisses.begin(), m_staticMisses.end(),
        [&key](const QueuedStaticMiss& miss) { return miss.key == key; });
    if (queued != m_staticMisses.end()) {
      recordFallback(GpuSkinFallbackReason::StaticResourcePending);
      return {nullptr, GpuSkinFallbackReason::StaticResourcePending};
    }
    if (m_staticMisses.size() >= m_budgets.missQueueCapacity) {
      recordFallback(GpuSkinFallbackReason::MissQueueFull);
      return {nullptr, GpuSkinFallbackReason::MissQueueFull};
    }
    return {nullptr, GpuSkinFallbackReason::StaticResourceMiss};
  }
  if (found->second == nullptr) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }
  if (found->second->state == GpuSkinStaticResourceState::Invalid) {
    recordFallback(GpuSkinFallbackReason::StaticBudgetExhausted);
    return {nullptr, GpuSkinFallbackReason::StaticBudgetExhausted};
  }
  if (found->second->state != GpuSkinStaticResourceState::Ready) {
    recordFallback(GpuSkinFallbackReason::StaticResourcePending);
    return {nullptr, GpuSkinFallbackReason::StaticResourcePending};
  }

  const auto& resource = found->second;
  const auto& record = resource->record;
  if (!(resource->key == key) || record == nullptr ||
      record->geosetDataPtr != stamp.geosetDataPtr ||
      record->contentHash != stamp.contentHash ||
      record->immutableModelGeneration !=
          stamp.immutableModelGeneration ||
      record->vertexCount != stamp.vertexCount ||
      !ownsFrozenPayload(*resource) ||
      resource->readyValidationAuthority != m_instanceAuthority) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }

  ++m_diagnostics.staticCacheHits;
  return {resource, GpuSkinFallbackReason::None};
}

uint32_t War3PersistentGpuPackageStore::prepareQueuedStaticResources(
    uint32_t maxRecords) {
  uint32_t prepared = 0u;
  while (prepared < maxRecords && !m_staticMisses.empty()) {
    QueuedStaticMiss miss = std::move(m_staticMisses.front());
    m_staticMisses.pop_front();
    if (m_staticResources.find(miss.key) != m_staticResources.end())
      continue;

    auto resource = createStaticResource(miss);
    if (resource == nullptr) {
      auto rejected = std::make_shared<GpuSkinStaticResource>();
      rejected->key = miss.key;
      rejected->record = std::move(miss.record);
      rejected->residencyCensus = RegisterStaticMirror(0u, false);
      rejected->state = GpuSkinStaticResourceState::Invalid;
      m_staticResources.emplace(rejected->key, std::move(rejected));
      recordFallback(GpuSkinFallbackReason::StaticBudgetExhausted);
      continue;
    }

    m_staticResources.emplace(miss.key, resource);
    m_readyStaticUploads.push_back(resource->pendingUpload);
    ++prepared;
  }
  return prepared;
}

std::shared_ptr<GpuSkinStaticResource>
War3PersistentGpuPackageStore::createStaticResource(
    const QueuedStaticMiss& miss) {
  if (miss.record == nullptr)
    return nullptr;
  const model::ShadowGeosetResourceRecord& record = *miss.record;
  PersistentGpuPackageImmutableProof recomputedProof = {};
  std::vector<PersistentGpuPackagePrimitiveProof> recomputedPrimitives;
  if (!BuildPersistentGpuPackageImmutableProof(
          record, recomputedProof, recomputedPrimitives) ||
      !SamePersistentGpuPackageImmutableProof(
          recomputedProof, miss.immutableProof) ||
      recomputedPrimitives.size() != miss.primitiveProofs.size())
    return nullptr;
  for (size_t i = 0u; i < recomputedPrimitives.size(); ++i) {
    if (!SamePersistentGpuPackagePrimitiveProof(
            recomputedPrimitives[i], miss.primitiveProofs[i]))
      return nullptr;
  }

  const GpuSkinStaticSourceLayout layout = {
      recomputedProof.positionOffset, recomputedProof.normalOffset,
      recomputedProof.groupSlotOffset, recomputedProof.texcoord0Offset,
      recomputedProof.texcoord1Offset, recomputedProof.staticByteSize};
  if (m_device == nullptr || m_mapEpoch == 0u || m_deviceEpoch == 0u ||
      m_instanceAuthority == 0u ||
      miss.key.mapEpoch != m_mapEpoch ||
      miss.key.deviceEpoch != m_deviceEpoch ||
      miss.key.geosetData !=
          reinterpret_cast<uintptr_t>(record.geosetDataPtr) ||
      miss.key.contentHash == 0u ||
      miss.key.contentHash != record.contentHash ||
      miss.key.immutableModelGeneration == 0u ||
      miss.key.immutableModelGeneration !=
          record.immutableModelGeneration ||
      miss.key.layoutGeneration != kStaticPackingLayoutGeneration ||
      recomputedProof.layoutGeneration != kStaticPackingLayoutGeneration)
    return nullptr;

  if (record.indexCount == 0u ||
      record.indices.size() != size_t(record.indexCount))
    return nullptr;

  const VkDeviceSize staticBytes = layout.byteSize;
  const VkDeviceSize indexBytes =
      VkDeviceSize(record.indices.size()) * sizeof(uint16_t);
  VkDeviceSize indexRelativeOffset = 0u;
  if (!TryAlignUp(staticBytes, alignof(uint16_t), indexRelativeOffset) ||
      indexBytes == 0u ||
      indexRelativeOffset > std::numeric_limits<VkDeviceSize>::max() -
          indexBytes)
    return nullptr;
  const VkDeviceSize packageBytes = indexRelativeOffset + indexBytes;

  VkDeviceSize atlasOffset = 0u;
  if (!TryAlignUp(m_staticCursor, m_storageBufferOffsetAlignment, atlasOffset))
    return nullptr;
  if (staticBytes == 0u || packageBytes == 0u ||
      atlasOffset > m_budgets.staticBytes ||
      packageBytes > m_budgets.staticBytes - atlasOffset ||
      atlasOffset > std::numeric_limits<uint32_t>::max() ||
      packageBytes > std::numeric_limits<uint32_t>::max() ||
      atlasOffset + indexRelativeOffset >
          std::numeric_limits<uint32_t>::max())
    return nullptr;

  if (m_staticAtlas == nullptr) {
    if (m_budgets.staticBytes == 0u ||
        m_budgets.staticBytes > std::numeric_limits<uint32_t>::max())
      return nullptr;
    m_staticAtlas = m_device->createBuffer(
        StaticBufferInfo(m_budgets.staticBytes, "War3GpuSkinStaticAtlas"),
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (m_staticAtlas == nullptr)
      return nullptr;

    resource_census::ResourceRegistration census = {};
    census.resourceClass =
        resource_census::ResourceClass::GpuSkinStaticAtlas;
    census.pool = 0xFFFFFFFFu;
    census.logicalBytes = m_budgets.staticBytes;
    const Rc<DxvkResourceAllocation> storage = m_staticAtlas->storage();
    census.deviceAllocationBytes = storage != nullptr
        ? uint64_t(storage->getMemoryInfo().size)
        : uint64_t(m_budgets.staticBytes);
    census.hasDeviceCopy = true;
    m_staticAtlasCensus = resource_census::Register(census);
  }

  auto resource = std::make_shared<GpuSkinStaticResource>();
  resource->key = miss.key;
  resource->record = miss.record;
  resource->indexContentHash = recomputedProof.indexContentHash;
  resource->primitiveProofs.reserve(recomputedPrimitives.size());
  for (const auto& primitive : recomputedPrimitives) {
    resource->primitiveProofs.push_back(GpuSkinStaticPrimitiveProof{
        primitive.indexContentHash, primitive.ordinal,
        primitive.primitiveTypeOrMaterialSlot, primitive.firstIndex,
        primitive.indexCount, primitive.minVertex, primitive.maxVertex});
  }
  resource->maxVertexGroupSlot = recomputedProof.maxVertexGroupSlot;
  resource->sourceLayout = layout;
  resource->indexType = VK_INDEX_TYPE_UINT16;
  resource->indexCount = record.indexCount;
  resource->allocatedBytes = packageBytes;
  resource->residencyCensus = RegisterStaticMirror(packageBytes, true);

  Rc<DxvkBuffer> staging = m_device->createBuffer(
      StaticUploadBufferInfo(
          packageBytes, "War3GpuSkinStaticPackageUpload"),
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (staging == nullptr || staging->mapPtr(0u) == nullptr)
    return nullptr;
  resource_census::ResourceHandle stagingCensus = RegisterGpuBuffer(
      resource_census::ResourceClass::GpuSkinStaticUploadStaging,
      staging, packageBytes, true, true);

  auto* blob = reinterpret_cast<uint8_t*>(staging->mapPtr(0u));
  std::memset(blob, 0, size_t(packageBytes));
  std::memcpy(blob + layout.positionOffset, record.positions.data(),
      size_t(record.vertexCount) * 12u);
  std::memcpy(blob + layout.normalOffset, record.normals.data(),
      size_t(record.vertexCount) * 12u);
  std::memcpy(blob + layout.groupSlotOffset,
      record.vertexGroupIndices.data(), record.vertexCount);
  if (record.uvLayerCount >= 1u) {
    std::memcpy(blob + layout.texcoord0Offset,
        record.uvLayers[0].uvPairs.data(),
        size_t(record.vertexCount) * 8u);
  }
  if (record.uvLayerCount >= 2u) {
    std::memcpy(blob + layout.texcoord1Offset,
        record.uvLayers[1].uvPairs.data(),
        size_t(record.vertexCount) * 8u);
  }
  std::memcpy(blob + indexRelativeOffset, record.indices.data(),
      size_t(indexBytes));
  if (!ValidatePersistentGpuPackagePackedBytes(
          blob, uint64_t(packageBytes), uint64_t(indexRelativeOffset),
          recomputedProof, recomputedPrimitives))
    return nullptr;

  resource->packageSlice = DxvkBufferSlice(
      m_staticAtlas, atlasOffset, packageBytes);
  resource->staticSource = DxvkBufferSlice(
      m_staticAtlas, atlasOffset, staticBytes);
  resource->indexSource = DxvkBufferSlice(
      m_staticAtlas, atlasOffset + indexRelativeOffset, indexBytes);

  if (m_nextStaticPackageGeneration == 0u ||
      m_nextStaticPackageGeneration == std::numeric_limits<uint64_t>::max())
    return nullptr;
  const uint64_t packageGeneration = m_nextStaticPackageGeneration++;
  GpuSkinStaticPackageProof packageProof = {};
  packageProof.mapEpoch = m_mapEpoch;
  packageProof.deviceEpoch = m_deviceEpoch;
  packageProof.packageGeneration = packageGeneration;
  packageProof.geosetData =
      reinterpret_cast<uintptr_t>(record.geosetDataPtr);
  packageProof.contentHash = record.contentHash;
  packageProof.immutableModelGeneration =
      record.immutableModelGeneration;
  packageProof.positionContentHash = recomputedProof.positionContentHash;
  packageProof.normalContentHash = recomputedProof.normalContentHash;
  packageProof.vertexGroupContentHash =
      recomputedProof.vertexGroupContentHash;
  packageProof.uv0ContentHash = recomputedProof.uv0ContentHash;
  packageProof.uv1ContentHash = recomputedProof.uv1ContentHash;
  packageProof.indexContentHash = recomputedProof.indexContentHash;
  packageProof.primitiveProofHash = recomputedProof.primitiveProofHash;
  packageProof.localBoundsHash = recomputedProof.localBoundsHash;
  packageProof.layoutGeneration = kStaticPackingLayoutGeneration;
  packageProof.vertexCount = recomputedProof.vertexCount;
  packageProof.indexCount = recomputedProof.indexCount;
  packageProof.uvLayerCount = recomputedProof.uvLayerCount;
  packageProof.primitiveProofCount = recomputedProof.primitiveProofCount;
  packageProof.indexType = VK_INDEX_TYPE_UINT16;
  packageProof.localMinX = recomputedProof.localMinX;
  packageProof.localMinY = recomputedProof.localMinY;
  packageProof.localMinZ = recomputedProof.localMinZ;
  packageProof.localMaxX = recomputedProof.localMaxX;
  packageProof.localMaxY = recomputedProof.localMaxY;
  packageProof.localMaxZ = recomputedProof.localMaxZ;
  packageProof.staticByteOffset = uint32_t(atlasOffset);
  packageProof.staticByteLength = uint32_t(staticBytes);
  packageProof.indexByteOffset =
      uint32_t(atlasOffset + indexRelativeOffset);
  packageProof.indexByteLength = uint32_t(indexBytes);
  resource->packageProof = packageProof;

  std::shared_ptr<GpuSkinStaticFrozenPackage> frozen(
      new GpuSkinStaticFrozenPackage());
  frozen->m_storeInstanceAuthority = m_instanceAuthority;
  frozen->m_snapshotIdentity = miss.record.get();
  frozen->m_key = resource->key;
  frozen->m_record = miss.record;
  frozen->m_packageProof = resource->packageProof;
  frozen->m_primitiveProofs = resource->primitiveProofs;
  frozen->m_immutableProof = recomputedProof;
  frozen->m_sourceLayout = resource->sourceLayout;
  frozen->m_packageSlice = resource->packageSlice;
  frozen->m_staticSource = resource->staticSource;
  frozen->m_indexSource = resource->indexSource;
  frozen->m_indexContentHash = resource->indexContentHash;
  frozen->m_maxVertexGroupSlot = resource->maxVertexGroupSlot;
  frozen->m_indexType = resource->indexType;
  frozen->m_indexCount = resource->indexCount;
  frozen->m_allocatedBytes = resource->allocatedBytes;
  resource->frozenPayload = std::move(frozen);
  if (!ownsFrozenPayload(*resource) ||
      !ValidateGpuSkinStaticFrozenPayload(*resource))
    return nullptr;

  resource->pendingUpload = {
      resource->key,
      DxvkBufferSlice(std::move(staging), 0u, packageBytes),
      resource->packageSlice,
      packageBytes,
      std::move(stagingCensus)};

  m_staticCursor = atlasOffset + packageBytes;
  m_staticBytes = m_staticCursor;
  ++m_diagnostics.staticUploadsPrepared;
  m_diagnostics.staticUploadBytes += packageBytes;
  return resource;
}

bool War3PersistentGpuPackageStore::ownsFrozenPayload(
    const GpuSkinStaticResource& resource) const noexcept {
  return m_instanceAuthority != 0u && resource.frozenPayload != nullptr &&
      resource.frozenPayload->storeInstanceAuthority() ==
          m_instanceAuthority;
}

std::vector<GpuSkinStaticUpload>
War3PersistentGpuPackageStore::takeStaticUploads() {
  std::vector<GpuSkinStaticUpload> uploads;
  uploads.swap(m_readyStaticUploads);
  return uploads;
}

bool War3PersistentGpuPackageStore::retireStaticUpload(
    const GpuSkinStaticUpload& upload,
    Rc<DxvkFence> fence, uint64_t value) {
  try {
    std::vector<GpuSkinStaticUpload> uploads;
    uploads.reserve(1u);
    uploads.push_back(upload);
    return retireStaticUploads(uploads, std::move(fence), value);
  } catch (...) {
    recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
    return false;
  }
}

bool War3PersistentGpuPackageStore::retireStaticUploads(
    const std::vector<GpuSkinStaticUpload>& uploads,
    Rc<DxvkFence> fence, uint64_t value) {
  if (uploads.empty() || fence == nullptr || value == 0u ||
      uploads.size() > m_budgets.missQueueCapacity) {
    recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
    return false;
  }

  struct PreparedRetirement {
    std::unique_ptr<RetiredStaticUpload> retirement;
    std::shared_ptr<GpuSkinStaticResource> resource;
  };
  std::vector<PreparedRetirement> prepared;
  try {
    prepared.reserve(uploads.size());
    for (size_t uploadIndex = 0u;
         uploadIndex < uploads.size(); ++uploadIndex) {
      const GpuSkinStaticUpload& upload = uploads[uploadIndex];
      if (!upload.source.defined() || !upload.destination.defined() ||
          upload.key.reserved != 0u || upload.byteCount == 0u ||
          upload.source.length() != upload.byteCount ||
          upload.destination.length() != upload.byteCount) {
        recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
        return false;
      }
      for (size_t earlier = 0u; earlier < uploadIndex; ++earlier) {
        if (uploads[earlier].key == upload.key) {
          recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
          return false;
        }
      }

      const auto found = m_staticResources.find(upload.key);
      bool exactPendingUpload = false;
      if (found != m_staticResources.end() && found->second != nullptr &&
          found->second->state == GpuSkinStaticResourceState::PendingUpload) {
        const GpuSkinStaticUpload& pending = found->second->pendingUpload;
        exactPendingUpload = found->second->key == upload.key &&
            pending.key == upload.key && pending.source.defined() &&
            pending.destination.defined() &&
            pending.byteCount == upload.byteCount &&
            pending.byteCount == found->second->allocatedBytes &&
            pending.residencyCensus == upload.residencyCensus &&
            pending.source.buffer() == upload.source.buffer() &&
            pending.source.offset() == upload.source.offset() &&
            pending.source.length() == upload.source.length() &&
            pending.destination.buffer() == upload.destination.buffer() &&
            pending.destination.offset() == upload.destination.offset() &&
            pending.destination.length() == upload.destination.length() &&
            pending.destination.buffer() ==
                found->second->packageSlice.buffer() &&
            pending.destination.offset() ==
                found->second->packageSlice.offset() &&
            pending.destination.length() ==
                found->second->packageSlice.length() &&
            ownsFrozenPayload(*found->second) &&
            ValidateGpuSkinStaticFrozenPayload(*found->second);
      }
      if (!exactPendingUpload) {
        recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
        return false;
      }

      // Both sides of the copy remain owned until the exact producer fence.
      auto retirement = std::make_unique<RetiredStaticUpload>();
      retirement->key = upload.key;
      retirement->source = upload.source;
      retirement->destination = upload.destination;
      retirement->fence = fence;
      retirement->retireValue = value;
      retirement->residencyCensus = upload.residencyCensus;
      retirement->destinationResidencyCensus = m_staticAtlasCensus;
      retirement->publicationResource = found->second;
      retirement->publishesReady = true;
      prepared.push_back(
          {std::move(retirement), found->second});
    }

    if (prepared.size() >
        m_retiredStaticUploads.max_size() - m_retiredStaticUploads.size()) {
      recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
      return false;
    }
    m_retiredStaticUploads.reserve(
        m_retiredStaticUploads.size() + prepared.size());
  } catch (...) {
    recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
    return false;
  }

  // All allocations and exact validation are complete. The following commit
  // cannot allocate and therefore cannot publish only half of this batch.
  for (PreparedRetirement& item : prepared)
    m_retiredStaticUploads.push_back(std::move(item.retirement));
  for (PreparedRetirement& item : prepared) {
    item.resource->readyValidationAuthority = 0u;
    item.resource->state = GpuSkinStaticResourceState::UploadSubmitted;
    item.resource->pendingUpload = {};
  }
  m_diagnostics.staticUploadRetirementsQueued += uploads.size();
  return true;
}

void War3PersistentGpuPackageStore::completeRetiredStaticUpload(
    RetiredStaticUpload& retirement) noexcept {
  if (!retirement.publishesReady)
    return;

  retirement.publishesReady = false;
  const std::shared_ptr<GpuSkinStaticResource> resource =
      retirement.publicationResource;
  const auto active = m_staticResources.find(retirement.key);
  const bool exactActiveGeneration = resource != nullptr &&
      m_mapEpoch == retirement.key.mapEpoch &&
      m_deviceEpoch == retirement.key.deviceEpoch &&
      active != m_staticResources.end() && active->second == resource;
  const bool exactSubmittedPayload = exactActiveGeneration &&
      resource->state == GpuSkinStaticResourceState::UploadSubmitted &&
      resource->key == retirement.key &&
      retirement.source.defined() && retirement.destination.defined() &&
      retirement.source.length() == resource->allocatedBytes &&
      retirement.destination.buffer() == resource->packageSlice.buffer() &&
      retirement.destination.offset() == resource->packageSlice.offset() &&
      retirement.destination.length() == resource->packageSlice.length() &&
      !resource->pendingUpload.source.defined() &&
      !resource->pendingUpload.destination.defined() &&
      ownsFrozenPayload(*resource) &&
      ValidateGpuSkinStaticFrozenPayload(*resource);

  if (exactSubmittedPayload) {
    resource->state = GpuSkinStaticResourceState::Ready;
    if (ValidateGpuSkinStaticPackage(*resource)) {
      resource->readyValidationAuthority = m_instanceAuthority;
      resource_census::UpdateHostBacking(
          resource->residencyCensus, 0u, 0u, 0u);
      resource_census::NoteDeviceUpload(resource->residencyCensus, 0u);
      ++m_diagnostics.staticUploadsCompleted;
      retirement.publicationResource.reset();
      return;
    }
  }

  if (resource != nullptr) {
    resource->readyValidationAuthority = 0u;
    resource->state = GpuSkinStaticResourceState::Invalid;
  }
  ++m_diagnostics.staticUploadCompletionsRejected;
  recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
  retirement.publicationResource.reset();
}

DxvkBufferSlice War3PersistentGpuPackageStore::staticAtlasSlice() const {
  if (m_staticAtlas == nullptr || m_staticCursor == 0u)
    return {};
  return DxvkBufferSlice(m_staticAtlas, 0u, m_staticCursor);
}

bool War3PersistentGpuPackageStore::hasRetiredUploads() const {
  return !m_retiredStaticUploads.empty();
}

void War3PersistentGpuPackageStore::fillResidencySnapshot(
    resource_census::GpuSkinPoolResidencySnapshot& snapshot) const {
  snapshot.staticAtlasReservedBytes =
      m_staticAtlas != nullptr ? uint64_t(m_budgets.staticBytes) : 0u;
  snapshot.staticAtlasUsedBytes = uint64_t(m_staticCursor);
  snapshot.staticResourceRecords = m_staticResources.size();
  for (const auto& entry : m_staticResources) {
    const auto& resource = entry.second;
    if (resource == nullptr)
      continue;
    switch (resource->state) {
      case GpuSkinStaticResourceState::Ready:
        ++snapshot.staticReadyRecords;
        break;
      case GpuSkinStaticResourceState::PendingUpload:
        ++snapshot.staticPendingRecords;
        break;
      case GpuSkinStaticResourceState::UploadSubmitted:
        ++snapshot.staticSubmittedRecords;
        break;
      case GpuSkinStaticResourceState::Invalid:
        ++snapshot.staticInvalidRecords;
        break;
    }
  }
  snapshot.queuedStaticMissRecords = m_staticMisses.size();
  snapshot.queuedStaticMissHostBytes = m_queuedStaticMissHostBytes;
  snapshot.peakQueuedStaticMissRecords = m_peakQueuedStaticMissRecords;
  snapshot.peakQueuedStaticMissHostBytes = m_peakQueuedStaticMissHostBytes;
  snapshot.readyStaticUploadCount = m_readyStaticUploads.size();
  for (const auto& upload : m_readyStaticUploads)
    snapshot.readyStaticUploadBytes += uint64_t(upload.byteCount);
  snapshot.retiredStaticUploadCount = m_retiredStaticUploads.size();
  for (const auto& upload : m_retiredStaticUploads) {
    if (upload != nullptr)
      snapshot.retiredStaticUploadBytes +=
          uint64_t(upload->source.length());
  }
}

void War3PersistentGpuPackageStore::recordFallback(
    GpuSkinFallbackReason reason) {
  ++m_diagnostics.fallbackCount;
  const size_t index = static_cast<size_t>(reason);
  if (index < 16u)
    ++m_diagnostics.fallbackByReason[index];
}

void War3PersistentGpuPackageStore::clearEpochResources() {
  m_staticResources.clear();
  m_staticMisses.clear();
  m_readyStaticUploads.clear();
  m_staticAtlas = nullptr;
  m_staticAtlasCensus.reset();
  // Submitted copies are generation-owned retirement records, not active
  // lookup state. Keep their source and destination slices alive across map,
  // device and reset boundaries until pollRetired observes the exact producer
  // fence. Consumer-side atlas retirement remains a separate P1 admission gate.
  m_staticBytes = 0u;
  m_staticCursor = 0u;
  m_queuedStaticMissHostBytes = 0u;
}

void War3PersistentGpuPackageStore::deferOutstandingProducerRetirements()
    noexcept {
  // The legacy manager normally destroys this store only after its whole
  // War3GpuSkinResources owner is quiescent. Keep destruction independently
  // producer-safe as well: if a submitted copy is unexpectedly still in
  // flight, transfer its two buffer slices to the timeline-fence callback.
  // The callback deliberately owns no fence reference, avoiding a cycle.
  while (!m_retiredStaticUploads.empty()) {
    std::unique_ptr<RetiredStaticUpload> retirement =
        std::move(m_retiredStaticUploads.back());
    m_retiredStaticUploads.pop_back();
    if (retirement == nullptr || retirement->fence == nullptr ||
        retirement->retireValue == 0u) {
      // A malformed retirement has no completion proof. Leak rather than
      // guess that the producer is idle and release GPU-visible storage.
      (void)retirement.release();
      continue;
    }

    Rc<DxvkFence> fence = std::move(retirement->fence);
    const uint64_t retireValue = retirement->retireValue;
    RetiredStaticUpload* const payload = retirement.release();
    try {
      fence->enqueueWait(retireValue, [payload] { delete payload; });
    } catch (...) {
      // Allocation failure while registering the callback must not turn into
      // an early GPU resource release. Intentionally leak this exceptional
      // payload; process teardown will reclaim it fail-closed.
    }
  }
}

}  // namespace dxvk::war3::gpu_skin
