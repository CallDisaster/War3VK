#include "war3_gpu_skin_manager.h"

#include "../core/war3_memory.h"
#include "../model/war3_model_resource_cache.h"
#include "../../../util/log/log.h"
#include "../../../util/util_small_vector.h"
#include "../../../util/util_time.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dxvk::war3::gpu_skin {

namespace {

constexpr uintptr_t kGlobalPaletteBufferRva = 0xBC6BD0u;
constexpr size_t kRenderablePaletteSlotOffset = 0x08u;
constexpr size_t kRenderableGeosetDataOffset = 0x0Cu;
constexpr size_t kGeosetPaletteGroupCountOffset = 0xF0u;
constexpr size_t kGeosetPrimitiveCountOffset = 0xC8u;
constexpr size_t kGeosetPrimitiveRecordsOffset = 0xCCu;
constexpr size_t kGeosetIndexCountOffset = 0xDCu;
constexpr size_t kGeosetIndicesOffset = 0xE0u;
constexpr size_t kGeosetPrimitiveSnapshotBeginOffset =
    kGeosetPrimitiveCountOffset;
constexpr size_t kGeosetPrimitiveSnapshotEndOffset =
    kGeosetIndicesOffset + sizeof(uint32_t);
constexpr size_t kGeosetPrimitiveSnapshotSize =
    kGeosetPrimitiveSnapshotEndOffset -
    kGeosetPrimitiveSnapshotBeginOffset;
static_assert(kGeosetPrimitiveSnapshotSize == 0x1Cu);
constexpr uint32_t kInvalidPaletteSlot = 0xFFFFFFFFu;
constexpr uint32_t kMaxPaletteSlots = 0x3A98u;
constexpr uint32_t kMaxPaletteGroups = 256u;
constexpr uint32_t kMaxNativeVertices = 0x4000u;
constexpr uint32_t kMaxNativeIndices = 0xC000u;
// The native format-2 SSE kernel is cheaper than proof + resource + dispatch
// setup for the dominant small-geoset population.  The latest exact bucket
// histogram contains 344 jobs at 1..64 vertices, 2908 at 193..448, and 1252
// at 449..960.  Keeping bucket 3+ on GPU removes 72.2% of production jobs
// while retaining a tightly bounded 30.6%..63.0% of transformed vertices.
static_assert(GetGpuSkinDispatchGroupCount(kMaxNativeVertices) ==
              kGpuSkinMaxDispatchGroupCountX);
static_assert(GetGpuSkinDispatchVertexBucket(kMaxNativeVertices) ==
              kGpuSkinVertexBucketCount - 1u);
static_assert(GetGpuSkinDispatchVertexBucket(kProductionGpuMinVertices) == 3u);
constexpr size_t kMaxLiveProofBytes =
    size_t(kMaxNativeVertices) * 3u * sizeof(float);
static_assert(kMaxLiveProofBytes >=
              size_t(kMaxNativeIndices) * sizeof(uint16_t));
static_assert(kMaxLiveProofBytes >=
              size_t(kMaxNativeVertices) * 2u * sizeof(float));
static_assert(kMaxLiveProofBytes >= size_t(kMaxPaletteGroups) * 48u);
constexpr uint32_t kMaxFlushElements = 65536u;
constexpr uint32_t kMaxJobsPerFlush = 512u;
static_assert(kMaxJobsPerFlush <=
              kNativeDispatchCpuOnlySealViewCapacity);
constexpr uint32_t kMaxStaticPreparesPerFlush = 32u;
constexpr size_t kMaxRetiredResourceEpochs = 8u;
constexpr size_t kMaxRetiringClaims = 8u;
constexpr size_t kMaxAutoRetiredBatchTombstones = 32u;
constexpr uint32_t kStaticPackingLayoutGeneration = 1u;
constexpr uint32_t kDispatchSpecialMask = 3u;
constexpr uint32_t kDispatchSpecialValue = 3u;
constexpr uint32_t kGxPrimitiveTriangleList = 3u;
constexpr uint32_t kD3dPrimitiveTriangleList = 4u;

class RawTickAccumulator {
public:
  RawTickAccumulator(bool enabled, uint64_t& calls, uint64_t& total,
                     uint64_t& maximum)
      : m_enabled(enabled), m_calls(calls), m_total(total),
        m_maximum(maximum),
        m_start(enabled ? dxvk::high_resolution_clock::get_counter() : 0) {
    if (m_enabled)
      ++m_calls;
  }

  RawTickAccumulator(bool enabled, GpuSkinRawTimingDiagnostics& timing)
      : RawTickAccumulator(enabled, timing.calls, timing.ticks,
                           timing.maxTicks) {
  }

  ~RawTickAccumulator() {
    if (!m_enabled)
      return;
    const int64_t elapsed =
        dxvk::high_resolution_clock::get_counter() - m_start;
    if (elapsed <= 0)
      return;
    const uint64_t ticks = uint64_t(elapsed);
    m_total += ticks;
    m_maximum = std::max(m_maximum, ticks);
  }

private:
  bool m_enabled;
  uint64_t& m_calls;
  uint64_t& m_total;
  uint64_t& m_maximum;
  int64_t m_start;
};

inline void RecordRawTiming(GpuSkinRawTimingDiagnostics& timing,
                            int64_t elapsed) noexcept {
  if (elapsed <= 0)
    return;
  const uint64_t ticks = uint64_t(elapsed);
  ++timing.calls;
  timing.ticks += ticks;
  timing.maxTicks = std::max(timing.maxTicks, ticks);
}

// Sampled callback call counts are contractual even when two adjacent QPC
// reads happen to be equal. This intentionally differs from RecordRawTiming,
// whose unsampled/full-diagnostics callers historically omit zero-tick calls.
inline void RecordSampledNativeThunkTiming(
    GpuSkinRawTimingDiagnostics& timing, int64_t elapsed) noexcept {
  ++timing.calls;
  if (elapsed <= 0)
    return;
  const uint64_t ticks = uint64_t(elapsed);
  timing.ticks += ticks;
  timing.maxTicks = std::max(timing.maxTicks, ticks);
}

// Records mutually-exclusive phases with one QPC read at each boundary.  It
// is used only after a sparse positive admission hit; early returns are charged
// to the phase that rejected the candidate.
class RawPhaseTimer {
public:
  RawPhaseTimer(bool enabled,
                GpuSkinRawTimingDiagnostics& timing) noexcept
      : m_enabled(enabled), m_timing(enabled ? &timing : nullptr),
        m_start(enabled ? dxvk::high_resolution_clock::get_counter() : 0) {
  }

  ~RawPhaseTimer() {
    finish();
  }

  void switchTo(GpuSkinRawTimingDiagnostics& timing) noexcept {
    if (!m_enabled)
      return;
    finish();
    m_timing = &timing;
    m_start = dxvk::high_resolution_clock::get_counter();
  }

private:
  void finish() noexcept {
    if (m_timing == nullptr)
      return;
    const int64_t elapsed =
        dxvk::high_resolution_clock::get_counter() - m_start;
    RecordRawTiming(*m_timing, elapsed);
    m_timing = nullptr;
  }

  bool m_enabled = false;
  GpuSkinRawTimingDiagnostics* m_timing = nullptr;
  int64_t m_start = 0;
};

// The native protocol is overwhelmingly one dispatch / one active upload /
// one or two DIPs, but malformed or nested input must still be represented
// exactly. Keep the common path allocation-free and spill to heap storage only
// when the inline capacity is exceeded.
template <typename Key, typename Value, size_t InlineCapacity>
class InlineFlatMap {
  static_assert(InlineCapacity != 0u);

  using Entry = std::pair<Key, Value>;
  using Storage = small_vector<Entry, InlineCapacity>;

public:
  using iterator = typename Storage::iterator;
  using const_iterator = typename Storage::const_iterator;

  iterator begin() { return m_entries.begin(); }
  iterator end() { return m_entries.end(); }
  const_iterator begin() const { return m_entries.begin(); }
  const_iterator end() const { return m_entries.end(); }

  bool empty() const { return m_entries.empty(); }
  size_t size() const { return m_entries.size(); }
  void clear() { m_entries.clear(); }

  iterator find(const Key& key) {
    return std::find_if(begin(), end(), [&](const Entry& entry) {
      return entry.first == key;
    });
  }

  const_iterator find(const Key& key) const {
    return std::find_if(begin(), end(), [&](const Entry& entry) {
      return entry.first == key;
    });
  }

  template <typename KeyArg, typename ValueArg>
  std::pair<iterator, bool> emplace(KeyArg&& key, ValueArg&& value) {
    const Key& lookupKey = key;
    const iterator existing = find(lookupKey);
    if (existing != end())
      return {existing, false};
    m_entries.emplace_back(std::forward<KeyArg>(key),
                           std::forward<ValueArg>(value));
    return {end() - 1u, true};
  }

  Value& operator[](const Key& key) {
    const iterator existing = find(key);
    if (existing != end())
      return existing->second;
    return m_entries.emplace_back(key, Value()).second;
  }

  iterator erase(iterator entry) {
    return m_entries.erase(entry);
  }

  size_t erase(const Key& key) {
    const iterator existing = find(key);
    if (existing == end())
      return 0u;
    m_entries.erase(existing);
    return 1u;
  }

private:
  Storage m_entries;
};

template <typename T, typename = void>
struct HasNativeRangePoisoned : std::false_type {};

template <typename T>
struct HasNativeRangePoisoned<
    T, std::void_t<decltype(std::declval<const T&>().nativeRangePoisoned)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasNativeSuppressionFlag : std::false_type {};

template <typename T>
struct HasNativeSuppressionFlag<
    T, std::void_t<decltype(std::declval<const T&>().requiresSuppression)>>
    : std::true_type {};

template <typename T>
bool NativeDipRangePoisonedImpl(const T& observation) {
  bool poisoned = observation.sourceUploadKernelBypassed;
  if constexpr (HasNativeRangePoisoned<T>::value)
    poisoned = poisoned || observation.nativeRangePoisoned;
  if constexpr (HasNativeSuppressionFlag<T>::value)
    poisoned = poisoned || observation.requiresSuppression;
  return poisoned;
}

bool NativeDipRangePoisoned(const NativeDipObservation& observation) {
  return NativeDipRangePoisonedImpl(observation);
}

// NativeFlushObservation exposes the confirmed 20-byte 32-bit queue layout.
// Fixed-width pointer values keep observation safe in non-32-bit tooling too.
struct NativeRenderBatchElement32 {
  uint32_t renderablePart = 0;
  uint32_t flags = 0;
  uint32_t layerIndex = 0;
  uint32_t subIndex = 0;
  uint32_t layerState = 0;
};

static_assert(sizeof(NativeRenderBatchElement32) == 20u);

// CRenderablePart stores these two fields next to each other.  Once the
// producer has learned that a renderable/layer can actually skin, read the
// pair with one fault-safe snapshot instead of two VirtualQuery-backed scalar
// probes.  Unknown queue entries never need to touch native memory at all.
struct NativeRenderableSkinBinding32 {
  uint32_t paletteSlot = kInvalidPaletteSlot;
  uint32_t geosetData = 0u;
};

static_assert(sizeof(NativeRenderableSkinBinding32) == 8u);
static_assert(kRenderableGeosetDataOffset ==
              kRenderablePaletteSlotOffset + sizeof(uint32_t));
static_assert(kMaxRetiredResourceEpochs > 0u);
static_assert(kMaxRetiringClaims > 0u);
static_assert(kMaxAutoRetiredBatchTombstones >= kMaxRetiringClaims);
static_assert(kGpuSkinManagerFallbackReasonCount <=
              std::numeric_limits<uint8_t>::max());

uint64_t HashMix(uint64_t hash, uint64_t value) {
  return (hash ^ value) * 0x100000001b3ull;
}

uint64_t HashBytes(const void* data, size_t size) {
  uint64_t hash = 0xcbf29ce484222325ull;
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0u; i < size; ++i)
    hash = HashMix(hash, bytes[i]);
  return hash;
}

uint32_t NextNonZero(uint32_t& value) {
  ++value;
  if (value == 0u)
    ++value;
  return value;
}

uint64_t NextNonZero(uint64_t& value) {
  ++value;
  if (value == 0u)
    ++value;
  return value;
}

uint32_t ConsumerMask(GpuSkinConsumerBits consumer) {
  return static_cast<uint32_t>(consumer);
}

constexpr uint32_t kRenderConsumerMask =
    static_cast<uint32_t>(GpuSkinConsumerBits::Main) |
    static_cast<uint32_t>(GpuSkinConsumerBits::Shadow) |
    static_cast<uint32_t>(GpuSkinConsumerBits::Outline);
constexpr uint32_t kAllConsumerMask = kRenderConsumerMask |
    static_cast<uint32_t>(GpuSkinConsumerBits::Parity);

bool IsSingleConsumerBit(uint32_t consumer) {
  return consumer != 0u && (consumer & (consumer - 1u)) == 0u &&
      (consumer & ~kAllConsumerMask) == 0u;
}

uint32_t CountConsumerBits(uint32_t consumers) {
  uint32_t count = 0u;
  while (consumers != 0u) {
    consumers &= consumers - 1u;
    ++count;
  }
  return count;
}

bool SameDipSignature(const DipSignature& lhs, const DipSignature& rhs) {
  return lhs.renderThreadId == rhs.renderThreadId &&
      lhs.uploadToken == rhs.uploadToken &&
      lhs.dispatchEpoch == rhs.dispatchEpoch &&
      lhs.uploadEpoch == rhs.uploadEpoch &&
      lhs.expectedStream0 == rhs.expectedStream0 &&
      lhs.dipOrdinal == rhs.dipOrdinal &&
      lhs.primitiveType == rhs.primitiveType &&
      lhs.baseVertexIndex == rhs.baseVertexIndex &&
      lhs.minVertexIndex == rhs.minVertexIndex &&
      lhs.numVertices == rhs.numVertices &&
      lhs.startIndex == rhs.startIndex &&
      lhs.primitiveCount == rhs.primitiveCount &&
      lhs.vertexStride == rhs.vertexStride;
}

bool IsComputeProducingMode(GpuSkinMode mode) {
  return mode == GpuSkinMode::Dual || mode == GpuSkinMode::Shadow ||
         mode == GpuSkinMode::Main || mode == GpuSkinMode::Bypass;
}

bool IsVertexShaderInputOnlyRoute(GpuSkinExecutionRoute route) {
  return route == GpuSkinExecutionRoute::VertexShaderInputOnly;
}

bool IsVertexShaderBypassRoute(GpuSkinExecutionRoute route) {
  return route == GpuSkinExecutionRoute::VertexShaderBypass;
}

bool IsVertexShaderNoComputeRoute(GpuSkinExecutionRoute route) {
  return IsVertexShaderInputOnlyRoute(route) ||
      IsVertexShaderBypassRoute(route);
}

bool IsMainOverrideMode(GpuSkinMode mode) {
  return mode == GpuSkinMode::Main || mode == GpuSkinMode::Bypass;
}

uint32_t AllowedFormalConsumerMask(GpuSkinMode mode) {
  if (mode == GpuSkinMode::Shadow)
    return ConsumerMask(GpuSkinConsumerBits::Shadow);
  if (IsMainOverrideMode(mode)) {
    return ConsumerMask(GpuSkinConsumerBits::Main) |
           ConsumerMask(GpuSkinConsumerBits::Shadow) |
           ConsumerMask(GpuSkinConsumerBits::Outline);
  }
  return 0u;
}

bool IsAllowedFormalConsumer(GpuSkinMode mode,
                             GpuSkinConsumerBits consumer) {
  const uint32_t requested = ConsumerMask(consumer);
  const uint32_t allowed = AllowedFormalConsumerMask(mode);
  return requested != 0u && (requested & ~allowed) == 0u;
}

bool IsStrictOutputFormat(uint32_t format) {
  // Production consumers remain on the byte-proven no-diffuse layouts. Odd
  // layouts are admitted only by Dual parity below and cannot authorize P2,
  // P3, or P4 takeover.
  return format == 0u || format == 2u || format == 4u;
}

bool IsCandidateOutputFormat(GpuSkinMode mode, uint32_t format) {
  return mode == GpuSkinMode::Dual
      ? GetGpuSkinFvfStride(format) != 0u
      : IsStrictOutputFormat(format);
}

bool PredictNativeStartIndex(uint32_t indexRingNext,
                             uint32_t expectedIndexCount,
                             uint32_t& predictedStartIndex) {
  if (expectedIndexCount == 0u ||
      expectedIndexCount > kMaxNativeIndices ||
      indexRingNext > kMaxNativeIndices) {
    return false;
  }
  predictedStartIndex = indexRingNext >
          kMaxNativeIndices - expectedIndexCount
      ? 0u
      : indexRingNext;
  return expectedIndexCount <=
      kMaxNativeIndices - predictedStartIndex;
}

GpuSkinManagerFallbackReason MapResourceFallback(
    GpuSkinFallbackReason reason) {
  switch (reason) {
    case GpuSkinFallbackReason::StaticResourceMiss:
      return GpuSkinManagerFallbackReason::StaticResourceMiss;
    case GpuSkinFallbackReason::StaticResourcePending:
      return GpuSkinManagerFallbackReason::StaticResourcePending;
    case GpuSkinFallbackReason::StaticResourceInvalid:
    case GpuSkinFallbackReason::StaticBudgetExhausted:
    case GpuSkinFallbackReason::MissQueueFull:
      return GpuSkinManagerFallbackReason::StaticResourceInvalid;
    case GpuSkinFallbackReason::UploadBudgetExhausted:
    case GpuSkinFallbackReason::OutputBudgetExhausted:
      return GpuSkinManagerFallbackReason::BudgetExhausted;
    default:
      return GpuSkinManagerFallbackReason::PreparedDrawMiss;
  }
}

struct LayoutKey {
  uintptr_t geosetData = 0;
  uint32_t layerIndex = 0;

  bool operator==(const LayoutKey& other) const {
    return geosetData == other.geosetData && layerIndex == other.layerIndex;
  }
};

struct LayoutKeyHash {
  size_t operator()(const LayoutKey& key) const {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = HashMix(hash, uint64_t(key.geosetData));
    hash = HashMix(hash, key.layerIndex);
    return size_t(hash ^ (hash >> 32u));
  }
};

struct RenderableLayoutKey {
  uintptr_t renderablePart = 0;
  uint32_t layerIndex = 0;

  bool operator==(const RenderableLayoutKey& other) const {
    return renderablePart == other.renderablePart &&
           layerIndex == other.layerIndex;
  }
};

struct RenderableLayoutKeyHash {
  size_t operator()(const RenderableLayoutKey& key) const {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = HashMix(hash, uint64_t(key.renderablePart));
    hash = HashMix(hash, key.layerIndex);
    return size_t(hash ^ (hash >> 32u));
  }
};

static bool RenderableLayoutKeyLess(const RenderableLayoutKey& lhs,
                                    const RenderableLayoutKey& rhs) noexcept {
  if (lhs.renderablePart != rhs.renderablePart)
    return lhs.renderablePart < rhs.renderablePart;
  return lhs.layerIndex < rhs.layerIndex;
}

// Negative-only admission accelerator for the overwhelmingly sparse native
// RenderQueue. A clear bit is authoritative absence; a positive result still
// performs the exact unordered_map lookup and every existing live proof. The
// filter is rebuilt monotonically within a map/device epoch, so stale bits can
// only create harmless false positives and never authorize a candidate.
class RenderableLayoutBloom {
public:
  static constexpr uint32_t kBitCount = 4096u;
  static constexpr uint32_t kWordCount = kBitCount / 64u;
  static constexpr uint32_t kBitMask = kBitCount - 1u;

  void clear() {
    m_words.fill(0u);
  }

  void insert(const RenderableLayoutKey& key) {
    const auto indices = bitIndices(key);
    set(indices[0]);
    set(indices[1]);
  }

  bool mayContain(const RenderableLayoutKey& key) const {
    const auto indices = bitIndices(key);
    return test(indices[0]) && test(indices[1]);
  }

private:
  static std::array<uint32_t, 2> bitIndices(
      const RenderableLayoutKey& key) {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = HashMix(hash, uint64_t(key.renderablePart));
    hash = HashMix(hash, key.layerIndex);
    const uint32_t low = uint32_t(hash);
    const uint32_t high = uint32_t(hash >> 32u);
    return {low & kBitMask,
            (high ^ (low * 0x9e3779b9u)) & kBitMask};
  }

  void set(uint32_t bit) {
    m_words[bit >> 6u] |= uint64_t(1u) << (bit & 63u);
  }

  bool test(uint32_t bit) const {
    return (m_words[bit >> 6u] &
            (uint64_t(1u) << (bit & 63u))) != 0u;
  }

  std::array<uint64_t, kWordCount> m_words = {};
};

static_assert((RenderableLayoutBloom::kBitCount &
               (RenderableLayoutBloom::kBitCount - 1u)) == 0u);

struct CandidateKey {
  uint64_t flushEpoch = 0;
  uintptr_t renderablePart = 0;
  uintptr_t geosetData = 0;
  uint32_t layerIndex = 0;
  uint32_t outputFormat = 0;

  bool operator==(const CandidateKey& other) const {
    return flushEpoch == other.flushEpoch &&
           renderablePart == other.renderablePart &&
           geosetData == other.geosetData &&
           layerIndex == other.layerIndex &&
           outputFormat == other.outputFormat;
  }
};

struct CandidateKeyHash {
  size_t operator()(const CandidateKey& key) const {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = HashMix(hash, key.flushEpoch);
    hash = HashMix(hash, uint64_t(key.renderablePart));
    hash = HashMix(hash, uint64_t(key.geosetData));
    hash = HashMix(hash, key.layerIndex);
    hash = HashMix(hash, key.outputFormat);
    return size_t(hash ^ (hash >> 32u));
  }
};

struct PreparedCandidate {
  CandidateKey candidateKey;
  std::shared_ptr<const GpuSkinStaticResource> resource;
  GpuSkinPreparedDrawKey key;
  GpuSkinParityMetadata parity;
  uintptr_t paletteAddress = 0;
  uint32_t paletteGroupCount = 0;
  uint32_t vertexCount = 0;
  uint32_t outputStride = 0;
  uint32_t outputFormat = 0;
  uint32_t sourceUvLayerCount = 0;
  uint32_t expectedIndexCount = 0;
  uint64_t expectedIndexContentHash = 0;
  uint64_t resourceContentHash = 0;
  bool bypassOpaqueEligible = false;
  std::vector<uint8_t> paletteBytes;
};

struct FlushPaletteBaseSnapshot {
  uintptr_t base = 0u;
  bool attempted = false;
};

struct LeasedCandidate {
  PreparedCandidate* candidate = nullptr;
  OutputLease lease;
  GpuSkinInputLease inputLease;
};

bool SameEpoch(const NativeEpochKey& lhs, const NativeEpochKey& rhs) {
  return lhs.renderThreadId == rhs.renderThreadId &&
         lhs.flushEpoch == rhs.flushEpoch &&
         lhs.dispatchEpoch == rhs.dispatchEpoch;
}

class OutputLeaseTransaction {
  struct Entry {
    OutputLease lease;
    std::shared_ptr<GpuSkinInputLeaseReceipt> inputReceipt;
    bool committed = false;
  };

public:
  explicit OutputLeaseTransaction(
      std::shared_ptr<War3GpuSkinResources> resources)
      : m_resources(std::move(resources)) {
  }

  ~OutputLeaseTransaction() {
    if (m_resources == nullptr)
      return;
    for (const Entry& entry : m_leases) {
      if (!entry.committed) {
        if (entry.inputReceipt != nullptr &&
            entry.inputReceipt->state ==
                GpuSkinInputLeaseReceiptState::Pending &&
            entry.inputReceipt->consumerFence == nullptr &&
            entry.inputReceipt->consumerFenceValue == 0u) {
          entry.inputReceipt->state =
              GpuSkinInputLeaseReceiptState::Cancelled;
        }
        m_resources->discardOutput(entry.lease);
      }
    }
  }

  void track(const OutputLease& lease) {
    try {
      m_leases.push_back({ lease, nullptr, false });
    } catch (...) {
      if (m_resources != nullptr)
        m_resources->discardOutput(lease);
      throw;
    }
  }

  void attachInputReceipt(
      uint64_t leaseId,
      const std::shared_ptr<GpuSkinInputLeaseReceipt>& receipt) {
    for (Entry& entry : m_leases) {
      if (entry.lease.leaseId == leaseId) {
        entry.inputReceipt = receipt;
        return;
      }
    }
  }

  void commit(uint64_t leaseId) {
    for (Entry& entry : m_leases) {
      if (entry.lease.leaseId == leaseId) {
        entry.committed = true;
        return;
      }
    }
  }

private:
  std::shared_ptr<War3GpuSkinResources> m_resources;
  std::vector<Entry> m_leases;
};

}  // namespace

class War3GpuSkinManager::Impl {
public:
  Impl(Rc<DxvkDevice> device, uintptr_t gameBase,
       const GpuSkinRuntimeConfig& config,
      const GpuSkinResourceBudgets& budgets,
      const GpuSkinManagerHostCallbacks& host)
      : m_mode(config.mode),
        m_executionRoute(config.executionRoute),
        m_executionRouteExplicit(config.executionRouteExplicit),
        m_executionRouteInvalid(config.executionRouteInvalid),
        m_diffSamplePeriod(config.mode == GpuSkinMode::Dual
            ? config.diffSamplePeriod
            : 0u),
        m_fullDiagnostics(config.fullDiagnostics),
        m_gameBase(gameBase),
        m_host(host),
        m_device(std::move(device)),
        m_budgets(budgets) {
    m_diagnostics.mode = m_mode;
    m_diagnostics.executionRoute = m_executionRoute;
    m_diagnostics.executionRouteExplicit = m_executionRouteExplicit;
    m_diagnostics.executionRouteInvalid = m_executionRouteInvalid;

    m_nativeCallbacks.abiVersion = kNativeBridgeCallbackAbi;
    m_nativeCallbacks.structSize = sizeof(m_nativeCallbacks);
    m_nativeCallbacks.userData = this;
    m_nativeCallbacks.onFlush = &NativeFlushThunk;
    m_nativeCallbacks.onDispatchBegin = &NativeDispatchBeginThunk;
    m_nativeCallbacks.onDispatchEnd = &NativeDispatchEndThunk;
    m_nativeCallbacks.onBypassPreflight = &NativeBypassPreflightThunk;
    m_nativeCallbacks.resolveCpuRewriteOutputProof =
        &NativeCpuRewriteOutputProofThunk;
    m_nativeCallbacks.onUpload = &NativeUploadThunk;
    m_nativeCallbacks.onDip = &NativeDipThunk;
    m_nativeCallbacks.onUploadFanout = &NativeFanoutThunk;
  }

  GpuSkinMode mode() const {
    return m_mode;
  }

  GpuSkinExecutionRoute executionRoute() const {
    return m_executionRoute;
  }

  bool executionRouteExplicit() const {
    return m_executionRouteExplicit;
  }

  bool executionRouteInvalid() const {
    return m_executionRouteInvalid;
  }

  void setHostCallbacks(const GpuSkinManagerHostCallbacks& host) {
    if (m_mode == GpuSkinMode::Disabled)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_host = host;
  }

  bool setDevice(Rc<DxvkDevice> device, uint64_t deviceEpoch,
                 uint64_t renderThreadId) {
    if (!IsComputeProducingMode(m_mode))
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_diagnostics.deviceRebindAttempts;
    m_deviceReady = false;
    m_pendingDeviceEpoch = deviceEpoch;
    if (device == nullptr || deviceEpoch == 0u || renderThreadId == 0u ||
        !acceptRenderThread(renderThreadId)) {
      ++m_diagnostics.deviceRebindFailures;
      recordFallback(GpuSkinManagerFallbackReason::DeviceEpochRequiresRebind);
      return false;
    }
    pollRetiredResources();
    const size_t retiredResourceCountBefore = m_retiredResources.size();
    const size_t retiringClaimCountBefore = m_retiringClaims.size();
    if (!retireCurrentEpoch()) {
      ++m_diagnostics.deviceRebindFailures;
      recordFallback(GpuSkinManagerFallbackReason::DeviceEpochRequiresRebind);
      return false;
    }
    if (m_retiredResources.size() > retiredResourceCountBefore ||
        m_retiringClaims.size() > retiringClaimCountBefore) {
      pollRetiredResources();
    }

    std::shared_ptr<War3GpuSkinResources> resources;
    try {
      resources = std::make_shared<War3GpuSkinResources>(device, m_budgets);
    } catch (...) {
      ++m_diagnostics.deviceRebindFailures;
      recordFallback(GpuSkinManagerFallbackReason::DeviceEpochRequiresRebind);
      return false;
    }

    m_device = std::move(device);
    m_boundDeviceEpoch = deviceEpoch;
    m_pendingDeviceEpoch = 0u;
    m_resources = std::move(resources);
    resetRegularRetirementPollKey();
    m_deviceReady = true;
    ++m_diagnostics.deviceRebindSuccesses;
    return true;
  }

