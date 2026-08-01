#include "war3_persistent_gpu_package_store.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dxvk::war3::gpu_skin {

namespace {

constexpr VkDeviceSize kMinimumStorageBufferOffsetAlignment = 16u;

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

uint64_t HashStaticBytes(const void* data, size_t size) {
  uint64_t hash = 0xcbf29ce484222325ull;
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0u; i < size; ++i)
    hash = (hash ^ bytes[i]) * 0x100000001b3ull;
  return hash;
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

bool GetValidatedStaticLayout(
    const model::ShadowGeosetResourceRecord& record,
    GpuSkinStaticSourceLayout* outLayout = nullptr) {
  const uint64_t vertexCount = record.vertexCount;
  if (record.geosetDataPtr == nullptr || vertexCount == 0u)
    return false;
  if (record.normalCount != vertexCount ||
      record.vertexGroupCount != vertexCount ||
      record.uvLayerCount > 2u ||
      record.uvLayers.size() != record.uvLayerCount)
    return false;
  if (uint64_t(record.positions.size()) != vertexCount * 3u ||
      uint64_t(record.normals.size()) != vertexCount * 3u ||
      uint64_t(record.vertexGroupIndices.size()) != vertexCount)
    return false;

  for (const auto& uv : record.uvLayers) {
    if (uv.uvCount != vertexCount ||
        uint64_t(uv.uvPairs.size()) != vertexCount * 2u)
      return false;
  }

  const uint64_t groupEnd = vertexCount * 25u;
  const uint64_t texcoord0Offset = (groupEnd + 3u) & ~uint64_t(3u);
  const uint64_t byteSize = texcoord0Offset +
      vertexCount * 8u * record.uvLayerCount;
  if (byteSize > std::numeric_limits<uint32_t>::max())
    return false;

  const GpuSkinStaticSourceLayout layout = GetGpuSkinStaticSourceLayout(
      record.vertexCount, record.uvLayerCount);
  if (layout.byteSize != byteSize || layout.positionOffset != 0u ||
      layout.normalOffset != vertexCount * 12u ||
      layout.groupSlotOffset != vertexCount * 24u ||
      layout.texcoord0Offset != texcoord0Offset ||
      layout.texcoord1Offset != texcoord0Offset + vertexCount * 8u)
    return false;

  if (outLayout != nullptr)
    *outLayout = layout;
  return true;
}

}  // namespace

