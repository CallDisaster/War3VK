#include "war3_gpu_skin_resources.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>

#include "../../../util/util_env.h"

namespace dxvk::war3::gpu_skin {

namespace {

constexpr VkDeviceSize kMinUploadPageBytes = 256ull << 10;
constexpr VkDeviceSize kMaxUploadPageBytes = 1ull << 20;
constexpr VkDeviceSize kOutputPageBytes = 8ull << 20;
constexpr VkDeviceSize kMinimumStorageBufferOffsetAlignment = 16u;

constexpr VkDeviceSize NormalizeStorageBufferOffsetAlignment(
    VkDeviceSize alignment) {
  return alignment < kMinimumStorageBufferOffsetAlignment
      ? kMinimumStorageBufferOffsetAlignment
      : alignment;
}

constexpr VkDeviceSize AlignUpUnchecked(
    VkDeviceSize value, VkDeviceSize alignment) {
  const VkDeviceSize remainder = value % alignment;
  return remainder == 0u ? value : value + alignment - remainder;
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
  // 不可变记录由 ShadowModelResourceCache 持有并计入账本。
  // 本资源只保留共享所有权；若在此再次统计其容量，会把同一份主机分配
  // 重复计作 GPU 蒙皮副本。被在途消费者保留的已取代快照仍受资源普查中
  // 显式的 retainedOldPublishedStoresIncluded=false 限制约束。
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

static_assert(NormalizeStorageBufferOffsetAlignment(0u) ==
    kMinimumStorageBufferOffsetAlignment);
static_assert(NormalizeStorageBufferOffsetAlignment(24u) == 24u);
static_assert(AlignUpUnchecked(17u, 16u) == 32u);
static_assert(AlignUpUnchecked(17u, 24u) == 24u);
static_assert(AlignUpUnchecked(48u, 24u) == 48u);

std::string NormalizeMode(const char* value) {
  if (value == nullptr)
    return {};

  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](char c) {
    return char(std::tolower(static_cast<unsigned char>(c)));
  });
  return result;
}

uint32_t ParseU32(const std::string& value, uint32_t fallback) {
  if (value.empty())
    return fallback;

  uint64_t result = 0;
  for (char c : value) {
    if (c < '0' || c > '9')
      return fallback;
    result = result * 10u + uint32_t(c - '0');
    if (result > std::numeric_limits<uint32_t>::max())
      return fallback;
  }
  return uint32_t(result);
}

GpuSkinOutsidePoisonSidecarPolicy ParseOutsidePoisonSidecarPolicy(
    const std::string& value, bool& invalid) {
  invalid = false;
  if (value.empty() || value == "none" || value == "off" || value == "0")
    return GpuSkinOutsidePoisonSidecarPolicy::None;
  if (value == "o0" || value == "1")
    return GpuSkinOutsidePoisonSidecarPolicy::O0;
  if (value == "o1" || value == "2")
    return GpuSkinOutsidePoisonSidecarPolicy::O1;
  if (value == "both" || value == "3")
    return GpuSkinOutsidePoisonSidecarPolicy::Both;
  invalid = true;
  return GpuSkinOutsidePoisonSidecarPolicy::None;
}

DxvkBufferCreateInfo StaticBufferInfo(VkDeviceSize size, const char* name) {
  DxvkBufferCreateInfo info = { };
  info.size = size;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT;
  info.access = VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                VK_ACCESS_TRANSFER_WRITE_BIT;
  info.debugName = name;
  return info;
}

DxvkBufferCreateInfo UploadBufferInfo(VkDeviceSize size, const char* name) {
  DxvkBufferCreateInfo info = { };
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

DxvkBufferCreateInfo OutputBufferInfo(VkDeviceSize size) {
  DxvkBufferCreateInfo info = { };
  info.size = size;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT;
  info.access = VK_ACCESS_SHADER_WRITE_BIT |
                VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                VK_ACCESS_TRANSFER_READ_BIT |
                VK_ACCESS_TRANSFER_WRITE_BIT;
  info.debugName = "War3GpuSkinOutput";
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


GpuSkinMode GpuSkinRuntimeConfig::parseMode(const char* value) {
  const std::string mode = NormalizeMode(value);
  if (mode.empty() || mode == "off" || mode == "disabled" || mode == "0")
    return GpuSkinMode::Disabled;
  if (mode == "observe" || mode == "1")
    return GpuSkinMode::Observe;
  if (mode == "dual" || mode == "2")
    return GpuSkinMode::Dual;
  if (mode == "shadow" || mode == "3")
    return GpuSkinMode::Shadow;
  if (mode == "main" || mode == "4")
    return GpuSkinMode::Main;
  if (mode == "bypass" || mode == "5")
    return GpuSkinMode::Bypass;
  return GpuSkinMode::Disabled;
}

GpuSkinExecutionRoute GpuSkinRuntimeConfig::parseExecutionRoute(
    const char* value, bool& invalid) {
  invalid = false;
  const std::string route = NormalizeMode(value);
  if (route.empty() || route == "compute" || route == "cs" || route == "0")
    return GpuSkinExecutionRoute::Compute;
  if (route == "vertex_shader" || route == "vertex" || route == "vs" ||
      route == "1")
    return GpuSkinExecutionRoute::VertexShader;
  if (route == "vertex_shader_input_only" || route == "vs_input_only" ||
      route == "2")
    return GpuSkinExecutionRoute::VertexShaderInputOnly;
  if (route == "vertex_shader_bypass" || route == "vs_bypass" ||
      route == "3")
    return GpuSkinExecutionRoute::VertexShaderBypass;
  invalid = true;
  return GpuSkinExecutionRoute::Compute;
}

GpuSkinRuntimeConfig GpuSkinRuntimeConfig::fromEnvironment() {
  GpuSkinRuntimeConfig config;
  const std::string mode = env::getEnvVar("DXVK_WAR3_GPU_SKIN_MODE");
  const std::string executionRoute = NormalizeMode(env::getEnvVar(
      "DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE").c_str());
  const std::string diagnostics = NormalizeMode(env::getEnvVar(
      "DXVK_WAR3_GPU_SKIN_DIAGNOSTICS").c_str());
  const std::string diagnosticPeriod = env::getEnvVar(
      "DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES");
  const std::string outsidePoisonSidecar = NormalizeMode(env::getEnvVar(
      "DXVK_WAR3_GPU_SKIN_POISON_SIDECAR").c_str());
  std::string sampling = env::getEnvVar(
      "DXVK_WAR3_GPU_SKIN_DIFF_EVERY_N");
  if (sampling.empty())
    sampling = env::getEnvVar("DXVK_WAR3_GPU_SKIN_DIFF_PERIOD");
  config.mode = parseMode(mode.c_str());
  config.executionRouteExplicit = !executionRoute.empty();
  config.executionRoute = parseExecutionRoute(
      executionRoute.c_str(), config.executionRouteInvalid);
  config.diffSamplePeriod = sampling.empty()
      ? (config.mode == GpuSkinMode::Dual ? 64u : 0u)
      : ParseU32(sampling, 0u);
  config.fullDiagnostics = diagnostics == "full" || diagnostics == "1";
  config.diagnosticPeriodFrames = ParseU32(diagnosticPeriod, 0u);
  config.outsidePoisonSidecarPolicyExplicit = !outsidePoisonSidecar.empty();
  config.outsidePoisonSidecarPolicy = ParseOutsidePoisonSidecarPolicy(
      outsidePoisonSidecar, config.outsidePoisonSidecarPolicyInvalid);
  return config;
}

bool ShouldSampleGpuSkinDiff(uint64_t sequence, uint32_t period) {
  return period != 0u && sequence % period == 0u;
}

size_t GpuSkinStaticResourceKeyHash::operator()(
    const GpuSkinStaticResourceKey& key) const {
  uint64_t hash = 0xcbf29ce484222325ull;
  const auto mix = [&hash](uint64_t value) {
    hash = (hash ^ value) * 0x100000001b3ull;
  };
  mix(key.mapEpoch);
  mix(key.deviceEpoch);
  mix(uint64_t(key.geosetData));
  mix(key.contentHash);
  mix(key.layoutGeneration);
  return size_t(hash ^ (hash >> 32u));
}


War3GpuSkinResources::War3GpuSkinResources(
    Rc<DxvkDevice> device, const GpuSkinResourceBudgets& budgets)
: m_device(std::move(device)), m_budgets(budgets) {
  updateStorageBufferOffsetAlignment();
}

War3GpuSkinResources::~War3GpuSkinResources() {
  reset();
}

bool War3GpuSkinResources::beginFrame(
    uint64_t mapEpoch, uint64_t deviceEpoch, uint64_t frameTag) {
  if (m_device == nullptr || mapEpoch == 0u || deviceEpoch == 0u ||
      frameTag == 0u)
    return false;
  if ((m_mapEpoch != 0u && m_mapEpoch != mapEpoch) ||
      (m_deviceEpoch != 0u && m_deviceEpoch != deviceEpoch))
    return false;

  m_mapEpoch = mapEpoch;
  m_deviceEpoch = deviceEpoch;
  if (m_lastRetirementPollMapEpoch != mapEpoch ||
      m_lastRetirementPollDeviceEpoch != deviceEpoch ||
      m_lastRetirementPollFrameTag != frameTag) {
    pollRetired();
    m_lastRetirementPollMapEpoch = mapEpoch;
    m_lastRetirementPollDeviceEpoch = deviceEpoch;
    m_lastRetirementPollFrameTag = frameTag;
    publishResidencySnapshot();
  }
  return true;
}

void War3GpuSkinResources::invalidateMapEpoch(uint64_t nextMapEpoch) {
  clearEpochResources();
  m_mapEpoch = nextMapEpoch;
}

void War3GpuSkinResources::invalidateDevice(
    Rc<DxvkDevice> device, uint64_t nextDeviceEpoch) {
  clearEpochResources();
  m_device = std::move(device);
  updateStorageBufferOffsetAlignment();
  m_deviceEpoch = nextDeviceEpoch;
}

void War3GpuSkinResources::reset() {
  clearEpochResources();
  m_mapEpoch = 0;
  m_deviceEpoch = 0;
}

GpuSkinStaticResourceKey War3GpuSkinResources::makeKey(
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

GpuSkinStaticLookup War3GpuSkinResources::findOrQueueStatic(
    model::ShadowGeosetResourceSnapshot record,
    uint32_t layoutGeneration) {
  if (record == nullptr) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return { nullptr, GpuSkinFallbackReason::StaticResourceInvalid };
  }
  const model::ShadowGeosetResourceRecord& snapshot = *record;
  if (m_device == nullptr) {
    recordFallback(GpuSkinFallbackReason::DeviceLost);
    return { nullptr, GpuSkinFallbackReason::DeviceLost };
  }
  if (m_mapEpoch == 0u || m_deviceEpoch == 0u) {
    recordFallback(GpuSkinFallbackReason::InvalidEpoch);
    return { nullptr, GpuSkinFallbackReason::InvalidEpoch };
  }
  GpuSkinStaticSourceLayout requestedLayout;
  if (!GetValidatedStaticLayout(snapshot, &requestedLayout)) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return { nullptr, GpuSkinFallbackReason::StaticResourceInvalid };
  }

  const GpuSkinStaticResourceKey key = makeKey(snapshot, layoutGeneration);
  const auto ready = m_staticResources.find(key);
  if (ready != m_staticResources.end()) {
    if (ready->second->state == GpuSkinStaticResourceState::Ready) {
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
          ready->second->record->uvLayerCount == snapshot.uvLayerCount;
      if (!exactLayout) {
        recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
        return { nullptr, GpuSkinFallbackReason::StaticResourceInvalid };
      }
      ++m_diagnostics.staticCacheHits;
      return { ready->second, GpuSkinFallbackReason::None };
    }
    if (ready->second->state == GpuSkinStaticResourceState::Invalid) {
      recordFallback(GpuSkinFallbackReason::StaticBudgetExhausted);
      return { nullptr, GpuSkinFallbackReason::StaticBudgetExhausted };
    }
    recordFallback(GpuSkinFallbackReason::StaticResourcePending);
    return { nullptr, GpuSkinFallbackReason::StaticResourcePending };
  }

  const auto queued = std::find_if(m_staticMisses.begin(), m_staticMisses.end(),
      [&key](const QueuedStaticMiss& miss) { return miss.key == key; });
  if (queued != m_staticMisses.end()) {
    recordFallback(GpuSkinFallbackReason::StaticResourcePending);
    return { nullptr, GpuSkinFallbackReason::StaticResourcePending };
  }
  if (m_staticMisses.size() >= m_budgets.missQueueCapacity) {
    recordFallback(GpuSkinFallbackReason::MissQueueFull);
    return { nullptr, GpuSkinFallbackReason::MissQueueFull };
  }

  ++m_diagnostics.staticCacheMisses;
  // 持有该记录的不可变快照生命周期长于缓存锁；无论未命中队列还是最终静态资源，
  // 都不会再复制模型向量。
  m_staticMisses.push_back({ key, std::move(record) });
  // 共享缓存所有权不是独立的主机分配，因此 queuedStaticMissHostBytes 保持为零。
  m_peakQueuedStaticMissRecords = std::max<uint64_t>(
      m_peakQueuedStaticMissRecords, m_staticMisses.size());
  m_peakQueuedStaticMissHostBytes = std::max(
      m_peakQueuedStaticMissHostBytes, m_queuedStaticMissHostBytes);
  recordFallback(GpuSkinFallbackReason::StaticResourceMiss);
  return { nullptr, GpuSkinFallbackReason::StaticResourceMiss };
}

GpuSkinStaticLookup War3GpuSkinResources::probeStatic(
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
    // 只有真正未命中时，调用方才应复制完整模型记录并进入 findOrQueueStatic。
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
  if (!(resource->key == key) ||
      record == nullptr ||
      record->geosetDataPtr != stamp.geosetDataPtr ||
      record->contentHash != stamp.contentHash ||
      record->vertexCount != stamp.vertexCount ||
      resource->indexContentHash == 0u ||
      !resource->staticSource.defined() ||
      resource->staticSource.buffer() == nullptr ||
      resource->staticSource.length() != resource->sourceLayout.byteSize) {
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
    return {nullptr, GpuSkinFallbackReason::StaticResourceInvalid};
  }

  ++m_diagnostics.staticCacheHits;
  return {resource, GpuSkinFallbackReason::None};
}

uint32_t War3GpuSkinResources::prepareQueuedStaticResources(uint32_t maxRecords) {
  uint32_t prepared = 0;
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
      rejected->residencyCensus =
          RegisterStaticMirror(0u, false);
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

std::shared_ptr<GpuSkinStaticResource> War3GpuSkinResources::createStaticResource(
    const QueuedStaticMiss& miss) {
  if (miss.record == nullptr)
    return nullptr;
  const model::ShadowGeosetResourceRecord& record = *miss.record;
  GpuSkinStaticSourceLayout layout;
  if (m_device == nullptr || !GetValidatedStaticLayout(record, &layout))
    return nullptr;

  const VkDeviceSize deviceBytes = layout.byteSize;
  VkDeviceSize atlasOffset = 0u;
  if (!TryAlignUp(m_staticCursor, m_storageBufferOffsetAlignment,
                  atlasOffset))
    return nullptr;
  if (deviceBytes == 0u || atlasOffset > m_budgets.staticBytes ||
      deviceBytes > m_budgets.staticBytes - atlasOffset ||
      atlasOffset > std::numeric_limits<uint32_t>::max())
    return nullptr;

  if (m_staticAtlas == nullptr) {
    if (m_budgets.staticBytes == 0u ||
        m_budgets.staticBytes > std::numeric_limits<uint32_t>::max())
      return nullptr;
    m_staticAtlas = m_device->createBuffer(
        StaticBufferInfo(m_budgets.staticBytes,
                         "War3GpuSkinStaticAtlas"),
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
  resource->maxVertexGroupSlot = *std::max_element(
      record.vertexGroupIndices.begin(), record.vertexGroupIndices.end());
  resource->sourceLayout = layout;
  resource->allocatedBytes = deviceBytes;
  resource->residencyCensus =
      RegisterStaticMirror(deviceBytes, true);

  Rc<DxvkBuffer> staging = m_device->createBuffer(
      UploadBufferInfo(deviceBytes, "War3GpuSkinStaticSourceUpload"),
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (staging == nullptr || staging->mapPtr(0u) == nullptr)
    return nullptr;
  resource_census::ResourceHandle stagingCensus = RegisterGpuBuffer(
      resource_census::ResourceClass::GpuSkinStaticUploadStaging,
      staging, deviceBytes, true, true);

  auto* blob = reinterpret_cast<uint8_t*>(staging->mapPtr(0u));
  std::memset(blob, 0, size_t(deviceBytes));
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

  resource->staticSource = DxvkBufferSlice(
      m_staticAtlas, atlasOffset, deviceBytes);
  resource->pendingUpload = {
      resource->key,
      DxvkBufferSlice(std::move(staging), 0u, deviceBytes),
      resource->staticSource,
      deviceBytes,
      std::move(stagingCensus) };

  m_staticCursor = atlasOffset + deviceBytes;
  m_staticBytes = m_staticCursor;
  ++m_diagnostics.staticUploadsPrepared;
  m_diagnostics.staticUploadBytes += deviceBytes;
  return resource;
}

std::vector<GpuSkinStaticUpload> War3GpuSkinResources::takeStaticUploads() {
  std::vector<GpuSkinStaticUpload> uploads;
  uploads.swap(m_readyStaticUploads);
  return uploads;
}

bool War3GpuSkinResources::retireStaticUpload(
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
  if (found != m_staticResources.end() &&
      found->second->state == GpuSkinStaticResourceState::Ready) {
    return true;
  }

  bool exactPendingUpload = false;
  if (found != m_staticResources.end() &&
      found->second->state == GpuSkinStaticResourceState::PendingUpload) {
    const GpuSkinStaticUpload& pending = found->second->pendingUpload;
    exactPendingUpload = pending.source.defined() &&
        pending.destination.defined() && pending.byteCount == upload.byteCount &&
        pending.source.buffer() == upload.source.buffer() &&
        pending.source.offset() == upload.source.offset() &&
        pending.source.length() == upload.source.length() &&
        pending.destination.buffer() == upload.destination.buffer() &&
        pending.destination.offset() == upload.destination.offset() &&
        pending.destination.length() == upload.destination.length();
    if (exactPendingUpload) {
      found->second->pendingUpload = {};
      found->second->state = GpuSkinStaticResourceState::Ready;
      resource_census::UpdateHostBacking(
          found->second->residencyCensus, 0u, 0u, 0u);
      // 这里只标记精确 atlas 上传事务已被接受，并纳入下方 producer fence 管理。
      // 该信息仅供诊断，绝不授权释放仍属于 P4 精确内容证明的 CPU mirror。
      resource_census::NoteDeviceUpload(
          found->second->residencyCensus, 0u);
    }
  }

    // 即使缓存账本已经失效，已记录 batch 自有的 source slice 仍是生命周期权威。
    // host producer fence 完成前绝不能释放它。
  m_retiredStaticUploads.push_back({
      upload.key, upload.source, std::move(fence), value,
      upload.residencyCensus });
  ++m_diagnostics.staticUploadRetirementsQueued;
  if (!exactPendingUpload)
    recordFallback(GpuSkinFallbackReason::StaticResourceInvalid);
  return true;
}

DxvkBufferSlice War3GpuSkinResources::staticAtlasSlice() const {
  if (m_staticAtlas == nullptr || m_staticCursor == 0u)
    return {};
  return DxvkBufferSlice(m_staticAtlas, 0u, m_staticCursor);
}

bool War3GpuSkinResources::ensureUploadPage(VkDeviceSize requiredBytes) {
  if (m_device == nullptr || requiredBytes == 0u ||
      requiredBytes > kMaxUploadPageBytes)
    return false;

  if (m_activeUpload != nullptr) {
    VkDeviceSize offset = 0u;
    if (!TryAlignUp(m_activeUpload->cursor,
                    m_storageBufferOffsetAlignment, offset))
      return false;
    if (offset <= m_activeUpload->capacity &&
        requiredBytes <= m_activeUpload->capacity - offset)
      return true;
    if (m_activeUpload->cursor != 0u)
      m_pendingUploads.push_back(std::move(m_activeUpload));
    else {
      if (!m_activeUpload->reclamationCounted) {
        ++m_diagnostics.uploadPagesReclaimed;
        m_activeUpload->reclamationCounted = true;
      }
      m_uploadBytes -= m_activeUpload->capacity;
      m_activeUpload.reset();
    }
  }

  // 常见路径只向 active page 追加，不需要处理 fence。仅在切页路径轮询，
  // 因为只有此时回收的字节才会影响 admission。
  pollRetired();

  VkDeviceSize roundedRequiredBytes = 0u;
  if (!TryAlignUp(requiredBytes, kMinUploadPageBytes, roundedRequiredBytes))
    return false;
  const VkDeviceSize capacity = std::max(kMinUploadPageBytes,
                                         roundedRequiredBytes);
  if (capacity > kMaxUploadPageBytes)
    return false;

  // 优先选择能满足请求的最小 page；相同容量的 page 保持退休顺序，
  // 使复用结果具备确定性。
  auto reusable = m_idleUploads.end();
  for (auto it = m_idleUploads.begin(); it != m_idleUploads.end(); ++it) {
    if ((*it)->capacity < requiredBytes)
      continue;
    if (reusable == m_idleUploads.end() ||
        (*it)->capacity < (*reusable)->capacity) {
      reusable = it;
    }
  }
  if (reusable != m_idleUploads.end()) {
    m_activeUpload = std::move(*reusable);
    m_idleUploads.erase(reusable);
    m_activeUpload->cursor = 0u;
    m_activeUpload->fence = nullptr;
    m_activeUpload->retireValue = 0u;
    return true;
  }

  if (capacity > m_budgets.uploadBytes)
    return false;

  const auto fitsBudget = [this, capacity]() {
    return m_uploadBytes <= m_budgets.uploadBytes &&
        capacity <= m_budgets.uploadBytes - m_uploadBytes;
  };
  while (!fitsBudget() && !m_idleUploads.empty()) {
  // 没有 idle page 能容纳请求时，先驱逐最大的 page，以最少的物理 buffer 析构
  // 释放所需预算；容量相同则维持 FIFO 退休顺序。
    auto victim = m_idleUploads.begin();
    for (auto it = m_idleUploads.begin(); it != m_idleUploads.end(); ++it) {
      if ((*it)->capacity > (*victim)->capacity)
        victim = it;
    }
    m_uploadBytes -= (*victim)->capacity;
    m_idleUploads.erase(victim);
  }
  if (!fitsBudget())
    return false;

  auto page = std::make_unique<UploadPage>();
  page->capacity = capacity;
  page->buffer = m_device->createBuffer(
      UploadBufferInfo(capacity, "War3GpuSkinUploadPage"),
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (page->buffer == nullptr || page->buffer->mapPtr(0u) == nullptr)
    return false;
  page->residencyCensus = RegisterGpuBuffer(
      resource_census::ResourceClass::GpuSkinUploadPage,
      page->buffer, capacity, true, false);

  m_uploadBytes += capacity;
  ++m_diagnostics.uploadPagesAllocated;
  m_activeUpload = std::move(page);
  return true;
}

GpuSkinBatchUpload War3GpuSkinResources::allocateBatchUpload(
    VkDeviceSize paletteByteCount, VkDeviceSize jobByteCount) {
  GpuSkinBatchUpload result;
  if (paletteByteCount == 0u || jobByteCount == 0u) {
    result.fallback = GpuSkinFallbackReason::InvalidJob;
    recordFallback(result.fallback);
    return result;
  }

  VkDeviceSize jobOffset = 0u;
  if (!TryAlignUp(paletteByteCount, m_storageBufferOffsetAlignment,
                  jobOffset) ||
      jobOffset > kMaxUploadPageBytes ||
      jobByteCount > kMaxUploadPageBytes - jobOffset ||
      !ensureUploadPage(jobOffset + jobByteCount)) {
    result.fallback = GpuSkinFallbackReason::UploadBudgetExhausted;
    recordFallback(result.fallback);
    return result;
  }

  VkDeviceSize baseOffset = 0u;
  if (!TryAlignUp(m_activeUpload->cursor,
                  m_storageBufferOffsetAlignment, baseOffset) ||
      baseOffset > m_activeUpload->capacity ||
      jobOffset > m_activeUpload->capacity - baseOffset ||
      jobByteCount > m_activeUpload->capacity - baseOffset - jobOffset) {
    result.fallback = GpuSkinFallbackReason::UploadBudgetExhausted;
    recordFallback(result.fallback);
    return result;
  }

  const VkDeviceSize palettePageOffset = baseOffset;
  const VkDeviceSize jobPageOffset = baseOffset + jobOffset;
  const VkDeviceSize endOffset = jobPageOffset + jobByteCount;

  m_activeUpload->cursor = endOffset;
  result.palette = {
      DxvkBufferSlice(m_activeUpload->buffer, palettePageOffset,
                      paletteByteCount),
      m_activeUpload->buffer->mapPtr(palettePageOffset),
      GpuSkinFallbackReason::None };
  result.jobs = {
      DxvkBufferSlice(m_activeUpload->buffer, jobPageOffset, jobByteCount),
      m_activeUpload->buffer->mapPtr(jobPageOffset),
      GpuSkinFallbackReason::None };
  m_diagnostics.paletteUploadBytes += paletteByteCount;
  m_diagnostics.jobUploadBytes += jobByteCount;
  return result;
}

GpuSkinUploadSlice War3GpuSkinResources::allocatePaletteUpload(
    VkDeviceSize paletteByteCount) {
  GpuSkinUploadSlice result;
  if (paletteByteCount == 0u ||
      paletteByteCount > kMaxUploadPageBytes ||
      !ensureUploadPage(paletteByteCount)) {
    result.fallback = paletteByteCount == 0u
        ? GpuSkinFallbackReason::InvalidPalette
        : GpuSkinFallbackReason::UploadBudgetExhausted;
    recordFallback(result.fallback);
    return result;
  }

  VkDeviceSize baseOffset = 0u;
  if (!TryAlignUp(m_activeUpload->cursor,
                  m_storageBufferOffsetAlignment, baseOffset) ||
      baseOffset > m_activeUpload->capacity ||
      paletteByteCount > m_activeUpload->capacity - baseOffset) {
    result.fallback = GpuSkinFallbackReason::UploadBudgetExhausted;
    recordFallback(result.fallback);
    return result;
  }

  m_activeUpload->cursor = baseOffset + paletteByteCount;
  result.slice = DxvkBufferSlice(
      m_activeUpload->buffer, baseOffset, paletteByteCount);
  result.mapPtr = m_activeUpload->buffer->mapPtr(baseOffset);
  m_diagnostics.paletteUploadBytes += paletteByteCount;
  return result;
}

bool War3GpuSkinResources::retireUploads(
    Rc<DxvkFence> fence, uint64_t value) {
  if (m_activeUpload == nullptr && m_pendingUploads.empty())
    return true;
  if (fence == nullptr || value == 0u) {
    recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
    return false;
  }
  for (auto& page : m_pendingUploads) {
    page->fence = fence;
    page->retireValue = value;
    m_retiredUploads.push_back(std::move(page));
  }
  m_pendingUploads.clear();
  if (m_activeUpload != nullptr) {
    m_activeUpload->fence = std::move(fence);
    m_activeUpload->retireValue = value;
    m_retiredUploads.push_back(std::move(m_activeUpload));
  }
  return true;
}

void War3GpuSkinResources::discardUploads() {
  for (auto& page : m_pendingUploads) {
    if (!page->reclamationCounted) {
      ++m_diagnostics.uploadPagesReclaimed;
      page->reclamationCounted = true;
    }
    m_uploadBytes -= page->capacity;
  }
  m_pendingUploads.clear();
  if (m_activeUpload != nullptr) {
    if (!m_activeUpload->reclamationCounted) {
      ++m_diagnostics.uploadPagesReclaimed;
      m_activeUpload->reclamationCounted = true;
    }
    m_uploadBytes -= m_activeUpload->capacity;
    m_activeUpload.reset();
  }
}

OutputLease War3GpuSkinResources::allocateOutput(const OutputLeaseDesc& desc) {
  OutputLease result;
  result.desc = desc;
  if (m_device == nullptr || desc.byteLength == 0u ||
      desc.mapEpoch != m_mapEpoch || desc.deviceEpoch != m_deviceEpoch) {
    result.fallback = GpuSkinFallbackReason::InvalidEpoch;
    recordFallback(result.fallback);
    return result;
  }

  const VkDeviceSize bytes = desc.byteLength;
  const auto findAvailablePage = [this, bytes]() -> OutputPage* {
    for (auto it = m_outputPages.rbegin(); it != m_outputPages.rend(); ++it) {
      VkDeviceSize offset = 0u;
      if (!TryAlignUp((*it)->cursor, m_storageBufferOffsetAlignment, offset))
        continue;
      if (offset <= (*it)->capacity && bytes <= (*it)->capacity - offset)
        return it->get();
    }
    return nullptr;
  };

  OutputPage* page = findAvailablePage();
  if (page == nullptr) {
  // beginFrame 已执行常规退休轮询。只有分配压力确实能利用回收的 output 空间时，
  // 才在这里再次检查 fence；否则每个 job 都轮询会把 assembly 变成 O(J*R)。
    pollRetired();
    page = findAvailablePage();
  }

  if (page == nullptr) {
    const VkDeviceSize capacity = std::max(kOutputPageBytes, bytes);
    if (m_outputBytes > m_budgets.outputBytes ||
        capacity > m_budgets.outputBytes - m_outputBytes) {
      result.fallback = GpuSkinFallbackReason::OutputBudgetExhausted;
      recordFallback(result.fallback);
      return result;
    }
    auto newPage = std::make_unique<OutputPage>();
    newPage->id = m_nextOutputPageId++;
    newPage->capacity = capacity;
    newPage->buffer = m_device->createBuffer(OutputBufferInfo(capacity),
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (newPage->buffer == nullptr) {
      result.fallback = GpuSkinFallbackReason::OutputBudgetExhausted;
      recordFallback(result.fallback);
      return result;
    }
    newPage->residencyCensus = RegisterGpuBuffer(
        resource_census::ResourceClass::GpuSkinOutputPage,
        newPage->buffer, capacity, false, false);
    m_outputBytes += capacity;
    ++m_diagnostics.outputPagesAllocated;
    page = newPage.get();
    m_outputPages.push_back(std::move(newPage));
  }

  VkDeviceSize offset = 0u;
  if (!TryAlignUp(page->cursor, m_storageBufferOffsetAlignment, offset) ||
      offset > std::numeric_limits<uint32_t>::max() ||
      offset > page->capacity || bytes > page->capacity - offset) {
    result.fallback = GpuSkinFallbackReason::OutputBudgetExhausted;
    recordFallback(result.fallback);
    return result;
  }
  page->cursor = offset + bytes;
  ++page->outstanding;
  result.desc.byteOffset = uint32_t(offset);
  result.slice = DxvkBufferSlice(page->buffer, offset, bytes);
  result.leaseId = m_nextOutputLeaseId++;
  result.pageGeneration = page->generation;
  result.pageId = page->id;
  m_activeOutputs.emplace(result.leaseId,
      ActiveOutput { result.pageId, result.pageGeneration,
                     result.desc.byteOffset,
                     result.desc.byteLength, false });
  m_diagnostics.outputLeaseBytes += bytes;
  ++m_diagnostics.outputLeasePending;
  return result;
}

bool War3GpuSkinResources::retireOutput(
    const OutputLease& lease, Rc<DxvkFence> fence, uint64_t value) {
  if (!lease || fence == nullptr || value == 0u ||
      lease.desc.mapEpoch != m_mapEpoch || lease.desc.deviceEpoch != m_deviceEpoch) {
    recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
    return false;
  }
  const auto active = m_activeOutputs.find(lease.leaseId);
  if (active == m_activeOutputs.end() ||
      active->second.pageId != lease.pageId ||
      active->second.pageGeneration != lease.pageGeneration ||
      active->second.byteOffset != lease.desc.byteOffset ||
      active->second.byteLength != lease.desc.byteLength) {
    recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
    return false;
  }
  if (active->second.retirementQueued)
    return true;
  const auto page = std::find_if(m_outputPages.begin(), m_outputPages.end(),
      [&lease](const std::unique_ptr<OutputPage>& candidate) {
        return candidate->id == lease.pageId &&
            candidate->generation == lease.pageGeneration;
      });
  if (page == m_outputPages.end() || (*page)->outstanding == 0u) {
    recordFallback(GpuSkinFallbackReason::OutputLeaseUnretired);
    return false;
  }
  OutputLease retiredLease = lease;
  retiredLease.retireFence = std::move(fence);
  retiredLease.retireValue = value;
  active->second.retirementQueued = true;
  m_retiredOutputs.push_back({ std::move(retiredLease) });
  return true;
}

void War3GpuSkinResources::discardOutput(const OutputLease& lease) {
  if (!lease)
    return;
  const auto active = m_activeOutputs.find(lease.leaseId);
  if (active == m_activeOutputs.end() ||
      active->second.pageId != lease.pageId ||
      active->second.pageGeneration != lease.pageGeneration ||
      active->second.byteOffset != lease.desc.byteOffset ||
      active->second.byteLength != lease.desc.byteLength ||
      active->second.retirementQueued)
    return;
  const auto page = std::find_if(m_outputPages.begin(), m_outputPages.end(),
      [&lease](const std::unique_ptr<OutputPage>& candidate) {
        return candidate->id == lease.pageId &&
            candidate->generation == lease.pageGeneration;
      });
  if (page != m_outputPages.end() && (*page)->outstanding != 0u) {
    --(*page)->outstanding;
    if ((*page)->outstanding == 0u) {
      (*page)->cursor = 0u;
      if (++(*page)->generation == 0u)
        ++(*page)->generation;
    }
    if (m_diagnostics.outputLeasePending != 0u)
      --m_diagnostics.outputLeasePending;
  }
  m_activeOutputs.erase(active);
}

void War3GpuSkinResources::pollRetired() {
  if (m_retiredStaticUploads.empty() && m_retiredUploads.empty() &&
      m_retiredOutputs.empty()) {
    return;
  }

  struct FencePollSample {
    DxvkFence* fence;
    uint64_t value;
  };
  std::array<FencePollSample, 64> fenceSamples;
  size_t fenceSampleCount = 0u;
  const auto getFenceValue = [&](const Rc<DxvkFence>& fence) {
    DxvkFence* const identity = fence.ptr();
    if (identity == nullptr)
      return uint64_t{0};
    for (size_t i = 0u; i < fenceSampleCount; ++i) {
      if (fenceSamples[i].fence == identity) {
        ++m_diagnostics.retirementFenceCacheHits;
        return fenceSamples[i].value;
      }
    }
    ++m_diagnostics.retirementFenceQueries;
    const uint64_t value = fence->getValue();
    if (fenceSampleCount < fenceSamples.size())
      fenceSamples[fenceSampleCount++] = {identity, value};
    return value;
  };

  for (auto it = m_retiredStaticUploads.begin();
       it != m_retiredStaticUploads.end();) {
    if (it->fence != nullptr && getFenceValue(it->fence) >= it->retireValue) {
      ++m_diagnostics.staticUploadRetirementsReclaimed;
      it = m_retiredStaticUploads.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = m_retiredUploads.begin(); it != m_retiredUploads.end();) {
    if (it->get()->fence != nullptr &&
        getFenceValue(it->get()->fence) >= it->get()->retireValue) {
      std::unique_ptr<UploadPage> page = std::move(*it);
      it = m_retiredUploads.erase(it);
      page->cursor = 0u;
      page->fence = nullptr;
      page->retireValue = 0u;
      if (!page->reclamationCounted) {
        ++m_diagnostics.uploadPagesReclaimed;
        page->reclamationCounted = true;
      }
      const VkDeviceSize pageCapacity = page->capacity;
      try {
        m_idleUploads.push_back(std::move(page));
      } catch (...) {
      // fence 已完成，可以安全析构该 page；即使扩展 idle 容器本身失败，
      // 也必须保持预算账本精确。
        m_uploadBytes -= pageCapacity;
        throw;
      }
    } else {
      ++it;
    }
  }

  for (auto it = m_retiredOutputs.begin(); it != m_retiredOutputs.end();) {
    if (it->lease.retireFence != nullptr &&
        getFenceValue(it->lease.retireFence) >= it->lease.retireValue) {
      const uint32_t pageId = it->lease.pageId;
      const uint64_t pageGeneration = it->lease.pageGeneration;
      const auto active = m_activeOutputs.find(it->lease.leaseId);
      const auto page = std::find_if(m_outputPages.begin(), m_outputPages.end(),
          [pageId, pageGeneration](
              const std::unique_ptr<OutputPage>& candidate) {
            return candidate->id == pageId &&
                candidate->generation == pageGeneration;
          });
      if (active != m_activeOutputs.end() &&
          active->second.pageId == pageId &&
          active->second.pageGeneration == pageGeneration &&
          active->second.retirementQueued &&
          page != m_outputPages.end() && (*page)->outstanding != 0u) {
        --(*page)->outstanding;
        if ((*page)->outstanding == 0u) {
          (*page)->cursor = 0u;
          if (++(*page)->generation == 0u)
            ++(*page)->generation;
        }
        m_activeOutputs.erase(active);
      }
      if (m_diagnostics.outputLeasePending != 0u)
        --m_diagnostics.outputLeasePending;
      ++m_diagnostics.outputLeaseRetired;
      it = m_retiredOutputs.erase(it);
    } else {
      ++it;
    }
  }

  for (const auto& page : m_outputPages) {
    if (page->outstanding == 0u)
      page->cursor = 0u;
  }
}

void War3GpuSkinResources::recordJobFallback(
    uint64_t frameTag, uint32_t token, GpuSkinFallbackReason reason) {
  if (reason == GpuSkinFallbackReason::None)
    return;
  if (m_budgets.jobFallbackCapacity == 0u) {
    recordFallback(reason);
    return;
  }
  if (m_jobFallbacks.size() >= m_budgets.jobFallbackCapacity)
    m_jobFallbacks.pop_front();
  m_jobFallbacks.push_back({ frameTag, token, reason, 0u });
  recordFallback(reason);
}

std::vector<GpuSkinJobFallback> War3GpuSkinResources::takeJobFallbacks() {
  std::vector<GpuSkinJobFallback> result;
  result.reserve(m_jobFallbacks.size());
  while (!m_jobFallbacks.empty()) {
    result.push_back(m_jobFallbacks.front());
    m_jobFallbacks.pop_front();
  }
  return result;
}

const GpuSkinResourceBudgets& War3GpuSkinResources::budgets() const {
  return m_budgets;
}

GpuSkinDiagnostics War3GpuSkinResources::diagnostics() const {
  return m_diagnostics;
}

VkDeviceSize War3GpuSkinResources::storageBufferOffsetAlignment() const {
  return m_storageBufferOffsetAlignment;
}

uint64_t War3GpuSkinResources::mapEpoch() const {
  return m_mapEpoch;
}

uint64_t War3GpuSkinResources::deviceEpoch() const {
  return m_deviceEpoch;
}

bool War3GpuSkinResources::hasInFlightResources() const {
  return !m_retiredStaticUploads.empty() || m_activeUpload != nullptr ||
         !m_pendingUploads.empty() ||
         !m_retiredUploads.empty() || !m_activeOutputs.empty() ||
         !m_retiredOutputs.empty();
}

void War3GpuSkinResources::updateStorageBufferOffsetAlignment() {
  const VkDeviceSize deviceAlignment = m_device != nullptr
      ? m_device->properties().core.properties.limits
          .minStorageBufferOffsetAlignment
      : 0u;
  m_storageBufferOffsetAlignment =
      NormalizeStorageBufferOffsetAlignment(deviceAlignment);
  m_diagnostics.storageBufferOffsetAlignment = m_storageBufferOffsetAlignment;
}

void War3GpuSkinResources::recordFallback(GpuSkinFallbackReason reason) {
  ++m_diagnostics.fallbackCount;
  const size_t index = static_cast<size_t>(reason);
  if (index < 16u)
    ++m_diagnostics.fallbackByReason[index];
}

void War3GpuSkinResources::publishResidencySnapshot() const {
  if (!resource_census::Enabled())
    return;

  resource_census::GpuSkinPoolResidencySnapshot snapshot = {};
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
  for (const auto& upload : m_retiredStaticUploads)
    snapshot.retiredStaticUploadBytes += uint64_t(upload.source.length());

  snapshot.uploadResidentBytes = uint64_t(m_uploadBytes);
  if (m_activeUpload != nullptr) {
    snapshot.uploadActivePages = 1u;
    snapshot.uploadActiveCapacityBytes = uint64_t(m_activeUpload->capacity);
    snapshot.uploadActiveUsedBytes = uint64_t(m_activeUpload->cursor);
  }
  for (const auto& page : m_pendingUploads) {
    ++snapshot.uploadPendingPages;
    snapshot.uploadPendingCapacityBytes += uint64_t(page->capacity);
    snapshot.uploadPendingUsedBytes += uint64_t(page->cursor);
  }
  for (const auto& page : m_retiredUploads) {
    ++snapshot.uploadRetiredPages;
    snapshot.uploadRetiredCapacityBytes += uint64_t(page->capacity);
    snapshot.uploadRetiredUsedBytes += uint64_t(page->cursor);
  }
  for (const auto& page : m_idleUploads) {
    ++snapshot.uploadIdlePages;
    snapshot.uploadIdleCapacityBytes += uint64_t(page->capacity);
  }

  snapshot.outputResidentBytes = uint64_t(m_outputBytes);
  snapshot.outputPages = m_outputPages.size();
  for (const auto& page : m_outputPages) {
    snapshot.outputCapacityBytes += uint64_t(page->capacity);
    snapshot.outputCursorBytes += uint64_t(page->cursor);
    snapshot.outputOutstandingSlices += page->outstanding;
  }
  snapshot.outputActiveLeases = m_activeOutputs.size();
  snapshot.outputRetiredLeases = m_retiredOutputs.size();
  resource_census::UpdateGpuSkinPoolSnapshot(snapshot);
}

void War3GpuSkinResources::clearEpochResources() {
  m_staticResources.clear();
  m_staticMisses.clear();
  m_jobFallbacks.clear();
  m_readyStaticUploads.clear();
  m_staticAtlas = nullptr;
  m_staticAtlasCensus.reset();
  m_activeUpload.reset();
  m_pendingUploads.clear();
  m_retiredUploads.clear();
  m_idleUploads.clear();
  m_outputPages.clear();
  m_activeOutputs.clear();
  m_retiredStaticUploads.clear();
  m_retiredOutputs.clear();
  m_staticBytes = 0;
  m_staticCursor = 0;
  m_uploadBytes = 0;
  m_outputBytes = 0;
  m_queuedStaticMissHostBytes = 0;
  m_lastRetirementPollMapEpoch = 0;
  m_lastRetirementPollDeviceEpoch = 0;
  m_lastRetirementPollFrameTag = 0;
  m_nextOutputLeaseId = 1;
  m_nextOutputPageId = 1;
  publishResidencySnapshot();
}

}  // namespace dxvk::war3::gpu_skin