  bool isDeviceReady(uint64_t deviceEpoch) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!IsComputeProducingMode(m_mode))
      return !m_callbackQuarantined;
    return m_deviceReady && m_pendingDeviceEpoch == 0u &&
        m_boundDeviceEpoch != 0u &&
        (deviceEpoch == 0u || deviceEpoch == m_boundDeviceEpoch);
  }

  bool attachNativeBridge() {
    if (m_mode == GpuSkinMode::Disabled)
      return true;

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_attached)
        return true;
    }

    const NativeBridgeCallbackStatus status =
        RegisterNativeBridgeCallbacks(this, &m_nativeCallbacks);
    const bool attached = status == NativeBridgeCallbackStatus::Success;
    if (attached) {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_attached = true;
    } else if (status == NativeBridgeCallbackStatus::OwnerMismatch) {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_diagnostics.callbackOwnerConflicts;
    } else if (status ==
                   NativeBridgeCallbackStatus::ExpectedPointerMismatch ||
               status == NativeBridgeCallbackStatus::QuiescencePending) {
      // The bridge still associates this exact owner with callback storage.
      // Keep detach responsibility even though registration did not complete.
      std::lock_guard<std::mutex> lock(m_mutex);
      m_attached = true;
      ++m_diagnostics.callbackOwnerConflicts;
      ++m_diagnostics.callbackDetachDeferrals;
    }
    return attached;
  }

  bool detachNativeBridge() {
    if (m_mode == GpuSkinMode::Disabled)
      return true;

    bool attached = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_attached)
        return true;
      if (m_callbackThread != std::thread::id() &&
          m_callbackThread != std::this_thread::get_id()) {
        ++m_diagnostics.renderThreadMismatches;
        ++m_diagnostics.callbackDetachDeferrals;
        recordFallback(GpuSkinManagerFallbackReason::RenderThreadMismatch);
        return false;
      }
      if (m_nativeCallbackActive) {
        ++m_diagnostics.callbackReentries;
        ++m_diagnostics.callbackDetachDeferrals;
        recordFallback(GpuSkinManagerFallbackReason::CallbackReentry);
        return false;
      }
      attached = m_attached;
    }
    if (!attached)
      return true;

    const NativeBridgeCallbackStatus status =
        UnregisterNativeBridgeCallbacks(this, &m_nativeCallbacks);
    if (status != NativeBridgeCallbackStatus::Success) {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (status == NativeBridgeCallbackStatus::OwnerMismatch ||
          status == NativeBridgeCallbackStatus::ExpectedPointerMismatch) {
        ++m_diagnostics.callbackOwnerConflicts;
      }
      ++m_diagnostics.callbackDetachDeferrals;
      return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_attached = false;
    return true;
  }

  void quarantineCallbackOwner() noexcept {
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_host = {};
      m_deviceReady = false;
      if (!m_callbackQuarantined) {
        m_callbackQuarantined = true;
        ++m_diagnostics.callbackQuarantines;
      }
    } catch (...) {
    }
  }

  void submitFlush(const NativeFlushObservation& observation) {
    if (m_mode == GpuSkinMode::Disabled)
      return;

    GpuSkinManagerHostCallbacks host;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (!acceptRenderThread(observation.renderThreadId))
        return;
      if (IsComputeProducingMode(m_mode) && !m_deviceReady) {
        ++m_diagnostics.flushCallbacks;
        recordFallback(
            GpuSkinManagerFallbackReason::DeviceEpochRequiresRebind);
        return;
      }
      host = m_host;
    }

    if (!IsComputeProducingMode(m_mode)) {
      noteFlushOnly(observation,
          m_mode == GpuSkinMode::Observe
              ? GpuSkinManagerFallbackReason::ObserveOnly
              : GpuSkinManagerFallbackReason::ModeNotProducing);
      return;
    }

    FlushRequest request = {};
    if (host.queryFlushRequest == nullptr) {
      noteFlushOnly(observation,
                    GpuSkinManagerFallbackReason::MissingFlushRequest);
      return;
    }
    const int64_t queryTimingStart = m_fullDiagnostics
        ? dxvk::high_resolution_clock::get_counter()
        : 0;
    const bool queryAccepted =
        host.queryFlushRequest(host.userData, observation, &request);
    const int64_t queryTimingElapsed = m_fullDiagnostics
        ? dxvk::high_resolution_clock::get_counter() - queryTimingStart
        : 0;
    if (!queryAccepted) {
      noteFlushOnly(observation,
                    GpuSkinManagerFallbackReason::MissingFlushRequest,
                    queryTimingElapsed);
      return;
    }

    submitFlush(request, observation, queryTimingElapsed);
  }

  bool submitFlush(const FlushRequest& request,
                   const NativeFlushObservation& observation,
                   int64_t queryTimingElapsed = 0) {
    if (m_mode == GpuSkinMode::Disabled)
      return false;

    GpuSkinManagerHostCallbacks host;
    GpuSkinPendingBatch hostBatch;
    bool hasBatch = false;
    bool invokeHost = false;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      // Every flush owns a unique bridge epoch. Invalidate the prior immutable
      // candidate view before any early return; an exact-negative dispatch
      // seal may be proposed only after this flush has completed assembly and,
      // when work exists, host submission.
      invalidateDispatchCpuOnlySealView();
      RawTickAccumulator flushAssemblyTimer(
          m_fullDiagnostics,
          m_diagnostics.flushAssemblyCalls,
          m_diagnostics.flushAssemblyTicks,
          m_diagnostics.flushAssemblyMaxTicks);
      RecordRawTiming(m_diagnostics.flushQueryTiming,
                      queryTimingElapsed);
      ++m_diagnostics.flushCallbacks;
      noteModeMismatch(observation.mode);

      if (!acceptRenderThread(observation.renderThreadId))
        return false;
      if (m_hostSubmissionActive) {
        ++m_diagnostics.callbackReentries;
        recordFallback(GpuSkinManagerFallbackReason::CallbackReentry);
        return false;
      }

      if (!IsComputeProducingMode(m_mode)) {
        recordFallback(m_mode == GpuSkinMode::Observe
                ? GpuSkinManagerFallbackReason::ObserveOnly
                : GpuSkinManagerFallbackReason::ModeNotProducing);
        return false;
      }
      if (!m_deviceReady || m_pendingDeviceEpoch != 0u ||
          request.deviceEpoch != m_boundDeviceEpoch) {
        ++m_diagnostics.deviceEpochRejects;
        recordFallback(
            GpuSkinManagerFallbackReason::DeviceEpochRequiresRebind);
        return false;
      }

      if (request.frameTag == 0u ||
          request.flushToken == 0u || request.mapEpoch == 0u ||
          request.deviceEpoch == 0u || observation.flushEpoch == 0u ||
          observation.renderThreadId == 0u || sizeof(void*) != 4u) {
        recordFallback(GpuSkinManagerFallbackReason::InvalidFlushEpoch);
        return false;
      }

      {
        RawTickAccumulator controlTimer(
            m_fullDiagnostics, m_diagnostics.flushControlTiming);
        pollRetiredResourcesForFlushFrame(request);
        if (rejectForRetirementBackpressure())
          return false;
        const size_t retiredResourceCountBefore = m_retiredResources.size();
        const size_t retiringClaimCountBefore = m_retiringClaims.size();
        if (!ensureEpoch(request))
          return false;
        // A map/device epoch transition may have moved the previous owner into
        // retirement after this frame's regular poll. Give that newly-created
        // owner one immediate forced poll; all other flushes remain throttled.
        if (m_retiredResources.size() > retiredResourceCountBefore ||
            m_retiringClaims.size() > retiringClaimCountBefore) {
          pollRetiredResources();
        }
        if (rejectForRetirementBackpressure())
          return false;
        if (m_pendingBatch != nullptr &&
            m_pendingBatch->state != GpuSkinBatchState::Pending) {
          ++m_diagnostics.batchClaimFailures;
          recordFallback(GpuSkinManagerFallbackReason::BatchClaimFailed);
          return false;
        }
        expireUnsubmittedBatch();
        clearLeakedDispatchesAtFlush(observation.renderThreadId);
        if (m_resources == nullptr ||
            !m_resources->beginFrame(request.mapEpoch, request.deviceEpoch,
                                     request.frameTag)) {
          recordFallback(GpuSkinManagerFallbackReason::InvalidFlushEpoch);
          return false;
        }
      }

      // Most native flushes produce no GPU-skin work. Build the tentative
      // batch on the stack and allocate its persistent owner only after it is
      // known to contain uploads, compute work, or retirement state.
      GpuSkinPendingBatch batch;
      batch.batchId = NextNonZero(m_nextBatchId);
      batch.renderThreadId = observation.renderThreadId;
      batch.request = request;
      batch.flush = observation;
      batch.resourceOwner = m_resources;
      batch.staticUploads.swap(m_carriedStaticUploads);
      batch.requiresUploadRetirement = m_carriedUploadRetirement;
      m_carriedUploadRetirement = false;

      const uint32_t staticPrepareCount = std::min(
          kMaxStaticPreparesPerFlush,
          request.requestedJobCount != 0u
              ? request.requestedJobCount
              : kMaxStaticPreparesPerFlush);
      {
        RawTickAccumulator staticPrepareTimer(
            m_fullDiagnostics, m_diagnostics.staticPrepareTiming);
        m_resources->prepareQueuedStaticResources(staticPrepareCount);
        std::vector<GpuSkinStaticUpload> newStaticUploads =
            m_resources->takeStaticUploads();
        batch.staticUploads.insert(batch.staticUploads.end(),
            std::make_move_iterator(newStaticUploads.begin()),
            std::make_move_iterator(newStaticUploads.end()));
      }
      if (!batch.staticUploads.empty())
        batch.requiresUploadRetirement = true;

      const uint32_t jobLimit = std::min(
          request.requestedJobCount != 0u
              ? request.requestedJobCount
              : kMaxJobsPerFlush,
          kMaxJobsPerFlush);
      std::vector<PreparedCandidate> candidates;
      FlushPaletteBaseSnapshot paletteBaseSnapshot;
      // Candidate acceptance is sparse (many empty/small native flushes).
      // Let the vector allocate lazily instead of reserving the 512-job hard
      // limit; accepted candidates also provide a compact dedup table.
      prepareArray(candidates, batch,
                   observation.opaqueArray, observation.opaqueCount,
                   jobLimit, true, paletteBaseSnapshot);
      if (candidates.size() < jobLimit)
        prepareArray(candidates, batch,
                     observation.transparentArray,
                     observation.transparentCount, jobLimit, false,
                     paletteBaseSnapshot);
      if (!excludeTransparentCandidateCollisions(
              candidates, batch, observation.transparentArray,
              observation.transparentCount)) {
        for (PreparedCandidate& candidate : candidates)
          candidate.bypassOpaqueEligible = false;
      }

      OutputLeaseTransaction outputTransaction(m_resources);
      std::vector<LeasedCandidate> publications;
      publications.reserve(candidates.size());
      assembleCandidates(batch, candidates, publications,
                         outputTransaction);

      hasBatch = !batch.staticUploads.empty() ||
          !batch.computeBatches.empty() ||
          batch.requiresUploadRetirement;
      if (!hasBatch) {
        // Full array preparation completed and produced no publishable GPU
        // work. This empty current-flush view is authoritative without a host
        // callback; all earlier validation failures returned above.
        if (m_mode == GpuSkinMode::Bypass && !m_fullDiagnostics) {
          m_dispatchCpuOnlySealCandidates.clear();
          m_dispatchCpuOnlySealFlushEpoch = observation.flushEpoch;
          ++m_diagnostics.dispatchCpuOnlySealViewPublishes;
          (void)PublishNativeDispatchCpuOnlySealView(
              observation.flushEpoch, nullptr, 0u);
        }
        return false;
      }

      auto pendingBatch =
          std::make_unique<GpuSkinPendingBatch>(std::move(batch));
      publishPreparedDraws(*pendingBatch, publications);
      ++m_diagnostics.batchesPrepared;
      m_diagnostics.jobsPrepared += pendingBatch->preparedDraws.size();
      m_diagnostics.computeDispatchesPrepared +=
          pendingBatch->computeBatches.size();
      for (const GpuSkinComputeBatch& compute :
           pendingBatch->computeBatches) {
        m_diagnostics.maxJobsPerDispatch = std::max<uint64_t>(
            m_diagnostics.maxJobsPerDispatch, compute.jobCount);
        m_diagnostics.computeVerticesPrepared +=
            compute.actualVertexCount;
        m_diagnostics.computeRoundedInvocationsPrepared +=
            compute.roundedInvocationCount;
        m_diagnostics.computeLaunchedInvocationsPrepared +=
            compute.launchedInvocationCount;
        if (compute.vertexBucket < kGpuSkinVertexBucketCount) {
          m_diagnostics.vertexBucketJobs[compute.vertexBucket] +=
              compute.jobCount;
        }
      }
      m_pendingBatch = std::move(pendingBatch);
      for (const OutputLease& lease : m_pendingBatch->outputLeases)
        outputTransaction.commit(lease.leaseId);
      for (const OutputLease& lease : m_pendingBatch->inputStorageLeases)
        outputTransaction.commit(lease.leaseId);
      host = m_host;
      if (host.submitFlushBatch != nullptr) {
        // 先完成可能抛出异常的批次快照复制。若复制失败，批次仍保持 Pending，
        // callbackException 可直接撤销租约，不会把无 producer fence 的批次
        // 错送进 retiring claim 并形成永久背压。
        hostBatch = *m_pendingBatch;
        // 快照复制发生在状态切换前，因此必须显式发布 host 所消费的
        // Submitting 值；该赋值不分配内存，也不会破坏上述异常安全顺序。
        hostBatch.state = GpuSkinBatchState::Submitting;
        m_pendingBatch->state = GpuSkinBatchState::Claimed;
        m_pendingBatch->state = GpuSkinBatchState::Submitting;
        m_hostSubmissionActive = true;
        invokeHost = true;
      }
    }

    if (invokeHost) {
      GpuSkinHostSubmitResult result;
      const int64_t hostTimingStart = m_fullDiagnostics
          ? dxvk::high_resolution_clock::get_counter()
          : 0;
      try {
        result = host.submitFlushBatch(host.userData, hostBatch);
      } catch (...) {
        const int64_t hostTimingElapsed = m_fullDiagnostics
            ? dxvk::high_resolution_clock::get_counter() - hostTimingStart
            : 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (hostTimingElapsed > 0) {
          const uint64_t ticks = uint64_t(hostTimingElapsed);
          ++m_diagnostics.flushHostSubmitCalls;
          m_diagnostics.flushHostSubmitTicks += ticks;
          m_diagnostics.flushHostSubmitMaxTicks = std::max(
              m_diagnostics.flushHostSubmitMaxTicks, ticks);
        }
        m_hostSubmissionActive = false;
        if (m_pendingBatch != nullptr &&
            m_pendingBatch->batchId == hostBatch.batchId)
          retireCurrentEpoch();
        m_submissionRecoveryBlocked = true;
        updateRetirementBackpressure();
        throw;
      }
      const int64_t hostTimingElapsed = m_fullDiagnostics
          ? dxvk::high_resolution_clock::get_counter() - hostTimingStart
          : 0;
      bool acceptedSubmissionCompleted = false;
      if (result.accepted) {
        acceptedSubmissionCompleted = completeAcceptedHostSubmission(
            hostBatch, std::move(result.uploadRetireFence),
            result.uploadRetireValue);
      } else {
        noteHostRejection(hostBatch.batchId);
      }
      std::lock_guard<std::mutex> lock(m_mutex);
      if (hostTimingElapsed > 0) {
        const uint64_t ticks = uint64_t(hostTimingElapsed);
        ++m_diagnostics.flushHostSubmitCalls;
        m_diagnostics.flushHostSubmitTicks += ticks;
        m_diagnostics.flushHostSubmitMaxTicks = std::max(
            m_diagnostics.flushHostSubmitMaxTicks, ticks);
      }
      // Keep both immutable views unavailable while the host callback is
      // active. Timing is settled first; only then may the completed batch
      // become an exact-negative authority for subsequent Common dispatches.
      m_hostSubmissionActive = false;
      if (result.accepted && acceptedSubmissionCompleted)
        publishDispatchCpuOnlySealView(hostBatch);
    }
    return hasBatch;
  }

  void beginDispatch(const NativeDispatchObservation& observation) {
    if (m_mode == GpuSkinMode::Disabled)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!acceptRenderThread(observation.epoch.renderThreadId))
      return;
    ++m_diagnostics.dispatchBegins;

    const NativeEpochKey& epoch = observation.epoch;
    if (epoch.renderThreadId == 0u || epoch.flushEpoch == 0u ||
        epoch.dispatchEpoch == 0u ||
        m_dispatches.find(epoch.dispatchEpoch) != m_dispatches.end()) {
      ++m_diagnostics.truePairingErrors;
      recordFallback(GpuSkinManagerFallbackReason::UploadEpochMismatch);
      return;
    }

    DispatchState state;
    state.observation = observation;
    m_dispatches.emplace(epoch.dispatchEpoch, std::move(state));
    m_dispatchStacks[epoch.renderThreadId].push_back(epoch.dispatchEpoch);

    // This is deliberately the last non-throwing action after both manager
    // pairing containers are complete. The bridge still commits only after
    // this callback returns and BeginIssued becomes authoritative.
    if (m_mode == GpuSkinMode::Bypass && !m_fullDiagnostics &&
        observation.path == NativeDispatchPath::Common &&
        !observation.failClosed && observation.nestingDepth == 0u &&
        (epoch.dispatchEpoch % kNativeBeginTimingSamplePeriod) != 0u) {
      ++m_diagnostics.dispatchCpuOnlySealViewQueries;
      const bool authoritative =
          epoch.flushEpoch == m_dispatchCpuOnlySealFlushEpoch &&
          !m_hostSubmissionActive && m_pendingDeviceEpoch == 0u &&
          m_pendingBridgeResetGeneration == 0u &&
          m_deviceReady && !m_callbackQuarantined;
      if (!authoritative) {
        ++m_diagnostics.dispatchCpuOnlySealAuthorityRejects;
        return;
      }

      const RenderableLayoutKey dispatchKey = {
          observation.renderablePart,
          observation.layerIndex,
      };
      const bool candidatePresent = std::binary_search(
          m_dispatchCpuOnlySealCandidates.begin(),
          m_dispatchCpuOnlySealCandidates.end(), dispatchKey,
          RenderableLayoutKeyLess);
      if (candidatePresent) {
        ++m_diagnostics.dispatchCpuOnlySealCandidateRejects;
        return;
      }

      ++m_diagnostics.dispatchCpuOnlySealProposals;
      (void)ProposeCurrentNativeDispatchCpuOnlySeal(observation);
    }
  }

  void endDispatch(const NativeDispatchSummary& summary) {
    if (m_mode == GpuSkinMode::Disabled)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!acceptRenderThread(summary.dispatch.epoch.renderThreadId))
      return;
    ++m_diagnostics.dispatchEnds;

    const uint64_t dispatchEpoch = summary.dispatch.epoch.dispatchEpoch;
    const auto found = m_dispatches.find(dispatchEpoch);
    if (found == m_dispatches.end()) {
      ++m_diagnostics.truePairingErrors;
      recordFallback(GpuSkinManagerFallbackReason::UploadEpochMismatch);
      return;
    }

    DispatchState& state = found->second;
    if (!SameEpoch(state.observation.epoch, summary.dispatch.epoch)) {
      ++m_diagnostics.truePairingErrors;
      ++m_diagnostics.epochLeaks;
    }
    if (!state.uploads.empty() || state.activeUploadEpoch != 0u) {
      for (const auto& upload : state.uploads) {
        if (upload.second.nativeBypassed) {
          ++m_diagnostics.bypassPending;
          fuseBypassPath(upload.second.fuseKey,
                         upload.second.observation.geosetData,
                         state.observation.layerIndex, true);
        }
      }
      m_diagnostics.uploadLeaks += state.uploads.size();
      ++m_diagnostics.epochLeaks;
      state.uploads.clear();
      state.activeUploadEpoch = 0u;
    }
    for (auto it = m_bypassAuthorizations.begin();
         it != m_bypassAuthorizations.end();) {
      if (it->second.dispatchEpoch == dispatchEpoch) {
        ++m_diagnostics.bypassPending;
        it = m_bypassAuthorizations.erase(it);
      } else {
        ++it;
      }
    }
    if (state.uploadCount != summary.uploadCount ||
        state.dipCount != summary.dipCount) {
      ++m_diagnostics.truePairingErrors;
    }

    auto stack = m_dispatchStacks.find(
        summary.dispatch.epoch.renderThreadId);
    if (stack == m_dispatchStacks.end() || stack->second.empty() ||
        stack->second.back() != dispatchEpoch) {
      ++m_diagnostics.truePairingErrors;
      if (stack != m_dispatchStacks.end()) {
        const auto item = std::find(stack->second.begin(),
                                    stack->second.end(), dispatchEpoch);
        if (item != stack->second.end())
          stack->second.erase(item);
      }
    } else {
      stack->second.pop_back();
    }
    // Keep the render thread's empty stack entry hot. The bridge accepts one
    // render thread, so retaining this node avoids a map-node + vector
    // allocation on every dispatch without allowing unbounded growth. Reset
    // paths still clear the table, while leaked non-empty stacks are erased.
    m_dispatches.erase(found);
  }

  bool preflightNativeBypass(
      const NativeUploadObservation& observation,
      NativeBypassAuthorization* authorization) {
    if (authorization == nullptr)
      return false;
    *authorization = {};

    GpuSkinManagerHostCallbacks host;
    GpuSkinNativeBypassHostRequest hostRequest;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      RawTickAccumulator managerTimer(
          m_fullDiagnostics, m_diagnostics.preflightManagerTiming);
      ++m_diagnostics.bypassPreflightAttempts;
      const auto reject = [&](GpuSkinManagerFallbackReason reason,
                              bool stale = false) {
        ++m_diagnostics.bypassFallbacks;
        ++m_diagnostics.kernelPreflightRejects;
        if (stale)
          ++m_diagnostics.bypassStale;
        recordFallback(reason);
        return false;
      };
      if (m_mode != GpuSkinMode::Bypass) {
        return reject(GpuSkinManagerFallbackReason::BypassModeRequired);
      }
      // VS-B0 先只证明不创建 compute output/job 时的 draw 输入闭合；
      // 原生 CPU kernel 必须继续执行，不能让旧 P4 output-lease 合同误把
      // palette storage 当成 post-skin 顶点输出并永久 fuse 该 layout。
      if (IsVertexShaderInputOnlyRoute(m_executionRoute)) {
        return reject(GpuSkinManagerFallbackReason::BypassHostRejected);
      }
      if (!m_deviceReady || m_pendingDeviceEpoch != 0u) {
        return reject(
            GpuSkinManagerFallbackReason::DeviceEpochRequiresRebind, true);
      }
      if (!acceptRenderThread(observation.epoch.renderThreadId))
        return reject(GpuSkinManagerFallbackReason::RenderThreadMismatch);

      const auto dispatch = m_dispatches.find(
          observation.epoch.dispatchEpoch);
      if (dispatch == m_dispatches.end() ||
          !SameEpoch(dispatch->second.observation.epoch, observation.epoch) ||
          dispatch->second.activeUploadEpoch != 0u ||
          observation.epoch.uploadEpoch == 0u ||
          observation.epoch.uploadOrdinal !=
              dispatch->second.uploadCount + 1u ||
          !isStrictNativeBypassPreflight(
              dispatch->second.observation, observation, false)) {
        return reject(GpuSkinManagerFallbackReason::BypassPreflightFailed,
                      true);
      }

      const CandidateKey candidate = {
          observation.epoch.flushEpoch,
          observation.renderablePart,
          observation.geosetData,
          dispatch->second.observation.layerIndex,
          observation.outputFormat,
      };
      const LayoutKey layoutKey = {
          observation.geosetData,
          dispatch->second.observation.layerIndex,
      };
      const auto learned = m_layouts.find(layoutKey);
      if (learned == m_layouts.end() ||
          !learned->second.exactSingleDipConfirmed) {
        return reject(GpuSkinManagerFallbackReason::DipSignatureMismatch);
      }
      const auto token = m_candidateTokens.find(candidate);
      if (token == m_candidateTokens.end())
        return reject(GpuSkinManagerFallbackReason::PreparedDrawMiss, true);
      auto prepared = m_preparedDraws.find(token->second);
      if (prepared == m_preparedDraws.end() ||
          !isSubmittedPreparedDraw(prepared->second) ||
          !(IsVertexShaderBypassRoute(m_executionRoute)
                ? isExactBypassInputLease(prepared->second)
                : isExactBypassLease(prepared->second))) {
        return reject(
            GpuSkinManagerFallbackReason::BypassAuthorizationMismatch, true);
      }
      const LearnedLayout& baseline = learned->second;
      const uint64_t fuseKey = prepared->second.bypassFuseKey;
      const bool fuseKeyRejected = fuseKey != 0u &&
          m_fusedBypassKeys.find(fuseKey) != m_fusedBypassKeys.end();
      if (fuseKeyRejected) {
        // Some late poison/DIP failures initially know only the native fuse
        // key. Once a prepared draw supplies the exact layout, promote that
        // key-only fuse into the layout index so the next flush is rejected
        // before static lookup, palette copy, or compute submission.
        fuseBypassPath(fuseKey, prepared->second.key.geosetData,
                       prepared->second.key.layerIndex, false);
      }
      if (baseline.bypassFused ||
          m_fusedBypassLayouts.find(layoutKey) !=
              m_fusedBypassLayouts.end() ||
          fuseKey == 0u ||
          fuseKeyRejected) {
        return reject(GpuSkinManagerFallbackReason::BypassFused);
      }
      if (baseline.baselineIndexCount !=
              prepared->second.expectedIndexCount ||
          baseline.baselinePrimitiveType != kD3dPrimitiveTriangleList ||
          baseline.baselineMinVertexIndex != 0u ||
          baseline.baselineNumVertices !=
              prepared->second.expectedVertexCount ||
          baseline.baselinePrimitiveCount !=
              prepared->second.expectedIndexCount / 3u) {
        return reject(GpuSkinManagerFallbackReason::DipSignatureMismatch);
      }
      if (!prepared->second.bypassOpaqueEligible)
        return reject(GpuSkinManagerFallbackReason::TransparentDispatch);
      if (!validateUploadPalette(prepared->second, observation) ||
          !validateBypassStaticInputs(prepared->second, observation)) {
        return reject(
            GpuSkinManagerFallbackReason::BypassAuthorizationMismatch, true);
      }

      const uint32_t expectedIndexCount =
          prepared->second.expectedIndexCount;
      uint32_t predictedStartIndex = 0u;
      if (expectedIndexCount == 0u ||
          expectedIndexCount > kMaxNativeIndices ||
          observation.indexRingBaseBefore >
              observation.indexRingNextBefore ||
          !PredictNativeStartIndex(observation.indexRingNextBefore,
                                   expectedIndexCount,
                                   predictedStartIndex)) {
        return reject(GpuSkinManagerFallbackReason::MultiPrimitiveSlice);
      }

      constexpr uint32_t requiredConsumers =
          static_cast<uint32_t>(GpuSkinConsumerBits::Main) |
          static_cast<uint32_t>(GpuSkinConsumerBits::Shadow) |
          static_cast<uint32_t>(GpuSkinConsumerBits::Outline);
      prepared->second.expectedDipSignature = makeExpectedDipSignature(
          prepared->second, observation, baseline, predictedStartIndex,
          dispatch->second.dipCount + 1u, requiredConsumers);
      hostRequest.key = prepared->second.key;
      hostRequest.lease = prepared->second.lease;
      hostRequest.inputLease = prepared->second.inputLease;
      hostRequest.executionRoute = m_executionRoute;
      hostRequest.expectedDipSignature =
          prepared->second.expectedDipSignature;
      hostRequest.epoch = observation.epoch;
      hostRequest.fuseKey = fuseKey;
      hostRequest.gxDeviceD3d = observation.gxDeviceD3d;
      hostRequest.nativeD3DDevice = observation.nativeD3DDevice;
      hostRequest.nativeVertexBuffer = observation.nativeVertexBuffer;
      hostRequest.nativeIndexBuffer = observation.nativeIndexBuffer;
      hostRequest.mappedDst = observation.mappedDst;
      hostRequest.expectedIndexCount = expectedIndexCount;
      hostRequest.predictedStartIndex = predictedStartIndex;
      hostRequest.requiredConsumerBits = requiredConsumers;
      hostRequest.vertexCount = observation.vertexCount;
      hostRequest.outputStride = observation.outputStride;
      hostRequest.fvf = observation.fvf;
      hostRequest.ringBaseVertexBefore =
          observation.ringBaseVertexBefore;
      hostRequest.ringNextVertexBefore =
          observation.ringNextVertexBefore;
      hostRequest.ringBaseVertexAfter = observation.ringBaseVertexAfter;
      hostRequest.ringNextVertexAfter = observation.ringNextVertexAfter;
      hostRequest.indexRingBaseBefore = observation.indexRingBaseBefore;
      hostRequest.indexRingNextBefore = observation.indexRingNextBefore;
      hostRequest.predictedIndexRingBase = predictedStartIndex;
      hostRequest.predictedIndexRingNext =
          predictedStartIndex + expectedIndexCount;
      host = m_host;
      if (host.preflightNativeBypass == nullptr)
        return reject(GpuSkinManagerFallbackReason::BypassHostRejected);

      BypassAuthorizationState state;
      state.key = prepared->second.key;
      state.dispatchEpoch = observation.epoch.dispatchEpoch;
      state.fuseKey = fuseKey;
      state.expectedIndexCount = expectedIndexCount;
      state.predictedStartIndex = predictedStartIndex;
      state.epoch = observation.epoch;
      state.gxDeviceD3d = observation.gxDeviceD3d;
      state.nativeD3DDevice = observation.nativeD3DDevice;
      state.nativeVertexBuffer = observation.nativeVertexBuffer;
      state.nativeIndexBuffer = observation.nativeIndexBuffer;
      state.ringBaseVertexAfter = observation.ringBaseVertexAfter;
      state.ringNextVertexAfter = observation.ringNextVertexAfter;
      state.indexRingNextBefore = observation.indexRingNextBefore;
      if (!m_bypassAuthorizations.emplace(
              observation.epoch.uploadEpoch, std::move(state)).second) {
        ++m_diagnostics.bypassPending;
        return reject(
            GpuSkinManagerFallbackReason::BypassAuthorizationMismatch);
      }
    }

    NativeVertexOutputProof hostOutputProof = {};
    const int64_t hostTimingStart = m_fullDiagnostics
        ? dxvk::high_resolution_clock::get_counter()
        : 0;
    const bool hostAccepted = host.preflightNativeBypass(
        host.userData, hostRequest, &hostOutputProof);
    const int64_t hostTimingElapsed = m_fullDiagnostics
        ? dxvk::high_resolution_clock::get_counter() - hostTimingStart
        : 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    RecordRawTiming(m_diagnostics.preflightHostTiming,
                    hostTimingElapsed);
    RawTickAccumulator finalizeTimer(
        m_fullDiagnostics, m_diagnostics.preflightFinalizeTiming);
    const auto pending = m_bypassAuthorizations.find(
        observation.epoch.uploadEpoch);
    const auto prepared = m_preparedDraws.find(hostRequest.key.token);
    const bool planComplete = prepared != m_preparedDraws.end() &&
        prepared->second.consumerPlanExplicit &&
        (prepared->second.consumerPlan.known &
         hostRequest.requiredConsumerBits) ==
            hostRequest.requiredConsumerBits &&
        (prepared->second.consumerPlan.notRequested |
         prepared->second.consumerPlan.leaseBacked) ==
            prepared->second.consumerPlan.known &&
        (prepared->second.consumerPlan.leaseBacked &
         ConsumerMask(GpuSkinConsumerBits::Main)) != 0u &&
        SameDipSignature(prepared->second.consumerPlan.signature,
                         hostRequest.expectedDipSignature);
    const bool outputProofComplete =
        hostOutputProof.resource.commonResource != 0u &&
        hostOutputProof.resource.comVertexBuffer != 0u &&
        hostOutputProof.resource.resourceGeneration != 0u &&
        hostOutputProof.resource.comVertexBuffer ==
            hostRequest.nativeVertexBuffer &&
        hostOutputProof.nativeD3DDevice == hostRequest.nativeD3DDevice &&
        hostOutputProof.outputFormat == hostRequest.key.outputFormat &&
        hostOutputProof.vertexStride == hostRequest.outputStride &&
        hostOutputProof.fvf == hostRequest.fvf;
    const bool explicitPlan = prepared != m_preparedDraws.end() &&
        prepared->second.consumerPlanExplicit;
    const bool hostPlanMismatch =
        (hostAccepted && !planComplete) || (!hostAccepted && explicitPlan);
    const bool pendingStateComplete =
        pending != m_bypassAuthorizations.end() &&
        pending->second.dispatchEpoch == observation.epoch.dispatchEpoch &&
        pending->second.fuseKey == hostRequest.fuseKey &&
        samePreparedKey(pending->second.key, hostRequest.key);
    if (!hostAccepted || !planComplete || !outputProofComplete ||
        !pendingStateComplete) {
      if (pending != m_bypassAuthorizations.end())
        m_bypassAuthorizations.erase(pending);
      ++m_diagnostics.bypassFallbacks;
      ++m_diagnostics.kernelPreflightRejects;
      if (hostPlanMismatch)
        ++m_diagnostics.planMismatch;
      const bool authorizationMismatch = hostPlanMismatch ||
          (hostAccepted && !outputProofComplete) || !pendingStateComplete ||
          prepared == m_preparedDraws.end();
      if (authorizationMismatch)
        ++m_diagnostics.bypassHostAuthorizationMismatches;
      recordFallback(authorizationMismatch
          ? GpuSkinManagerFallbackReason::BypassAuthorizationMismatch
          : GpuSkinManagerFallbackReason::BypassHostRejected);
      return false;
    }
    pending->second.hostApproved = true;

    authorization->fuseKey = hostRequest.fuseKey;
    authorization->token = hostRequest.key.token;
    authorization->expectedIndexCount = hostRequest.expectedIndexCount;
    authorization->requiredConsumerBits = hostRequest.requiredConsumerBits;
    authorization->predictedStartIndex = hostRequest.predictedStartIndex;
    authorization->vertexOutputProof = hostOutputProof;
    authorization->approvedPreflight =
        NativePreflightResourceGenerationReady |
        NativePreflightStaticResourceReady |
        NativePreflightPaletteCopied |
        NativePreflightGpuConsumerCapabilityReady |
        NativePreflightBatchSubmitted |
        NativePreflightExactGpuWorkContract |
        NativePreflightStaticInputsMatch |
        NativePreflightAllConsumersLeased |
        NativePreflightHostDrawSafe |
        NativePreflightIndexPathSafe |
        NativePreflightNoUnsupportedDrawPath |
        NativePreflightCpuBaselineExact |
        NativePreflightBypassKeyNotFused;
    ++m_diagnostics.bypassAuthorizations;
    return true;
  }

  bool resolveNativeCpuRewriteOutputProof(
      const NativeUploadObservation& observation,
      NativeCpuRewriteOutputProof* outputProof) {
    if (outputProof == nullptr)
      return false;
    *outputProof = {};

    GpuSkinManagerHostCallbacks host;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      RawTickAccumulator managerTimer(
          m_fullDiagnostics, m_diagnostics.cpuProofManagerTiming);
      ++m_diagnostics.cpuRewriteProofAttempts;
      if (m_mode != GpuSkinMode::Bypass ||
          !acceptRenderThread(observation.epoch.renderThreadId) ||
          m_host.resolveNativeCpuRewriteOutputProof == nullptr) {
        ++m_diagnostics.cpuRewriteProofRejects;
        return false;
      }
      host = m_host;
    }

    NativeCpuRewriteOutputProof hostProof = {};
    const int64_t hostTimingStart = m_fullDiagnostics
        ? dxvk::high_resolution_clock::get_counter()
        : 0;
    const bool hostAccepted = host.resolveNativeCpuRewriteOutputProof(
        host.userData, observation, &hostProof);
    const int64_t hostTimingElapsed = m_fullDiagnostics
        ? dxvk::high_resolution_clock::get_counter() - hostTimingStart
        : 0;
    const NativeVertexOutputProof& vertexProof =
        hostProof.vertexOutputProof;
    const uint64_t proofEnd = uint64_t(hostProof.baseVertex) +
        hostProof.vertexCount;
    const uint64_t proofByteOffset = uint64_t(hostProof.baseVertex) *
        vertexProof.vertexStride;
    const uint64_t proofByteLength = uint64_t(hostProof.vertexCount) *
        vertexProof.vertexStride;
    const bool proofComplete = hostAccepted &&
        observation.cpuSkinOriginalTrampolineSelected &&
        observation.cpuSkinOriginalReturnedNormally &&
        observation.cpuSkinOriginalRangeExact &&
        !observation.cpuSkinKernelBypassed &&
        vertexProof.resource.commonResource != 0u &&
        vertexProof.resource.comVertexBuffer != 0u &&
        vertexProof.resource.resourceGeneration != 0u &&
        vertexProof.resource.comVertexBuffer ==
            observation.nativeVertexBuffer &&
        vertexProof.nativeD3DDevice != 0u &&
        vertexProof.nativeD3DDevice == observation.nativeD3DDevice &&
        vertexProof.outputFormat == observation.outputFormat &&
        vertexProof.vertexStride != 0u &&
        vertexProof.vertexStride == observation.outputStride &&
        vertexProof.vertexStride ==
            GetGpuSkinFvfStride(vertexProof.outputFormat) &&
        vertexProof.fvf != 0u && vertexProof.fvf == observation.fvf &&
        hostProof.baseVertex == observation.ringBaseVertexAfter &&
        hostProof.vertexCount != 0u &&
        hostProof.vertexCount == observation.vertexCount &&
        proofEnd <= kMaxNativeVertices &&
        proofEnd == observation.ringNextVertexAfter &&
        proofByteOffset <= std::numeric_limits<uint32_t>::max() &&
        proofByteLength <= std::numeric_limits<uint32_t>::max() &&
        hostProof.byteOffset == proofByteOffset &&
        hostProof.byteLength == proofByteLength;

    std::lock_guard<std::mutex> lock(m_mutex);
    RecordRawTiming(m_diagnostics.cpuProofHostTiming,
                    hostTimingElapsed);
    RawTickAccumulator finalizeTimer(
        m_fullDiagnostics, m_diagnostics.cpuProofFinalizeTiming);
    if (!proofComplete) {
      ++m_diagnostics.cpuRewriteProofRejects;
      return false;
    }
    ++m_diagnostics.cpuRewriteProofExact;
    *outputProof = hostProof;
    return true;
  }

  void noteNativeUpload(const NativeUploadObservation& observation) {
    if (m_mode == GpuSkinMode::Disabled)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator completionTimer(
        m_fullDiagnostics, m_diagnostics.completionManagerTiming);
    if (!acceptRenderThread(observation.epoch.renderThreadId))
      return;
    ++m_diagnostics.nativeUploads;
    noteModeMismatch(observation.mode);
    const size_t formatBucket = observation.nativeDeviceStateReadable &&
            observation.outputFormat < 7u
        ? size_t(observation.outputFormat) : size_t(7u);
    ++m_diagnostics.formatHistogram[formatBucket];
    ++m_diagnostics.skinModeHistogram[
        observation.nativeDeviceStateReadable && observation.skinMode < 7u
            ? observation.skinMode : 7u];

    // Vertex uploads also occur outside RenderQueue common/special dispatch
    // scopes. They remain useful raw bridge telemetry, but they do not belong
    // to the dispatch/upload/DIP protocol and therefore are not pairing faults.
    if (observation.epoch.dispatchEpoch == 0u) {
      ++m_diagnostics.outsideUploadsByFormat[formatBucket];
      return;
    }

    const auto dispatch = m_dispatches.find(
        observation.epoch.dispatchEpoch);
    if (dispatch == m_dispatches.end() ||
        !SameEpoch(dispatch->second.observation.epoch, observation.epoch) ||
        observation.epoch.uploadEpoch == 0u) {
      ++m_diagnostics.truePairingErrors;
      recordFallback(GpuSkinManagerFallbackReason::UploadEpochMismatch);
      return;
    }
    ++m_diagnostics.insideUploadsByFormat[formatBucket];

    DispatchState& dispatchState = dispatch->second;
    // The native bridge may refine stage/tag after the dispatch-begin callback.
    // The completed upload carries the authoritative values from that scope.
    dispatchState.observation.stage = observation.stage;
    dispatchState.observation.batchTag = observation.batchTag;
    bool uploadPairingValid = true;
    if (dispatchState.activeUploadEpoch != 0u) {
      ++m_diagnostics.uploadLeaks;
      ++m_diagnostics.epochLeaks;
      dispatchState.uploads.erase(dispatchState.activeUploadEpoch);
      dispatchState.activeUploadEpoch = 0u;
      uploadPairingValid = false;
      recordFallback(GpuSkinManagerFallbackReason::UploadEpochMismatch);
    }

    ++dispatchState.uploadCount;
    if (observation.epoch.uploadOrdinal == 0u ||
        observation.epoch.uploadOrdinal != dispatchState.uploadCount) {
      ++m_diagnostics.truePairingErrors;
      uploadPairingValid = false;
      recordFallback(GpuSkinManagerFallbackReason::UploadEpochMismatch);
    }

    UploadState upload;
    upload.observation = observation;
    upload.fuseKey = observation.fuseKey;
    upload.nativeBypassed = observation.cpuSkinKernelBypassed;
    const auto pendingAuthorization = m_bypassAuthorizations.find(
        observation.epoch.uploadEpoch);
    const bool hasPendingAuthorization =
        pendingAuthorization != m_bypassAuthorizations.end();
    const uint32_t authorizedToken = hasPendingAuthorization
        ? pendingAuthorization->second.key.token : 0u;
    const bool authorizationMatches = hasPendingAuthorization &&
        pendingAuthorization->second.hostApproved &&
        SameEpoch(pendingAuthorization->second.epoch, observation.epoch) &&
        pendingAuthorization->second.epoch.uploadEpoch ==
            observation.epoch.uploadEpoch &&
        pendingAuthorization->second.epoch.uploadOrdinal ==
            observation.epoch.uploadOrdinal &&
        pendingAuthorization->second.dispatchEpoch ==
            observation.epoch.dispatchEpoch &&
        pendingAuthorization->second.gxDeviceD3d ==
            observation.gxDeviceD3d &&
        pendingAuthorization->second.nativeD3DDevice ==
            observation.nativeD3DDevice &&
        pendingAuthorization->second.nativeVertexBuffer ==
            observation.nativeVertexBuffer &&
        pendingAuthorization->second.nativeIndexBuffer ==
            observation.nativeIndexBuffer &&
        pendingAuthorization->second.ringBaseVertexAfter ==
            observation.ringBaseVertexAfter &&
        pendingAuthorization->second.ringNextVertexAfter ==
            observation.ringNextVertexAfter &&
        pendingAuthorization->second.indexRingNextBefore ==
            observation.indexRingNextBefore &&
        pendingAuthorization->second.expectedIndexCount ==
            observation.expectedIndexCount &&
        pendingAuthorization->second.predictedStartIndex ==
            observation.predictedIndexRingBase &&
        pendingAuthorization->second.fuseKey == observation.fuseKey &&
        authorizedToken != 0u && authorizedToken == observation.bypassToken;
    bool postSkipFailure = observation.cpuSkinKernelBypassed &&
        (observation.postSkipMismatch || !authorizationMatches);
    if (observation.cpuSkinKernelBypassed && !authorizationMatches) {
      uploadPairingValid = false;
      recordFallback(
          GpuSkinManagerFallbackReason::BypassAuthorizationMismatch);
    } else if (!observation.cpuSkinKernelBypassed &&
               hasPendingAuthorization) {
      ++m_diagnostics.bypassFallbacks;
      if (observation.bypassFailure ==
              NativeBypassFailureReason::NativeStateStale ||
          observation.bypassFailure ==
              NativeBypassFailureReason::ThreadMismatch ||
          observation.bypassFailure ==
              NativeBypassFailureReason::SelfMismatch ||
          observation.bypassFailure ==
              NativeBypassFailureReason::EpochMismatch) {
        ++m_diagnostics.bypassStale;
      }
    }

    const bool strictNative = uploadPairingValid && isStrictNativeUpload(
        dispatchState.observation, observation);
    if (observation.cpuSkinKernelBypassed && !strictNative)
      postSkipFailure = true;
    if (strictNative) {
      ++m_diagnostics.nativeEligibleUploads;
      ++m_diagnostics.eligibleUploadsByFormat[formatBucket];
      if (IsComputeProducingMode(m_mode) &&
          observation.originalUploadExecuted &&
          !observation.cpuSkinKernelBypassed) {
        learnLayout(dispatchState.observation, observation);
        ++m_diagnostics.learnedLayoutsByFormat[formatBucket];
      }
    }

    if (IsComputeProducingMode(m_mode) && strictNative) {
      const CandidateKey candidate = {
          observation.epoch.flushEpoch,
          observation.renderablePart,
          observation.geosetData,
          dispatchState.observation.layerIndex,
          observation.outputFormat,
      };
      const auto preparedToken = m_candidateTokens.find(candidate);
      if (preparedToken == m_candidateTokens.end() ||
          (observation.cpuSkinKernelBypassed &&
           preparedToken->second != authorizedToken)) {
        recordFallback(GpuSkinManagerFallbackReason::PreparedDrawMiss);
      } else {
        auto prepared = m_preparedDraws.find(preparedToken->second);
        if (prepared == m_preparedDraws.end()) {
          recordFallback(GpuSkinManagerFallbackReason::PreparedDrawMiss);
        } else if (!isSubmittedPreparedDraw(prepared->second)) {
          recordFallback(GpuSkinManagerFallbackReason::BatchNotSubmitted);
        } else if (validateUploadPalette(prepared->second, observation)) {
          upload.preparedToken = prepared->first;
          prepared->second.lease.desc.dispatchEpoch =
              observation.epoch.dispatchEpoch;
          prepared->second.lease.desc.uploadEpoch =
              observation.epoch.uploadEpoch;
          if (prepared->second.inputLease) {
            prepared->second.inputLease.desc.dispatchEpoch =
                observation.epoch.dispatchEpoch;
            prepared->second.inputLease.desc.uploadEpoch =
                observation.epoch.uploadEpoch;
            // receipt 与 draw lease 共享同一 capability 身份；epoch 必须在
            // manager 锁内同步发布，避免值语义已更新但寿命权限仍指向旧上传。
            prepared->second.inputLease.receipt->desc.dispatchEpoch =
                observation.epoch.dispatchEpoch;
            prepared->second.inputLease.receipt->desc.uploadEpoch =
                observation.epoch.uploadEpoch;
          }
          if (observation.cpuSkinKernelBypassed) {
            prepared->second.bypassCommitted = true;
            ++m_diagnostics.bypassCommits;
          }
        }
      }
    }

    if (hasPendingAuthorization)
      m_bypassAuthorizations.erase(pendingAuthorization);
    if (observation.cpuSkinKernelBypassed && upload.preparedToken == 0u)
      postSkipFailure = true;
    if (postSkipFailure) {
      fuseBypassPath(observation.fuseKey, observation.geosetData,
                     dispatchState.observation.layerIndex, true);
    }

    dispatchState.activeUploadEpoch = observation.epoch.uploadEpoch;
    dispatchState.uploads.emplace(observation.epoch.uploadEpoch,
                                  std::move(upload));
  }

  void noteNativeDip(const NativeDipObservation& observation) {
    if (m_mode == GpuSkinMode::Disabled)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!acceptRenderThread(observation.epoch.renderThreadId))
      return;
    if (!observation.correlated) {
      ++m_diagnostics.truePairingErrors;
      recordFallback(GpuSkinManagerFallbackReason::DipEpochMismatch);
      if (NativeDipRangePoisoned(observation)) {
        fuseBypassPath(observation.sourceUploadFuseKey, 0u, 0u, true);
      }
      return;
    }
    ++m_diagnostics.nativeDips;

    const auto dispatch = m_dispatches.find(
        observation.epoch.dispatchEpoch);
    if (dispatch == m_dispatches.end() ||
        !SameEpoch(dispatch->second.observation.epoch, observation.epoch)) {
      ++m_diagnostics.truePairingErrors;
      recordFallback(GpuSkinManagerFallbackReason::DipEpochMismatch);
      if (NativeDipRangePoisoned(observation)) {
        fuseBypassPath(observation.sourceUploadFuseKey, 0u, 0u, true);
      }
      return;
    }

    DispatchState& dispatchState = dispatch->second;
    ++dispatchState.dipCount;
    const auto upload = dispatchState.uploads.find(
        observation.epoch.uploadEpoch);
    if (upload == dispatchState.uploads.end() ||
        dispatchState.activeUploadEpoch != observation.epoch.uploadEpoch) {
      ++m_diagnostics.truePairingErrors;
      recordFallback(GpuSkinManagerFallbackReason::DipEpochMismatch);
      if (NativeDipRangePoisoned(observation)) {
        fuseBypassPath(observation.sourceUploadFuseKey, 0u, 0u, true);
      }
      return;
    }

    UploadState& uploadState = upload->second;
    const uint32_t expectedFanoutOrdinal = uploadState.dipCount + 1u;
    const bool ordinalValid = observation.epoch.dipOrdinal != 0u &&
        observation.epoch.dipOrdinal == dispatchState.dipCount &&
        observation.epoch.dipOrdinal > uploadState.lastDipOrdinal &&
        observation.uploadFanoutOrdinal == expectedFanoutOrdinal;
    if (!ordinalValid) {
      ++m_diagnostics.truePairingErrors;
      recordFallback(GpuSkinManagerFallbackReason::DipOrdinalMismatch);
    }

    const bool irreversibleBypass =
        NativeDipRangePoisoned(observation) || uploadState.nativeBypassed;
    const uint64_t fuseKey = observation.sourceUploadFuseKey != 0u
        ? observation.sourceUploadFuseKey
        : uploadState.fuseKey;
    const bool sourceMatches =
        NativeDipRangePoisoned(observation) == uploadState.nativeBypassed &&
        (!irreversibleBypass ||
         (fuseKey != 0u && fuseKey == uploadState.fuseKey));
    bool exact = ordinalValid && observation.correlated &&
        observation.exactNativeMatch && sourceMatches &&
        (!irreversibleBypass || observation.takeoverEligible);
    if (exact && uploadState.preparedToken != 0u) {
      const auto prepared = m_preparedDraws.find(uploadState.preparedToken);
      exact = prepared != m_preparedDraws.end() &&
          validateSinglePrimitiveDip(prepared->second,
                                     uploadState.observation,
                                     observation);
    }

    SeenDip seen;
    seen.dip = observation.dip;
    seen.exact = exact;
    uploadState.seenDips[observation.epoch.dipOrdinal] = seen;
    uploadState.lastDipOrdinal = std::max(
        uploadState.lastDipOrdinal, observation.epoch.dipOrdinal);
    ++uploadState.dipCount;
    if (!exact) {
      recordFallback(GpuSkinManagerFallbackReason::DipSignatureMismatch);
      if (irreversibleBypass) {
        fuseBypassPath(fuseKey, uploadState.observation.geosetData,
                       dispatchState.observation.layerIndex, true);
      }
    }
  }

  void noteNativeFanout(
      const NativeUploadFanoutObservation& observation) {
    if (m_mode == GpuSkinMode::Disabled)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!acceptRenderThread(observation.epoch.renderThreadId))
      return;
    if (observation.dipCount == 0u)
      ++m_diagnostics.fanoutZero;
    else if (observation.dipCount == 1u)
      ++m_diagnostics.fanoutOne;
    else
      ++m_diagnostics.fanoutMany;
    m_diagnostics.maxFanout = std::max<uint64_t>(
        m_diagnostics.maxFanout, observation.dipCount);

    // See noteNativeUpload: an orphan upload has a legitimate zero-DIP fanout
    // summary, but there is intentionally no manager DispatchState to resolve.
    if (observation.epoch.dispatchEpoch == 0u)
      return;

    const auto dispatch = m_dispatches.find(
        observation.epoch.dispatchEpoch);
    if (dispatch == m_dispatches.end() ||
        !SameEpoch(dispatch->second.observation.epoch, observation.epoch)) {
      ++m_diagnostics.truePairingErrors;
      recordFallback(GpuSkinManagerFallbackReason::UploadEpochMismatch);
      return;
    }

    DispatchState& dispatchState = dispatch->second;
    const auto upload = dispatchState.uploads.find(
        observation.epoch.uploadEpoch);
    if (upload == dispatchState.uploads.end()) {
      ++m_diagnostics.truePairingErrors;
      recordFallback(GpuSkinManagerFallbackReason::UploadEpochMismatch);
      return;
    }
    const bool fanoutCountMismatch =
        upload->second.dipCount != observation.dipCount;
    if (fanoutCountMismatch)
      ++m_diagnostics.truePairingErrors;

    if (upload->second.preparedToken != 0u) {
      const auto prepared = m_preparedDraws.find(
          upload->second.preparedToken);
      const bool consumerContractValid =
          prepared != m_preparedDraws.end() &&
          !prepared->second.bypassConsumerContractFailed;
      const bool exactSingleDip = consumerContractValid &&
          !fanoutCountMismatch &&
          observation.dipCount == 1u &&
          upload->second.seenDips.size() == 1u &&
          upload->second.seenDips.begin()->second.exact;
      if (exactSingleDip && prepared != m_preparedDraws.end() &&
          IsVertexShaderBypassRoute(m_executionRoute)) {
        const PreparedDraw& draw = prepared->second;
        const uint32_t leaseBacked = draw.consumerPlan.leaseBacked;
        const uint32_t terminal = terminalConsumerMask(draw);
        // B1 的 native poison 只在 Main/Shadow 都已消费、没有 fallback/
        // suppress，且当前上传已确认单 DIP 时提前退休。这里不改变
        // output/input lease 的 Vulkan fence 生命周期，只缩短 native
        // 动态环 stale 区间的观察窗口。
        const bool consumerSettlementExact =
            leaseBacked != 0u &&
            (leaseBacked & ~terminal) == 0u &&
            (draw.consumerLedger.consumed & leaseBacked) == leaseBacked &&
            (draw.consumerLedger.cpuFallback & leaseBacked) == 0u &&
            (draw.consumerLedger.suppressFuse & leaseBacked) == 0u &&
            !draw.bypassConsumerContractFailed;
        if (consumerSettlementExact) {
          RetireNativeBypassPoisonAfterExactConsumer(
              upload->second.observation, true, true);
        }
      }
      const LayoutKey layoutKey = {
          upload->second.observation.geosetData,
          dispatchState.observation.layerIndex,
      };
      const auto layout = m_layouts.find(layoutKey);
      if (upload->second.nativeBypassed) {
        if (!exactSingleDip) {
          if (observation.dipCount == 0u)
            ++m_diagnostics.bypassPending;
          fuseBypassPath(upload->second.fuseKey,
                         upload->second.observation.geosetData,
                         dispatchState.observation.layerIndex, true);
        }
      } else if (layout != m_layouts.end()) {
        LearnedLayout& learned = layout->second;
        if (learned.bypassFused) {
          clearCpuDipBaseline(learned);
        } else {
          learned.exactSingleDipConfirmed = exactSingleDip;
          if (exactSingleDip) {
            const SeenDip& baseline =
                upload->second.seenDips.begin()->second;
            learned.baselineIndexCount =
                prepared->second.expectedIndexCount;
            learned.baselinePrimitiveType = baseline.dip.primitiveType;
            learned.baselineMinVertexIndex = baseline.dip.minVertexIndex;
            learned.baselineNumVertices = baseline.dip.numVertices;
            learned.baselinePrimitiveCount = baseline.dip.primitiveCount;
          } else {
            clearCpuDipBaseline(learned);
          }
        }
      }
    } else if (upload->second.nativeBypassed) {
      fuseBypassPath(upload->second.fuseKey,
                     upload->second.observation.geosetData,
                     dispatchState.observation.layerIndex, true);
    } else {
      const LayoutKey layoutKey = {
          upload->second.observation.geosetData,
          dispatchState.observation.layerIndex,
      };
      const auto layout = m_layouts.find(layoutKey);
      if (layout != m_layouts.end())
        clearCpuDipBaseline(layout->second);
    }

    dispatchState.uploads.erase(upload);
    if (dispatchState.activeUploadEpoch == observation.epoch.uploadEpoch)
      dispatchState.activeUploadEpoch = 0u;
  }

  void noteBypassDrawResult(const GpuSkinBypassDrawResult& result) {
    if (m_mode != GpuSkinMode::Bypass)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator resultTimer(
        m_fullDiagnostics, m_diagnostics.bypassDrawResultTiming);
    auto prepared = m_preparedDraws.find(result.key.token);
    if (prepared == m_preparedDraws.end() ||
        !samePreparedKey(prepared->second.key, result.key) ||
        !prepared->second.bypassCommitted ||
        !isSubmittedPreparedDraw(prepared->second)) {
      ++m_diagnostics.bypassStale;
      fuseBypassPath(result.fuseKey, result.key.geosetData,
                     result.key.layerIndex, true);
      return;
    }

    if (!result.streamRestoreArmed) {
      ++m_diagnostics.bypassRestoreFailures;
      recordFallback(GpuSkinManagerFallbackReason::BypassRestoreFailure);
    }
    if (!result.mainConsumed || !result.shadowLeaseAvailable ||
        !result.outlineLeaseAvailable || !result.streamRestoreArmed) {
      prepared->second.bypassConsumerContractFailed = true;
      fuseBypassPath(result.fuseKey != 0u ? result.fuseKey
                                          : prepared->second.bypassFuseKey,
                     result.key.geosetData,
                     result.key.layerIndex, true);
    }
  }

  void fuseIrreversibleBypass(
      const GpuSkinResolvedDraw& resolved,
      GpuSkinConsumerBits consumer) {
    if (!resolved.nativeUploadBypassed)
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator fuseTimer(
        m_fullDiagnostics, m_diagnostics.irreversibleFuseTiming);
    uint64_t notificationKey = resolved.bypassFuseKey;
    if (notificationKey == 0u) {
      notificationKey = HashMix(0x50434f4e53554d45ull,
                                resolved.key.frameTag);
      notificationKey = HashMix(notificationKey, resolved.key.flushEpoch);
      notificationKey = HashMix(notificationKey, resolved.key.batchId);
      notificationKey = HashMix(notificationKey, resolved.key.token);
      notificationKey = HashMix(
          notificationKey, uint64_t(resolved.key.renderablePart));
      notificationKey = HashMix(
          notificationKey, uint64_t(resolved.key.geosetData));
    }

    const uint32_t consumerMask = ConsumerMask(consumer);
    bool consumerNoted = false;
    if ((consumerMask & ConsumerMask(GpuSkinConsumerBits::Main)) != 0u &&
        m_mainSuppressedKeys.emplace(notificationKey).second) {
      ++m_diagnostics.mainSuppressed;
      consumerNoted = true;
    }
    if ((consumerMask & ConsumerMask(GpuSkinConsumerBits::Shadow)) != 0u &&
        m_shadowSuppressedKeys.emplace(notificationKey).second) {
      ++m_diagnostics.shadowSuppressed;
      consumerNoted = true;
    }
    if (consumerNoted &&
        m_consumerFuseKeys.emplace(notificationKey).second) {
      ++m_diagnostics.consumerFuses;
    }

    auto prepared = m_preparedDraws.find(resolved.key.token);
    if (prepared != m_preparedDraws.end() &&
        samePreparedKey(prepared->second.key, resolved.key) &&
        resolved.leaseId == prepared->second.leaseId) {
      prepared->second.bypassConsumerContractFailed = true;
      const uint32_t bit = ConsumerMask(consumer);
      if (IsSingleConsumerBit(bit) &&
          (prepared->second.consumerPlan.leaseBacked & bit) != 0u &&
          (terminalConsumerMask(prepared->second) & bit) == 0u) {
        prepared->second.consumerLedger.suppressFuse |= bit;
        ++m_diagnostics.suppressed;
      } else if ((prepared->second.consumerPlan.known & bit) == 0u) {
        ++m_diagnostics.unreserved;
        ++m_diagnostics.unreservedFuseUnknownConsumer;
      } else if ((terminalConsumerMask(prepared->second) & bit) != 0u) {
        ++m_diagnostics.duplicate;
      } else {
        ++m_diagnostics.planMismatch;
      }
    }
    fuseBypassPath(resolved.bypassFuseKey, resolved.key.geosetData,
                   resolved.key.layerIndex, false);
  }

  void terminateIrreversibleBypassConsumers(
      const GpuSkinResolvedDraw& resolved) {
    if (!resolved.nativeUploadBypassed)
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator terminateTimer(
        m_fullDiagnostics, m_diagnostics.terminateConsumerTiming);
    const bool poisonSuppression = resolved.nativeRangePoisoned;
    enum class LatePoisonMatch : uint8_t {
      MatchedOpen,
      MatchedTerminal,
      Unmatched,
    };
    const auto noteLatePoison = [&](LatePoisonMatch match) {
      if (!poisonSuppression)
        return;

      ++m_diagnostics.latePoisonSuppressions;
      if (resolved.nativeDipCorrelated)
        ++m_diagnostics.latePoisonCorrelated;
      else
        ++m_diagnostics.latePoisonUncorrelated;
      if (resolved.nativeDipExact)
        ++m_diagnostics.latePoisonExact;
      else
        ++m_diagnostics.latePoisonInexact;
      if (resolved.nativeDipUploadEpoch == 0u)
        ++m_diagnostics.latePoisonUploadEpochZero;
      if (resolved.nativeDipPrimitiveCount == 0u ||
          resolved.nativeDipNumVertices == 0u) {
        ++m_diagnostics.latePoisonZeroGeometry;
      }

      if (resolved.nativeDipFlags == NativeDipFlagNone)
        ++m_diagnostics.latePoisonFlagNone;
      if ((resolved.nativeDipFlags & NativeDipFlagDebugSkip) != 0u)
        ++m_diagnostics.latePoisonFlagDebugSkip;
      if ((resolved.nativeDipFlags & NativeDipFlagAutoInstancing) != 0u)
        ++m_diagnostics.latePoisonFlagAutoInstancing;
      if ((resolved.nativeDipFlags & NativeDipFlagIndexSplit) != 0u)
        ++m_diagnostics.latePoisonFlagIndexSplit;
      if ((resolved.nativeDipFlags & NativeDipFlagRecursive) != 0u)
        ++m_diagnostics.latePoisonFlagRecursive;
      if ((resolved.nativeDipFlags & NativeDipFlagNonMainPass) != 0u)
        ++m_diagnostics.latePoisonFlagNonMainPass;

      switch (resolved.nativeDipFlags) {
      case NativeDipFlagNone:
        ++m_diagnostics.latePoisonMaskNone;
        break;
      case NativeDipFlagDebugSkip:
        ++m_diagnostics.latePoisonMaskDebug;
        break;
      case NativeDipFlagAutoInstancing:
        ++m_diagnostics.latePoisonMaskAutoInstancing;
        break;
      case NativeDipFlagIndexSplit:
        ++m_diagnostics.latePoisonMaskIndexSplit;
        break;
      case NativeDipFlagRecursive:
        ++m_diagnostics.latePoisonMaskRecursive;
        break;
      case NativeDipFlagDebugSkip | NativeDipFlagRecursive:
        ++m_diagnostics.latePoisonMaskDebugRecursive;
        break;
      case NativeDipFlagNonMainPass:
        ++m_diagnostics.latePoisonMaskNonMainPass;
        break;
      case NativeDipFlagDebugSkip | NativeDipFlagNonMainPass:
        ++m_diagnostics.latePoisonMaskDebugNonMainPass;
        break;
      case NativeDipFlagRecursive | NativeDipFlagNonMainPass:
        ++m_diagnostics.latePoisonMaskRecursiveNonMainPass;
        break;
      case NativeDipFlagDebugSkip | NativeDipFlagRecursive |
          NativeDipFlagNonMainPass:
        ++m_diagnostics.latePoisonMaskDebugRecursiveNonMainPass;
        break;
      default:
        ++m_diagnostics.latePoisonMaskUnknown;
        break;
      }

      const bool hadBypassedSource =
          resolved.nativePoisonHadBypassedSource;
      switch (match) {
      case LatePoisonMatch::MatchedOpen:
        ++m_diagnostics.latePoisonMatchedOpen;
        if (hadBypassedSource)
          ++m_diagnostics.latePoisonBypassMatchedOpen;
        else
          ++m_diagnostics.latePoisonPoisonOnlyMatchedOpen;
        break;
      case LatePoisonMatch::MatchedTerminal:
        ++m_diagnostics.latePoisonMatchedTerminal;
        if (hadBypassedSource)
          ++m_diagnostics.latePoisonBypassMatchedTerminal;
        else
          ++m_diagnostics.latePoisonPoisonOnlyMatchedTerminal;
        break;
      case LatePoisonMatch::Unmatched:
        ++m_diagnostics.latePoisonUnmatched;
        if (hadBypassedSource)
          ++m_diagnostics.latePoisonBypassUnmatched;
        else
          ++m_diagnostics.latePoisonPoisonOnlyUnmatched;
        break;
      }
    };
    uint64_t notificationKey = resolved.bypassFuseKey;
    if (notificationKey == 0u) {
      notificationKey = HashMix(0x50434f4e53554d45ull,
                                resolved.key.frameTag);
      notificationKey = HashMix(notificationKey, resolved.key.flushEpoch);
      notificationKey = HashMix(notificationKey, resolved.key.batchId);
      notificationKey = HashMix(notificationKey, resolved.key.token);
      notificationKey = HashMix(
          notificationKey, uint64_t(resolved.key.renderablePart));
      notificationKey = HashMix(
          notificationKey, uint64_t(resolved.key.geosetData));
    }

    uint32_t terminated = 0u;
    bool matched = false;
    const auto terminate = [&](PreparedDraw& draw) {
      const uint32_t leaseBacked =
          draw.consumerPlan.leaseBacked & kRenderConsumerMask;
      const uint32_t pending = leaseBacked & ~terminalConsumerMask(draw);
      matched = true;
      if (pending == 0u)
        return;
      draw.consumerLedger.suppressFuse |= pending;
      draw.bypassConsumerContractFailed = true;
      terminated |= pending;
      m_diagnostics.suppressed += CountConsumerBits(pending);
      m_diagnostics.irreversibleConsumerTerminations +=
          CountConsumerBits(pending);
    };

    auto prepared = m_preparedDraws.find(resolved.key.token);
    const bool exactPrepared = prepared != m_preparedDraws.end() &&
        samePreparedKey(prepared->second.key, resolved.key) &&
        resolved.leaseId != 0u &&
        resolved.leaseId == prepared->second.leaseId;
    if (exactPrepared) {
      terminate(prepared->second);
    } else if (resolved.bypassFuseKey != 0u) {
      // Poison-only observations can outlive dispatch correlation. The fuse
      // key still identifies every current plan that can consume that stale
      // native range, so terminate all exact-key matches fail closed.
      for (auto& item : m_preparedDraws) {
        if (item.second.bypassFuseKey == resolved.bypassFuseKey)
          terminate(item.second);
      }
    }

    if (!matched) {
      noteLatePoison(LatePoisonMatch::Unmatched);
      ++m_diagnostics.unreserved;
      ++m_diagnostics.unreservedTerminateUnmatched;
      fuseBypassPath(resolved.bypassFuseKey, resolved.key.geosetData,
                     resolved.key.layerIndex, false);
      return;
    }
    if (terminated == 0u) {
      noteLatePoison(LatePoisonMatch::MatchedTerminal);
      // This helper is cleanup, not a protocol transition. A second caller
      // observing already-terminal bits is an idempotent no-op.
      ++m_diagnostics.irreversibleConsumerCleanupNoops;
      fuseBypassPath(resolved.bypassFuseKey, resolved.key.geosetData,
                     resolved.key.layerIndex, false);
      return;
    }
    noteLatePoison(LatePoisonMatch::MatchedOpen);

    if ((terminated & ConsumerMask(GpuSkinConsumerBits::Main)) != 0u &&
        m_mainSuppressedKeys.emplace(notificationKey).second) {
      ++m_diagnostics.mainSuppressed;
    }
    if ((terminated & ConsumerMask(GpuSkinConsumerBits::Shadow)) != 0u &&
        m_shadowSuppressedKeys.emplace(notificationKey).second) {
      ++m_diagnostics.shadowSuppressed;
    }
    if ((terminated & ConsumerMask(GpuSkinConsumerBits::Outline)) != 0u &&
        m_outlineSuppressedKeys.emplace(notificationKey).second) {
      ++m_diagnostics.outlineSuppressed;
    }
    if (m_consumerFuseKeys.emplace(notificationKey).second)
      ++m_diagnostics.consumerFuses;
    fuseBypassPath(resolved.bypassFuseKey, resolved.key.geosetData,
                   resolved.key.layerIndex, false);
  }

  GpuSkinResolvedDraw resolveDip(
      const NativeDipObservation& observation,
      GpuSkinConsumerBits consumer) {
    return resolveDipForConsumer(observation, consumer, false);
  }

  GpuSkinResolvedDraw resolveParityDip(
      const NativeDipObservation& observation) {
    return resolveDipForConsumer(
        observation, GpuSkinConsumerBits::Parity, true);
  }

  GpuSkinResolvedDraw resolveDipForConsumer(
      const NativeDipObservation& observation,
      GpuSkinConsumerBits consumer, bool parityOnly) {
    GpuSkinResolvedDraw result;
    result.bypassFuseKey = observation.sourceUploadFuseKey;
    result.nativeUploadBypassed =
        NativeDipRangePoisoned(observation);
    result.nativeRangePoisoned = observation.nativeRangePoisoned;
    result.nativePoisonHadBypassedSource =
        observation.nativePoisonHadBypassedSource;
    result.nativeDipCorrelated = observation.correlated;
    result.nativeDipExact = observation.exactNativeMatch;
    result.nativeDipFlags = observation.dip.flags;
    result.nativeDipPrimitiveCount = observation.dip.primitiveCount;
    result.nativeDipNumVertices = observation.dip.numVertices;
    result.nativeDipUploadEpoch = observation.epoch.uploadEpoch;
    const uint32_t requestedConsumerMask = ConsumerMask(consumer);
    const bool requestsShadow =
        (requestedConsumerMask & ConsumerMask(GpuSkinConsumerBits::Shadow)) != 0u;
    if (m_mode == GpuSkinMode::Disabled) {
      result.fallback = GpuSkinManagerFallbackReason::ModeNotProducing;
      return result;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!parityOnly) {
      ++m_diagnostics.formalResolveAttempts;
      if (requestsShadow)
        ++m_diagnostics.shadowResolveAttempts;
    }
    const auto failed = [&](GpuSkinManagerFallbackReason reason) {
      recordFallback(reason);
      result.fallback = reason;
      if (!parityOnly)
        ++m_diagnostics.formalResolveFailures;
      if (result.nativeUploadBypassed) {
        fuseBypassPath(result.bypassFuseKey, result.key.geosetData,
                       result.key.layerIndex, false);
      }
      return result;
    };
    if (!IsComputeProducingMode(m_mode)) {
      return failed(m_mode == GpuSkinMode::Observe
          ? GpuSkinManagerFallbackReason::ObserveOnly
          : GpuSkinManagerFallbackReason::ModeNotProducing);
    }
    if (!m_deviceReady || m_pendingDeviceEpoch != 0u)
      return failed(GpuSkinManagerFallbackReason::DeviceEpochRequiresRebind);
    if ((parityOnly && m_mode != GpuSkinMode::Dual) ||
        (!parityOnly && !IsAllowedFormalConsumer(m_mode, consumer))) {
      return failed(GpuSkinManagerFallbackReason::ConsumerNotAllowed);
    }
    if (!acceptRenderThread(observation.epoch.renderThreadId))
      return failed(GpuSkinManagerFallbackReason::RenderThreadMismatch);
    const auto dispatch = m_dispatches.find(
        observation.epoch.dispatchEpoch);
    if (dispatch == m_dispatches.end() ||
        !SameEpoch(dispatch->second.observation.epoch, observation.epoch)) {
      return failed(GpuSkinManagerFallbackReason::DipEpochMismatch);
    }

    const auto upload = dispatch->second.uploads.find(
        observation.epoch.uploadEpoch);
    if (upload == dispatch->second.uploads.end() ||
        upload->second.preparedToken == 0u) {
      return failed(GpuSkinManagerFallbackReason::PreparedDrawMiss);
    }
    RawTickAccumulator resolveTimer(
        m_fullDiagnostics, m_diagnostics.resolvePositiveTiming);
    result.nativeUploadBypassed = result.nativeUploadBypassed ||
        upload->second.nativeBypassed;
    if (result.bypassFuseKey == 0u)
      result.bypassFuseKey = upload->second.fuseKey;
    const auto seen = upload->second.seenDips.find(
        observation.epoch.dipOrdinal);
    if (seen == upload->second.seenDips.end() || !seen->second.exact) {
      return failed(GpuSkinManagerFallbackReason::DipSignatureMismatch);
    }

    auto prepared = m_preparedDraws.find(upload->second.preparedToken);
    if (prepared == m_preparedDraws.end()) {
      return failed(GpuSkinManagerFallbackReason::PreparedDrawMiss);
    }
    result.key = prepared->second.key;
    result.leaseId = prepared->second.leaseId;
    result.plan = prepared->second.consumerPlan;
    result.ledger = prepared->second.consumerLedger;
    if (result.bypassFuseKey == 0u)
      result.bypassFuseKey = prepared->second.bypassFuseKey;
    if (!isSubmittedPreparedDraw(prepared->second)) {
      return failed(GpuSkinManagerFallbackReason::BatchNotSubmitted);
    }
    if (!isExactDipEpoch(prepared->second, observation))
      return failed(GpuSkinManagerFallbackReason::DipEpochMismatch);

    if (m_mode == GpuSkinMode::Bypass &&
        !prepared->second.consumerPlanExplicit &&
        !prepared->second.bypassCommitted) {
      const uint32_t inputOnlyAllowed =
          ConsumerMask(GpuSkinConsumerBits::Main) |
          ConsumerMask(GpuSkinConsumerBits::Shadow);
      const bool inputOnlyMain =
          IsVertexShaderInputOnlyRoute(m_executionRoute) &&
          m_executionRouteExplicit && !m_executionRouteInvalid &&
          static_cast<bool>(prepared->second.inputLease) &&
          prepared->second.inputLease.desc.consumerBits ==
              inputOnlyAllowed &&
          (requestedConsumerMask &
           ConsumerMask(GpuSkinConsumerBits::Main)) != 0u &&
          (requestedConsumerMask & ~inputOnlyAllowed) == 0u &&
          !result.nativeUploadBypassed;
      if (inputOnlyMain) {
        const uint32_t allowed = ledgerConsumerMask();
        const DipSignature signature = makeDipSignature(
            prepared->second, observation, allowed);
        prepared->second.expectedDipSignature = signature;
        prepared->second.consumerPlan.known = allowed;
        prepared->second.consumerPlan.leaseBacked = requestedConsumerMask;
        prepared->second.consumerPlan.notRequested =
            allowed & ~prepared->second.consumerPlan.leaseBacked;
        prepared->second.consumerPlan.signature = signature;
        prepared->second.consumerPlan.signature.consumerBits = allowed;
        prepared->second.consumerPlanExplicit = true;
        m_diagnostics.classified += CountConsumerBits(allowed);
        ++m_diagnostics.vsInputOnlyMainResolves;
      } else {
        if (IsVertexShaderInputOnlyRoute(m_executionRoute) &&
            static_cast<bool>(prepared->second.inputLease)) {
          ++m_diagnostics.vsInputOnlyConsumerRejects;
        }
        // 普通 compute candidate 仍执行原生 CPU kernel；缺少 Bypass plan
        // 表示没有被选中。VS-B0 只接受 Main 与可选 Shadow；Outline
        // 仍在这里退回原生 CPU draw。
        return failed(GpuSkinManagerFallbackReason::ConsumerNotAllowed);
      }
    }

    const uint32_t consumerMask = requestedConsumerMask;
    const DipSignature signature = makeDipSignature(
        prepared->second, observation, consumerMask);
    if (!reserveResolvedConsumers(prepared->second, consumerMask,
                                  signature)) {
      return failed(GpuSkinManagerFallbackReason::ConsumerNotAllowed);
    }
    seen->second.consumerBits |= consumerMask;

    result.lease = prepared->second.lease;
    result.inputLease = prepared->second.inputLease;
    result.parity = prepared->second.parity;
    result.plan = prepared->second.consumerPlan;
    result.ledger = prepared->second.consumerLedger;
    result.leaseId = prepared->second.leaseId;
    result.lease.desc.dipOrdinal = observation.epoch.dipOrdinal;
    result.lease.desc.consumerBits = consumerMask;
    result.exactMatch = true;
    result.mainOverrideAllowed = !parityOnly &&
        IsMainOverrideMode(m_mode) &&
        (consumerMask & ConsumerMask(GpuSkinConsumerBits::Main)) != 0u;
    result.nativeUploadBypassed = result.nativeUploadBypassed ||
        upload->second.nativeBypassed;
    if (prepared->second.bypassFuseKey != 0u)
      result.bypassFuseKey = prepared->second.bypassFuseKey;
    if (!parityOnly) {
      ++m_diagnostics.formalResolveSuccesses;
      if (IsVertexShaderBypassRoute(m_executionRoute) &&
          (consumerMask & ConsumerMask(GpuSkinConsumerBits::Main)) != 0u) {
        ++m_diagnostics.vsInputOnlyMainResolves;
      }
      if (requestsShadow) {
        ++m_diagnostics.shadowResolveSuccesses;
      }
    }
    return result;
  }

  GpuSkinResolvedDraw resolveShadowLease(
      const GpuSkinPreparedDrawKey& key) {
    GpuSkinResolvedDraw result;
    if (m_mode == GpuSkinMode::Disabled) {
      result.fallback = GpuSkinManagerFallbackReason::ModeNotProducing;
      return result;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto failed = [&](GpuSkinManagerFallbackReason reason) {
      recordFallback(reason);
      result.fallback = reason;
      if (result.nativeUploadBypassed) {
        fuseBypassPath(result.bypassFuseKey, result.key.geosetData,
                       result.key.layerIndex, false);
      }
      return result;
    };
    if ((AllowedFormalConsumerMask(m_mode) &
         ConsumerMask(GpuSkinConsumerBits::Shadow)) == 0u) {
      return failed(m_mode == GpuSkinMode::Observe
          ? GpuSkinManagerFallbackReason::ObserveOnly
          : GpuSkinManagerFallbackReason::ConsumerNotAllowed);
    }
    if (!m_deviceReady || m_pendingDeviceEpoch != 0u)
      return failed(GpuSkinManagerFallbackReason::DeviceEpochRequiresRebind);
    auto prepared = m_preparedDraws.find(key.token);
    if (prepared == m_preparedDraws.end())
      return failed(GpuSkinManagerFallbackReason::PreparedDrawMiss);
    RawTickAccumulator resolveTimer(
        m_fullDiagnostics, m_diagnostics.shadowResolvePositiveTiming);
    result.key = prepared->second.key;
    result.leaseId = prepared->second.leaseId;
    result.plan = prepared->second.consumerPlan;
    result.ledger = prepared->second.consumerLedger;
    result.nativeUploadBypassed = prepared->second.bypassCommitted;
    result.bypassFuseKey = prepared->second.bypassFuseKey;
    if (!samePreparedKey(prepared->second.key, key))
      return failed(GpuSkinManagerFallbackReason::PreparedDrawMiss);
    if (!isSubmittedPreparedDraw(prepared->second)) {
      return failed(GpuSkinManagerFallbackReason::BatchNotSubmitted);
    }
    if (prepared->second.bypassCommitted &&
        (prepared->second.bypassConsumerContractFailed ||
         m_fusedBypassKeys.find(prepared->second.bypassFuseKey) !=
             m_fusedBypassKeys.end())) {
      return failed(GpuSkinManagerFallbackReason::BypassFused);
    }

    const uint32_t shadowMask = ConsumerMask(GpuSkinConsumerBits::Shadow);
    DipSignature signature = prepared->second.consumerPlan.signature;
    signature.consumerBits = shadowMask;
    if (signature.uploadToken == 0u ||
        !reserveResolvedConsumers(prepared->second, shadowMask, signature))
      return failed(GpuSkinManagerFallbackReason::ConsumerNotAllowed);

    result.lease = prepared->second.lease;
    // VS-S1 的阴影消费者通过独立 Shadow resolve 到达；它必须拿到与
    // 普通 DIP resolve 完全相同的 generation-pinned 静态输入租约。
    // isSubmittedPreparedDraw 已先闭合 batch/storage 身份，失败仍保持空租约。
    result.inputLease = prepared->second.inputLease;
    result.parity = prepared->second.parity;
    result.plan = prepared->second.consumerPlan;
    result.ledger = prepared->second.consumerLedger;
    result.leaseId = prepared->second.leaseId;
    result.lease.desc.consumerBits = shadowMask;
    result.exactMatch = true;
    result.mainOverrideAllowed = false;
    return result;
  }

  bool planConsumers(const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
                     const GpuSkinConsumerPlan& plan) {
    if (!IsComputeProducingMode(m_mode))
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator planTimer(
        m_fullDiagnostics, m_diagnostics.planConsumerTiming);
    PreparedDraw* prepared = findSettlementDraw(
        key, leaseId, SettlementLookupCaller::Plan);
    if (prepared == nullptr)
      return false;

    const uint32_t allowed = ledgerConsumerMask();
    const bool partitioned = plan.known != 0u &&
        prepared->expectedDipSignature.uploadToken != 0u &&
        prepared->expectedDipSignature.dispatchEpoch != 0u &&
        prepared->expectedDipSignature.uploadEpoch != 0u &&
        (plan.known & ~allowed) == 0u &&
        (plan.notRequested & plan.leaseBacked) == 0u &&
        (plan.notRequested | plan.leaseBacked) == plan.known &&
        plan.signature.consumerBits == plan.known &&
        SameDipSignature(plan.signature, prepared->expectedDipSignature);
    if (!partitioned || prepared->consumerState ==
            GpuSkinConsumerWindowState::Closed) {
      ++m_diagnostics.planMismatch;
      return false;
    }

    if (prepared->consumerPlan.known != 0u) {
      const bool samePlan =
          prepared->consumerPlan.known == plan.known &&
          prepared->consumerPlan.notRequested == plan.notRequested &&
          prepared->consumerPlan.leaseBacked == plan.leaseBacked &&
          SameDipSignature(prepared->consumerPlan.signature,
                           plan.signature);
      const bool promotesLegacyPlan =
          !prepared->consumerPlanExplicit &&
          (prepared->consumerPlan.known & ~plan.known) == 0u &&
          (prepared->consumerPlan.notRequested &
           ~plan.notRequested) == 0u &&
          (prepared->consumerPlan.leaseBacked & ~plan.leaseBacked) == 0u &&
          SameDipSignature(prepared->consumerPlan.signature,
                           plan.signature);
      if (samePlan) {
        prepared->consumerPlanExplicit = true;
        return true;
      }
      if (promotesLegacyPlan) {
        m_diagnostics.classified += CountConsumerBits(
            plan.known & ~prepared->consumerPlan.known);
        prepared->consumerPlan = plan;
        prepared->consumerPlanExplicit = true;
        return true;
      }
      ++m_diagnostics.planMismatch;
      return false;
    }

    prepared->consumerPlan = plan;
    prepared->consumerPlanExplicit = true;
    m_diagnostics.classified += CountConsumerBits(plan.known);
    return true;
  }

  bool commitConsumer(const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
                      GpuSkinConsumerBits consumer) {
    if (!IsComputeProducingMode(m_mode))
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator commitTimer(
        m_fullDiagnostics, m_diagnostics.commitConsumerTiming);
    PreparedDraw* prepared = findSettlementDraw(
        key, leaseId, SettlementLookupCaller::Commit);
    const uint32_t bit = ConsumerMask(consumer);
    if (prepared == nullptr)
      return false;
    if (IsSingleConsumerBit(bit) &&
        (terminalConsumerMask(*prepared) & bit) != 0u) {
      ++m_diagnostics.duplicate;
      return false;
    }
    if (!validateSettlementConsumer(*prepared, bit))
      return false;
    if ((prepared->consumerLedger.resolved & bit) == 0u) {
      ++m_diagnostics.planMismatch;
      return false;
    }
    prepared->consumerLedger.consumed |= bit;
    ++m_diagnostics.consumed;
    ++m_diagnostics.dipLeasesConsumed;
    if (bit == ConsumerMask(GpuSkinConsumerBits::Shadow))
      ++m_diagnostics.shadowLeasesConsumed;
    return true;
  }

  bool failConsumer(const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
                    GpuSkinConsumerBits consumer,
                    GpuSkinConsumerFailure failure) {
    if (!IsComputeProducingMode(m_mode))
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator failTimer(
        m_fullDiagnostics, m_diagnostics.failConsumerTiming);
    PreparedDraw* prepared = findSettlementDraw(
        key, leaseId, SettlementLookupCaller::Fail);
    const uint32_t bit = ConsumerMask(consumer);
    if (prepared == nullptr)
      return false;
    if (IsSingleConsumerBit(bit) &&
        (terminalConsumerMask(*prepared) & bit) != 0u) {
      ++m_diagnostics.duplicate;
      return false;
    }
    if (!validateSettlementConsumer(*prepared, bit))
      return false;
    // 旧 P4 Bypass 一旦授权 native kernel skip，就不能再宣称 CPU fallback。
    // VS-B0 则在 preflight 中强制保留 CPU kernel，且 bypassCommitted 必须为
    // false；此时某个 Shadow consumer 在逐 draw 过滤后不提交 GPU 是合法的
    // CPU fallback，不能把它错误升级成 planMismatch/suppress/leak。
    const bool inputOnlyCpuFallback =
        IsVertexShaderInputOnlyRoute(m_executionRoute) &&
        m_executionRouteExplicit && !m_executionRouteInvalid &&
        !prepared->bypassCommitted &&
        failure == GpuSkinConsumerFailure::CpuFallback;
    if (m_mode == GpuSkinMode::Bypass &&
        failure != GpuSkinConsumerFailure::SuppressAndFuse &&
        !inputOnlyCpuFallback) {
      ++m_diagnostics.planMismatch;
      return false;
    }
    settleConsumerFailure(*prepared, bit, failure);
    return true;
  }

  bool closeBatchConsumerWindow(uint64_t batchId,
                                uint64_t renderThreadId) {
    if (!IsComputeProducingMode(m_mode) || batchId == 0u)
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator closeTimer(
        m_fullDiagnostics, m_diagnostics.closeConsumerTiming);
    if (!acceptRenderThread(renderThreadId) ||
        m_submittedBatches.find(batchId) == m_submittedBatches.end()) {
      return false;
    }
    return closeConsumerBatch(batchId, false);
  }

  bool takePendingBatch(uint64_t renderThreadId,
                        GpuSkinPendingBatch& output) {
    if (!IsComputeProducingMode(m_mode))
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!acceptRenderThread(renderThreadId) || m_pendingBatch == nullptr ||
        m_pendingBatch->renderThreadId != renderThreadId ||
        m_pendingBatch->state != GpuSkinBatchState::Pending) {
      ++m_diagnostics.batchClaimFailures;
      recordFallback(GpuSkinManagerFallbackReason::BatchClaimFailed);
      return false;
    }
    m_pendingBatch->state = GpuSkinBatchState::Claimed;
    output = *m_pendingBatch;
    return true;
  }

  bool markPendingBatchSubmitted(uint64_t batchId,
                                 uint64_t renderThreadId,
                                 Rc<DxvkFence> uploadRetireFence,
                                 uint64_t uploadRetireValue) {
    if (!IsComputeProducingMode(m_mode))
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!acceptRenderThread(renderThreadId))
      return false;
    if (m_pendingBatch == nullptr || m_pendingBatch->batchId != batchId) {
      const auto retiring = m_retiringClaims.find(batchId);
      if (retiring == m_retiringClaims.end() ||
          retiring->second.resources == nullptr ||
          retiring->second.batch.renderThreadId != renderThreadId ||
          (retiring->second.batch.state != GpuSkinBatchState::Claimed &&
           retiring->second.batch.state != GpuSkinBatchState::Submitting)) {
        ++m_diagnostics.batchClaimFailures;
        recordFallback(GpuSkinManagerFallbackReason::BatchClaimFailed);
        return false;
      }
      return completeRetiringClaim(
          retiring, std::move(uploadRetireFence), uploadRetireValue, false);
    }
    if (m_pendingBatch == nullptr ||
        m_resources == nullptr ||
        m_pendingBatch->resourceOwner != m_resources ||
        m_pendingBatch->renderThreadId != renderThreadId ||
        (m_pendingBatch->state != GpuSkinBatchState::Claimed &&
         m_pendingBatch->state != GpuSkinBatchState::Submitting)) {
      ++m_diagnostics.batchClaimFailures;
      recordFallback(GpuSkinManagerFallbackReason::BatchClaimFailed);
      return false;
    }
    return completeCurrentSubmission(
        std::move(uploadRetireFence), uploadRetireValue);
  }

  bool retireBatch(uint64_t batchId, uint64_t renderThreadId,
                   Rc<DxvkFence> fence, uint64_t value) {
    if (!IsComputeProducingMode(m_mode))
      return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!acceptRenderThread(renderThreadId))
      return false;
    if (fence == nullptr || value == 0u) {
      recordFallback(GpuSkinManagerFallbackReason::OutputRetirementFailed);
      return false;
    }

    auto submitted = m_submittedBatches.find(batchId);
    bool retiringEpoch = false;
    if (submitted == m_submittedBatches.end()) {
      auto retired = m_retiringBatches.find(batchId);
      if (retired == m_retiringBatches.end()) {
        const auto autoRetired = std::find(
            m_autoRetiredBatchIds.begin(), m_autoRetiredBatchIds.end(),
            batchId);
        if (autoRetired != m_autoRetiredBatchIds.end()) {
          m_autoRetiredBatchIds.erase(autoRetired);
          return true;
        }
        return false;
      }
      submitted = retired;
      retiringEpoch = true;
    }

    std::shared_ptr<War3GpuSkinResources> resources =
        submitted->second.resources;
    if (resources == nullptr)
      return false;

    if (!retiringEpoch) {
      if (deferOpenConsumerBatch(batchId))
        return false;
      if (!consumerBatchReadyForRetirement(batchId)) {
        ++m_diagnostics.retireDeferred;
        closeConsumerBatch(batchId, true);
        return false;
      }
    }

    const auto retireLeases = [&](std::vector<OutputLease>& leases) {
      for (auto lease = leases.begin(); lease != leases.end();) {
        if (resources->retireOutput(*lease, fence, value)) {
          // 每次成功立即移除；若 owner 身份不闭合，只重试失败的租约。
          lease = leases.erase(lease);
        } else {
          ++lease;
        }
      }
    };
    retireLeases(submitted->second.outputLeases);
    if (submitted->second.inputReceipts.size() <
        submitted->second.inputStorageLeases.size()) {
      recordFallback(GpuSkinManagerFallbackReason::OutputRetirementFailed);
      return false;
    }
    for (auto storage = submitted->second.inputStorageLeases.begin();
         storage != submitted->second.inputStorageLeases.end();) {
      const auto receipt = findInputReceipt(
          submitted->second.inputReceipts, *storage);
      if (receipt != nullptr &&
          resources->retireOutput(*storage, fence, value) &&
          settleInputReceipt(
              receipt, *storage,
              GpuSkinInputLeaseReceiptState::ConsumerCommitted,
              fence, value)) {
        storage = submitted->second.inputStorageLeases.erase(storage);
      } else {
        ++storage;
      }
    }
    if (!submitted->second.outputLeases.empty() ||
        !submitted->second.inputStorageLeases.empty()) {
      recordFallback(GpuSkinManagerFallbackReason::OutputRetirementFailed);
      resources->pollRetired();
      pollRetiredResources();
      return false;
    }

    submitted->second.state = GpuSkinBatchState::Retired;
    if (retiringEpoch) {
      m_retiringBatches.erase(batchId);
    } else {
      erasePreparedBatch(batchId);
      m_submittedBatches.erase(batchId);
    }
    resources->pollRetired();
    pollRetiredResources();
    return true;
  }

  bool refreshRetirementDiagnosticsForTest(uint64_t renderThreadId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (renderThreadId == 0u || m_renderThreadId == 0u ||
        renderThreadId != m_renderThreadId || m_hostSubmissionActive) {
      return false;
    }
    if (m_resources != nullptr)
      m_resources->pollRetired();
    pollRetiredResources();
    return true;
  }

  GpuSkinManagerDiagnostics snapshotDiagnostics() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    GpuSkinManagerDiagnostics result = m_diagnostics;
    result.hotPathTimingFrequency = uint64_t(
        dxvk::high_resolution_clock::get_frequency());
    result.renderableReverseEntries = m_renderableLayouts.size();
    result.pendingDispatches = m_dispatches.size();
    uint64_t pendingUploads = 0;
    for (const auto& dispatch : m_dispatches)
      pendingUploads += dispatch.second.uploads.size();
    result.pendingUploads = pendingUploads;
    result.pendingPreparedDraws = m_preparedDraws.size();
    result.pendingSubmissions = m_submittedBatches.size() +
        m_retiringBatches.size() +
        m_retiringClaims.size() +
        (m_pendingBatch != nullptr ? 1u : 0u);
    result.retiredResourceEpochs = m_retiredResources.size();
    result.retiringClaims = m_retiringClaims.size();
    result.retiredResourceEpochLimit = kMaxRetiredResourceEpochs;
    result.retiringClaimLimit = kMaxRetiringClaims;
    result.retirementBackpressured = m_retirementBackpressured;
    result.pendingDeviceEpoch = m_pendingDeviceEpoch;
    result.pendingBridgeResetGeneration =
        m_pendingBridgeResetGeneration;
    result.deviceReady = IsComputeProducingMode(m_mode)
        ? m_deviceReady : !m_callbackQuarantined;
    result.callbackQuarantined = m_callbackQuarantined;
    result.activeResourceInFlight =
        m_resources != nullptr && m_resources->hasInFlightResources();
    result.bypassPending += m_bypassAuthorizations.size();
    result.pendingBypassAuthorizations = m_bypassAuthorizations.size();
    if (m_resources != nullptr)
      result.resources = m_resources->diagnostics();
    return result;
  }

  void reset(uint64_t bridgeResetGeneration, bool bridgeQuiescent) {
    if (m_mode == GpuSkinMode::Disabled)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    invalidateDispatchCpuOnlySealView();
    if (bridgeResetGeneration > m_pendingBridgeResetGeneration)
      m_pendingBridgeResetGeneration = bridgeResetGeneration;
    if (!bridgeQuiescent)
      return;
    pollRetiredResources();
    if (!retireCurrentEpoch())
      return;
    m_currentMapEpoch = 0u;
    m_nextToken = 0u;
    m_nextParitySequence = 0u;
    resetRegularRetirementPollKey();
    pollRetiredResources();
  }

  void callbackException() noexcept {
    // Every native thunk is synchronous on its producer thread. Revoke the
    // bridge's passive current-flush view before any locking/recovery work so a
    // swallowed exception cannot leave managerless authorization behind.
    InvalidateNativeDispatchCpuOnlySealViewForCallbackException();
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      invalidateDispatchCpuOnlySealView();
      ++m_diagnostics.callbackExceptions;
      recordFallback(GpuSkinManagerFallbackReason::CallbackException);
      m_hostSubmissionActive = false;
      if (m_pendingBatch != nullptr) {
        if (m_pendingBatch->state == GpuSkinBatchState::Pending)
          cancelPendingBatch(m_pendingBatch->batchId, true);
        else {
          retireCurrentEpoch();
          m_submissionRecoveryBlocked = true;
          updateRetirementBackpressure();
        }
      }
      m_diagnostics.dispatchLeaks += m_dispatches.size();
      for (const auto& dispatch : m_dispatches) {
        for (const auto& upload : dispatch.second.uploads) {
          if (upload.second.nativeBypassed) {
            fuseBypassPath(upload.second.fuseKey,
                           upload.second.observation.geosetData,
                           dispatch.second.observation.layerIndex, true);
          }
        }
      }
      m_dispatches.clear();
      m_dispatchStacks.clear();
      m_diagnostics.bypassPending += m_bypassAuthorizations.size();
      for (const auto& authorization : m_bypassAuthorizations) {
        fuseBypassPath(authorization.second.fuseKey,
                       authorization.second.key.geosetData,
                       authorization.second.key.layerIndex, true);
      }
      m_bypassAuthorizations.clear();
    } catch (...) {
    }
  }