bool ValidateGpuSkinStaticPackage(
    const GpuSkinStaticResource& resource,
    const GpuSkinStaticPackageProof& expected) noexcept {
  if (resource.state != GpuSkinStaticResourceState::Ready ||
      !SameGpuSkinStaticPackageProof(resource.packageProof, expected) ||
      expected.mapEpoch == 0u || expected.deviceEpoch == 0u ||
      expected.packageGeneration == 0u || expected.geosetData == 0u ||
      expected.contentHash == 0u || expected.indexContentHash == 0u ||
      expected.layoutGeneration == 0u || expected.vertexCount == 0u ||
      expected.indexCount == 0u ||
      expected.indexType != VK_INDEX_TYPE_UINT16 ||
      expected.staticByteLength == 0u || expected.indexByteLength == 0u ||
      resource.record == nullptr || resource.indexContentHash == 0u ||
      resource.indexContentHash != expected.indexContentHash ||
      resource.indexType != expected.indexType ||
      resource.indexCount != expected.indexCount ||
      resource.key.mapEpoch != expected.mapEpoch ||
      resource.key.deviceEpoch != expected.deviceEpoch ||
      resource.key.geosetData != expected.geosetData ||
      resource.key.contentHash != expected.contentHash ||
      resource.key.layoutGeneration != expected.layoutGeneration) {
    return false;
  }

  const model::ShadowGeosetResourceRecord& record = *resource.record;
  GpuSkinStaticSourceLayout validatedLayout = {};
  if (!GetValidatedStaticLayout(record, &validatedLayout) ||
      record.geosetDataPtr != reinterpret_cast<void*>(expected.geosetData) ||
      record.contentHash != expected.contentHash ||
      record.vertexCount != expected.vertexCount ||
      record.indexCount != expected.indexCount ||
      record.indices.size() != expected.indexCount ||
      resource.sourceLayout.positionOffset != validatedLayout.positionOffset ||
      resource.sourceLayout.normalOffset != validatedLayout.normalOffset ||
      resource.sourceLayout.groupSlotOffset != validatedLayout.groupSlotOffset ||
      resource.sourceLayout.texcoord0Offset != validatedLayout.texcoord0Offset ||
      resource.sourceLayout.texcoord1Offset != validatedLayout.texcoord1Offset ||
      resource.sourceLayout.byteSize != validatedLayout.byteSize ||
      expected.staticByteLength != validatedLayout.byteSize ||
      uint64_t(expected.indexByteLength) !=
          uint64_t(expected.indexCount) * sizeof(uint16_t)) {
    return false;
  }

  if (!resource.packageSlice.defined() ||
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
  if (staticOffset < packageOffset || indexOffset < packageOffset ||
      staticLength > packageLength || indexLength > packageLength ||
      staticOffset - packageOffset > packageLength - staticLength ||
      indexOffset - packageOffset > packageLength - indexLength ||
      indexOffset < staticOffset + staticLength ||
      indexOffset - packageOffset + indexLength != packageLength ||
      indexOffset % alignof(uint16_t) != 0u) {
    return false;
  }

  const DxvkBufferCreateInfo& info = resource.packageSlice.buffer()->info();
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

War3PersistentGpuPackageStore::War3PersistentGpuPackageStore(
    Rc<DxvkDevice> device,
    const GpuSkinResourceBudgets& budgets,
    VkDeviceSize storageBufferOffsetAlignment,
    GpuSkinDiagnostics& diagnostics)
: m_device(std::move(device)),
  m_budgets(budgets),
  m_storageBufferOffsetAlignment(std::max(
      storageBufferOffsetAlignment, kMinimumStorageBufferOffsetAlignment)),
  m_diagnostics(diagnostics) {
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
  key.layoutGeneration = layoutGeneration;
  return key;
}

GpuSkinStaticLookup War3PersistentGpuPackageStore::findOrQueueStatic(
    model::ShadowGeosetResourceSnapshot record,
    uint32_t layoutGeneration) {
  if (record == nullptr) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }
  const model::ShadowGeosetResourceRecord& snapshot = *record;
  if (m_device == nullptr) {
    recordFallback(GpuSkinFallbackReason::DeviceLost);
    return {nullptr, GpuSkinFallbackReason::DeviceLost};
  }
  if (m_mapEpoch == 0u || m_deviceEpoch == 0u) {
    recordFallback(GpuSkinFallbackReason::InvalidEpoch);
    return {nullptr, GpuSkinFallbackReason::InvalidEpoch};
  }
  GpuSkinStaticSourceLayout requestedLayout;
  if (!GetValidatedStaticLayout(snapshot, &requestedLayout)) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }

  const GpuSkinStaticResourceKey key = makeKey(snapshot, layoutGeneration);
  const auto ready = m_staticResources.find(key);
  if (ready != m_staticResources.end()) {
    if (ready->second != nullptr &&
        ready->second->state == GpuSkinStaticResourceState::Ready) {
      const GpuSkinStaticSourceLayout& cachedLayout =
          ready->second->sourceLayout;
      const bool exactLayout =
          cachedLayout.positionOffset == requestedLayout.positionOffset &&
          cachedLayout.normalOffset == requestedLayout.normalOffset &&
          cachedLayout.groupSlotOffset == requestedLayout.groupSlotOffset &&
          cachedLayout.texcoord0Offset == requestedLayout.texcoord0Offset &&
          cachedLayout.texcoord1Offset == requestedLayout.texcoord1Offset &&
          cachedLayout.byteSize == requestedLayout.byteSize &&
          ready->second->staticSource.length() == requestedLayout.byteSize &&
          ready->second->record != nullptr &&
          ready->second->record->contentHash == snapshot.contentHash &&
          ready->second->record->vertexCount == snapshot.vertexCount &&
          ready->second->record->normalCount == snapshot.normalCount &&
          ready->second->record->uvLayerCount == snapshot.uvLayerCount &&
          ready->second->record->indexCount == snapshot.indexCount &&
          ready->second->record->indices.size() == snapshot.indices.size() &&
          ValidateGpuSkinStaticPackage(
              *ready->second, ready->second->packageProof);
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

  ++m_diagnostics.staticCacheMisses;
  m_staticMisses.push_back({key, std::move(record)});
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
  if (stamp.geosetDataPtr == nullptr || stamp.contentHash == 0u ||
      stamp.vertexCount == 0u) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }

  GpuSkinStaticResourceKey key;
  key.mapEpoch = m_mapEpoch;
  key.deviceEpoch = m_deviceEpoch;
  key.geosetData = reinterpret_cast<uintptr_t>(stamp.geosetDataPtr);
  key.contentHash = stamp.contentHash;
  key.layoutGeneration = layoutGeneration;
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
      record->vertexCount != stamp.vertexCount ||
      !ValidateGpuSkinStaticPackage(*resource, resource->packageProof)) {
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
  GpuSkinStaticSourceLayout layout;
  if (m_device == nullptr || m_mapEpoch == 0u || m_deviceEpoch == 0u ||
      miss.key.mapEpoch != m_mapEpoch ||
      miss.key.deviceEpoch != m_deviceEpoch ||
      miss.key.geosetData !=
          reinterpret_cast<uintptr_t>(record.geosetDataPtr) ||
      miss.key.contentHash == 0u ||
      miss.key.contentHash != record.contentHash ||
      miss.key.layoutGeneration == 0u ||
      !GetValidatedStaticLayout(record, &layout))
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
  resource->indexContentHash = HashStaticBytes(
      record.indices.data(), record.indices.size() * sizeof(uint16_t));
  if (resource->indexContentHash == 0u)
    return nullptr;
  resource->maxVertexGroupSlot = *std::max_element(
      record.vertexGroupIndices.begin(), record.vertexGroupIndices.end());
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
  resource->packageProof = {
      m_mapEpoch,
      m_deviceEpoch,
      packageGeneration,
      reinterpret_cast<uintptr_t>(record.geosetDataPtr),
      record.contentHash,
      resource->indexContentHash,
      miss.key.layoutGeneration,
      record.vertexCount,
      record.indexCount,
      VK_INDEX_TYPE_UINT16,
      uint32_t(atlasOffset),
      uint32_t(staticBytes),
      uint32_t(atlasOffset + indexRelativeOffset),
      uint32_t(indexBytes)};
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

std::vector<GpuSkinStaticUpload>
War3PersistentGpuPackageStore::takeStaticUploads() {
  std::vector<GpuSkinStaticUpload> uploads;
  uploads.swap(m_readyStaticUploads);
  return uploads;
}

bool War3PersistentGpuPackageStore::retireStaticUpload(
    const GpuSkinStaticUpload& upload,
    Rc<DxvkFence> fence, uint64_t value) {
  if (!upload.source.defined() || !upload.destination.defined() ||
      upload.byteCount == 0u || upload.source.length() != upload.byteCount ||
      upload.destination.length() != upload.byteCount ||
      fence == nullptr || value == 0u) {
    recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
    return false;
  }

  const auto found = m_staticResources.find(upload.key);
  if (found != m_staticResources.end() && found->second != nullptr &&
      found->second->state == GpuSkinStaticResourceState::Ready) {
    const bool valid = ValidateGpuSkinStaticPackage(
        *found->second, found->second->packageProof);
    if (!valid)
      recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return valid;
  }

  bool exactPendingUpload = false;
  if (found != m_staticResources.end() && found->second != nullptr &&
      found->second->state == GpuSkinStaticResourceState::PendingUpload) {
    const GpuSkinStaticUpload& pending = found->second->pendingUpload;
    exactPendingUpload = pending.source.defined() &&
        pending.destination.defined() && pending.byteCount == upload.byteCount &&
        pending.source.buffer() == upload.source.buffer() &&
        pending.source.offset() == upload.source.offset() &&
        pending.source.length() == upload.source.length() &&
        pending.destination.buffer() == upload.destination.buffer() &&
        pending.destination.offset() == upload.destination.offset() &&
        pending.destination.length() == upload.destination.length() &&
        pending.destination.buffer() == found->second->packageSlice.buffer() &&
        pending.destination.offset() == found->second->packageSlice.offset() &&
        pending.destination.length() == found->second->packageSlice.length();
    if (exactPendingUpload) {
      found->second->state = GpuSkinStaticResourceState::Ready;
      const bool validPackage = ValidateGpuSkinStaticPackage(
          *found->second, found->second->packageProof);
      found->second->pendingUpload = {};
      if (validPackage) {
        resource_census::UpdateHostBacking(
            found->second->residencyCensus, 0u, 0u, 0u);
        resource_census::NoteDeviceUpload(
            found->second->residencyCensus, 0u);
      } else {
        found->second->state = GpuSkinStaticResourceState::Invalid;
        exactPendingUpload = false;
      }
    }
  }

  // The staging slice remains authoritative until the producer fence passes,
  // even if an epoch invalidates the package lookup table in the meantime.
  // Retain the exact destination slice and atlas census for the same interval:
  // a copy command references both resources, not only its host-visible side.
  m_retiredStaticUploads.push_back(std::make_unique<RetiredStaticUpload>(
      RetiredStaticUpload{
          upload.key, upload.source, upload.destination, std::move(fence),
          value, upload.residencyCensus, m_staticAtlasCensus}));
  ++m_diagnostics.staticUploadRetirementsQueued;
  if (!exactPendingUpload)
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
  return true;
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
        std::move(m_retiredStaticUploads.front());
    m_retiredStaticUploads.pop_front();
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