private:
  struct LearnedLayout {
    uint32_t outputFormat = 0;
    uint32_t outputStride = 0;
    uint32_t vertexCount = 0;
    uint32_t fvf = 0;
    uint32_t generation = 1;
    uint32_t baselineIndexCount = 0;
    uint32_t baselinePrimitiveType = 0;
    uint32_t baselineMinVertexIndex = 0;
    uint32_t baselineNumVertices = 0;
    uint32_t baselinePrimitiveCount = 0;
    // Production-only admission hint. It can avoid the first model-cache and
    // static-cache lookups, but it never participates in irreversible
    // authorization: preflight still re-reads the current model stamp, palette,
    // and every static stream exactly; completion rechecks the current stamp
    // and palette before the native kernel may return normally.
    std::shared_ptr<const GpuSkinStaticResource> bypassStaticHint;
    bool exactSingleDipConfirmed = false;
    bool bypassFused = false;
  };

  struct SeenDip {
    NativeDipInput dip;
    uint32_t consumerBits = 0;
    bool exact = false;
  };

  struct UploadState {
    NativeUploadObservation observation;
    uint32_t preparedToken = 0;
    uint32_t dipCount = 0;
    uint32_t lastDipOrdinal = 0;
    InlineFlatMap<uint32_t, SeenDip, 2u> seenDips;
    uint64_t fuseKey = 0;
    bool nativeBypassed = false;
  };

  struct DispatchState {
    NativeDispatchObservation observation;
    uint64_t activeUploadEpoch = 0;
    uint32_t uploadCount = 0;
    uint32_t dipCount = 0;
    InlineFlatMap<uint64_t, UploadState, 1u> uploads;
  };

  struct PreparedDraw {
    GpuSkinPreparedDrawKey key;
    OutputLease lease;
    GpuSkinInputLease inputLease;
    std::shared_ptr<const GpuSkinStaticResource> resource;
    GpuSkinParityMetadata parity;
    uintptr_t paletteAddress = 0;
    uint32_t paletteGroupCount = 0;
    uint32_t expectedVertexCount = 0;
    uint32_t expectedOutputStride = 0;
    uint32_t expectedIndexCount = 0;
    uint64_t expectedIndexContentHash = 0;
    uint64_t resourceContentHash = 0;
    uint64_t bypassFuseKey = 0;
    uint64_t leaseId = 0;
    GpuSkinConsumerPlan consumerPlan;
    GpuSkinConsumerLedger consumerLedger;
    DipSignature expectedDipSignature;
    GpuSkinConsumerWindowState consumerState =
        GpuSkinConsumerWindowState::Open;
    std::vector<uint8_t> paletteBytes;
    bool submitted = false;
    bool consumerPlanExplicit = false;
    bool bypassCommitted = false;
    bool bypassOpaqueEligible = false;
    bool bypassConsumerContractFailed = false;
  };

  struct BypassAuthorizationState {
    GpuSkinPreparedDrawKey key;
    NativeEpochKey epoch;
    uint64_t dispatchEpoch = 0;
    uint64_t fuseKey = 0;
    uintptr_t gxDeviceD3d = 0;
    uintptr_t nativeD3DDevice = 0;
    uintptr_t nativeVertexBuffer = 0;
    uintptr_t nativeIndexBuffer = 0;
    uint32_t expectedIndexCount = 0;
    uint32_t predictedStartIndex = 0;
    uint32_t ringBaseVertexAfter = 0;
    uint32_t ringNextVertexAfter = 0;
    uint32_t indexRingNextBefore = 0;
    bool hostApproved = false;
  };

  struct SubmittedBatch {
    GpuSkinBatchState state = GpuSkinBatchState::Submitted;
    std::shared_ptr<War3GpuSkinResources> resources;
    std::vector<OutputLease> outputLeases;
    std::vector<OutputLease> inputStorageLeases;
    std::vector<std::shared_ptr<GpuSkinInputLeaseReceipt>> inputReceipts;
  };

  struct RetiringClaim {
    GpuSkinPendingBatch batch;
    std::shared_ptr<War3GpuSkinResources> resources;
    bool hostAccepted = false;
  };

  bool isSubmittedPreparedDraw(const PreparedDraw& prepared) const {
    const auto& resource = prepared.resource;
    if (!m_deviceReady || m_pendingDeviceEpoch != 0u ||
        !prepared.submitted || !prepared.lease || prepared.leaseId == 0u ||
        prepared.leaseId != prepared.lease.leaseId ||
        prepared.key.frameTag == 0u || prepared.key.flushEpoch == 0u ||
        prepared.key.batchId == 0u || prepared.key.token == 0u ||
        prepared.lease.desc.frameTag != prepared.key.frameTag ||
        prepared.lease.desc.token != prepared.key.token ||
        prepared.lease.desc.mapEpoch == 0u ||
        prepared.lease.desc.mapEpoch != m_currentMapEpoch ||
        prepared.lease.desc.deviceEpoch == 0u ||
        prepared.lease.desc.deviceEpoch != m_boundDeviceEpoch ||
        resource == nullptr ||
        resource->state != GpuSkinStaticResourceState::Ready ||
        resource->key.mapEpoch != prepared.lease.desc.mapEpoch ||
        resource->key.deviceEpoch != prepared.lease.desc.deviceEpoch ||
        resource->key.geosetData != prepared.key.geosetData ||
        resource->key.contentHash == 0u ||
        resource->key.contentHash != prepared.resourceContentHash ||
        resource->key.layoutGeneration != kStaticPackingLayoutGeneration ||
        resource->record == nullptr ||
        resource->record->geosetDataPtr !=
            reinterpret_cast<void*>(prepared.key.geosetData) ||
        resource->record->contentHash != prepared.resourceContentHash ||
        resource->record->vertexCount != prepared.expectedVertexCount ||
        resource->indexContentHash == 0u ||
        resource->indexContentHash != prepared.expectedIndexContentHash) {
      return false;
    }

    const auto batch = m_submittedBatches.find(prepared.key.batchId);
    if (batch == m_submittedBatches.end() ||
        batch->second.state != GpuSkinBatchState::Submitted ||
        batch->second.resources == nullptr ||
        batch->second.resources != m_resources) {
      return false;
    }
    const bool outputOwnedByBatch = std::any_of(
        batch->second.outputLeases.begin(),
        batch->second.outputLeases.end(), [&](const OutputLease& lease) {
          return lease.leaseId == prepared.lease.leaseId &&
                 lease.desc.token == prepared.key.token &&
                 lease.desc.frameTag == prepared.key.frameTag;
        });
    const GpuSkinInputLease& input = prepared.inputLease;
    const auto storage = std::find_if(
        batch->second.inputStorageLeases.begin(),
        batch->second.inputStorageLeases.end(),
        [&](const OutputLease& lease) {
          return lease.desc.token == prepared.key.token;
        });
    const auto receipt = std::find_if(
        batch->second.inputReceipts.begin(),
        batch->second.inputReceipts.end(),
        [&](const std::shared_ptr<GpuSkinInputLeaseReceipt>& candidate) {
          return candidate != nullptr &&
              candidate->desc.token == prepared.key.token;
        });
    if (!input) {
      // 没有发布 VS-A 输入租约时，同一 token 也不得残留匿名存储租约或
      // receipt capability。
      return outputOwnedByBatch &&
          storage == batch->second.inputStorageLeases.end() &&
          receipt == batch->second.inputReceipts.end();
    }
    if (storage == batch->second.inputStorageLeases.end() ||
        receipt == batch->second.inputReceipts.end())
      return false;

    const bool inputOnlyRoute =
        IsVertexShaderNoComputeRoute(m_executionRoute);
    const bool inputStorageOwnsCapability = inputOnlyRoute &&
        prepared.lease.leaseId == storage->leaseId &&
        prepared.lease.pageId == storage->pageId &&
        prepared.lease.pageGeneration == storage->pageGeneration &&
        prepared.lease.slice.buffer() == storage->slice.buffer() &&
        prepared.lease.slice.offset() == storage->slice.offset() &&
        prepared.lease.slice.length() == storage->slice.length();
    if (!outputOwnedByBatch && !inputStorageOwnsCapability)
      return false;

    const uint32_t expectedInputConsumers =
        ConsumerMask(GpuSkinConsumerBits::Main) |
        ConsumerMask(GpuSkinConsumerBits::Shadow);
    const uint64_t paletteBytes =
        uint64_t(input.desc.paletteMatrixCount) * 48u;
    return input.storageLeaseId == storage->leaseId &&
        input.storagePageId == storage->pageId &&
        input.storagePageGeneration == storage->pageGeneration &&
        input.receipt == *receipt &&
        input.desc.mapEpoch == prepared.lease.desc.mapEpoch &&
        input.desc.deviceEpoch == prepared.lease.desc.deviceEpoch &&
        input.desc.frameTag == prepared.key.frameTag &&
        input.desc.token == prepared.key.token &&
        input.desc.dispatchEpoch == prepared.lease.desc.dispatchEpoch &&
        input.desc.uploadEpoch == prepared.lease.desc.uploadEpoch &&
        input.desc.staticByteOffset == input.staticSource.offset() &&
        input.desc.staticByteLength == input.staticSource.length() &&
        input.desc.paletteByteOffset == storage->slice.offset() &&
        input.desc.paletteByteLength == storage->slice.length() &&
        input.desc.vertexCount == prepared.expectedVertexCount &&
        input.desc.paletteMatrixCount != 0u &&
        paletteBytes == input.desc.paletteByteLength &&
        input.desc.outputFormat == prepared.key.outputFormat &&
        input.desc.layoutGeneration == kStaticPackingLayoutGeneration &&
        input.desc.consumerBits == expectedInputConsumers &&
        input.staticSource.buffer() == resource->staticSource.buffer() &&
        input.staticSource.offset() == resource->staticSource.offset() &&
        input.staticSource.length() == resource->staticSource.length() &&
        input.palette.buffer() == storage->slice.buffer() &&
        input.palette.offset() == storage->slice.offset() &&
        input.palette.length() == storage->slice.length() &&
        (*receipt)->state == GpuSkinInputLeaseReceiptState::Pending &&
        (*receipt)->consumerFence == nullptr &&
        (*receipt)->consumerFenceValue == 0u &&
        (*receipt)->storageLeaseId == storage->leaseId &&
        (*receipt)->storagePageId == storage->pageId &&
        (*receipt)->storagePageGeneration == storage->pageGeneration &&
        SameGpuSkinInputLeaseDesc((*receipt)->desc, input.desc) &&
        (*receipt)->staticSource.buffer() == input.staticSource.buffer() &&
        (*receipt)->staticSource.offset() == input.staticSource.offset() &&
        (*receipt)->staticSource.length() == input.staticSource.length() &&
        (*receipt)->palette.buffer() == storage->slice.buffer() &&
        (*receipt)->palette.offset() == storage->slice.offset() &&
        (*receipt)->palette.length() == storage->slice.length() &&
        storage->desc.mapEpoch == prepared.lease.desc.mapEpoch &&
        storage->desc.deviceEpoch == prepared.lease.desc.deviceEpoch &&
        storage->desc.frameTag == prepared.key.frameTag &&
        storage->desc.byteOffset == storage->slice.offset() &&
        storage->desc.byteLength == storage->slice.length() &&
        storage->desc.vertexStride == 0u &&
        storage->desc.vertexCount == 0u &&
        storage->desc.consumerBits == input.desc.consumerBits;
  }

  bool isExactDipEpoch(
      const PreparedDraw& prepared,
      const NativeDipObservation& observation) const {
    return observation.epoch.flushEpoch == prepared.key.flushEpoch &&
           observation.epoch.dispatchEpoch != 0u &&
           observation.epoch.uploadEpoch != 0u &&
           observation.epoch.dipOrdinal != 0u &&
           prepared.lease.desc.dispatchEpoch ==
               observation.epoch.dispatchEpoch &&
           prepared.lease.desc.uploadEpoch == observation.epoch.uploadEpoch;
  }

  uint32_t ledgerConsumerMask() const {
    if (m_mode == GpuSkinMode::Dual)
      return ConsumerMask(GpuSkinConsumerBits::Parity);
    return AllowedFormalConsumerMask(m_mode);
  }

  DipSignature makeDipSignature(
      const PreparedDraw& prepared,
      const NativeDipObservation& observation,
      uint32_t consumerBits) const {
    DipSignature signature;
    signature.renderThreadId = observation.epoch.renderThreadId;
    signature.uploadToken = prepared.key.token;
    signature.dispatchEpoch = observation.epoch.dispatchEpoch;
    signature.uploadEpoch = observation.epoch.uploadEpoch;
    signature.expectedStream0 =
        observation.dip.stream0Resource.comVertexBuffer;
    signature.dipOrdinal = observation.epoch.dipOrdinal;
    signature.consumerBits = consumerBits;
    signature.primitiveType = observation.dip.primitiveType;
    signature.baseVertexIndex = uint32_t(observation.dip.baseVertexIndex);
    signature.minVertexIndex = observation.dip.minVertexIndex;
    signature.numVertices = observation.dip.numVertices;
    signature.startIndex = observation.dip.startIndex;
    signature.primitiveCount = observation.dip.primitiveCount;
    signature.vertexStride = observation.dip.vertexStride;
    return signature;
  }

  DipSignature makeExpectedDipSignature(
      const PreparedDraw& prepared,
      const NativeUploadObservation& upload,
      const LearnedLayout& baseline,
      uint32_t predictedStartIndex,
      uint32_t expectedDipOrdinal,
      uint32_t consumerBits) const {
    DipSignature signature;
    signature.renderThreadId = upload.epoch.renderThreadId;
    signature.uploadToken = prepared.key.token;
    signature.dispatchEpoch = upload.epoch.dispatchEpoch;
    signature.uploadEpoch = upload.epoch.uploadEpoch;
    signature.expectedStream0 = upload.nativeVertexBuffer;
    signature.dipOrdinal = expectedDipOrdinal;
    signature.consumerBits = consumerBits;
    signature.primitiveType = baseline.baselinePrimitiveType;
    signature.baseVertexIndex = upload.ringBaseVertexAfter;
    signature.minVertexIndex = baseline.baselineMinVertexIndex;
    signature.numVertices = baseline.baselineNumVertices;
    signature.startIndex = predictedStartIndex;
    signature.primitiveCount = baseline.baselinePrimitiveCount;
    signature.vertexStride = prepared.expectedOutputStride;
    return signature;
  }

  enum class SettlementLookupCaller : uint8_t {
    Plan,
    Commit,
    Fail,
  };

  PreparedDraw* findSettlementDraw(
      const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
      SettlementLookupCaller caller) {
    const auto noteCaller = [&]() {
      switch (caller) {
      case SettlementLookupCaller::Plan:
        ++m_diagnostics.unreservedPlanLookup;
        break;
      case SettlementLookupCaller::Commit:
        ++m_diagnostics.unreservedCommitLookup;
        break;
      case SettlementLookupCaller::Fail:
        ++m_diagnostics.unreservedFailLookup;
        break;
      }
    };
    const auto found = m_preparedDraws.find(key.token);
    if (found == m_preparedDraws.end()) {
      ++m_diagnostics.unreserved;
      noteCaller();
      ++m_diagnostics.unreservedLookupTokenMiss;
      return nullptr;
    }
    if (!samePreparedKey(found->second.key, key)) {
      ++m_diagnostics.unreserved;
      noteCaller();
      ++m_diagnostics.unreservedLookupKeyMismatch;
      return nullptr;
    }
    if (leaseId == 0u || found->second.leaseId != leaseId ||
        found->second.lease.leaseId != leaseId) {
      ++m_diagnostics.unreserved;
      noteCaller();
      ++m_diagnostics.unreservedLookupLeaseMismatch;
      return nullptr;
    }
    if (!isSubmittedPreparedDraw(found->second)) {
      ++m_diagnostics.unreserved;
      noteCaller();
      ++m_diagnostics.unreservedLookupNotSubmitted;
      return nullptr;
    }
    return &found->second;
  }

  bool validateSettlementConsumer(const PreparedDraw& prepared,
                                  uint32_t consumer) {
    if (!IsSingleConsumerBit(consumer) ||
        (consumer & ~ledgerConsumerMask()) != 0u ||
        prepared.consumerState == GpuSkinConsumerWindowState::Closed ||
        (prepared.consumerPlan.known & consumer) == 0u) {
      ++m_diagnostics.unreserved;
      ++m_diagnostics.unreservedSettlementInvalid;
      return false;
    }
    if ((prepared.consumerPlan.leaseBacked & consumer) == 0u ||
        (prepared.consumerPlan.notRequested & consumer) != 0u) {
      ++m_diagnostics.planMismatch;
      return false;
    }
    return true;
  }

  static uint32_t terminalConsumerMask(const PreparedDraw& prepared) {
    return prepared.consumerLedger.consumed |
        prepared.consumerLedger.cpuFallback |
        prepared.consumerLedger.suppressFuse;
  }

  bool reserveResolvedConsumers(PreparedDraw& prepared,
                                uint32_t consumers,
                                const DipSignature& signature) {
    if (consumers == 0u || (consumers & ~ledgerConsumerMask()) != 0u) {
      ++m_diagnostics.unreserved;
      ++m_diagnostics.unreservedReserveMaskInvalid;
      return false;
    }
    if (prepared.expectedDipSignature.uploadToken == 0u)
      prepared.expectedDipSignature = signature;
    if (!SameDipSignature(prepared.expectedDipSignature, signature)) {
      ++m_diagnostics.planMismatch;
      return false;
    }

    const uint32_t missing = consumers & ~prepared.consumerPlan.known;
    if (missing != 0u) {
      if (m_mode == GpuSkinMode::Bypass || prepared.consumerPlanExplicit) {
        ++m_diagnostics.unreserved;
        ++m_diagnostics.unreservedReserveMissingKnown;
        return false;
      }
      prepared.consumerPlan.known |= missing;
      prepared.consumerPlan.leaseBacked |= missing;
      prepared.consumerPlan.signature = signature;
      prepared.consumerPlan.signature.consumerBits =
          prepared.consumerPlan.known;
      m_diagnostics.classified += CountConsumerBits(missing);
    }
    if ((prepared.consumerPlan.leaseBacked & consumers) != consumers ||
        (prepared.consumerPlan.notRequested & consumers) != 0u ||
        !SameDipSignature(prepared.consumerPlan.signature, signature)) {
      ++m_diagnostics.planMismatch;
      return false;
    }

    const uint32_t newlyResolved =
        consumers & ~prepared.consumerLedger.resolved;
    prepared.consumerLedger.resolved |= consumers;
    m_diagnostics.resolved += CountConsumerBits(newlyResolved);
    return true;
  }

  void settleConsumerFailure(PreparedDraw& prepared, uint32_t consumers,
                             GpuSkinConsumerFailure failure) {
    if (failure == GpuSkinConsumerFailure::CpuFallback) {
      prepared.consumerLedger.cpuFallback |= consumers;
      m_diagnostics.cpuFallback += CountConsumerBits(consumers);
      return;
    }

    prepared.consumerLedger.suppressFuse |= consumers;
    m_diagnostics.suppressed += CountConsumerBits(consumers);
    prepared.bypassConsumerContractFailed = true;
    fuseBypassPath(prepared.bypassFuseKey, prepared.key.geosetData,
                   prepared.key.layerIndex, false);
  }

  bool closeConsumerBatch(uint64_t batchId, bool retirementFallback) {
    bool clean = true;
    bool found = false;
    const uint32_t allowed = ledgerConsumerMask();
    for (auto& item : m_preparedDraws) {
      PreparedDraw& prepared = item.second;
      if (prepared.key.batchId != batchId)
        continue;
      found = true;
      if (prepared.consumerState == GpuSkinConsumerWindowState::Closed)
        continue;

      const uint32_t unknown = allowed & ~prepared.consumerPlan.known;
      if (unknown != 0u) {
        prepared.consumerPlan.known |= unknown;
        if (m_mode == GpuSkinMode::Bypass) {
          if (prepared.consumerPlanExplicit || prepared.bypassCommitted) {
            // An explicit plan or a committed native bypass makes every
            // missing consumer bit a real protocol error. Keep those bits
            // lease-backed so the pending pass suppresses and fuses them.
            prepared.consumerPlan.leaseBacked |= unknown;
            ++m_diagnostics.planMismatch;
            clean = false;
          } else {
            // Only an unselected implicit compute candidate is known to have
            // retained the native CPU path. Its unused consumers are safely
            // classified as not requested rather than invented leases.
            prepared.consumerPlan.notRequested |= unknown;
          }
        } else {
          // Preserve the P1-P3 implicit close behavior outside Bypass mode.
          prepared.consumerPlan.notRequested |= unknown;
          clean = false;
        }
        prepared.consumerPlan.signature = prepared.expectedDipSignature;
        prepared.consumerPlan.signature.consumerBits =
            prepared.consumerPlan.known;
        m_diagnostics.classified += CountConsumerBits(unknown);
      }

      const uint32_t pending = prepared.consumerPlan.leaseBacked &
          ~terminalConsumerMask(prepared);
      if (pending != 0u) {
        m_diagnostics.reservationLeak += CountConsumerBits(pending);
        settleConsumerFailure(
            prepared, pending,
            m_mode == GpuSkinMode::Bypass
                ? GpuSkinConsumerFailure::SuppressAndFuse
                : GpuSkinConsumerFailure::CpuFallback);
        clean = false;
      }
      prepared.consumerState = GpuSkinConsumerWindowState::Closed;
    }
    return found && clean && !retirementFallback;
  }

  bool deferOpenConsumerBatch(uint64_t batchId) {
    bool open = false;
    for (auto& item : m_preparedDraws) {
      PreparedDraw& prepared = item.second;
      if (prepared.key.batchId == batchId &&
          prepared.consumerState != GpuSkinConsumerWindowState::Closed) {
        prepared.consumerState = GpuSkinConsumerWindowState::RetireDeferred;
        open = true;
      }
    }
    if (!open)
      return false;
    ++m_diagnostics.retireDeferred;
    closeConsumerBatch(batchId, true);
    return true;
  }

  bool consumerBatchReadyForRetirement(uint64_t batchId) const {
    bool found = false;
    for (const auto& item : m_preparedDraws) {
      const PreparedDraw& prepared = item.second;
      if (prepared.key.batchId != batchId)
        continue;
      found = true;
      if (prepared.consumerState != GpuSkinConsumerWindowState::Closed ||
          (prepared.consumerPlan.leaseBacked &
           ~terminalConsumerMask(prepared)) != 0u) {
        return false;
      }
    }
    return found;
  }

  struct NativeThunkTimingSample {
    GpuSkinManagerNativeThunkKind kind =
        GpuSkinManagerNativeThunkKind::Count;
    uint64_t stableId = 0u;
    int64_t enterStart = 0;
    int64_t enterElapsed = 0;
    int64_t bodyStart = 0;
    bool enabled = false;
  };

  NativeThunkTimingSample beginNativeThunkTiming(
      GpuSkinManagerNativeThunkKind kind, uint64_t stableId) const noexcept {
    NativeThunkTimingSample sample;
    sample.kind = kind;
    sample.stableId = stableId;
    // The event epoch is carried once in this sample object so enter, body and
    // leave cannot accidentally choose different cohorts. Orphan id 0 is
    // explicitly excluded; the low-byte predicate matches the native bridge.
    sample.enabled = m_mode == GpuSkinMode::Bypass && !m_fullDiagnostics &&
        stableId != 0u && (stableId & 0xffu) == 0xa5u;
    sample.enterStart = sample.enabled
        ? dxvk::high_resolution_clock::get_counter() : 0;
    return sample;
  }

  GpuSkinManagerNativeThunkTimingDiagnostics& nativeThunkTiming(
      GpuSkinManagerNativeThunkKind kind) noexcept {
    return m_diagnostics.productionLightNativeThunkTiming[
        static_cast<size_t>(kind)];
  }

  void recordNativeThunkEnter(NativeThunkTimingSample& sample,
                              bool accepted) noexcept {
    if (!sample.enabled)
      return;
    sample.enterElapsed =
        dxvk::high_resolution_clock::get_counter() - sample.enterStart;
    // An admitted sample is published atomically with body/leave under the
    // existing leave lock. Thus even a relaxed diagnostics snapshot cannot
    // observe a half-published enter/body/leave call closure.
    if (accepted)
      return;
    GpuSkinManagerNativeThunkTimingDiagnostics& timing =
        nativeThunkTiming(sample.kind);
    RecordSampledNativeThunkTiming(timing.enter, sample.enterElapsed);
    ++timing.enterRejected;
  }

  void beginNativeThunkBody(NativeThunkTimingSample& sample) const noexcept {
    if (sample.enabled)
      sample.bodyStart = dxvk::high_resolution_clock::get_counter();
  }

  bool enterNativeCallback(NativeThunkTimingSample& sample) noexcept {
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      const std::thread::id thread = std::this_thread::get_id();
      if (m_callbackQuarantined) {
        recordNativeThunkEnter(sample, false);
        return false;
      }
      if (m_nativeCallbackActive ||
          (m_callbackThread != std::thread::id() &&
           m_callbackThread != thread)) {
        if (m_nativeCallbackActive)
          ++m_diagnostics.callbackReentries;
        else
          ++m_diagnostics.renderThreadMismatches;
        recordFallback(m_nativeCallbackActive
            ? GpuSkinManagerFallbackReason::CallbackReentry
            : GpuSkinManagerFallbackReason::RenderThreadMismatch);
        recordNativeThunkEnter(sample, false);
        return false;
      }
      m_callbackThread = thread;
      m_nativeCallbackActive = true;
      recordNativeThunkEnter(sample, true);
      return true;
    } catch (...) {
      InvalidateNativeDispatchCpuOnlySealViewForCallbackException();
      return false;
    }
  }

  void leaveNativeCallback(NativeThunkTimingSample& sample) noexcept {
    const int64_t leaveStart = sample.enabled
        ? dxvk::high_resolution_clock::get_counter() : 0;
    try {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_nativeCallbackActive = false;
      if (sample.enabled) {
        const int64_t leaveElapsed =
            dxvk::high_resolution_clock::get_counter() - leaveStart;
        GpuSkinManagerNativeThunkTimingDiagnostics& timing =
            nativeThunkTiming(sample.kind);
        RecordSampledNativeThunkTiming(timing.enter, sample.enterElapsed);
        RecordSampledNativeThunkTiming(
            timing.body, leaveStart - sample.bodyStart);
        RecordSampledNativeThunkTiming(timing.leave, leaveElapsed);
      }
    } catch (...) {
      InvalidateNativeDispatchCpuOnlySealViewForCallbackException();
    }
  }

  // Stable-id contract shared with the bridge sampler:
  //   flush -> flushEpoch; dispatch begin/end -> dispatchEpoch;
  //   preflight/cpu-rewrite/upload/fanout -> uploadEpoch;
  //   DIP -> uploadEpoch only while the observation is correlated.
  // Each thunk creates one sample object and carries it unchanged through all
  // three phases, including the body-exception settlement path.
  static void NativeFlushThunk(
      void* data, const NativeFlushObservation& observation) {
    Impl* self = reinterpret_cast<Impl*>(data);
    NativeThunkTimingSample timing = self->beginNativeThunkTiming(
        GpuSkinManagerNativeThunkKind::Flush, observation.flushEpoch);
    if (!self->enterNativeCallback(timing))
      return;
    self->beginNativeThunkBody(timing);
    try {
      self->submitFlush(observation);
    } catch (...) {
      self->callbackException();
    }
    self->leaveNativeCallback(timing);
  }

  static void NativeDispatchBeginThunk(
      void* data, const NativeDispatchObservation& observation) {
    Impl* self = reinterpret_cast<Impl*>(data);
    NativeThunkTimingSample timing = self->beginNativeThunkTiming(
        GpuSkinManagerNativeThunkKind::DispatchBegin,
        observation.epoch.dispatchEpoch);
    if (!self->enterNativeCallback(timing))
      return;
    self->beginNativeThunkBody(timing);
    try {
      self->beginDispatch(observation);
    } catch (...) {
      self->callbackException();
    }
    self->leaveNativeCallback(timing);
  }

  static void NativeDispatchEndThunk(
      void* data, const NativeDispatchSummary& summary) {
    Impl* self = reinterpret_cast<Impl*>(data);
    NativeThunkTimingSample timing = self->beginNativeThunkTiming(
        GpuSkinManagerNativeThunkKind::DispatchEnd,
        summary.dispatch.epoch.dispatchEpoch);
    if (!self->enterNativeCallback(timing))
      return;
    self->beginNativeThunkBody(timing);
    try {
      self->endDispatch(summary);
    } catch (...) {
      self->callbackException();
    }
    self->leaveNativeCallback(timing);
  }

  static bool NativeBypassPreflightThunk(
      void* data, const NativeUploadObservation& observation,
      NativeBypassAuthorization* authorization) {
    Impl* self = reinterpret_cast<Impl*>(data);
    if (authorization != nullptr)
      *authorization = {};
    NativeThunkTimingSample timing = self->beginNativeThunkTiming(
        GpuSkinManagerNativeThunkKind::Preflight,
        observation.epoch.uploadEpoch);
    if (!self->enterNativeCallback(timing))
      return false;
    self->beginNativeThunkBody(timing);
    bool result = false;
    try {
      result = self->preflightNativeBypass(observation, authorization);
    } catch (...) {
      self->callbackException();
    }
    self->leaveNativeCallback(timing);
    return result;
  }

  static bool NativeCpuRewriteOutputProofThunk(
      void* data, const NativeUploadObservation& observation,
      NativeCpuRewriteOutputProof* outputProof) {
    Impl* self = reinterpret_cast<Impl*>(data);
    if (outputProof != nullptr)
      *outputProof = {};
    NativeThunkTimingSample timing = self->beginNativeThunkTiming(
        GpuSkinManagerNativeThunkKind::CpuRewrite,
        observation.epoch.uploadEpoch);
    if (!self->enterNativeCallback(timing))
      return false;
    self->beginNativeThunkBody(timing);
    bool result = false;
    try {
      result = self->resolveNativeCpuRewriteOutputProof(
          observation, outputProof);
    } catch (...) {
      self->callbackException();
    }
    self->leaveNativeCallback(timing);
    return result;
  }

  static void NativeUploadThunk(
      void* data, const NativeUploadObservation& observation) {
    Impl* self = reinterpret_cast<Impl*>(data);
    NativeThunkTimingSample timing = self->beginNativeThunkTiming(
        GpuSkinManagerNativeThunkKind::Upload,
        observation.epoch.uploadEpoch);
    if (!self->enterNativeCallback(timing))
      return;
    self->beginNativeThunkBody(timing);
    try {
      self->noteNativeUpload(observation);
    } catch (...) {
      self->callbackException();
    }
    self->leaveNativeCallback(timing);
  }

  static void NativeDipThunk(
      void* data, const NativeDipObservation& observation) {
    Impl* self = reinterpret_cast<Impl*>(data);
    const uint64_t stableId = observation.correlated
        ? observation.epoch.uploadEpoch : 0u;
    NativeThunkTimingSample timing = self->beginNativeThunkTiming(
        GpuSkinManagerNativeThunkKind::Dip, stableId);
    if (!self->enterNativeCallback(timing))
      return;
    self->beginNativeThunkBody(timing);
    try {
      self->noteNativeDip(observation);
    } catch (...) {
      self->callbackException();
    }
    self->leaveNativeCallback(timing);
  }

  static void NativeFanoutThunk(
      void* data, const NativeUploadFanoutObservation& observation) {
    Impl* self = reinterpret_cast<Impl*>(data);
    NativeThunkTimingSample timing = self->beginNativeThunkTiming(
        GpuSkinManagerNativeThunkKind::Fanout,
        observation.epoch.uploadEpoch);
    if (!self->enterNativeCallback(timing))
      return;
    self->beginNativeThunkBody(timing);
    try {
      self->noteNativeFanout(observation);
    } catch (...) {
      self->callbackException();
    }
    self->leaveNativeCallback(timing);
  }

  void noteFlushOnly(const NativeFlushObservation& observation,
                     GpuSkinManagerFallbackReason reason,
                     int64_t queryTimingElapsed = 0) {
    std::lock_guard<std::mutex> lock(m_mutex);
    RecordRawTiming(m_diagnostics.flushQueryTiming,
                    queryTimingElapsed);
    if (!acceptRenderThread(observation.renderThreadId))
      return;
    ++m_diagnostics.flushCallbacks;
    noteModeMismatch(observation.mode);
    recordFallback(reason);
  }

  void noteHostRejection(uint64_t batchId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator finalizeTimer(
        m_fullDiagnostics, m_diagnostics.flushHostFinalizeTiming);
    ++m_diagnostics.batchesRejected;
    recordFallback(GpuSkinManagerFallbackReason::BatchNotSubmitted);
    if (m_pendingBatch != nullptr && m_pendingBatch->batchId == batchId) {
      cancelPendingBatch(batchId, false);
    } else {
      const auto retiring = m_retiringClaims.find(batchId);
      if (retiring != m_retiringClaims.end()) {
        std::shared_ptr<War3GpuSkinResources> resources =
            retiring->second.resources;
        cancelInputReceipts(retiring->second.batch.inputReceipts);
        if (resources != nullptr) {
          for (const OutputLease& lease : retiring->second.batch.outputLeases)
            resources->discardOutput(lease);
          for (const OutputLease& lease :
               retiring->second.batch.inputStorageLeases)
            resources->discardOutput(lease);
          resources->discardUploads();
        }
        m_retiringClaims.erase(retiring);
        if (resources != nullptr)
          resources->pollRetired();
        pollRetiredResources();
      }
    }
    if (m_pendingBatch == nullptr && m_retiringClaims.empty())
      m_submissionRecoveryBlocked = false;
    updateRetirementBackpressure();
  }

  void invalidateDispatchCpuOnlySealView() noexcept {
    m_dispatchCpuOnlySealFlushEpoch = 0u;
    m_dispatchCpuOnlySealCandidates.clear();
    m_dispatchCpuOnlySealCandidateTokensScratch.clear();
  }

  void publishDispatchCpuOnlySealView(
      const GpuSkinPendingBatch& recordedBatch) noexcept {
    invalidateDispatchCpuOnlySealView();
    if (m_mode != GpuSkinMode::Bypass || m_fullDiagnostics ||
        m_hostSubmissionActive ||
        m_pendingBridgeResetGeneration != 0u ||
        recordedBatch.flush.flushEpoch == 0u ||
        recordedBatch.flush.renderThreadId != m_renderThreadId) {
      return;
    }

    if (recordedBatch.preparedDraws.size() >
        kNativeDispatchCpuOnlySealViewCapacity) {
      return;
    }

    // The submitted batch is the positive half of an exact-negative proof.
    // Require a bijection with every current-flush candidate token before
    // publishing the compact renderable/layer view. This one scan happens at
    // publication; Common dispatches then use a bounded binary search instead
    // of rescanning all live batches.
    size_t currentFlushCandidateCount = 0u;
    for (const auto& candidate : m_candidateTokens) {
      if (candidate.first.flushEpoch == recordedBatch.flush.flushEpoch)
        ++currentFlushCandidateCount;
    }
    if (currentFlushCandidateCount != recordedBatch.preparedDraws.size())
      return;

    try {
      m_dispatchCpuOnlySealCandidates.reserve(
          recordedBatch.preparedDraws.size());
      m_dispatchCpuOnlySealCandidateTokensScratch.reserve(
          recordedBatch.preparedDraws.size());
      for (const GpuSkinPreparedDrawInfo& info :
           recordedBatch.preparedDraws) {
        if (info.key.flushEpoch != recordedBatch.flush.flushEpoch) {
          invalidateDispatchCpuOnlySealView();
          return;
        }
        const auto prepared = m_preparedDraws.find(info.key.token);
        const CandidateKey candidate = {
            info.key.flushEpoch,
            info.key.renderablePart,
            info.key.geosetData,
            info.key.layerIndex,
            info.key.outputFormat,
        };
        const auto candidateToken = m_candidateTokens.find(candidate);
        if (prepared == m_preparedDraws.end() ||
            !prepared->second.submitted ||
            !samePreparedKey(prepared->second.key, info.key) ||
            candidateToken == m_candidateTokens.end() ||
            candidateToken->second != info.key.token) {
          invalidateDispatchCpuOnlySealView();
          return;
        }
        m_dispatchCpuOnlySealCandidates.push_back({
            info.key.renderablePart,
            info.key.layerIndex,
        });
        m_dispatchCpuOnlySealCandidateTokensScratch.push_back(
            info.key.token);
      }
      std::sort(m_dispatchCpuOnlySealCandidateTokensScratch.begin(),
                m_dispatchCpuOnlySealCandidateTokensScratch.end());
      if (std::adjacent_find(
              m_dispatchCpuOnlySealCandidateTokensScratch.begin(),
              m_dispatchCpuOnlySealCandidateTokensScratch.end()) !=
          m_dispatchCpuOnlySealCandidateTokensScratch.end()) {
        invalidateDispatchCpuOnlySealView();
        return;
      }
      std::sort(m_dispatchCpuOnlySealCandidates.begin(),
                m_dispatchCpuOnlySealCandidates.end(),
                RenderableLayoutKeyLess);
      m_dispatchCpuOnlySealCandidates.erase(
          std::unique(m_dispatchCpuOnlySealCandidates.begin(),
                      m_dispatchCpuOnlySealCandidates.end()),
          m_dispatchCpuOnlySealCandidates.end());
      m_dispatchCpuOnlySealCandidateTokensScratch.clear();
    } catch (...) {
      invalidateDispatchCpuOnlySealView();
      return;
    }

    std::array<NativeDispatchCpuOnlySealCandidate,
               kNativeDispatchCpuOnlySealViewCapacity> nativeCandidates = {};
    if (m_dispatchCpuOnlySealCandidates.size() > nativeCandidates.size()) {
      invalidateDispatchCpuOnlySealView();
      return;
    }
    for (size_t i = 0u; i < m_dispatchCpuOnlySealCandidates.size(); ++i) {
      nativeCandidates[i] = {
          m_dispatchCpuOnlySealCandidates[i].renderablePart,
          m_dispatchCpuOnlySealCandidates[i].layerIndex,
      };
    }

    m_dispatchCpuOnlySealFlushEpoch =
        recordedBatch.flush.flushEpoch;
    ++m_diagnostics.dispatchCpuOnlySealViewPublishes;
    // Publication is deliberately the final action: bridge rejection keeps
    // its local path fail-closed without revoking the established manager
    // view or changing the legacy manager diagnostics contract.
    (void)PublishNativeDispatchCpuOnlySealView(
        recordedBatch.flush.flushEpoch, nativeCandidates.data(),
        uint32_t(m_dispatchCpuOnlySealCandidates.size()));
  }

  bool completeAcceptedHostSubmission(
      const GpuSkinPendingBatch& recordedBatch,
      Rc<DxvkFence> uploadRetireFence,
      uint64_t uploadRetireValue) {
    std::lock_guard<std::mutex> lock(m_mutex);
    RawTickAccumulator finalizeTimer(
        m_fullDiagnostics, m_diagnostics.flushHostFinalizeTiming);
    bool completed = false;

    if (m_pendingBatch != nullptr &&
        m_pendingBatch->batchId == recordedBatch.batchId &&
        m_pendingBatch->resourceOwner == recordedBatch.resourceOwner) {
      completed = completeCurrentSubmission(
          std::move(uploadRetireFence), uploadRetireValue);
    } else {
      auto retiring = m_retiringClaims.find(recordedBatch.batchId);
      if (retiring == m_retiringClaims.end()) {
        if (recordedBatch.resourceOwner != nullptr &&
            m_retiringClaims.size() < kMaxRetiringClaims) {
          RetiringClaim claim;
          claim.batch = recordedBatch;
          claim.batch.state = GpuSkinBatchState::Submitting;
          claim.resources = recordedBatch.resourceOwner;
          retiring = m_retiringClaims.emplace(
              recordedBatch.batchId, std::move(claim)).first;
          updateRetirementHighWater();
        }
      }
      if (retiring != m_retiringClaims.end()) {
        completed = completeRetiringClaim(
            retiring, std::move(uploadRetireFence),
            uploadRetireValue, true);
      } else {
        completed = completeDetachedRecordedBatch(
            recordedBatch, std::move(uploadRetireFence),
            uploadRetireValue);
      }
    }

    if (completed) {
      m_submissionRecoveryBlocked = !m_retiringClaims.empty();
      updateRetirementBackpressure();
      return true;
    }

    ++m_diagnostics.acceptedSubmissionRecoveryFailures;
    m_submissionRecoveryBlocked = true;
    recordFallback(GpuSkinManagerFallbackReason::UploadRetirementMissing);
    updateRetirementBackpressure();
    return false;
  }

  static bool inputReceiptMatchesStorage(
      const std::shared_ptr<GpuSkinInputLeaseReceipt>& receipt,
      const OutputLease& storage) {
    return receipt != nullptr && storage &&
        receipt->storageLeaseId == storage.leaseId &&
        receipt->storagePageGeneration == storage.pageGeneration &&
        receipt->storagePageId == storage.pageId &&
        receipt->desc.mapEpoch == storage.desc.mapEpoch &&
        receipt->desc.deviceEpoch == storage.desc.deviceEpoch &&
        receipt->desc.frameTag == storage.desc.frameTag &&
        receipt->desc.token == storage.desc.token &&
        receipt->desc.consumerBits == storage.desc.consumerBits &&
        receipt->desc.paletteByteOffset == storage.slice.offset() &&
        receipt->desc.paletteByteLength == storage.slice.length() &&
        receipt->palette.buffer() == storage.slice.buffer() &&
        receipt->palette.offset() == storage.slice.offset() &&
        receipt->palette.length() == storage.slice.length();
  }

  static bool settleInputReceipt(
      const std::shared_ptr<GpuSkinInputLeaseReceipt>& receipt,
      const OutputLease& storage, GpuSkinInputLeaseReceiptState state,
      const Rc<DxvkFence>& fence, uint64_t value) {
    if (!inputReceiptMatchesStorage(receipt, storage) ||
        state == GpuSkinInputLeaseReceiptState::Pending ||
        state == GpuSkinInputLeaseReceiptState::Cancelled ||
        fence == nullptr || value == 0u) {
      return false;
    }
    if (receipt->state == state) {
      return receipt->consumerFence == fence &&
          receipt->consumerFenceValue == value;
    }
    if (receipt->state != GpuSkinInputLeaseReceiptState::Pending ||
        receipt->consumerFence != nullptr ||
        receipt->consumerFenceValue != 0u) {
      return false;
    }
    receipt->consumerFence = fence;
    receipt->consumerFenceValue = value;
    receipt->state = state;
    return true;
  }

  static void cancelInputReceipts(
      const std::vector<std::shared_ptr<GpuSkinInputLeaseReceipt>>& receipts) {
    for (const auto& receipt : receipts) {
      if (receipt != nullptr &&
          receipt->state == GpuSkinInputLeaseReceiptState::Pending &&
          receipt->consumerFence == nullptr &&
          receipt->consumerFenceValue == 0u) {
        receipt->state = GpuSkinInputLeaseReceiptState::Cancelled;
      }
    }
  }

  static std::shared_ptr<GpuSkinInputLeaseReceipt> findInputReceipt(
      const std::vector<std::shared_ptr<GpuSkinInputLeaseReceipt>>& receipts,
      const OutputLease& storage) {
    const auto receipt = std::find_if(
        receipts.begin(), receipts.end(),
        [&storage](
            const std::shared_ptr<GpuSkinInputLeaseReceipt>& candidate) {
          return inputReceiptMatchesStorage(candidate, storage);
        });
    return receipt != receipts.end() ? *receipt : nullptr;
  }

  bool retireRecordedProducerResources(
      const GpuSkinPendingBatch& batch,
      const std::shared_ptr<War3GpuSkinResources>& resources,
      const Rc<DxvkFence>& fence, uint64_t value,
      bool retireOutputsOnProducerFence) {
    const bool needsFence = batch.requiresUploadRetirement ||
        !batch.staticUploads.empty() ||
        (retireOutputsOnProducerFence &&
         (!batch.outputLeases.empty() ||
          !batch.inputStorageLeases.empty()));
    if (resources == nullptr || batch.resourceOwner != resources ||
        batch.request.mapEpoch == 0u || batch.request.deviceEpoch == 0u ||
        resources->mapEpoch() != batch.request.mapEpoch ||
        resources->deviceEpoch() != batch.request.deviceEpoch ||
        (needsFence && (fence == nullptr || value == 0u))) {
      return false;
    }

    bool retired = true;
    for (const GpuSkinStaticUpload& upload : batch.staticUploads)
      retired &= resources->retireStaticUpload(upload, fence, value);
    if (batch.requiresUploadRetirement)
      retired &= resources->retireUploads(fence, value);
    if (retireOutputsOnProducerFence) {
      for (const OutputLease& lease : batch.outputLeases)
        retired &= resources->retireOutput(lease, fence, value);
      if (batch.inputStorageLeases.size() != batch.inputReceipts.size()) {
        retired = false;
      } else {
        for (const OutputLease& lease : batch.inputStorageLeases) {
          const auto receipt = findInputReceipt(batch.inputReceipts, lease);
          const bool storageRetired = receipt != nullptr &&
              resources->retireOutput(lease, fence, value);
          retired &= storageRetired && settleInputReceipt(
              receipt, lease, GpuSkinInputLeaseReceiptState::ProducerOnly,
              fence, value);
        }
      }
    }
    return retired;
  }

  void noteSubmittedComputeWork(const GpuSkinPendingBatch& batch) {
    for (const GpuSkinComputeBatch& compute : batch.computeBatches) {
      m_diagnostics.computeVerticesSubmitted +=
          compute.actualVertexCount;
      m_diagnostics.computeRoundedInvocationsSubmitted +=
          compute.roundedInvocationCount;
      m_diagnostics.computeLaunchedInvocationsSubmitted +=
          compute.launchedInvocationCount;
    }
  }

  void noteSubmittedVsInputs(const GpuSkinPendingBatch& batch) {
    m_diagnostics.vsInputCopiesSubmitted += batch.inputCopies.size();
    for (const GpuSkinInputCopy& copy : batch.inputCopies)
      m_diagnostics.vsInputBytesSubmitted += copy.byteCount;
  }

  bool completeCurrentSubmission(
      Rc<DxvkFence> uploadRetireFence,
      uint64_t uploadRetireValue) {
    if (m_pendingBatch == nullptr || m_resources == nullptr ||
        m_pendingBatch->resourceOwner != m_resources ||
        (m_pendingBatch->state != GpuSkinBatchState::Claimed &&
         m_pendingBatch->state != GpuSkinBatchState::Submitting)) {
      return false;
    }

    m_pendingBatch->state = GpuSkinBatchState::Submitting;
    if (uploadRetireFence != nullptr && uploadRetireValue != 0u) {
      m_pendingBatch->producerRetireFence =
          std::move(uploadRetireFence);
      m_pendingBatch->producerRetireValue = uploadRetireValue;
    }
    if (!retireRecordedProducerResources(
            *m_pendingBatch, m_resources,
            m_pendingBatch->producerRetireFence,
            m_pendingBatch->producerRetireValue, false)) {
      recordFallback(GpuSkinManagerFallbackReason::UploadRetirementMissing);
      return false;
    }

    const uint64_t batchId = m_pendingBatch->batchId;
    for (const GpuSkinPreparedDrawInfo& info :
         m_pendingBatch->preparedDraws) {
      const GpuSkinPreparedDrawKey& key = info.key;
      auto prepared = m_preparedDraws.find(key.token);
      if (prepared != m_preparedDraws.end() &&
          prepared->second.key.batchId == batchId)
        prepared->second.submitted = true;
    }

    if (!m_pendingBatch->outputLeases.empty() ||
        !m_pendingBatch->inputStorageLeases.empty()) {
      SubmittedBatch submitted;
      submitted.state = GpuSkinBatchState::Submitted;
      submitted.resources = m_resources;
      submitted.outputLeases = m_pendingBatch->outputLeases;
      submitted.inputStorageLeases =
          m_pendingBatch->inputStorageLeases;
      submitted.inputReceipts = m_pendingBatch->inputReceipts;
      m_submittedBatches[batchId] = std::move(submitted);
    }
    m_pendingBatch->state = GpuSkinBatchState::Submitted;
    ++m_diagnostics.batchesSubmitted;
    m_diagnostics.jobsSubmitted += m_pendingBatch->preparedDraws.size();
    m_diagnostics.computeDispatchesSubmitted +=
        m_pendingBatch->computeBatches.size();
    noteSubmittedVsInputs(*m_pendingBatch);
    noteSubmittedComputeWork(*m_pendingBatch);
    m_pendingBatch.reset();
    if (m_retiringClaims.empty())
      m_submissionRecoveryBlocked = false;
    updateRetirementBackpressure();
    return true;
  }

  bool completeRetiringClaim(
      std::unordered_map<uint64_t, RetiringClaim>::iterator retiring,
      Rc<DxvkFence> uploadRetireFence,
      uint64_t uploadRetireValue,
      bool recoveredHostSubmission) {
    if (retiring == m_retiringClaims.end() ||
        retiring->second.resources == nullptr ||
        retiring->second.batch.resourceOwner != retiring->second.resources ||
        !trackRetiredResource(retiring->second.resources)) {
      return false;
    }

    RetiringClaim& claim = retiring->second;
    claim.hostAccepted |= recoveredHostSubmission;
    claim.batch.state = GpuSkinBatchState::Submitting;
    if (uploadRetireFence != nullptr && uploadRetireValue != 0u) {
      claim.batch.producerRetireFence = std::move(uploadRetireFence);
      claim.batch.producerRetireValue = uploadRetireValue;
    }
    if (!retireRecordedProducerResources(
            claim.batch, claim.resources,
            claim.batch.producerRetireFence,
            claim.batch.producerRetireValue, true)) {
      recordFallback(GpuSkinManagerFallbackReason::UploadRetirementMissing);
      return false;
    }

    const uint64_t batchId = claim.batch.batchId;
    const size_t jobCount = claim.batch.preparedDraws.size();
    const size_t dispatchCount = claim.batch.computeBatches.size();
    const bool hostAccepted = claim.hostAccepted;
    noteSubmittedVsInputs(claim.batch);
    noteSubmittedComputeWork(claim.batch);
    claim.resources->pollRetired();
    m_retiringClaims.erase(retiring);
    rememberAutoRetiredBatch(batchId);
    ++m_diagnostics.batchesSubmitted;
    m_diagnostics.jobsSubmitted += jobCount;
    m_diagnostics.computeDispatchesSubmitted += dispatchCount;
    ++m_diagnostics.epochClaimAutoRetirements;
    if (hostAccepted)
      ++m_diagnostics.acceptedSubmissionRecoveries;
    if (m_retiringClaims.empty())
      m_submissionRecoveryBlocked = false;
    updateRetirementBackpressure();
    return true;
  }

  bool completeDetachedRecordedBatch(
      const GpuSkinPendingBatch& recordedBatch,
      Rc<DxvkFence> uploadRetireFence,
      uint64_t uploadRetireValue) {
    std::shared_ptr<War3GpuSkinResources> resources =
        recordedBatch.resourceOwner;
    if (resources == nullptr || !trackRetiredResource(resources))
      return false;

    GpuSkinPendingBatch retirement = recordedBatch;
    retirement.state = GpuSkinBatchState::Submitting;
    retirement.producerRetireFence = std::move(uploadRetireFence);
    retirement.producerRetireValue = uploadRetireValue;
    if (!retireRecordedProducerResources(
            retirement, resources, retirement.producerRetireFence,
            retirement.producerRetireValue, true)) {
      return false;
    }

    rememberAutoRetiredBatch(retirement.batchId);
    ++m_diagnostics.batchesSubmitted;
    m_diagnostics.jobsSubmitted += retirement.preparedDraws.size();
    m_diagnostics.computeDispatchesSubmitted +=
        retirement.computeBatches.size();
    noteSubmittedVsInputs(retirement);
    noteSubmittedComputeWork(retirement);
    ++m_diagnostics.epochClaimAutoRetirements;
    ++m_diagnostics.acceptedSubmissionRecoveries;
    resources->pollRetired();
    updateRetirementBackpressure();
    return true;
  }

  void rememberAutoRetiredBatch(uint64_t batchId) {
    if (batchId == 0u)
      return;
    if (std::find(m_autoRetiredBatchIds.begin(),
                  m_autoRetiredBatchIds.end(), batchId) !=
        m_autoRetiredBatchIds.end()) {
      return;
    }
    if (m_autoRetiredBatchIds.size() >=
        kMaxAutoRetiredBatchTombstones) {
      m_autoRetiredBatchIds.pop_front();
    }
    m_autoRetiredBatchIds.push_back(batchId);
  }

  void noteModeMismatch(GpuSkinMode observedMode) {
    if (observedMode != GpuSkinMode::Disabled && observedMode != m_mode)
      ++m_diagnostics.modeMismatches;
  }

  void recordFallback(GpuSkinManagerFallbackReason reason) {
    const size_t index = static_cast<size_t>(reason);
    if (index != 0u && index < m_diagnostics.fallbackByReason.size()) {
      ++m_diagnostics.fallbackCount;
      ++m_diagnostics.fallbackByReason[index];
    }
  }

  void invalidateBypassStaticHint(const GpuSkinPreparedDrawKey& key) {
    if (m_mode != GpuSkinMode::Bypass)
      return;
    const LayoutKey layoutKey = {key.geosetData, key.layerIndex};
    const auto layout = m_layouts.find(layoutKey);
    if (layout != m_layouts.end())
      layout->second.bypassStaticHint.reset();
  }

  void recordStrictUploadReject(
      GpuSkinStrictUploadRejectReason strictReason,
      GpuSkinManagerFallbackReason fallbackReason,
      uint32_t outputFormat) {
    const size_t index = static_cast<size_t>(strictReason);
    if (index != 0u &&
        index < m_diagnostics.strictUploadRejectByReason.size()) {
      ++m_diagnostics.strictUploadRejectByReason[index];
      const size_t formatBucket = outputFormat < 7u
          ? size_t(outputFormat) : size_t(7u);
      ++m_diagnostics.strictUploadRejectByFormat[formatBucket][index];
    }
    recordFallback(fallbackReason);
  }

  static void clearCpuDipBaseline(LearnedLayout& layout) {
    layout.baselineIndexCount = 0u;
    layout.baselinePrimitiveType = 0u;
    layout.baselineMinVertexIndex = 0u;
    layout.baselineNumVertices = 0u;
    layout.baselinePrimitiveCount = 0u;
    layout.exactSingleDipConfirmed = false;
  }

  static uint64_t makeBypassFuseKey(const PreparedDraw& prepared) {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = HashMix(hash, uint64_t(prepared.key.geosetData));
    hash = HashMix(hash, prepared.key.layerIndex);
    hash = HashMix(hash, prepared.key.outputFormat);
    hash = HashMix(hash, prepared.resourceContentHash);
    hash = HashMix(hash, prepared.expectedVertexCount);
    hash = HashMix(hash, prepared.expectedOutputStride);
    hash = HashMix(hash, prepared.expectedIndexCount);
    hash = HashMix(hash, prepared.expectedIndexContentHash);
    return hash != 0u ? hash : 1u;
  }

  void fuseBypassPath(uint64_t fuseKey, uintptr_t geosetData,
                      uint32_t layerIndex, bool countMismatch) {
    bool newlyFused = false;
    if (fuseKey != 0u)
      newlyFused = m_fusedBypassKeys.emplace(fuseKey).second;

    if (geosetData != 0u) {
      const LayoutKey layoutKey = {geosetData, layerIndex};
      newlyFused = m_fusedBypassLayouts.emplace(layoutKey).second ||
          newlyFused;
      const auto layout = m_layouts.find(layoutKey);
      if (layout != m_layouts.end()) {
        newlyFused = newlyFused || !layout->second.bypassFused;
        layout->second.bypassFused = true;
        layout->second.bypassStaticHint.reset();
        clearCpuDipBaseline(layout->second);
      }
    }

    if (countMismatch) {
      ++m_diagnostics.bypassMismatches;
      ++m_diagnostics.postSkipMismatches;
      recordFallback(
          GpuSkinManagerFallbackReason::BypassPostCommitMismatch);
    }
    if (newlyFused)
      ++m_diagnostics.bypassFuses;
  }

  bool acceptRenderThread(uint64_t renderThreadId) {
    if (renderThreadId == 0u)
      return false;
    if (m_renderThreadId == 0u) {
      m_renderThreadId = renderThreadId;
      return true;
    }
    if (m_renderThreadId == renderThreadId)
      return true;
    ++m_diagnostics.renderThreadMismatches;
    recordFallback(GpuSkinManagerFallbackReason::RenderThreadMismatch);
    return false;
  }

  bool ensureEpoch(const FlushRequest& request) {
    if (!m_deviceReady || m_pendingDeviceEpoch != 0u ||
        m_boundDeviceEpoch == 0u ||
        request.deviceEpoch != m_boundDeviceEpoch) {
      ++m_diagnostics.deviceEpochRejects;
      recordFallback(
          GpuSkinManagerFallbackReason::DeviceEpochRequiresRebind);
      return false;
    }

    if (m_currentMapEpoch != 0u &&
        m_currentMapEpoch != request.mapEpoch &&
        !retireCurrentEpoch()) {
      return false;
    }

    if (m_resources == nullptr) {
      if (m_device == nullptr)
        return false;
      m_resources = std::make_shared<War3GpuSkinResources>(
          m_device, m_budgets);
    }
    m_currentMapEpoch = request.mapEpoch;
    return true;
  }

  bool canRetireCurrentEpoch() {
    if (m_pendingBatch != nullptr &&
        m_pendingBatch->resourceOwner != m_resources) {
      ++m_diagnostics.retirementLimitViolations;
      m_submissionRecoveryBlocked = true;
      updateRetirementBackpressure();
      return false;
    }
    const bool hasClaimedPending = m_pendingBatch != nullptr &&
        m_pendingBatch->state != GpuSkinBatchState::Pending;
    const bool duplicateClaim = hasClaimedPending &&
        m_retiringClaims.find(m_pendingBatch->batchId) !=
            m_retiringClaims.end();
    const bool needsClaimSlot = hasClaimedPending && !duplicateClaim;
    const bool ownerAlreadyTracked = m_resources != nullptr &&
        std::find(m_retiredResources.begin(), m_retiredResources.end(),
                  m_resources) != m_retiredResources.end();
    const bool needsResourceSlot = m_resources != nullptr &&
        m_resources->hasInFlightResources() && !ownerAlreadyTracked;
    if (duplicateClaim) {
      ++m_diagnostics.retirementLimitViolations;
      m_submissionRecoveryBlocked = true;
      updateRetirementBackpressure();
      return false;
    }
    if ((needsClaimSlot &&
         m_retiringClaims.size() >= kMaxRetiringClaims) ||
        (needsResourceSlot &&
         m_retiredResources.size() >= kMaxRetiredResourceEpochs)) {
      updateRetirementBackpressure();
      return false;
    }
    return true;
  }

  bool trackRetiredResource(
      const std::shared_ptr<War3GpuSkinResources>& resources,
      bool includeCurrent = false) {
    if (resources == nullptr || (resources == m_resources && !includeCurrent))
      return resources != nullptr;
    if (std::find(m_retiredResources.begin(), m_retiredResources.end(),
                  resources) != m_retiredResources.end()) {
      return true;
    }
    if (m_retiredResources.size() >= kMaxRetiredResourceEpochs) {
      ++m_diagnostics.retirementLimitViolations;
      m_submissionRecoveryBlocked = true;
      updateRetirementBackpressure();
      return false;
    }
    m_retiredResources.push_back(resources);
    updateRetirementHighWater();
    updateRetirementBackpressure();
    return true;
  }

  bool retireCurrentEpoch() {
    if (!canRetireCurrentEpoch())
      return false;

    const bool hadEpoch = m_currentMapEpoch != 0u ||
        m_pendingBatch != nullptr || !m_submittedBatches.empty() ||
        !m_preparedDraws.empty() || !m_bypassAuthorizations.empty();
    if (!m_dispatches.empty()) {
      m_diagnostics.dispatchLeaks += m_dispatches.size();
      ++m_diagnostics.epochLeaks;
    }

    if (m_pendingBatch != nullptr &&
        m_pendingBatch->resourceOwner != nullptr) {
      std::shared_ptr<War3GpuSkinResources> resources =
          m_pendingBatch->resourceOwner;
      if (m_pendingBatch->state == GpuSkinBatchState::Pending) {
        cancelInputReceipts(m_pendingBatch->inputReceipts);
        for (const OutputLease& lease : m_pendingBatch->outputLeases)
          resources->discardOutput(lease);
        for (const OutputLease& lease :
             m_pendingBatch->inputStorageLeases)
          resources->discardOutput(lease);
        resources->discardUploads();
      } else {
        ++m_diagnostics.batchClaimFailures;
        recordFallback(GpuSkinManagerFallbackReason::BatchClaimFailed);
        RetiringClaim claim;
        claim.batch = std::move(*m_pendingBatch);
        claim.resources = resources;
        const bool inserted = m_retiringClaims.emplace(
            claim.batch.batchId, std::move(claim)).second;
        if (!inserted) {
          ++m_diagnostics.retirementLimitViolations;
          m_submissionRecoveryBlocked = true;
          updateRetirementBackpressure();
          return false;
        }
        updateRetirementHighWater();
      }
    }
    m_pendingBatch.reset();
    m_carriedStaticUploads.clear();
    m_carriedUploadRetirement = false;

    // Epoch turnover is not a license to erase open reservations. Settle them
    // fail-closed and leave an explicit deferred/leak trail before ownership
    // moves to the retiring resource epoch.
    for (const auto& submitted : m_submittedBatches) {
      if (!deferOpenConsumerBatch(submitted.first) &&
          !consumerBatchReadyForRetirement(submitted.first)) {
        ++m_diagnostics.retireDeferred;
        closeConsumerBatch(submitted.first, true);
      }
    }
    for (auto& submitted : m_submittedBatches)
      m_retiringBatches.emplace(submitted.first, std::move(submitted.second));
    m_submittedBatches.clear();

    if (m_resources != nullptr && m_resources->hasInFlightResources() &&
        !trackRetiredResource(m_resources, true)) {
      ++m_diagnostics.retirementLimitViolations;
      return false;
    }
    m_resources.reset();
    m_currentMapEpoch = 0u;
    invalidateDispatchCpuOnlySealView();
    for (const auto& dispatch : m_dispatches) {
      for (const auto& upload : dispatch.second.uploads) {
        if (upload.second.nativeBypassed) {
          fuseBypassPath(upload.second.fuseKey,
                         upload.second.observation.geosetData,
                         dispatch.second.observation.layerIndex, true);
        }
      }
    }
    m_dispatches.clear();
    m_dispatchStacks.clear();
    m_diagnostics.bypassPending += m_bypassAuthorizations.size();
    for (const auto& authorization : m_bypassAuthorizations) {
      fuseBypassPath(authorization.second.fuseKey,
                     authorization.second.key.geosetData,
                     authorization.second.key.layerIndex, true);
    }
    m_bypassAuthorizations.clear();
    m_layouts.clear();
    m_renderableLayouts.clear();
    m_renderableLayoutBloom.clear();
    m_candidateTokens.clear();
    m_preparedDraws.clear();
    if (hadEpoch)
      ++m_diagnostics.epochTransitions;
    updateRetirementBackpressure();
    if (m_pendingBridgeResetGeneration != 0u &&
        AcknowledgeNativeBridgeOwnerEpochRetired(
            m_pendingBridgeResetGeneration)) {
      m_pendingBridgeResetGeneration = 0u;
    }
    return true;
  }

  void pollRetiredResources() {
    for (auto& claim : m_retiringClaims) {
      if (claim.second.resources != nullptr)
        claim.second.resources->pollRetired();
    }
    for (auto it = m_retiredResources.begin();
         it != m_retiredResources.end();) {
      (*it)->pollRetired();
      if (!(*it)->hasInFlightResources())
        it = m_retiredResources.erase(it);
      else
        ++it;
    }

    for (auto it = m_retiringClaims.begin();
         it != m_retiringClaims.end();) {
      auto candidate = it++;
      if (candidate->second.batch.producerRetireFence != nullptr &&
          candidate->second.batch.producerRetireValue != 0u) {
        completeRetiringClaim(candidate, {}, 0u, false);
      }
    }
    for (auto it = m_retiredResources.begin();
         it != m_retiredResources.end();) {
      (*it)->pollRetired();
      if (!(*it)->hasInFlightResources())
        it = m_retiredResources.erase(it);
      else
        ++it;
    }
    updateRetirementHighWater();
    updateRetirementBackpressure();
  }

  void pollRetiredResourcesForFlushFrame(const FlushRequest& request) {
    if (m_lastRetirementPollMapEpoch == request.mapEpoch &&
        m_lastRetirementPollDeviceEpoch == request.deviceEpoch &&
        m_lastRetirementPollFrameTag == request.frameTag) {
      return;
    }

    pollRetiredResources();
    m_lastRetirementPollMapEpoch = request.mapEpoch;
    m_lastRetirementPollDeviceEpoch = request.deviceEpoch;
    m_lastRetirementPollFrameTag = request.frameTag;
  }

  void resetRegularRetirementPollKey() {
    m_lastRetirementPollMapEpoch = 0u;
    m_lastRetirementPollDeviceEpoch = 0u;
    m_lastRetirementPollFrameTag = 0u;
  }

  void updateRetirementHighWater() {
    m_diagnostics.retiredResourceEpochHighWater = std::max<uint64_t>(
        m_diagnostics.retiredResourceEpochHighWater,
        m_retiredResources.size());
    m_diagnostics.retiringClaimHighWater = std::max<uint64_t>(
        m_diagnostics.retiringClaimHighWater,
        m_retiringClaims.size());
  }

  void updateRetirementBackpressure() {
    if (m_retiredResources.size() > kMaxRetiredResourceEpochs ||
        m_retiringClaims.size() > kMaxRetiringClaims) {
      ++m_diagnostics.retirementLimitViolations;
    }
    const bool backpressured = m_submissionRecoveryBlocked ||
        m_retiredResources.size() >= kMaxRetiredResourceEpochs ||
        m_retiringClaims.size() >= kMaxRetiringClaims;
    if (backpressured && !m_retirementBackpressured)
      ++m_diagnostics.retirementBackpressureEntries;
    if (!backpressured && m_retirementBackpressured)
      ++m_diagnostics.retirementBackpressureRecoveries;
    m_retirementBackpressured = backpressured;
    m_diagnostics.retirementBackpressured = backpressured;
  }

  bool rejectForRetirementBackpressure() {
    updateRetirementBackpressure();
    if (!m_retirementBackpressured)
      return false;
    ++m_diagnostics.retirementBackpressureRejects;
    recordFallback(GpuSkinManagerFallbackReason::RetirementBackpressure);
    return true;
  }

  void cancelPendingBatch(uint64_t batchId, bool callbackFailure) {
    if (m_pendingBatch == nullptr || m_pendingBatch->batchId != batchId)
      return;
    std::shared_ptr<War3GpuSkinResources> resources =
        m_pendingBatch->resourceOwner;
    cancelInputReceipts(m_pendingBatch->inputReceipts);
    if (resources != nullptr) {
      for (const OutputLease& lease : m_pendingBatch->outputLeases)
        resources->discardOutput(lease);
      for (const OutputLease& lease : m_pendingBatch->inputStorageLeases)
        resources->discardOutput(lease);
      resources->discardUploads();
    }
    m_carriedStaticUploads.insert(m_carriedStaticUploads.end(),
        std::make_move_iterator(m_pendingBatch->staticUploads.begin()),
        std::make_move_iterator(m_pendingBatch->staticUploads.end()));
    m_carriedUploadRetirement = false;
    erasePreparedBatch(batchId);
    m_pendingBatch.reset();
    if (callbackFailure)
      recordFallback(GpuSkinManagerFallbackReason::CallbackException);
  }

  void expireUnsubmittedBatch() {
    if (m_pendingBatch == nullptr)
      return;

    if (m_pendingBatch->state != GpuSkinBatchState::Pending)
      return;
    ++m_diagnostics.batchesRejected;
    recordFallback(
        GpuSkinManagerFallbackReason::PendingBatchNotConsumed);
    cancelPendingBatch(m_pendingBatch->batchId, false);
  }

  void clearLeakedDispatchesAtFlush(uint64_t renderThreadId) {
    const auto stack = m_dispatchStacks.find(renderThreadId);
    if (stack == m_dispatchStacks.end() || stack->second.empty())
      return;

    m_diagnostics.dispatchLeaks += stack->second.size();
    ++m_diagnostics.epochLeaks;
    for (uint64_t epoch : stack->second) {
      const auto leaked = m_dispatches.find(epoch);
      if (leaked != m_dispatches.end()) {
        for (const auto& upload : leaked->second.uploads) {
          if (upload.second.nativeBypassed) {
            fuseBypassPath(upload.second.fuseKey,
                           upload.second.observation.geosetData,
                           leaked->second.observation.layerIndex, true);
          }
        }
      }
      m_dispatches.erase(epoch);
      for (auto it = m_bypassAuthorizations.begin();
           it != m_bypassAuthorizations.end();) {
        if (it->second.dispatchEpoch == epoch) {
          ++m_diagnostics.bypassPending;
          it = m_bypassAuthorizations.erase(it);
        } else {
          ++it;
        }
      }
    }
    m_dispatchStacks.erase(stack);
  }

  void prepareArray(
      std::vector<PreparedCandidate>& candidates,
      const GpuSkinPendingBatch& batch, uintptr_t arrayAddress,
      uint32_t count, uint32_t jobLimit, bool bypassOpaqueEligible,
      FlushPaletteBaseSnapshot& paletteBaseSnapshot) {
    if (count == 0u || candidates.size() >= jobLimit)
      return;
    RawTickAccumulator prepareTimer(
        m_fullDiagnostics,
        m_diagnostics.prepareArrayCalls,
        m_diagnostics.prepareArrayTicks,
        m_diagnostics.prepareArrayMaxTicks);
    if (arrayAddress == 0u || count > kMaxFlushElements ||
        size_t(count) > std::numeric_limits<size_t>::max() /
                            sizeof(NativeRenderBatchElement32)) {
      recordFallback(GpuSkinManagerFallbackReason::InvalidFlushArray);
      return;
    }

    const size_t byteCount =
        size_t(count) * sizeof(NativeRenderBatchElement32);
    if (!dxvk::war3::IsReadableRange(
            reinterpret_cast<const void*>(arrayAddress), byteCount)) {
      recordFallback(GpuSkinManagerFallbackReason::InvalidFlushArray);
      return;
    }

    const auto* elements =
        reinterpret_cast<const NativeRenderBatchElement32*>(arrayAddress);
    {
      RawTickAccumulator scanTimer(
          m_fullDiagnostics, m_diagnostics.queueScanTiming);
      for (uint32_t i = 0u;
           i < count && candidates.size() < jobLimit; ++i) {
        ++m_diagnostics.flushElementsVisited;
        prepareElement(candidates, batch, elements[i],
                       bypassOpaqueEligible, paletteBaseSnapshot);
      }
    }
  }

  bool excludeTransparentCandidateCollisions(
      std::vector<PreparedCandidate>& candidates,
      const GpuSkinPendingBatch& batch, uintptr_t arrayAddress,
      uint32_t count) {
    if (candidates.empty() || count == 0u)
      return true;
    RawTickAccumulator collisionTimer(
        m_fullDiagnostics, m_diagnostics.transparentCollisionTiming);
    if (arrayAddress == 0u || count > kMaxFlushElements ||
        size_t(count) > std::numeric_limits<size_t>::max() /
                            sizeof(NativeRenderBatchElement32) ||
        !dxvk::war3::IsReadableRange(
            reinterpret_cast<const void*>(arrayAddress),
            size_t(count) * sizeof(NativeRenderBatchElement32))) {
      return false;
    }

    const auto* elements =
        reinterpret_cast<const NativeRenderBatchElement32*>(arrayAddress);
    for (uint32_t i = 0u; i < count; ++i) {
      const auto& element = elements[i];
      if (element.renderablePart == 0u)
        continue;
      const uintptr_t renderablePart = uintptr_t(element.renderablePart);
      const bool mayCollide = std::any_of(
          candidates.begin(), candidates.end(),
          [&](const PreparedCandidate& candidate) {
            return candidate.key.renderablePart == renderablePart;
          });
      if (!mayCollide)
        continue;
      void* geosetDataPtr = nullptr;
      if (!dxvk::war3::SafeReadPtrFast(
              reinterpret_cast<const void*>(renderablePart),
              kRenderableGeosetDataOffset, geosetDataPtr) ||
          geosetDataPtr == nullptr) {
        return false;
      }
      const uintptr_t geosetData = reinterpret_cast<uintptr_t>(geosetDataPtr);
      const LayoutKey layoutKey = {geosetData, element.layerIndex};
      const auto layout = m_layouts.find(layoutKey);
      if (layout == m_layouts.end())
        continue;
      const CandidateKey transparentKey = {
          batch.flush.flushEpoch,
          renderablePart,
          geosetData,
          element.layerIndex,
          layout->second.outputFormat,
      };
      for (PreparedCandidate& candidate : candidates) {
        if (candidate.candidateKey == transparentKey)
          candidate.bypassOpaqueEligible = false;
      }
    }
    return true;
  }

  void prepareElement(
      std::vector<PreparedCandidate>& candidates,
      const GpuSkinPendingBatch& batch,
      const NativeRenderBatchElement32& element,
      bool bypassOpaqueEligible,
      FlushPaletteBaseSnapshot& paletteBaseSnapshot) {
    if ((element.flags & kDispatchSpecialMask) == kDispatchSpecialValue) {
      recordFallback(GpuSkinManagerFallbackReason::SpecialDispatch);
      return;
    }
    if (element.renderablePart == 0u) {
      recordFallback(GpuSkinManagerFallbackReason::InvalidRenderablePart);
      return;
    }

    const uintptr_t renderablePart = uintptr_t(element.renderablePart);
    const RenderableLayoutKey renderableKey = {
        renderablePart, element.layerIndex};
    if (!m_renderableLayoutBloom.mayContain(renderableKey)) {
      ++m_diagnostics.renderableBloomRejects;
      ++m_diagnostics.renderableReverseMisses;
      recordFallback(GpuSkinManagerFallbackReason::LayoutUnknown);
      return;
    }
    ++m_diagnostics.renderableBloomMaybes;
    const auto renderableLayout = m_renderableLayouts.find(renderableKey);
    if (renderableLayout == m_renderableLayouts.end()) {
      ++m_diagnostics.renderableReverseMisses;
      recordFallback(GpuSkinManagerFallbackReason::LayoutUnknown);
      return;
    }
    ++m_diagnostics.renderableReverseHits;

    // The reverse index is learned from the exact native upload callback.  It
    // is only an admission accelerator: a compact live snapshot below still
    // validates pointer reuse before any resource or takeover is published.
    const LayoutKey layoutKey = renderableLayout->second;
    const auto learned = m_layouts.find(layoutKey);
    if (learned == m_layouts.end()) {
      recordFallback(GpuSkinManagerFallbackReason::LayoutUnknown);
      return;
    }
    if (!IsCandidateOutputFormat(m_mode, learned->second.outputFormat) ||
        learned->second.outputStride !=
            GetGpuSkinFvfStride(learned->second.outputFormat)) {
      recordFallback(
          GpuSkinManagerFallbackReason::UnsupportedOutputFormat);
      return;
    }
    if (m_mode == GpuSkinMode::Bypass &&
        (learned->second.bypassFused ||
         m_fusedBypassLayouts.find(layoutKey) !=
             m_fusedBypassLayouts.end())) {
      learned->second.bypassStaticHint.reset();
      recordFallback(GpuSkinManagerFallbackReason::BypassFused);
      return;
    }
    if (m_mode == GpuSkinMode::Bypass &&
        learned->second.vertexCount < kProductionGpuMinVertices) {
      ++m_diagnostics.productionCpuPreferredSmallJobs;
      recordFallback(GpuSkinManagerFallbackReason::SmallBatchCpuPreferred);
      return;
    }

    const CandidateKey candidate = {
        batch.flush.flushEpoch,
        renderablePart,
        layoutKey.geosetData,
        element.layerIndex,
        learned->second.outputFormat,
    };
    if (m_candidateTokens.find(candidate) != m_candidateTokens.end())
      return;
    // Only successfully prepared candidates are present here, so this early
    // linear check skips repeated model-cache/palette proof work without
    // suppressing a later retry after a transient failure.
    if (std::any_of(candidates.begin(), candidates.end(),
                    [&](const PreparedCandidate& prepared) {
                      return prepared.candidateKey == candidate;
                    })) {
      return;
    }

    RawTickAccumulator candidateTimer(
        m_fullDiagnostics, m_diagnostics.candidatePositiveTiming);
    RawPhaseTimer candidatePhase(
        m_fullDiagnostics, m_diagnostics.liveBindingTiming);
    NativeRenderableSkinBinding32 liveBinding;
    const uintptr_t bindingAddress =
        renderablePart + kRenderablePaletteSlotOffset;
    ++m_diagnostics.liveBindingReads;
    if (bindingAddress < renderablePart ||
        !dxvk::war3::SafeCopy(
            &liveBinding, reinterpret_cast<const void*>(bindingAddress),
            sizeof(liveBinding))) {
      if (m_mode == GpuSkinMode::Bypass)
        learned->second.bypassStaticHint.reset();
      m_renderableLayouts.erase(renderableKey);
      ++m_diagnostics.liveBindingReadFailures;
      recordFallback(GpuSkinManagerFallbackReason::InvalidRenderablePart);
      return;
    }
    if (liveBinding.geosetData == 0u ||
        uintptr_t(liveBinding.geosetData) != layoutKey.geosetData) {
      if (m_mode == GpuSkinMode::Bypass)
        learned->second.bypassStaticHint.reset();
      m_renderableLayouts.erase(renderableKey);
      recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
      return;
    }

    candidatePhase.switchTo(m_diagnostics.staticLookupTiming);
    const uint32_t paletteSlot = liveBinding.paletteSlot;
    const uintptr_t geosetData = layoutKey.geosetData;
    void* const geosetDataPtr =
        reinterpret_cast<void*>(geosetData);

    auto& modelCache = model::ShadowModelResourceCache::instance();
    model::ShadowGeosetResourceStamp recordStamp = {};
    std::shared_ptr<const GpuSkinStaticResource> staticResource;
    LearnedLayout& learnedLayout = learned->second;
    bool usedBypassStaticHint = false;
    if (m_mode == GpuSkinMode::Bypass) {
      const auto& hint = learnedLayout.bypassStaticHint;
      const bool exactHint = hint != nullptr &&
          hint->state == GpuSkinStaticResourceState::Ready &&
          hint->key.mapEpoch == m_currentMapEpoch &&
          hint->key.deviceEpoch == m_boundDeviceEpoch &&
          hint->key.geosetData == geosetData &&
          hint->key.contentHash != 0u &&
          hint->key.layoutGeneration == kStaticPackingLayoutGeneration &&
          hint->record != nullptr &&
          hint->record->geosetDataPtr == geosetDataPtr &&
          hint->record->contentHash == hint->key.contentHash &&
          hint->record->vertexCount == learnedLayout.vertexCount &&
          hint->sourceLayout.byteSize != 0u &&
          hint->staticSource.defined() &&
          hint->staticSource.buffer() != nullptr &&
          hint->staticSource.length() == hint->sourceLayout.byteSize &&
          hint->indexContentHash != 0u;
      if (exactHint) {
        staticResource = hint;
        recordStamp.geosetDataPtr = hint->record->geosetDataPtr;
        recordStamp.contentHash = hint->record->contentHash;
        recordStamp.vertexCount = hint->record->vertexCount;
        usedBypassStaticHint = true;
        ++m_diagnostics.bypassStaticHintHits;
      } else {
        learnedLayout.bypassStaticHint.reset();
        ++m_diagnostics.bypassStaticHintMisses;
      }
    }

    if (!usedBypassStaticHint) {
      if (!modelCache.findGeosetStampByData(geosetDataPtr, recordStamp)) {
        recordFallback(GpuSkinManagerFallbackReason::ModelRecordMiss);
        return;
      }
      if (recordStamp.geosetDataPtr != geosetDataPtr ||
          recordStamp.contentHash == 0u || recordStamp.vertexCount == 0u ||
          recordStamp.vertexCount != learnedLayout.vertexCount) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return;
      }

      const GpuSkinStaticLookup staticProbe = m_resources->probeStatic(
          recordStamp, kStaticPackingLayoutGeneration);
      staticResource = staticProbe.resource;
      if (!staticProbe &&
          staticProbe.fallback != GpuSkinFallbackReason::StaticResourceMiss) {
        recordFallback(MapResourceFallback(staticProbe.fallback));
        return;
      }
    }
    if (staticResource == nullptr) {
      model::ShadowGeosetResourceSnapshot coldRecord =
          modelCache.findGeosetSnapshotByData(geosetDataPtr);
      if (coldRecord == nullptr) {
        recordFallback(GpuSkinManagerFallbackReason::ModelRecordMiss);
        return;
      }
      if (coldRecord->geosetDataPtr != recordStamp.geosetDataPtr ||
          coldRecord->contentHash != recordStamp.contentHash ||
          coldRecord->vertexCount != recordStamp.vertexCount) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return;
      }
      // Preserve the original admission order: unsupported multi-primitive
      // records must never consume static-atlas space before falling back.
      if (coldRecord->primitiveCount != 1u ||
          coldRecord->primitiveRecords.size() != 1u ||
          coldRecord->primitiveRecords[0].indexCount == 0u ||
          coldRecord->indexCount !=
              coldRecord->primitiveRecords[0].indexCount ||
          coldRecord->indices.size() !=
              coldRecord->primitiveRecords[0].indexCount ||
          (coldRecord->primitiveRecords[0].indexCount % 3u) != 0u) {
        recordFallback(GpuSkinManagerFallbackReason::MultiPrimitiveSlice);
        return;
      }
      const GpuSkinStaticLookup staticLookup =
          m_resources->findOrQueueStatic(
              std::move(coldRecord), kStaticPackingLayoutGeneration);
      if (!staticLookup) {
        recordFallback(MapResourceFallback(staticLookup.fallback));
        return;
      }
      staticResource = staticLookup.resource;
    }

    if (staticResource == nullptr ||
        staticResource->state != GpuSkinStaticResourceState::Ready ||
        staticResource->key.mapEpoch != m_currentMapEpoch ||
        staticResource->key.deviceEpoch != m_boundDeviceEpoch ||
        staticResource->key.geosetData != geosetData ||
        staticResource->key.contentHash != recordStamp.contentHash ||
        staticResource->key.layoutGeneration !=
            kStaticPackingLayoutGeneration ||
        staticResource->record == nullptr ||
        staticResource->record->geosetDataPtr != geosetDataPtr ||
        staticResource->record->contentHash != recordStamp.contentHash ||
        staticResource->record->vertexCount != recordStamp.vertexCount ||
        staticResource->indexContentHash == 0u) {
      recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
      return;
    }
    const model::ShadowGeosetResourceRecord& record =
        *staticResource->record;
    if (record.primitiveCount != 1u ||
        record.primitiveRecords.size() != 1u ||
        record.primitiveRecords[0].indexCount == 0u ||
        record.indexCount != record.primitiveRecords[0].indexCount ||
        record.indices.size() != record.primitiveRecords[0].indexCount ||
        (record.primitiveRecords[0].indexCount % 3u) != 0u) {
      recordFallback(GpuSkinManagerFallbackReason::MultiPrimitiveSlice);
      return;
    }

    candidatePhase.switchTo(m_diagnostics.paletteCopyTiming);
    uint32_t livePaletteCount = 0u;
    if (kGeosetPaletteGroupCountOffset >
            std::numeric_limits<uintptr_t>::max() - geosetData ||
        !dxvk::war3::SafeCopy(
            &livePaletteCount,
            reinterpret_cast<const void*>(
                geosetData + kGeosetPaletteGroupCountOffset),
            sizeof(livePaletteCount)) ||
        livePaletteCount == 0u || livePaletteCount > kMaxPaletteGroups) {
      recordFallback(GpuSkinManagerFallbackReason::PaletteCountInvalid);
      return;
    }
    if (record.matrixGroupCount != livePaletteCount ||
        staticResource->maxVertexGroupSlot >= livePaletteCount) {
      // A readable but structurally different live CGeosetData is not a
      // transient palette failure. Drop the admission hint so pointer reuse
      // cannot pin this layout to an obsolete immutable resource forever.
      if (m_mode == GpuSkinMode::Bypass)
        learnedLayout.bypassStaticHint.reset();
      recordFallback(GpuSkinManagerFallbackReason::PaletteCountInvalid);
      return;
    }
    if (paletteSlot == kInvalidPaletteSlot ||
        paletteSlot >= kMaxPaletteSlots ||
        livePaletteCount > kMaxPaletteSlots - paletteSlot) {
      recordFallback(GpuSkinManagerFallbackReason::PaletteSlotInvalid);
      return;
    }

    if (!paletteBaseSnapshot.attempted) {
      paletteBaseSnapshot.attempted = true;
      uint32_t globalPalette = 0u;
      if (m_gameBase != 0u &&
          m_gameBase <= std::numeric_limits<uintptr_t>::max() -
                            kGlobalPaletteBufferRva &&
          dxvk::war3::SafeCopy(
              &globalPalette,
              reinterpret_cast<const void*>(
                  m_gameBase + kGlobalPaletteBufferRva),
              sizeof(globalPalette)) && globalPalette != 0u) {
        paletteBaseSnapshot.base = uintptr_t(globalPalette);
      }
    }
    if (paletteBaseSnapshot.base == 0u) {
      recordFallback(GpuSkinManagerFallbackReason::PaletteBaseUnreadable);
      return;
    }
    const uintptr_t paletteBase = paletteBaseSnapshot.base;
    const size_t paletteOffset = size_t(paletteSlot) * 48u;
    const size_t paletteByteCount = size_t(livePaletteCount) * 48u;
    if (paletteOffset >
            std::numeric_limits<uintptr_t>::max() - paletteBase) {
      recordFallback(GpuSkinManagerFallbackReason::PaletteSlotInvalid);
      return;
    }
    const uintptr_t paletteAddress = paletteBase + paletteOffset;
    if (paletteByteCount >
        std::numeric_limits<uintptr_t>::max() - paletteAddress) {
      recordFallback(GpuSkinManagerFallbackReason::PaletteRangeUnreadable);
      return;
    }

    std::vector<uint8_t> paletteBytes;
    try {
      paletteBytes.resize(paletteByteCount);
    } catch (...) {
      recordFallback(GpuSkinManagerFallbackReason::BudgetExhausted);
      return;
    }
    if (!dxvk::war3::SafeCopy(
            paletteBytes.data(),
            reinterpret_cast<const void*>(paletteAddress),
            paletteByteCount)) {
      recordFallback(GpuSkinManagerFallbackReason::PaletteRangeUnreadable);
      return;
    }

    candidatePhase.switchTo(m_diagnostics.candidateBuildTiming);
    const uint64_t outputByteCount64 =
        uint64_t(record.vertexCount) * learned->second.outputStride;
    if (outputByteCount64 == 0u ||
        outputByteCount64 > std::numeric_limits<uint32_t>::max()) {
      recordFallback(GpuSkinManagerFallbackReason::BudgetExhausted);
      return;
    }

    PreparedCandidate prepared;
    prepared.candidateKey = candidate;
    prepared.resource = staticResource;
    prepared.key.frameTag = batch.request.frameTag;
    prepared.key.flushEpoch = batch.flush.flushEpoch;
    prepared.key.batchId = batch.batchId;
    prepared.key.renderablePart = uintptr_t(element.renderablePart);
    prepared.key.geosetData = geosetData;
    prepared.key.layerIndex = element.layerIndex;
    prepared.key.outputFormat = learned->second.outputFormat;
    prepared.key.token = NextNonZero(m_nextToken);
    prepared.paletteAddress = paletteAddress;
    prepared.paletteGroupCount = livePaletteCount;
    prepared.vertexCount = record.vertexCount;
    prepared.outputStride = learned->second.outputStride;
    prepared.outputFormat = learned->second.outputFormat;
    prepared.sourceUvLayerCount = record.uvLayerCount;
    prepared.expectedIndexCount = record.primitiveRecords[0].indexCount;
    prepared.expectedIndexContentHash = staticResource->indexContentHash;
    prepared.resourceContentHash = record.contentHash;
    prepared.bypassOpaqueEligible = bypassOpaqueEligible;
    prepared.paletteBytes = std::move(paletteBytes);

    // Production bypass validates the exact live palette with a fault-safe
    // copy + memcmp before both authorization and completion. Its compute
    // groups intentionally keep one palette slice per job, so hashing every
    // byte here cannot strengthen the irreversible proof and only burns the
    // synchronous render lane. Evidence modes retain the signature for their
    // existing parity and palette-dedup contracts.
    if (m_mode != GpuSkinMode::Bypass) {
      prepared.parity.paletteSignature = HashBytes(
          prepared.paletteBytes.data(), prepared.paletteBytes.size());
      uint64_t sourceSignature = HashMix(
          0xcbf29ce484222325ull, record.contentHash);
      sourceSignature = HashMix(sourceSignature,
          prepared.parity.paletteSignature);
      sourceSignature = HashMix(sourceSignature, record.vertexCount);
      sourceSignature = HashMix(sourceSignature, prepared.outputFormat);
      sourceSignature = HashMix(sourceSignature, prepared.outputStride);
      prepared.parity.cpuSourceSignature = sourceSignature;
    }
    prepared.parity.expectedCpuByteCount = uint32_t(outputByteCount64);
    prepared.parity.expectedVertexCount = record.vertexCount;
    prepared.parity.expectedVertexStride = prepared.outputStride;
    if (m_mode == GpuSkinMode::Dual) {
      prepared.parity.sampleSequence = NextNonZero(m_nextParitySequence);
      prepared.parity.diffSamplePeriod = m_diffSamplePeriod;
      prepared.parity.sampleRequested = ShouldSampleGpuSkinDiff(
          prepared.parity.sampleSequence, m_diffSamplePeriod) ? 1u : 0u;
    }
    ++m_diagnostics.preparedCandidatesByFormat[
        prepared.outputFormat < 7u ? prepared.outputFormat : 7u];
    if (m_mode == GpuSkinMode::Bypass)
      learnedLayout.bypassStaticHint = staticResource;
    candidates.push_back(std::move(prepared));
  }

  bool finalizeInputOnlyGroup(
      GpuSkinPendingBatch& batch,
      const std::vector<PreparedCandidate*>& group,
      std::vector<LeasedCandidate>& publications,
      OutputLeaseTransaction& outputTransaction) {
    if (group.empty() || !IsVertexShaderNoComputeRoute(m_executionRoute) ||
        !m_executionRouteExplicit || m_executionRouteInvalid ||
        m_resources == nullptr)
      return false;

    // 先完整检查整个组，再申请任何 lease。这样某个候选的布局漂移不会
    // 留下“前几个已经发布、后几个尚未发布”的半组状态。
    const auto candidateValid = [](const PreparedCandidate& candidate) {
      if (!candidate.bypassOpaqueEligible || candidate.outputFormat != 2u ||
          candidate.sourceUvLayerCount != 1u ||
          candidate.paletteGroupCount == 0u ||
          candidate.paletteGroupCount > 256u || candidate.resource == nullptr ||
          candidate.paletteBytes.size() !=
              size_t(candidate.paletteGroupCount) * 48u)
        return false;
      const GpuSkinStaticSourceLayout expectedLayout =
          GetGpuSkinStaticSourceLayout(candidate.vertexCount,
                                       candidate.sourceUvLayerCount);
      const GpuSkinStaticSourceLayout& actualLayout =
          candidate.resource->sourceLayout;
      return expectedLayout.byteSize != 0u &&
          actualLayout.positionOffset == expectedLayout.positionOffset &&
          actualLayout.normalOffset == expectedLayout.normalOffset &&
          actualLayout.groupSlotOffset == expectedLayout.groupSlotOffset &&
          actualLayout.texcoord0Offset == expectedLayout.texcoord0Offset &&
          actualLayout.texcoord1Offset == expectedLayout.texcoord1Offset &&
          actualLayout.byteSize == expectedLayout.byteSize &&
          candidate.resource->staticSource.length() ==
              expectedLayout.byteSize &&
          candidate.resource->staticSource.offset() <=
              std::numeric_limits<uint32_t>::max() &&
          candidate.resource->staticSource.length() <=
              std::numeric_limits<uint32_t>::max();
    };
    for (const PreparedCandidate* candidate : group) {
      if (candidate == nullptr || !candidateValid(*candidate)) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return false;
      }
    }

    const VkDeviceSize alignment = std::max<VkDeviceSize>(
        1u, m_resources->storageBufferOffsetAlignment());
    std::vector<VkDeviceSize> paletteOffsets(group.size());
    VkDeviceSize totalPaletteBytes = 0u;
    for (size_t i = 0u; i < group.size(); ++i) {
      const VkDeviceSize remainder = totalPaletteBytes % alignment;
      const VkDeviceSize padding = remainder == 0u
          ? 0u : alignment - remainder;
      if (padding > std::numeric_limits<VkDeviceSize>::max() -
                        totalPaletteBytes) {
        recordFallback(GpuSkinManagerFallbackReason::BudgetExhausted);
        return false;
      }
      totalPaletteBytes += padding;
      paletteOffsets[i] = totalPaletteBytes;
      const VkDeviceSize paletteBytes = group[i]->paletteBytes.size();
      if (paletteBytes > std::numeric_limits<VkDeviceSize>::max() -
                             totalPaletteBytes) {
        recordFallback(GpuSkinManagerFallbackReason::BudgetExhausted);
        return false;
      }
      totalPaletteBytes += paletteBytes;
    }
    if (totalPaletteBytes == 0u || totalPaletteBytes >
        std::numeric_limits<uint32_t>::max()) {
      recordFallback(GpuSkinManagerFallbackReason::BudgetExhausted);
      return false;
    }

    GpuSkinUploadSlice paletteUpload =
        m_resources->allocatePaletteUpload(totalPaletteBytes);
    if (!paletteUpload) {
      recordFallback(MapResourceFallback(paletteUpload.fallback));
      return false;
    }
    batch.requiresUploadRetirement = true;
    for (size_t i = 0u; i < group.size(); ++i) {
      const PreparedCandidate& candidate = *group[i];
      std::memcpy(static_cast<uint8_t*>(paletteUpload.mapPtr) +
                      paletteOffsets[i],
                  candidate.paletteBytes.data(), candidate.paletteBytes.size());
    }

    constexpr uint32_t inputConsumers =
        static_cast<uint32_t>(GpuSkinConsumerBits::Main) |
        static_cast<uint32_t>(GpuSkinConsumerBits::Shadow);
    for (size_t i = 0u; i < group.size(); ++i) {
      PreparedCandidate& candidate = *group[i];
      const VkDeviceSize paletteBytes = candidate.paletteBytes.size();
      OutputLeaseDesc storageDesc = {};
      storageDesc.mapEpoch = batch.request.mapEpoch;
      storageDesc.deviceEpoch = batch.request.deviceEpoch;
      storageDesc.frameTag = batch.request.frameTag;
      storageDesc.token = candidate.key.token;
      storageDesc.byteLength = uint32_t(paletteBytes);
      storageDesc.consumerBits = inputConsumers;
      OutputLease storage = m_resources->allocateOutput(storageDesc);
      if (!storage) {
        recordFallback(MapResourceFallback(storage.fallback));
        return false;
      }
      outputTransaction.track(storage);

      GpuSkinInputLease input = {};
      input.desc.mapEpoch = batch.request.mapEpoch;
      input.desc.deviceEpoch = batch.request.deviceEpoch;
      input.desc.frameTag = batch.request.frameTag;
      input.desc.token = candidate.key.token;
      input.desc.staticByteOffset =
          uint32_t(candidate.resource->staticSource.offset());
      input.desc.staticByteLength =
          uint32_t(candidate.resource->staticSource.length());
      input.desc.paletteByteOffset = storage.desc.byteOffset;
      input.desc.paletteByteLength = storage.desc.byteLength;
      input.desc.vertexCount = candidate.vertexCount;
      input.desc.paletteMatrixCount = candidate.paletteGroupCount;
      input.desc.sourceUvLayerCount = candidate.sourceUvLayerCount;
      input.desc.outputFormat = candidate.outputFormat;
      input.desc.layoutGeneration = kStaticPackingLayoutGeneration;
      input.desc.consumerBits = inputConsumers;
      input.staticSource = candidate.resource->staticSource;
      input.palette = storage.slice;
      input.storageLeaseId = storage.leaseId;
      input.storagePageGeneration = storage.pageGeneration;
      input.storagePageId = storage.pageId;

      auto receipt = std::make_shared<GpuSkinInputLeaseReceipt>();
      receipt->desc = input.desc;
      receipt->staticSource = input.staticSource;
      receipt->palette = input.palette;
      receipt->storageLeaseId = storage.leaseId;
      receipt->storagePageGeneration = storage.pageGeneration;
      receipt->storagePageId = storage.pageId;
      input.receipt = receipt;
      outputTransaction.attachInputReceipt(storage.leaseId, receipt);

      GpuSkinInputCopy copy = {};
      copy.source = DxvkBufferSlice(
          paletteUpload.slice.buffer(),
          paletteUpload.slice.offset() + paletteOffsets[i], paletteBytes);
      copy.destination = storage.slice;
      copy.byteCount = paletteBytes;
      copy.token = candidate.key.token;
      batch.inputCopies.push_back(std::move(copy));
      batch.inputStorageLeases.push_back(storage);
      batch.inputReceipts.push_back(receipt);

      GpuSkinPreparedDrawInfo info;
      info.key = candidate.key;
      // VS-B1 仍为每个候选保留独立 storage lease；这里只合并 host
      // palette 上传，不合并 consumer 身份或 fence 结算。
      info.lease = storage;
      info.inputLease = input;
      info.parity = candidate.parity;
      info.leaseId = storage.leaseId;
      info.consumerState = GpuSkinConsumerWindowState::Open;
      batch.preparedDraws.push_back(std::move(info));
      publications.push_back({&candidate, storage, std::move(input)});

      ++m_diagnostics.vsInputLeasesPrepared;
      m_diagnostics.vsInputBytesPrepared += paletteBytes;
      m_diagnostics.vsInputExpectedConsumerMask = inputConsumers;
      ++m_diagnostics.vsInputConsumerExactLeases;
      ++m_diagnostics.vsInputOnlyLeasesPrepared;
      m_diagnostics.vsInputOnlyBytesPrepared += paletteBytes;
      ++m_diagnostics.vsInputOnlyComputeJobsSkipped;
      m_diagnostics.vsInputOnlyOutputBytesSkipped +=
          candidate.parity.expectedCpuByteCount;
      // B1 不建立 compute batch；此计数只用于观察候选规模分布。
      const uint32_t inputBucket =
          GetGpuSkinDispatchVertexBucket(candidate.vertexCount);
      if (inputBucket < kGpuSkinVertexBucketCount)
        ++m_diagnostics.vertexBucketJobs[inputBucket];
    }
    return true;
  }

  bool finalizeInputOnlyCandidate(
      GpuSkinPendingBatch& batch,
      PreparedCandidate& candidate,
      std::vector<LeasedCandidate>& publications,
      OutputLeaseTransaction& outputTransaction) {
    const std::vector<PreparedCandidate*> group = { &candidate };
    return finalizeInputOnlyGroup(batch, group, publications,
                                  outputTransaction);
  }

  bool finalizeComputeGroup(
      GpuSkinPendingBatch& batch,
      std::vector<LeasedCandidate>& group,
      std::vector<LeasedCandidate>& publications,
      OutputLeaseTransaction& outputTransaction) {
    if (group.empty() || group.size() > kMaxJobsPerFlush ||
        m_resources == nullptr)
      return false;
    RawTickAccumulator finalizeTimer(
        m_fullDiagnostics, m_diagnostics.finalizeComputeTiming);

    VkDeviceSize logicalPaletteByteCount = 0u;
    uint32_t maxVertexCount = 0u;
    uint32_t vertexBucket = kGpuSkinVertexBucketCount;
    uint64_t actualVertexCount = 0u;
    uint64_t roundedInvocationCount = 0u;
    for (const LeasedCandidate& item : group) {
      if (item.candidate == nullptr ||
          item.candidate->paletteGroupCount == 0u ||
          item.candidate->paletteGroupCount > kMaxPaletteGroups ||
          item.candidate->paletteBytes.size() !=
              size_t(item.candidate->paletteGroupCount) * 48u ||
          item.candidate->paletteBytes.size() >
              std::numeric_limits<VkDeviceSize>::max() -
                  logicalPaletteByteCount) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return false;
      }
      const uint32_t candidateBucket =
          GetGpuSkinDispatchVertexBucket(item.candidate->vertexCount);
      if (vertexBucket == kGpuSkinVertexBucketCount)
        vertexBucket = candidateBucket;
      else if (vertexBucket != candidateBucket) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return false;
      }
      logicalPaletteByteCount += item.candidate->paletteBytes.size();
      maxVertexCount = std::max(maxVertexCount,
                                item.candidate->vertexCount);
      actualVertexCount += item.candidate->vertexCount;
      roundedInvocationCount +=
          uint64_t(GetGpuSkinDispatchGroupCount(
                       item.candidate->vertexCount)) *
          kGpuSkinLocalSizeX;
    }
    const VkDeviceSize jobByteCount =
        VkDeviceSize(group.size()) * sizeof(GpuSkinJob);
    const DxvkBufferSlice staticAtlas = m_resources->staticAtlasSlice();
    const OutputLease& firstLease = group.front().lease;
    const OutputLease& lastLease = group.back().lease;
    if (!staticAtlas.defined() || !firstLease.slice.defined() ||
        !lastLease.slice.defined() ||
        firstLease.pageId != lastLease.pageId ||
        firstLease.slice.buffer() != lastLease.slice.buffer() ||
        lastLease.slice.offset() < firstLease.slice.offset()) {
      recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
      return false;
    }

    const VkDeviceSize outputBegin = firstLease.slice.offset();
    const VkDeviceSize outputEnd = lastLease.slice.offset() +
        lastLease.slice.length();
    DxvkBufferSlice outputBinding(
        firstLease.slice.buffer(), outputBegin, outputEnd - outputBegin);

    if (m_jobScratch.size() < group.size()) {
      try {
        m_jobScratch.resize(group.size());
      } catch (...) {
        recordFallback(GpuSkinManagerFallbackReason::BudgetExhausted);
        return false;
      }
    }
    std::bitset<kMaxJobsPerFlush> uniquePalettes;
    VkDeviceSize uniquePaletteByteCount = 0u;
    for (size_t i = 0u; i < group.size(); ++i) {
      const PreparedCandidate& candidate = *group[i].candidate;
      const GpuSkinStaticSourceLayout expectedSourceLayout =
          GetGpuSkinStaticSourceLayout(candidate.vertexCount,
                                       candidate.sourceUvLayerCount);
      const GpuSkinStaticSourceLayout& actualSourceLayout =
          candidate.resource != nullptr
              ? candidate.resource->sourceLayout
              : expectedSourceLayout;
      const bool exactSourceLayout = candidate.resource != nullptr &&
          expectedSourceLayout.byteSize != 0u &&
          actualSourceLayout.positionOffset ==
              expectedSourceLayout.positionOffset &&
          actualSourceLayout.normalOffset ==
              expectedSourceLayout.normalOffset &&
          actualSourceLayout.groupSlotOffset ==
              expectedSourceLayout.groupSlotOffset &&
          actualSourceLayout.texcoord0Offset ==
              expectedSourceLayout.texcoord0Offset &&
          actualSourceLayout.texcoord1Offset ==
              expectedSourceLayout.texcoord1Offset &&
          actualSourceLayout.byteSize == expectedSourceLayout.byteSize &&
          candidate.resource->staticSource.length() ==
              expectedSourceLayout.byteSize;
      if (candidate.resource == nullptr ||
          !exactSourceLayout ||
          candidate.resource->staticSource.buffer() != staticAtlas.buffer() ||
          candidate.resource->staticSource.offset() < staticAtlas.offset() ||
          group[i].lease.slice.offset() < outputBinding.offset()) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return false;
      }

      const VkDeviceSize inputOffset =
          candidate.resource->staticSource.offset() - staticAtlas.offset();
      const VkDeviceSize outputOffset =
          group[i].lease.slice.offset() - outputBinding.offset();
      if (inputOffset > std::numeric_limits<uint32_t>::max() ||
          inputOffset > staticAtlas.length() ||
          expectedSourceLayout.byteSize >
              staticAtlas.length() - inputOffset ||
          outputOffset > std::numeric_limits<uint32_t>::max()) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return false;
      }

      GpuSkinJob& job = m_jobScratch[i];
      job = GpuSkinJob{};

      uint32_t paletteOffset = 0u;
      bool sharedPalette = false;
      if (m_mode != GpuSkinMode::Bypass) {
        for (size_t j = 0u; j < i; ++j) {
          if (!uniquePalettes.test(j))
            continue;
          const PreparedCandidate& previous = *group[j].candidate;
          if (previous.paletteBytes.size() == candidate.paletteBytes.size() &&
              previous.parity.paletteSignature ==
                  candidate.parity.paletteSignature &&
              std::memcmp(previous.paletteBytes.data(),
                          candidate.paletteBytes.data(),
                          candidate.paletteBytes.size()) == 0) {
            paletteOffset = m_jobScratch[j].paletteOffset;
            sharedPalette = true;
            break;
          }
        }
      }
      if (!sharedPalette) {
        const VkDeviceSize paletteBytes = candidate.paletteBytes.size();
        if (uniquePaletteByteCount >
                std::numeric_limits<uint32_t>::max() ||
            paletteBytes > std::numeric_limits<uint32_t>::max() -
                               uniquePaletteByteCount ||
            paletteBytes > std::numeric_limits<VkDeviceSize>::max() -
                               uniquePaletteByteCount) {
          recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
          return false;
        }
        paletteOffset = uint32_t(uniquePaletteByteCount);
        uniquePaletteByteCount += paletteBytes;
        uniquePalettes.set(i);
      }

      job.resourceKeyHash = uint64_t(GpuSkinStaticResourceKeyHash{}(
          candidate.resource->key));
      job.frameTag = batch.request.frameTag;
      job.inputVertexOffset = uint32_t(inputOffset);
      job.paletteOffset = paletteOffset;
      job.outputOffset = uint32_t(outputOffset);
      job.vertexCount = candidate.vertexCount;
      job.outputFormat = candidate.outputFormat;
      job.layoutGeneration = kStaticPackingLayoutGeneration;
      job.flags = 1u;
      job.token = candidate.key.token;
      if (!ConfigureGpuSkinComputeJob(job,
                                      candidate.paletteGroupCount,
                                      candidate.sourceUvLayerCount)) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return false;
      }
    }

    if (uniquePaletteByteCount == 0u ||
        uniquePaletteByteCount > logicalPaletteByteCount ||
        uniquePaletteByteCount > kGpuSkinMaxBatchUploadBytes ||
        jobByteCount >
            kGpuSkinMaxBatchUploadBytes - uniquePaletteByteCount) {
      recordFallback(GpuSkinManagerFallbackReason::BudgetExhausted);
      return false;
    }
    for (size_t i = 0u; i < group.size(); ++i) {
      const VkDeviceSize paletteOffset = m_jobScratch[i].paletteOffset;
      const VkDeviceSize paletteBytes =
          group[i].candidate->paletteBytes.size();
      if (paletteOffset > uniquePaletteByteCount ||
          paletteBytes > uniquePaletteByteCount - paletteOffset) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return false;
      }
    }

    GpuSkinBatchUpload upload;
    {
      RawTickAccumulator uploadTimer(
          m_fullDiagnostics, m_diagnostics.batchUploadTiming);
      upload = m_resources->allocateBatchUpload(
          uniquePaletteByteCount, jobByteCount);
      if (!upload) {
        recordFallback(MapResourceFallback(upload.fallback));
        return false;
      }
      batch.requiresUploadRetirement = true;
      for (size_t i = 0u; i < group.size(); ++i) {
        if (!uniquePalettes.test(i))
          continue;
        const PreparedCandidate& candidate = *group[i].candidate;
        std::memcpy(reinterpret_cast<uint8_t*>(upload.palette.mapPtr) +
                        m_jobScratch[i].paletteOffset,
                    candidate.paletteBytes.data(),
                    candidate.paletteBytes.size());
      }
      std::memcpy(upload.jobs.mapPtr, m_jobScratch.data(),
                  size_t(jobByteCount));
    }

    const bool prepareVsInputs =
        m_executionRoute == GpuSkinExecutionRoute::VertexShader &&
        m_executionRouteExplicit && !m_executionRouteInvalid;
    if (prepareVsInputs) {
      constexpr uint32_t vsS1ConsumerMask =
          static_cast<uint32_t>(GpuSkinConsumerBits::Main) |
          static_cast<uint32_t>(GpuSkinConsumerBits::Shadow);
      for (size_t i = 0u; i < group.size(); ++i) {
        LeasedCandidate& item = group[i];
        PreparedCandidate& candidate = *item.candidate;
        // VS-A 首轮只覆盖已经证明的 position+normal+UV1 格式；其余格式继续
        // 使用 compute 输出，不因私有输入租约缺失而丢失 draw。
        if (candidate.outputFormat != 2u ||
            candidate.sourceUvLayerCount != 1u ||
            candidate.paletteGroupCount == 0u ||
            candidate.paletteGroupCount > 256u ||
            candidate.resource == nullptr)
          continue;

        const VkDeviceSize paletteBytes = candidate.paletteBytes.size();
        const VkDeviceSize paletteSourceOffset =
            m_jobScratch[i].paletteOffset;
        if (paletteBytes == 0u ||
            paletteSourceOffset > upload.palette.slice.length() ||
            paletteBytes > upload.palette.slice.length() -
                               paletteSourceOffset ||
            candidate.resource->staticSource.offset() >
                std::numeric_limits<uint32_t>::max() ||
            candidate.resource->staticSource.length() >
                std::numeric_limits<uint32_t>::max()) {
          continue;
        }

        OutputLeaseDesc storageDesc = {};
        storageDesc.mapEpoch = batch.request.mapEpoch;
        storageDesc.deviceEpoch = batch.request.deviceEpoch;
        storageDesc.frameTag = batch.request.frameTag;
        storageDesc.token = candidate.key.token;
        storageDesc.byteLength = uint32_t(paletteBytes);
        storageDesc.consumerBits = vsS1ConsumerMask;
        OutputLease storage = m_resources->allocateOutput(storageDesc);
        if (!storage)
          continue;
        outputTransaction.track(storage);

        GpuSkinInputLease input = {};
        input.desc.mapEpoch = batch.request.mapEpoch;
        input.desc.deviceEpoch = batch.request.deviceEpoch;
        input.desc.frameTag = batch.request.frameTag;
        input.desc.token = candidate.key.token;
        input.desc.staticByteOffset =
            uint32_t(candidate.resource->staticSource.offset());
        input.desc.staticByteLength =
            uint32_t(candidate.resource->staticSource.length());
        input.desc.paletteByteOffset = storage.desc.byteOffset;
        input.desc.paletteByteLength = storage.desc.byteLength;
        input.desc.vertexCount = candidate.vertexCount;
        input.desc.paletteMatrixCount = candidate.paletteGroupCount;
        input.desc.sourceUvLayerCount = candidate.sourceUvLayerCount;
        input.desc.outputFormat = candidate.outputFormat;
        input.desc.layoutGeneration = kStaticPackingLayoutGeneration;
        input.desc.consumerBits = vsS1ConsumerMask;
        input.staticSource = candidate.resource->staticSource;
        input.palette = storage.slice;
        input.storageLeaseId = storage.leaseId;
        input.storagePageGeneration = storage.pageGeneration;
        input.storagePageId = storage.pageId;

        auto receipt = std::make_shared<GpuSkinInputLeaseReceipt>();
        receipt->desc = input.desc;
        receipt->staticSource = input.staticSource;
        receipt->palette = input.palette;
        receipt->storageLeaseId = storage.leaseId;
        receipt->storagePageGeneration = storage.pageGeneration;
        receipt->storagePageId = storage.pageId;
        input.receipt = receipt;
        outputTransaction.attachInputReceipt(storage.leaseId, receipt);

        GpuSkinInputCopy copy = {};
        copy.source = DxvkBufferSlice(
            upload.palette.slice.buffer(),
            upload.palette.slice.offset() + paletteSourceOffset,
            paletteBytes);
        copy.destination = storage.slice;
        copy.byteCount = paletteBytes;
        copy.token = candidate.key.token;

        batch.inputCopies.push_back(std::move(copy));
        batch.inputStorageLeases.push_back(storage);
        batch.inputReceipts.push_back(std::move(receipt));
        item.inputLease = std::move(input);
        ++m_diagnostics.vsInputLeasesPrepared;
        m_diagnostics.vsInputBytesPrepared += paletteBytes;
        m_diagnostics.vsInputExpectedConsumerMask = vsS1ConsumerMask;
        if (item.inputLease.desc.consumerBits == vsS1ConsumerMask)
          ++m_diagnostics.vsInputConsumerExactLeases;
        else
          ++m_diagnostics.vsInputConsumerMismatches;
      }
    }

    GpuSkinComputeBatch compute;
    compute.staticSource = staticAtlas;
    compute.palette = upload.palette.slice;
    compute.jobs = upload.jobs.slice;
    compute.output = outputBinding;
    compute.jobCount = uint32_t(group.size());
    compute.maxVertexCount = maxVertexCount;
    compute.vertexBucket = vertexBucket;
    compute.actualVertexCount = actualVertexCount;
    compute.roundedInvocationCount = roundedInvocationCount;
    compute.launchedInvocationCount =
        uint64_t(GetGpuSkinDispatchGroupCount(maxVertexCount)) *
        kGpuSkinLocalSizeX * group.size();
    batch.computeBatches.push_back(std::move(compute));

    for (const LeasedCandidate& item : group) {
      GpuSkinPreparedDrawInfo info;
      info.key = item.candidate->key;
      info.lease = item.lease;
      info.inputLease = item.inputLease;
      info.parity = item.candidate->parity;
      info.leaseId = item.lease.leaseId;
      info.consumerState = GpuSkinConsumerWindowState::Open;
      batch.outputLeases.push_back(item.lease);
      batch.preparedDraws.push_back(std::move(info));
      publications.push_back(item);
      ++m_diagnostics.computeJobsPreparedByFormat[
          item.candidate->outputFormat < 7u
              ? item.candidate->outputFormat : 7u];
    }
    const uint64_t uniquePaletteCount = uniquePalettes.count();
    m_diagnostics.paletteDedupCandidates += group.size();
    m_diagnostics.paletteDedupUnique += uniquePaletteCount;
    m_diagnostics.paletteDedupHits += group.size() - uniquePaletteCount;
    m_diagnostics.paletteDedupBytesSaved +=
        logicalPaletteByteCount - uniquePaletteByteCount;
    return true;
  }

  void assembleCandidates(
      GpuSkinPendingBatch& batch,
      std::vector<PreparedCandidate>& candidates,
      std::vector<LeasedCandidate>& publications,
      OutputLeaseTransaction& outputTransaction) {
    RawTickAccumulator assembleTimer(
        m_fullDiagnostics, m_diagnostics.assembleTiming);
    std::vector<LeasedCandidate> group;
    std::vector<PreparedCandidate*> inputOnlyGroup;
    VkDeviceSize groupPaletteBytes = 0u;

    const auto flushGroup = [&]() {
      if (!group.empty())
        finalizeComputeGroup(
            batch, group, publications, outputTransaction);
      group.clear();
      groupPaletteBytes = 0u;
    };
    const auto flushInputOnlyGroup = [&]() {
      if (inputOnlyGroup.empty())
        return;
      if (!finalizeInputOnlyGroup(
              batch, inputOnlyGroup, publications, outputTransaction)) {
        for (PreparedCandidate* candidate : inputOnlyGroup) {
          if (candidate != nullptr)
            recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        }
      }
      inputOnlyGroup.clear();
    };

    if (candidates.size() > kMaxJobsPerFlush) {
      for (size_t i = 0u; i < candidates.size(); ++i)
        recordFallback(GpuSkinManagerFallbackReason::BudgetExhausted);
      return;
    }
    group.reserve(candidates.size());

    // Dispatch Y selects a job while X spans that batch's largest geoset.
    // A mixed-size batch therefore launches max(groups) for every job. Stable
    // counting partitioning before output allocation preserves native order
    // within each scale class while bounding cross-job padding to <2x.
    std::array<uint32_t, kGpuSkinVertexBucketCount> bucketCounts = {};
    for (const PreparedCandidate& candidate : candidates) {
      const uint32_t bucket =
          GetGpuSkinDispatchVertexBucket(candidate.vertexCount);
      if (bucket >= kGpuSkinVertexBucketCount) {
        recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
        return;
      }
      ++bucketCounts[bucket];
    }

    std::array<uint32_t, kGpuSkinVertexBucketCount + 1u> bucketOffsets = {};
    for (uint32_t bucket = 0u; bucket < kGpuSkinVertexBucketCount; ++bucket) {
      bucketOffsets[bucket + 1u] =
          bucketOffsets[bucket] + bucketCounts[bucket];
    }
    auto bucketCursors = bucketOffsets;
    std::array<uint32_t, kMaxJobsPerFlush> orderedCandidateIndices = {};
    for (size_t index = 0u; index < candidates.size(); ++index) {
      const uint32_t bucket =
          GetGpuSkinDispatchVertexBucket(candidates[index].vertexCount);
      orderedCandidateIndices[bucketCursors[bucket]++] = uint32_t(index);
    }

    uint32_t activeBucket = kGpuSkinVertexBucketCount;
    for (size_t orderedIndex = 0u;
         orderedIndex < candidates.size(); ++orderedIndex) {
      PreparedCandidate& candidate =
          candidates[orderedCandidateIndices[orderedIndex]];
      const bool inputOnlyEligible =
          IsVertexShaderNoComputeRoute(m_executionRoute) &&
          m_executionRouteExplicit && !m_executionRouteInvalid &&
          candidate.bypassOpaqueEligible &&
          candidate.outputFormat == 2u &&
          candidate.sourceUvLayerCount == 1u &&
          candidate.paletteGroupCount != 0u &&
          candidate.paletteGroupCount <= 256u &&
          candidate.resource != nullptr;
      if (inputOnlyEligible) {
        // B1 候选只收集到当前 flush；稍后一次性申请 host palette
        // upload，避免每个小模型都触发独立的 allocator 路径。
        inputOnlyGroup.push_back(&candidate);
        continue;
      }
      const uint32_t candidateBucket =
          GetGpuSkinDispatchVertexBucket(candidate.vertexCount);
      if (candidateBucket != activeBucket) {
        flushGroup();
        activeBucket = candidateBucket;
      }
      const VkDeviceSize nextPaletteBytes = groupPaletteBytes +
          candidate.paletteBytes.size();
      const VkDeviceSize nextJobBytes =
          VkDeviceSize(group.size() + 1u) * sizeof(GpuSkinJob);
      const VkDeviceSize nextUploadBytes =
          (nextPaletteBytes + 15u) & ~VkDeviceSize(15u);
      if (!group.empty() &&
          (nextUploadBytes > kGpuSkinMaxBatchUploadBytes ||
           nextJobBytes > kGpuSkinMaxBatchUploadBytes - nextUploadBytes)) {
        flushGroup();
      }

      const VkDeviceSize singleUploadBytes =
          (VkDeviceSize(candidate.paletteBytes.size()) + 15u) &
          ~VkDeviceSize(15u);
      if (singleUploadBytes > kGpuSkinMaxBatchUploadBytes ||
          sizeof(GpuSkinJob) >
              kGpuSkinMaxBatchUploadBytes - singleUploadBytes) {
        recordFallback(GpuSkinManagerFallbackReason::BudgetExhausted);
        continue;
      }

      OutputLeaseDesc outputDesc;
      outputDesc.mapEpoch = batch.request.mapEpoch;
      outputDesc.deviceEpoch = batch.request.deviceEpoch;
      outputDesc.frameTag = batch.request.frameTag;
      outputDesc.token = candidate.key.token;
      outputDesc.byteLength = candidate.parity.expectedCpuByteCount;
      outputDesc.vertexStride = candidate.outputStride;
      outputDesc.vertexCount = candidate.vertexCount;
      OutputLease output;
      {
        RawTickAccumulator outputTimer(
            m_fullDiagnostics, m_diagnostics.outputLeaseTiming);
        output = m_resources->allocateOutput(outputDesc);
      }
      if (!output) {
        recordFallback(MapResourceFallback(output.fallback));
        continue;
      }
      outputTransaction.track(output);

      if (!group.empty() &&
          (group.front().lease.pageId != output.pageId ||
           group.front().lease.slice.buffer() != output.slice.buffer())) {
        flushGroup();
      }
      group.push_back({ &candidate, output });
      groupPaletteBytes += candidate.paletteBytes.size();
    }
    flushGroup();
    flushInputOnlyGroup();
  }

  void publishPreparedDraws(
      const GpuSkinPendingBatch& batch,
      const std::vector<LeasedCandidate>& publications) {
    RawTickAccumulator publishTimer(
        m_fullDiagnostics, m_diagnostics.publishTiming);
    std::vector<uint32_t> insertedTokens;
    insertedTokens.reserve(publications.size());
    try {
      for (const LeasedCandidate& item : publications) {
        PreparedCandidate& candidate = *item.candidate;
        PreparedDraw prepared;
        prepared.key = candidate.key;
        prepared.lease = item.lease;
        prepared.inputLease = item.inputLease;
        prepared.resource = candidate.resource;
        prepared.leaseId = item.lease.leaseId;
        prepared.parity = candidate.parity;
        prepared.paletteAddress = candidate.paletteAddress;
        prepared.paletteGroupCount = candidate.paletteGroupCount;
        prepared.expectedVertexCount = candidate.vertexCount;
        prepared.expectedOutputStride = candidate.outputStride;
        prepared.expectedIndexCount = candidate.expectedIndexCount;
        prepared.expectedIndexContentHash =
            candidate.expectedIndexContentHash;
        prepared.resourceContentHash = candidate.resourceContentHash;
        prepared.bypassOpaqueEligible = candidate.bypassOpaqueEligible;
        // candidate 快照已经复制进映射的 GPU upload，发布后不会再次读取。
        // 此处把所有权转交给长生命周期 parity/bypass 记录，避免再次深拷贝。
        prepared.paletteBytes = std::move(candidate.paletteBytes);
        prepared.bypassFuseKey = makeBypassFuseKey(prepared);
        m_candidateTokens.emplace(candidate.candidateKey,
                                  candidate.key.token);
        insertedTokens.push_back(candidate.key.token);
        m_preparedDraws.emplace(candidate.key.token, std::move(prepared));
      }
    } catch (...) {
      for (uint32_t token : insertedTokens) {
        m_preparedDraws.erase(token);
        for (auto candidate = m_candidateTokens.begin();
             candidate != m_candidateTokens.end();) {
          if (candidate->second == token)
            candidate = m_candidateTokens.erase(candidate);
          else
            ++candidate;
        }
      }
      throw;
    }
    (void)batch;
  }

  bool isStrictNativeUpload(
      const NativeDispatchObservation& dispatch,
      const NativeUploadObservation& upload) {
    if (!isStrictNativeBypassPreflight(dispatch, upload, true))
      return false;

    if (upload.cpuSkinKernelBypassed) {
      const uint64_t expectedByteCount =
          uint64_t(upload.vertexCount) * upload.outputStride;
      if (m_mode != GpuSkinMode::Bypass || !upload.bypassAuthorized ||
          !upload.takeoverEligible || !upload.originalUploadExecuted ||
          !upload.cpuSkinKernelCalled || upload.postSkipMismatch ||
          upload.fuseKey == 0u || expectedByteCount == 0u ||
          upload.cpuSkinBytesSkipped != expectedByteCount ||
          upload.bypassFailure != NativeBypassFailureReason::None ||
          (upload.observedPreflight & kP4BypassRequiredPreflight) !=
              kP4BypassRequiredPreflight ||
          (upload.observedPreflight & upload.requiredPreflight) !=
              upload.requiredPreflight) {
        recordStrictUploadReject(
            GpuSkinStrictUploadRejectReason::BypassAuthorization,
            GpuSkinManagerFallbackReason::BypassAuthorizationMismatch,
            upload.outputFormat);
        return false;
      }
      return true;
    }

    const bool cleanCpuKernelPath =
        upload.bypassFailure == NativeBypassFailureReason::None ||
        upload.bypassFailure == NativeBypassFailureReason::ModeNotBypass ||
        upload.bypassFailure == NativeBypassFailureReason::ManagerRejected;
    if (!upload.nativeObservationEligible || !cleanCpuKernelPath ||
        !upload.originalUploadExecuted || !upload.cpuSkinKernelCalled ||
        (upload.observedPreflight & upload.requiredPreflight) !=
            upload.requiredPreflight) {
      recordStrictUploadReject(
          GpuSkinStrictUploadRejectReason::CpuCompletion,
          GpuSkinManagerFallbackReason::NativePreflightFailed,
          upload.outputFormat);
      return false;
    }
    return true;
  }

  bool isStrictNativeBypassPreflight(
      const NativeDispatchObservation& dispatch,
      const NativeUploadObservation& upload,
      bool recordReject) {
    const auto reject = [&](GpuSkinStrictUploadRejectReason strictReason,
                            GpuSkinManagerFallbackReason fallbackReason) {
      if (recordReject) {
        recordStrictUploadReject(
            strictReason, fallbackReason, upload.outputFormat);
      }
      return false;
    };
    if (dispatch.path != NativeDispatchPath::Common ||
        upload.path != NativeDispatchPath::Common) {
      return reject(
          GpuSkinStrictUploadRejectReason::DispatchPath,
          GpuSkinManagerFallbackReason::UnsupportedDispatchPath);
    }
    if (dispatch.stage != 11 || upload.stage != 11) {
      return reject(
          GpuSkinStrictUploadRejectReason::Stage,
          GpuSkinManagerFallbackReason::UnsupportedStage);
    }
    if (upload.skinMode != 1u) {
      return reject(
          GpuSkinStrictUploadRejectReason::SkinMode,
          GpuSkinManagerFallbackReason::UnsupportedSkinMode);
    }
    if ((upload.observedPreflight & kNativeUploadInputRequiredPreflight) !=
        kNativeUploadInputRequiredPreflight) {
      return reject(
          GpuSkinStrictUploadRejectReason::InputPreflight,
          GpuSkinManagerFallbackReason::NativePreflightFailed);
    }
    const bool oddWhiteDiffuseFormat = (upload.outputFormat & 1u) != 0u;
    if (!IsCandidateOutputFormat(m_mode, upload.outputFormat) ||
        upload.extra != 0u ||
        (oddWhiteDiffuseFormat && upload.extraStride != 0u) ||
        upload.outputStride != GetGpuSkinFvfStride(upload.outputFormat)) {
      return reject(
          GpuSkinStrictUploadRejectReason::OutputFormat,
          GpuSkinManagerFallbackReason::UnsupportedOutputFormat);
    }
    if (dispatch.renderablePart == 0u || upload.renderablePart == 0u ||
        dispatch.renderablePart != upload.renderablePart ||
        upload.geosetData == 0u || upload.vertexCount == 0u) {
      return reject(
          GpuSkinStrictUploadRejectReason::RenderableIdentity,
          GpuSkinManagerFallbackReason::InvalidRenderablePart);
    }
    return true;
  }

  bool isExactBypassLease(const PreparedDraw& prepared) const {
    constexpr uint32_t requiredConsumers =
        static_cast<uint32_t>(GpuSkinConsumerBits::Main) |
        static_cast<uint32_t>(GpuSkinConsumerBits::Shadow) |
        static_cast<uint32_t>(GpuSkinConsumerBits::Outline);
    const OutputLease& lease = prepared.lease;
    const uint64_t byteCount =
        uint64_t(prepared.expectedVertexCount) *
        prepared.expectedOutputStride;
    return (AllowedFormalConsumerMask(m_mode) & requiredConsumers) ==
               requiredConsumers &&
        prepared.key.token != 0u && prepared.key.batchId != 0u &&
        IsStrictOutputFormat(prepared.key.outputFormat) &&
        prepared.expectedVertexCount != 0u &&
        prepared.expectedOutputStride ==
            GetGpuSkinFvfStride(prepared.key.outputFormat) &&
        prepared.expectedIndexCount != 0u &&
        (prepared.expectedIndexCount % 3u) == 0u &&
        byteCount != 0u &&
        byteCount == lease.desc.byteLength &&
        lease.desc.vertexCount == prepared.expectedVertexCount &&
        lease.desc.vertexStride == prepared.expectedOutputStride &&
        lease.desc.token == prepared.key.token && lease.slice.defined() &&
        lease.slice.buffer() != nullptr &&
        lease.slice.offset() == lease.desc.byteOffset &&
        lease.slice.length() == byteCount &&
        (lease.slice.buffer()->info().usage &
         (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) ==
            (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  }

  bool isExactBypassInputLease(const PreparedDraw& prepared) const {
    constexpr uint32_t inputConsumers =
        static_cast<uint32_t>(GpuSkinConsumerBits::Main) |
        static_cast<uint32_t>(GpuSkinConsumerBits::Shadow);
    const GpuSkinInputLease& input = prepared.inputLease;
    const OutputLease& capability = prepared.lease;
    const uint64_t paletteBytes =
        uint64_t(prepared.paletteGroupCount) * 48u;
    const GpuSkinStaticSourceLayout layout = GetGpuSkinStaticSourceLayout(
        prepared.expectedVertexCount, input.desc.sourceUvLayerCount);
    return IsVertexShaderBypassRoute(m_executionRoute) &&
        m_executionRouteExplicit && !m_executionRouteInvalid &&
        prepared.key.outputFormat == 2u &&
        prepared.bypassOpaqueEligible && prepared.resource != nullptr &&
        prepared.resource->maxVertexGroupSlot < prepared.paletteGroupCount &&
        prepared.expectedVertexCount != 0u &&
        prepared.paletteGroupCount != 0u &&
        prepared.paletteGroupCount <= 256u &&
        paletteBytes != 0u &&
        prepared.paletteBytes.size() == paletteBytes &&
        static_cast<bool>(input) &&
        input.desc.mapEpoch == capability.desc.mapEpoch &&
        input.desc.deviceEpoch == capability.desc.deviceEpoch &&
        input.desc.frameTag == prepared.key.frameTag &&
        input.desc.token == prepared.key.token &&
        input.desc.dispatchEpoch == capability.desc.dispatchEpoch &&
        input.desc.uploadEpoch == capability.desc.uploadEpoch &&
        input.desc.vertexCount == prepared.expectedVertexCount &&
        input.desc.paletteMatrixCount == prepared.paletteGroupCount &&
        input.desc.paletteByteLength == paletteBytes &&
        input.desc.sourceUvLayerCount == 1u &&
        input.desc.outputFormat == 2u &&
        input.desc.layoutGeneration == kStaticPackingLayoutGeneration &&
        input.desc.consumerBits == inputConsumers &&
        input.desc.staticByteOffset == input.staticSource.offset() &&
        input.desc.staticByteLength == input.staticSource.length() &&
        input.desc.paletteByteOffset == input.palette.offset() &&
        input.desc.paletteByteLength == input.palette.length() &&
        layout.byteSize != 0u &&
        layout.byteSize == input.staticSource.length() &&
        input.staticSource.buffer() == prepared.resource->staticSource.buffer() &&
        input.staticSource.offset() == prepared.resource->staticSource.offset() &&
        input.staticSource.length() == prepared.resource->staticSource.length() &&
        (input.staticSource.buffer()->info().usage &
         (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) ==
            (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) &&
        input.storageLeaseId == capability.leaseId &&
        input.storagePageId == capability.pageId &&
        input.storagePageGeneration == capability.pageGeneration &&
        input.palette.buffer() == capability.slice.buffer() &&
        input.palette.offset() == capability.slice.offset() &&
        input.palette.length() == capability.slice.length() &&
        capability.desc.consumerBits == inputConsumers &&
        capability.desc.vertexCount == 0u &&
        capability.desc.vertexStride == 0u &&
        capability.desc.byteLength == paletteBytes;
  }

  void learnLayout(const NativeDispatchObservation& dispatch,
                   const NativeUploadObservation& upload) {
    const LayoutKey key = {upload.geosetData, dispatch.layerIndex};
    if (upload.renderablePart != 0u && upload.geosetData != 0u) {
      const RenderableLayoutKey renderableKey = {
          upload.renderablePart, dispatch.layerIndex};
      m_renderableLayoutBloom.insert(renderableKey);
      const auto reverse = m_renderableLayouts.find(renderableKey);
      if (reverse == m_renderableLayouts.end())
        m_renderableLayouts.emplace(renderableKey, key);
      else
        reverse->second = key;
      m_diagnostics.renderableReverseHighWater = std::max<uint64_t>(
          m_diagnostics.renderableReverseHighWater,
          m_renderableLayouts.size());
    }
    const auto existing = m_layouts.find(key);
    if (existing == m_layouts.end()) {
      LearnedLayout layout;
      layout.outputFormat = upload.outputFormat;
      layout.outputStride = upload.outputStride;
      layout.vertexCount = upload.vertexCount;
      layout.fvf = upload.fvf;
      layout.bypassFused =
          m_fusedBypassLayouts.find(key) != m_fusedBypassLayouts.end();
      m_layouts.emplace(key, layout);
      return;
    }

    LearnedLayout& layout = existing->second;
    if (layout.outputFormat != upload.outputFormat ||
        layout.outputStride != upload.outputStride ||
        layout.vertexCount != upload.vertexCount ||
        layout.fvf != upload.fvf) {
      layout.outputFormat = upload.outputFormat;
      layout.outputStride = upload.outputStride;
      layout.vertexCount = upload.vertexCount;
      layout.fvf = upload.fvf;
      layout.bypassStaticHint.reset();
      ++layout.generation;
      clearCpuDipBaseline(layout);
      if (layout.generation == 0u)
        layout.generation = 1u;
      recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
    }
  }

  bool ensureLiveProofScratch(size_t byteCount) noexcept {
    if (byteCount == 0u || byteCount > kMaxLiveProofBytes)
      return false;
    if (m_liveProofScratch.size() >= byteCount)
      return true;
    try {
      // Keep the high-water mark. Shrinking per stream would repeatedly
      // zero/grow this buffer for position, normal, group, UV and palette
      // proofs on every upload.
      m_liveProofScratch.resize(byteCount);
    } catch (...) {
      return false;
    }
    return true;
  }

  bool copyLiveProof(uintptr_t source, size_t byteCount) noexcept {
    if (source == 0u ||
        byteCount > std::numeric_limits<uintptr_t>::max() - source ||
        !ensureLiveProofScratch(byteCount)) {
      return false;
    }
    return dxvk::war3::SafeCopy(
        m_liveProofScratch.data(), reinterpret_cast<const void*>(source),
        byteCount);
  }

  bool matchLiveToTrusted(
      uintptr_t source, const void* trusted, size_t byteCount) noexcept {
    return trusted != nullptr && copyLiveProof(source, byteCount) &&
        std::memcmp(m_liveProofScratch.data(), trusted, byteCount) == 0;
  }

  bool validateUploadPalette(
      PreparedDraw& prepared,
      const NativeUploadObservation& upload) {
    RawTickAccumulator validateTimer(
        m_fullDiagnostics, m_diagnostics.validatePaletteTiming);
    ++m_diagnostics.paletteParityChecks;
    model::ShadowGeosetResourceStamp currentStamp;
    const auto& resource = prepared.resource;
    if (resource == nullptr ||
        resource->state != GpuSkinStaticResourceState::Ready ||
        !model::ShadowModelResourceCache::instance().findGeosetStampByData(
            reinterpret_cast<void*>(upload.geosetData), currentStamp) ||
        currentStamp.geosetDataPtr !=
            reinterpret_cast<void*>(prepared.key.geosetData) ||
        currentStamp.contentHash != prepared.resourceContentHash ||
        currentStamp.vertexCount != prepared.expectedVertexCount ||
        resource->key.geosetData != upload.geosetData ||
        resource->key.contentHash != currentStamp.contentHash ||
        resource->record == nullptr ||
        resource->record->geosetDataPtr != currentStamp.geosetDataPtr ||
        resource->record->contentHash != currentStamp.contentHash ||
        resource->record->vertexCount != currentStamp.vertexCount ||
        resource->record->matrixGroupCount != prepared.paletteGroupCount ||
        resource->maxVertexGroupSlot >= prepared.paletteGroupCount ||
        prepared.paletteGroupCount == 0u ||
        prepared.paletteGroupCount > kMaxPaletteGroups ||
        prepared.paletteBytes.size() !=
            size_t(prepared.paletteGroupCount) * 48u) {
      invalidateBypassStaticHint(prepared.key);
      recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
      return false;
    }
    if (upload.palette != prepared.paletteAddress) {
      ++m_diagnostics.palettePointerMismatches;
      recordFallback(
          GpuSkinManagerFallbackReason::PalettePointerMismatch);
      return false;
    }
    if (upload.paletteGroupCount != prepared.paletteGroupCount) {
      ++m_diagnostics.paletteCountMismatches;
      recordFallback(GpuSkinManagerFallbackReason::PaletteCountMismatch);
      return false;
    }
    if (!copyLiveProof(upload.palette, prepared.paletteBytes.size())) {
      ++m_diagnostics.paletteUnreadableAtUpload;
      recordFallback(
          GpuSkinManagerFallbackReason::PaletteRangeUnreadable);
      return false;
    }
    if (std::memcmp(m_liveProofScratch.data(), prepared.paletteBytes.data(),
                    prepared.paletteBytes.size()) != 0) {
      ++m_diagnostics.paletteContentMismatches;
      recordFallback(
          GpuSkinManagerFallbackReason::PaletteContentMismatch);
      return false;
    }
    if (upload.vertexCount != prepared.expectedVertexCount ||
        upload.outputStride != prepared.expectedOutputStride) {
      invalidateBypassStaticHint(prepared.key);
      recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
      return false;
    }
    ++m_diagnostics.paletteParityMatches;
    return true;
  }

  bool validateBypassStaticInputs(
      const PreparedDraw& prepared,
      const NativeUploadObservation& upload) {
    RawTickAccumulator validateTimer(
        m_fullDiagnostics, m_diagnostics.validateStaticTiming);
    // Stable addresses alone are insufficient for a destructive bypass. Match
    // every live static stream against the immutable record actually packed
    // into the already-submitted compute resource.
    const auto rejectLayout = [&]() {
      invalidateBypassStaticHint(prepared.key);
      recordFallback(GpuSkinManagerFallbackReason::LayoutChanged);
      return false;
    };
    const auto& resource = prepared.resource;
    if (resource == nullptr ||
        resource->state != GpuSkinStaticResourceState::Ready ||
        resource->record == nullptr ||
        resource->key.geosetData != upload.geosetData ||
        resource->key.contentHash != prepared.resourceContentHash ||
        resource->indexContentHash != prepared.expectedIndexContentHash) {
      return rejectLayout();
    }
    const model::ShadowGeosetResourceRecord& record = *resource->record;
    if (record.geosetDataPtr !=
            reinterpret_cast<void*>(upload.geosetData) ||
        record.contentHash != prepared.resourceContentHash ||
        record.vertexCount != prepared.expectedVertexCount ||
        record.vertexCount == 0u ||
        record.vertexCount > kMaxNativeVertices ||
        record.positions.size() != size_t(record.vertexCount) * 3u ||
        record.normals.size() != size_t(record.vertexCount) * 3u ||
        record.vertexGroupIndices.size() != record.vertexCount ||
        record.primitiveCount != 1u ||
        record.primitiveRecords.size() != 1u ||
        record.primitiveRecords[0].primitiveTypeOrMaterialSlot !=
            kGxPrimitiveTriangleList ||
        record.primitiveRecords[0].indexCount !=
            prepared.expectedIndexCount ||
        prepared.expectedIndexCount == 0u ||
        prepared.expectedIndexCount > kMaxNativeIndices ||
        record.indexCount != prepared.expectedIndexCount ||
        record.indices.size() != prepared.expectedIndexCount ||
        record.matrixGroupCount != prepared.paletteGroupCount ||
        resource->maxVertexGroupSlot >= prepared.paletteGroupCount) {
      return rejectLayout();
    }

    uint32_t livePrimitiveCount = 0u;
    uint32_t liveIndexCount = 0u;
    uint32_t livePrimitiveRecordsRaw = 0u;
    uint32_t liveIndicesRaw = 0u;
    const uintptr_t geosetAddress = upload.geosetData;
    if (geosetAddress == 0u ||
        kGeosetPrimitiveSnapshotBeginOffset >
            std::numeric_limits<uintptr_t>::max() - geosetAddress) {
      return rejectLayout();
    }
    std::array<uint8_t, kGeosetPrimitiveSnapshotSize> geosetSnapshot = {};
    // Keep the manager's independent live proof, but collapse the four
    // adjacent CGeosetData reads into one RPM call. A failed span read falls
    // back to the original per-field fail-closed path below.
    const bool geosetSnapshotReady = dxvk::war3::SafeCopy(
        geosetSnapshot.data(),
        reinterpret_cast<const void*>(
            geosetAddress + kGeosetPrimitiveSnapshotBeginOffset),
        geosetSnapshot.size());
    const auto readGeosetDword = [&](size_t offset,
                                     uint32_t& value) noexcept {
      if (geosetSnapshotReady) {
        const size_t relativeOffset =
            offset - kGeosetPrimitiveSnapshotBeginOffset;
        if (relativeOffset <=
            geosetSnapshot.size() - sizeof(value)) {
          std::memcpy(&value, geosetSnapshot.data() + relativeOffset,
                      sizeof(value));
          return true;
        }
      }
      if (offset >
          std::numeric_limits<uintptr_t>::max() - geosetAddress) {
        return false;
      }
      return dxvk::war3::SafeCopy(
          &value, reinterpret_cast<const void*>(geosetAddress + offset),
          sizeof(value));
    };
    if (!readGeosetDword(
            kGeosetPrimitiveCountOffset, livePrimitiveCount) ||
        !readGeosetDword(
            kGeosetPrimitiveRecordsOffset, livePrimitiveRecordsRaw) ||
        !readGeosetDword(kGeosetIndexCountOffset, liveIndexCount) ||
        !readGeosetDword(kGeosetIndicesOffset, liveIndicesRaw) ||
        livePrimitiveCount != 1u || livePrimitiveRecordsRaw == 0u ||
        liveIndexCount != prepared.expectedIndexCount ||
        liveIndicesRaw == 0u) {
      return rejectLayout();
    }
    const uintptr_t livePrimitiveRecords = livePrimitiveRecordsRaw;
    const auto* liveIndices = reinterpret_cast<const uint16_t*>(
        uintptr_t(liveIndicesRaw));

    uint32_t livePrimitiveType = 0u;
    uint32_t livePrimitiveIndexCount = 0u;
    std::array<uint32_t, 2u> primitiveSnapshot = {};
    if (dxvk::war3::SafeCopy(
            primitiveSnapshot.data(),
            reinterpret_cast<const void*>(livePrimitiveRecords),
            sizeof(primitiveSnapshot))) {
      livePrimitiveType = primitiveSnapshot[0u];
      livePrimitiveIndexCount = primitiveSnapshot[1u];
    } else if (livePrimitiveRecords >
                   std::numeric_limits<uintptr_t>::max() -
                       sizeof(uint32_t) ||
               !dxvk::war3::SafeCopy(
                   &livePrimitiveType,
                   reinterpret_cast<const void*>(livePrimitiveRecords),
                   sizeof(livePrimitiveType)) ||
               !dxvk::war3::SafeCopy(
                   &livePrimitiveIndexCount,
                   reinterpret_cast<const void*>(
                       livePrimitiveRecords + sizeof(uint32_t)),
                   sizeof(livePrimitiveIndexCount))) {
      return rejectLayout();
    }
    if (livePrimitiveType != kGxPrimitiveTriangleList ||
        livePrimitiveIndexCount != prepared.expectedIndexCount ||
        !matchLiveToTrusted(
            reinterpret_cast<uintptr_t>(liveIndices), record.indices.data(),
            record.indices.size() * sizeof(uint16_t))) {
      return rejectLayout();
    }

    if (!matchLiveToTrusted(upload.positions, record.positions.data(),
                            record.positions.size() * sizeof(float)) ||
        !matchLiveToTrusted(upload.normals, record.normals.data(),
                            record.normals.size() * sizeof(float)) ||
        !matchLiveToTrusted(upload.groupSlots,
                            record.vertexGroupIndices.data(),
                            record.vertexGroupIndices.size())) {
      return rejectLayout();
    }

    const uint32_t requiredUvLayers = upload.outputFormat / 2u;
    if ((upload.outputFormat & 1u) != 0u || requiredUvLayers > 2u) {
      return rejectLayout();
    }
    if (requiredUvLayers == 0u)
      return true;
    if (record.uvLayerCount < requiredUvLayers ||
        record.uvLayers.size() < requiredUvLayers) {
      return rejectLayout();
    }
    const uintptr_t uvSources[2] = {upload.uv0, upload.uv1};
    for (uint32_t i = 0u; i < requiredUvLayers; ++i) {
      const auto& uv = record.uvLayers[i];
      if (uv.uvCount != record.vertexCount ||
          uv.uvPairs.size() != size_t(record.vertexCount) * 2u ||
          !matchLiveToTrusted(uvSources[i], uv.uvPairs.data(),
                              uv.uvPairs.size() * sizeof(float))) {
        return rejectLayout();
      }
    }
    return true;
  }

  bool validateSinglePrimitiveDip(
      const PreparedDraw& prepared,
      const NativeUploadObservation& upload,
      const NativeDipObservation& observation) const {
    uint32_t predictedStartIndex = 0u;
    if (!PredictNativeStartIndex(upload.indexRingNextBefore,
                                 prepared.expectedIndexCount,
                                 predictedStartIndex)) {
      return false;
    }
    if (observation.dip.flags != NativeDipFlagNone ||
        observation.dip.primitiveType != kD3dPrimitiveTriangleList ||
        observation.dip.baseVertexIndex < 0 ||
        uint32_t(observation.dip.baseVertexIndex) !=
            upload.ringBaseVertexAfter ||
        observation.dip.minVertexIndex != 0u ||
        observation.dip.numVertices != prepared.expectedVertexCount ||
        observation.dip.startIndex != predictedStartIndex ||
        prepared.expectedIndexCount == 0u ||
        (prepared.expectedIndexCount % 3u) != 0u)
      return false;
    if (upload.cpuSkinKernelBypassed) {
      constexpr uint32_t requiredTicketStages =
          NativeIndexTicketStageExpectedProof |
          NativeIndexTicketStageLockAttempt |
          NativeIndexTicketStageLockExact |
          NativeIndexTicketStageContentsExact |
          NativeIndexTicketStageUnlockExact |
          NativeIndexTicketStageSetIndicesExact |
          NativeIndexTicketStageActualProof |
          NativeIndexTicketStageDipConsumed;
      const NativeIndexTicketObservation& ticket = observation.indexTicket;
      if (!NativeDipRangePoisoned(observation) ||
          observation.sourceUploadPostSkipMismatch || !ticket.exact ||
          ticket.suppressed || ticket.leaked ||
          ticket.failureMask != NativeIndexTicketFailureNone ||
          (ticket.stageMask & requiredTicketStages) != requiredTicketStages ||
          (observation.observedPreflight &
           NativePreflightIndexPathActualProof) == 0u ||
          ticket.expectedIndexCount != prepared.expectedIndexCount ||
          ticket.actualIndexCount != prepared.expectedIndexCount ||
          ticket.predictedStartIndex != predictedStartIndex ||
          ticket.expectedIndexHash != ticket.actualIndexHash ||
          ticket.expectedMinIndex != ticket.actualMinIndex ||
          ticket.expectedMaxIndex != ticket.actualMaxIndex ||
          upload.expectedIndexCount != prepared.expectedIndexCount ||
          upload.predictedIndexRingBase != predictedStartIndex ||
          upload.predictedIndexRingNext !=
              predictedStartIndex + prepared.expectedIndexCount) {
        return false;
      }
    }
    return observation.dip.primitiveCount ==
        prepared.expectedIndexCount / 3u;
  }

  static bool samePreparedKey(const GpuSkinPreparedDrawKey& lhs,
                              const GpuSkinPreparedDrawKey& rhs) {
    return lhs.frameTag == rhs.frameTag &&
           lhs.flushEpoch == rhs.flushEpoch &&
           lhs.batchId == rhs.batchId &&
           lhs.renderablePart == rhs.renderablePart &&
           lhs.geosetData == rhs.geosetData &&
           lhs.layerIndex == rhs.layerIndex &&
           lhs.outputFormat == rhs.outputFormat &&
           lhs.token == rhs.token;
  }

  void erasePreparedBatch(uint64_t batchId) {
    for (auto it = m_preparedDraws.begin();
         it != m_preparedDraws.end();) {
      if (it->second.key.batchId == batchId) {
        const uint32_t token = it->first;
        for (auto candidate = m_candidateTokens.begin();
             candidate != m_candidateTokens.end();) {
          if (candidate->second == token)
            candidate = m_candidateTokens.erase(candidate);
          else
            ++candidate;
        }
        it = m_preparedDraws.erase(it);
      } else {
        ++it;
      }
    }
  }

  GpuSkinMode m_mode = GpuSkinMode::Disabled;
  GpuSkinExecutionRoute m_executionRoute = GpuSkinExecutionRoute::Compute;
  bool m_executionRouteExplicit = false;
  bool m_executionRouteInvalid = false;
  uint32_t m_diffSamplePeriod = 0u;
  bool m_fullDiagnostics = false;
  uintptr_t m_gameBase = 0u;
  mutable std::mutex m_mutex;
  GpuSkinManagerHostCallbacks m_host;
  NativeBridgeCallbacks m_nativeCallbacks;
  bool m_attached = false;
  bool m_nativeCallbackActive = false;
  bool m_hostSubmissionActive = false;
  bool m_retirementBackpressured = false;
  bool m_submissionRecoveryBlocked = false;
  bool m_deviceReady = false;
  bool m_callbackQuarantined = false;
  std::thread::id m_callbackThread;
  Rc<DxvkDevice> m_device;
  GpuSkinResourceBudgets m_budgets;
  uint64_t m_renderThreadId = 0u;
  uint64_t m_boundDeviceEpoch = 0u;
  uint64_t m_pendingDeviceEpoch = 0u;
  uint64_t m_pendingBridgeResetGeneration = 0u;
  uint64_t m_currentMapEpoch = 0u;
  uint64_t m_lastRetirementPollMapEpoch = 0u;
  uint64_t m_lastRetirementPollDeviceEpoch = 0u;
  uint64_t m_lastRetirementPollFrameTag = 0u;
  // Immutable only after the matching flush has fully assembled and either
  // produced no batch or completed accepted host submission.
  uint64_t m_dispatchCpuOnlySealFlushEpoch = 0u;
  std::vector<RenderableLayoutKey> m_dispatchCpuOnlySealCandidates;
  std::vector<uint32_t> m_dispatchCpuOnlySealCandidateTokensScratch;
  std::shared_ptr<War3GpuSkinResources> m_resources;
  std::vector<std::shared_ptr<War3GpuSkinResources>> m_retiredResources;
  // Native callbacks are serialized by m_mutex. Reuse a grow-only high-water
  // buffer for exact palette/static-stream proofs without per-proof allocation.
  std::vector<uint8_t> m_liveProofScratch;
  // Compute groups are serialized by m_mutex. Reuse the 64-byte POD staging
  // jobs without over-aligning them (the 32-bit allocator guarantees 8 only).
  std::vector<GpuSkinJob> m_jobScratch;
  std::unique_ptr<GpuSkinPendingBatch> m_pendingBatch;
  std::vector<GpuSkinStaticUpload> m_carriedStaticUploads;
  bool m_carriedUploadRetirement = false;
  uint64_t m_nextBatchId = 0u;
  uint32_t m_nextToken = 0u;
  uint64_t m_nextParitySequence = 0u;
  GpuSkinManagerDiagnostics m_diagnostics;
  std::unordered_map<LayoutKey, LearnedLayout, LayoutKeyHash> m_layouts;
  std::unordered_map<RenderableLayoutKey, LayoutKey,
                     RenderableLayoutKeyHash> m_renderableLayouts;
  RenderableLayoutBloom m_renderableLayoutBloom;
  std::unordered_map<CandidateKey, uint32_t, CandidateKeyHash>
      m_candidateTokens;
  std::unordered_map<uint32_t, PreparedDraw> m_preparedDraws;
  InlineFlatMap<uint64_t, DispatchState, 4u> m_dispatches;
  InlineFlatMap<uint64_t, small_vector<uint64_t, 4u>, 1u>
      m_dispatchStacks;
  InlineFlatMap<uint64_t, BypassAuthorizationState, 4u>
      m_bypassAuthorizations;
  std::unordered_set<uint64_t> m_fusedBypassKeys;
  std::unordered_set<LayoutKey, LayoutKeyHash> m_fusedBypassLayouts;
  std::unordered_set<uint64_t> m_mainSuppressedKeys;
  std::unordered_set<uint64_t> m_shadowSuppressedKeys;
  std::unordered_set<uint64_t> m_outlineSuppressedKeys;
  std::unordered_set<uint64_t> m_consumerFuseKeys;
  std::unordered_map<uint64_t, SubmittedBatch> m_submittedBatches;
  std::unordered_map<uint64_t, SubmittedBatch> m_retiringBatches;
  std::unordered_map<uint64_t, RetiringClaim> m_retiringClaims;
  std::deque<uint64_t> m_autoRetiredBatchIds;
};

War3GpuSkinManager::War3GpuSkinManager(
    Rc<DxvkDevice> device, uintptr_t gameBase,
    const GpuSkinRuntimeConfig& config,
    const GpuSkinResourceBudgets& budgets,
    const GpuSkinManagerHostCallbacks& host)
    : m_impl(std::make_unique<Impl>(std::move(device), gameBase, config,
                                    budgets, host)) {
}

War3GpuSkinManager::~War3GpuSkinManager() {
  if (!m_impl->detachNativeBridge()) {
    m_impl->quarantineCallbackOwner();
    Logger::err(
        "War3GpuSkinManager: callback owner did not quiesce; "
        "retaining it in leaky quarantine");
    // The bridge may still hold a callback pin to Impl. Process-lifetime
    // retention is the only safe destruction policy after a failed drain.
    (void)m_impl.release();
    return;
  }
  // Successful detach removed callback ingress and drained callback pins.
  // Generation zero creates no reset ticket; retirement may acknowledge only
  // an exact generation that was already pending before destruction.
  m_impl->reset(0u, true);
}

GpuSkinMode War3GpuSkinManager::mode() const {
  return m_impl->mode();
}

GpuSkinExecutionRoute War3GpuSkinManager::executionRoute() const {
  return m_impl->executionRoute();
}

bool War3GpuSkinManager::executionRouteExplicit() const {
  return m_impl->executionRouteExplicit();
}

bool War3GpuSkinManager::executionRouteInvalid() const {
  return m_impl->executionRouteInvalid();
}

void War3GpuSkinManager::SetHostCallbacks(
    const GpuSkinManagerHostCallbacks& host) {
  m_impl->setHostCallbacks(host);
}

bool War3GpuSkinManager::SetDevice(
    Rc<DxvkDevice> device, uint64_t deviceEpoch,
    uint64_t renderThreadId) {
  return m_impl->setDevice(
      std::move(device), deviceEpoch, renderThreadId);
}

bool War3GpuSkinManager::IsDeviceReady(uint64_t deviceEpoch) const {
  return m_impl->isDeviceReady(deviceEpoch);
}

bool War3GpuSkinManager::AttachNativeBridge() {
  return m_impl->attachNativeBridge();
}

bool War3GpuSkinManager::DetachNativeBridge() {
  const bool detached = m_impl->detachNativeBridge();
  if (!detached)
    m_impl->quarantineCallbackOwner();
  return detached;
}

void War3GpuSkinManager::SubmitFlush(
    const NativeFlushObservation& observation) {
  m_impl->submitFlush(observation);
}

bool War3GpuSkinManager::SubmitFlush(
    const FlushRequest& request,
    const NativeFlushObservation& observation) {
  return m_impl->submitFlush(request, observation);
}

void War3GpuSkinManager::BeginDispatch(
    const NativeDispatchObservation& observation) {
  m_impl->beginDispatch(observation);
}

void War3GpuSkinManager::EndDispatch(
    const NativeDispatchSummary& summary) {
  m_impl->endDispatch(summary);
}

void War3GpuSkinManager::NoteNativeUpload(
    const NativeUploadObservation& observation) {
  m_impl->noteNativeUpload(observation);
}

void War3GpuSkinManager::NoteNativeDip(
    const NativeDipObservation& observation) {
  m_impl->noteNativeDip(observation);
}

void War3GpuSkinManager::NoteNativeUploadFanout(
    const NativeUploadFanoutObservation& observation) {
  m_impl->noteNativeFanout(observation);
}

void War3GpuSkinManager::NoteBypassDrawResult(
    const GpuSkinBypassDrawResult& result) {
  m_impl->noteBypassDrawResult(result);
}

void War3GpuSkinManager::FuseIrreversibleBypass(
    const GpuSkinResolvedDraw& resolved,
    GpuSkinConsumerBits consumer) {
  m_impl->fuseIrreversibleBypass(resolved, consumer);
}

void War3GpuSkinManager::TerminateIrreversibleBypassConsumers(
    const GpuSkinResolvedDraw& resolved) {
  m_impl->terminateIrreversibleBypassConsumers(resolved);
}

GpuSkinResolvedDraw War3GpuSkinManager::ResolveDip(
    const NativeDipObservation& observation,
    GpuSkinConsumerBits consumer) {
  return m_impl->resolveDip(observation, consumer);
}

GpuSkinResolvedDraw War3GpuSkinManager::ResolveParityDip(
    const NativeDipObservation& observation) {
  return m_impl->resolveParityDip(observation);
}

GpuSkinResolvedDraw War3GpuSkinManager::ResolveShadowLease(
    const GpuSkinPreparedDrawKey& key) {
  return m_impl->resolveShadowLease(key);
}

bool War3GpuSkinManager::PlanConsumers(
    const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
    const GpuSkinConsumerPlan& plan) {
  return m_impl->planConsumers(key, leaseId, plan);
}

bool War3GpuSkinManager::CommitConsumer(
    const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
    GpuSkinConsumerBits consumer) {
  return m_impl->commitConsumer(key, leaseId, consumer);
}

bool War3GpuSkinManager::FailConsumer(
    const GpuSkinPreparedDrawKey& key, uint64_t leaseId,
    GpuSkinConsumerBits consumer, GpuSkinConsumerFailure failure) {
  return m_impl->failConsumer(key, leaseId, consumer, failure);
}

bool War3GpuSkinManager::CloseBatchConsumerWindow(
    uint64_t batchId, uint64_t renderThreadId) {
  return m_impl->closeBatchConsumerWindow(batchId, renderThreadId);
}

bool War3GpuSkinManager::TakePendingBatch(
    uint64_t renderThreadId, GpuSkinPendingBatch& output) {
  return m_impl->takePendingBatch(renderThreadId, output);
}

bool War3GpuSkinManager::MarkPendingBatchSubmitted(
    uint64_t batchId, uint64_t renderThreadId,
    Rc<DxvkFence> uploadRetireFence,
    uint64_t uploadRetireValue) {
  return m_impl->markPendingBatchSubmitted(
      batchId, renderThreadId, std::move(uploadRetireFence),
      uploadRetireValue);
}

bool War3GpuSkinManager::RetireBatch(
    uint64_t batchId, uint64_t renderThreadId,
    Rc<DxvkFence> fence, uint64_t value) {
  return m_impl->retireBatch(
      batchId, renderThreadId, std::move(fence), value);
}

bool War3GpuSkinManager::RefreshRetirementDiagnosticsForTest(
    uint64_t renderThreadId) {
  return m_impl->refreshRetirementDiagnosticsForTest(renderThreadId);
}

GpuSkinManagerDiagnostics War3GpuSkinManager::SnapshotDiagnostics() const {
  return m_impl->snapshotDiagnostics();
}

void War3GpuSkinManager::Reset(uint64_t bridgeResetGeneration) {
  const NativeBridgeQuiescenceSnapshot bridge =
      GetNativeBridgeQuiescenceSnapshot();
  const bool fullyQuiescent =
      !(bridge.observedRenderThreadId != 0u &&
        !bridge.currentThreadIsObservedRenderThread) &&
      bridge.currentThreadTlsQuiescent &&
      bridge.activeCallbackPins == 0u &&
      bridge.pendingKernelAuthorizations == 0u &&
      bridge.activeFlushTransactions == 0u &&
      bridge.activeDispatchTransactions == 0u &&
      bridge.activeSemanticTransactions == 0u &&
      bridge.activeUploadTransactions == 0u &&
      bridge.activeDipObserverTransactions == 0u;
  m_impl->reset(bridgeResetGeneration, fullyQuiescent);
}

}  // namespace dxvk::war3::gpu_skin
