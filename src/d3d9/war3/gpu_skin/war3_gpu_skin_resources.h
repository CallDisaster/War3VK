#pragma once

#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../../../dxvk/dxvk_buffer.h"
#include "../../../dxvk/dxvk_device.h"
#include "../../../dxvk/dxvk_fence.h"
#include "../model/war3_model_resource_cache.h"
#include "war3_gpu_skin_types.h"
#include "war3_gpu_skin_compute.h"
#include "war3_persistent_gpu_package_immutable.h"
#include "../tools/war3_resource_residency_census.h"

namespace dxvk::war3::gpu_skin {

class War3PersistentGpuPackageStore;

struct GpuSkinResourceBudgets {
  VkDeviceSize staticBytes = 128ull << 20;
  VkDeviceSize outputBytes = 64ull << 20;
  VkDeviceSize uploadBytes = 16ull << 20;
  uint32_t missQueueCapacity = 128;
  uint32_t jobFallbackCapacity = 512;
};

struct GpuSkinStaticUpload {
  GpuSkinStaticResourceKey key;
  DxvkBufferSlice source;
  DxvkBufferSlice destination;
  VkDeviceSize byteCount = 0;
  resource_census::ResourceHandle residencyCensus;
};

// Immutable proof for one primitive's exact UINT16 index interval.  The
// material-looking source word is deliberately treated as opaque identity;
// current draw-side material and alpha state still need an independent proof.
struct GpuSkinStaticPrimitiveProof {
  uint64_t indexContentHash = 0u;
  uint32_t ordinal = 0u;
  uint32_t primitiveTypeOrMaterialSlot = 0u;
  uint32_t firstIndex = 0u;
  uint32_t indexCount = 0u;
  uint32_t minVertex = 0u;
  uint32_t maxVertex = 0u;
};

inline bool SameGpuSkinStaticPrimitiveProof(
    const GpuSkinStaticPrimitiveProof& lhs,
    const GpuSkinStaticPrimitiveProof& rhs) noexcept {
  return lhs.indexContentHash == rhs.indexContentHash &&
      lhs.ordinal == rhs.ordinal &&
      lhs.primitiveTypeOrMaterialSlot ==
          rhs.primitiveTypeOrMaterialSlot &&
      lhs.firstIndex == rhs.firstIndex &&
      lhs.indexCount == rhs.indexCount &&
      lhs.minVertex == rhs.minVertex &&
      lhs.maxVertex == rhs.maxVertex;
}

// Value-only identity for one immutable vertex/index package in the static
// atlas.  A future Main/Shadow/Outline consumer may retain this proof together
// with the resource shared_ptr, but must still compare every field before
// borrowing either slice.  In particular, a matching geoset pointer alone is
// not sufficient after a map/device epoch or packing-layout change.
struct GpuSkinStaticPackageProof {
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  uint64_t packageGeneration = 0u;
  uintptr_t geosetData = 0u;
  uint64_t contentHash = 0u;
  uint64_t immutableModelGeneration = 0u;
  uint64_t positionContentHash = 0u;
  uint64_t normalContentHash = 0u;
  uint64_t vertexGroupContentHash = 0u;
  uint64_t uv0ContentHash = 0u;
  uint64_t uv1ContentHash = 0u;
  uint64_t indexContentHash = 0u;
  uint64_t primitiveProofHash = 0u;
  uint64_t localBoundsHash = 0u;
  uint32_t layoutGeneration = 0u;
  uint32_t vertexCount = 0u;
  uint32_t indexCount = 0u;
  uint32_t uvLayerCount = 0u;
  uint32_t primitiveProofCount = 0u;
  VkIndexType indexType = VK_INDEX_TYPE_UINT16;
  float localMinX = 0.0f;
  float localMinY = 0.0f;
  float localMinZ = 0.0f;
  float localMaxX = 0.0f;
  float localMaxY = 0.0f;
  float localMaxZ = 0.0f;
  uint32_t staticByteOffset = 0u;
  uint32_t staticByteLength = 0u;
  uint32_t indexByteOffset = 0u;
  uint32_t indexByteLength = 0u;
};

inline bool SameGpuSkinStaticPackageProof(
    const GpuSkinStaticPackageProof& lhs,
    const GpuSkinStaticPackageProof& rhs) noexcept {
  return lhs.mapEpoch == rhs.mapEpoch &&
      lhs.deviceEpoch == rhs.deviceEpoch &&
      lhs.packageGeneration == rhs.packageGeneration &&
      lhs.geosetData == rhs.geosetData &&
      lhs.contentHash == rhs.contentHash &&
      lhs.immutableModelGeneration == rhs.immutableModelGeneration &&
      lhs.positionContentHash == rhs.positionContentHash &&
      lhs.normalContentHash == rhs.normalContentHash &&
      lhs.vertexGroupContentHash == rhs.vertexGroupContentHash &&
      lhs.uv0ContentHash == rhs.uv0ContentHash &&
      lhs.uv1ContentHash == rhs.uv1ContentHash &&
      lhs.indexContentHash == rhs.indexContentHash &&
      lhs.primitiveProofHash == rhs.primitiveProofHash &&
      lhs.localBoundsHash == rhs.localBoundsHash &&
      lhs.layoutGeneration == rhs.layoutGeneration &&
      lhs.vertexCount == rhs.vertexCount &&
      lhs.indexCount == rhs.indexCount &&
      lhs.uvLayerCount == rhs.uvLayerCount &&
      lhs.primitiveProofCount == rhs.primitiveProofCount &&
      lhs.indexType == rhs.indexType &&
      lhs.localMinX == rhs.localMinX &&
      lhs.localMinY == rhs.localMinY &&
      lhs.localMinZ == rhs.localMinZ &&
      lhs.localMaxX == rhs.localMaxX &&
      lhs.localMaxY == rhs.localMaxY &&
      lhs.localMaxZ == rhs.localMaxZ &&
      lhs.staticByteOffset == rhs.staticByteOffset &&
      lhs.staticByteLength == rhs.staticByteLength &&
      lhs.indexByteOffset == rhs.indexByteOffset &&
      lhs.indexByteLength == rhs.indexByteLength;
}

// Store-created immutable descriptor. Its constructor and fields are private,
// so a public GpuSkinStaticPackageProof value cannot authorize itself.  The
// descriptor pins the exact cache snapshot identity, Store instance, epochs,
// atlas slices and independently derived CPU proof used at creation.
class GpuSkinStaticFrozenPackage final {
public:
  const GpuSkinStaticResourceKey& key() const noexcept { return m_key; }
  const model::ShadowGeosetResourceSnapshot& record() const noexcept {
    return m_record;
  }
  const GpuSkinStaticPackageProof& packageProof() const noexcept {
    return m_packageProof;
  }
  const std::vector<GpuSkinStaticPrimitiveProof>& primitiveProofs()
      const noexcept {
    return m_primitiveProofs;
  }
  const PersistentGpuPackageImmutableProof& immutableProof() const noexcept {
    return m_immutableProof;
  }
  uint64_t storeInstanceAuthority() const noexcept {
    return m_storeInstanceAuthority;
  }
  const void* snapshotIdentity() const noexcept { return m_snapshotIdentity; }
  const GpuSkinStaticSourceLayout& sourceLayout() const noexcept {
    return m_sourceLayout;
  }
  const DxvkBufferSlice& packageSlice() const noexcept {
    return m_packageSlice;
  }
  const DxvkBufferSlice& staticSource() const noexcept {
    return m_staticSource;
  }
  const DxvkBufferSlice& indexSource() const noexcept {
    return m_indexSource;
  }
  uint64_t indexContentHash() const noexcept { return m_indexContentHash; }
  uint32_t maxVertexGroupSlot() const noexcept {
    return m_maxVertexGroupSlot;
  }
  VkIndexType indexType() const noexcept { return m_indexType; }
  uint32_t indexCount() const noexcept { return m_indexCount; }
  VkDeviceSize allocatedBytes() const noexcept { return m_allocatedBytes; }

private:
  friend class War3PersistentGpuPackageStore;
  GpuSkinStaticFrozenPackage() = default;

  uint64_t m_storeInstanceAuthority = 0u;
  const void* m_snapshotIdentity = nullptr;
  GpuSkinStaticResourceKey m_key;
  model::ShadowGeosetResourceSnapshot m_record;
  GpuSkinStaticPackageProof m_packageProof;
  std::vector<GpuSkinStaticPrimitiveProof> m_primitiveProofs;
  PersistentGpuPackageImmutableProof m_immutableProof;
  GpuSkinStaticSourceLayout m_sourceLayout;
  DxvkBufferSlice m_packageSlice;
  DxvkBufferSlice m_staticSource;
  DxvkBufferSlice m_indexSource;
  uint64_t m_indexContentHash = 0u;
  uint32_t m_maxVertexGroupSlot = 0u;
  VkIndexType m_indexType = VK_INDEX_TYPE_UINT16;
  uint32_t m_indexCount = 0u;
  VkDeviceSize m_allocatedBytes = 0u;
};

struct GpuSkinStaticResource {
  GpuSkinStaticResourceKey key;
  model::ShadowGeosetResourceSnapshot record;
  GpuSkinStaticPackageProof packageProof;
  std::vector<GpuSkinStaticPrimitiveProof> primitiveProofs;
  uint64_t indexContentHash = 0;
  uint32_t maxVertexGroupSlot = 0;
  GpuSkinStaticSourceLayout sourceLayout;
  // packageSlice is the one contiguous immutable upload transaction.
  // staticSource preserves the existing compute/VS ABI; indexSource is a
  // UINT16 subrange in the same device-local atlas allocation.
  DxvkBufferSlice packageSlice;
  DxvkBufferSlice staticSource;
  DxvkBufferSlice indexSource;
  VkIndexType indexType = VK_INDEX_TYPE_UINT16;
  uint32_t indexCount = 0u;
  GpuSkinStaticUpload pendingUpload;
  GpuSkinStaticResourceState state = GpuSkinStaticResourceState::PendingUpload;
  // Minted by the Store only after the producer fence completed and both the
  // frozen CPU payload and GPU range mirrors passed their full validators.
  // Ready resources are immutable, so hot lookups can check this authority
  // instead of re-walking every stream and primitive for every draw.
  uint64_t readyValidationAuthority = 0u;
  VkDeviceSize allocatedBytes = 0;
  resource_census::ResourceHandle residencyCensus;
  // Only War3PersistentGpuPackageStore can construct this descriptor. Public
  // proof PODs and mutable resource mirrors cannot validate themselves.
  std::shared_ptr<const class GpuSkinStaticFrozenPackage> frozenPayload;
};

bool ValidateGpuSkinStaticPackage(
    const GpuSkinStaticResource& resource) noexcept;
bool ValidateGpuSkinStaticFrozenPayload(
    const GpuSkinStaticResource& resource) noexcept;

static_assert(!std::is_default_constructible_v<GpuSkinStaticFrozenPackage>);
static_assert(std::is_standard_layout_v<GpuSkinStaticPackageProof>);
static_assert(std::is_trivially_copyable_v<GpuSkinStaticPackageProof>);
static_assert(std::is_standard_layout_v<GpuSkinStaticPrimitiveProof>);
static_assert(std::is_trivially_copyable_v<GpuSkinStaticPrimitiveProof>);

struct GpuSkinStaticLookup {
  std::shared_ptr<const GpuSkinStaticResource> resource;
  GpuSkinFallbackReason fallback = GpuSkinFallbackReason::None;

  explicit operator bool() const {
    return resource != nullptr && fallback == GpuSkinFallbackReason::None;
  }
};

struct GpuSkinUploadSlice {
  DxvkBufferSlice slice;
  void* mapPtr = nullptr;
  GpuSkinFallbackReason fallback = GpuSkinFallbackReason::None;

  explicit operator bool() const {
    return slice.defined() && mapPtr != nullptr &&
           fallback == GpuSkinFallbackReason::None;
  }
};

struct GpuSkinBatchUpload {
  GpuSkinUploadSlice palette;
  GpuSkinUploadSlice jobs;
  GpuSkinFallbackReason fallback = GpuSkinFallbackReason::None;

  explicit operator bool() const {
    return static_cast<bool>(palette) && static_cast<bool>(jobs) &&
           fallback == GpuSkinFallbackReason::None;
  }
};

// OutputLease 有意采用持有所有权的值类型，而不是裸 slice；消费者可以安全地
// 跨 Main、Shadow 与 Outline pass 保留它。
struct OutputLease {
  OutputLeaseDesc desc;
  DxvkBufferSlice slice;
  Rc<DxvkFence> retireFence;
  uint64_t retireValue = 0;
  uint64_t leaseId = 0;
  uint64_t pageGeneration = 0;
  uint32_t pageId = 0;
  GpuSkinFallbackReason fallback = GpuSkinFallbackReason::None;

  explicit operator bool() const {
    return slice.defined() && fallback == GpuSkinFallbackReason::None;
  }
};

enum class GpuSkinInputLeaseReceiptState : uint8_t {
  Pending = 0u,
  ConsumerCommitted = 1u,
  ProducerOnly = 2u,
  Cancelled = 3u,
};

// receipt 是输入租约的唯一寿命权限对象。Pending 期间 Main/Shadow 可以排队消费；
// consumer window 关闭后，manager 必须把同一对象提交到真实 Rc<DxvkFence> identity。
// ProducerOnly/Cancelled 都禁止后续 CPU 侧消费者继续发布陈旧 draw。
struct GpuSkinInputLeaseReceipt {
  GpuSkinInputLeaseDesc desc;
  DxvkBufferSlice staticSource;
  DxvkBufferSlice palette;
  Rc<DxvkFence> consumerFence;
  uint64_t consumerFenceValue = 0u;
  uint64_t storageLeaseId = 0u;
  uint64_t storagePageGeneration = 0u;
  uint32_t storagePageId = 0u;
  GpuSkinInputLeaseReceiptState state =
      GpuSkinInputLeaseReceiptState::Pending;
};

inline bool SameGpuSkinInputLeaseDesc(
    const GpuSkinInputLeaseDesc& lhs,
    const GpuSkinInputLeaseDesc& rhs) noexcept {
  return lhs.mapEpoch == rhs.mapEpoch &&
      lhs.deviceEpoch == rhs.deviceEpoch &&
      lhs.frameTag == rhs.frameTag && lhs.token == rhs.token &&
      lhs.dispatchEpoch == rhs.dispatchEpoch &&
      lhs.uploadEpoch == rhs.uploadEpoch &&
      lhs.staticByteOffset == rhs.staticByteOffset &&
      lhs.staticByteLength == rhs.staticByteLength &&
      lhs.paletteByteOffset == rhs.paletteByteOffset &&
      lhs.paletteByteLength == rhs.paletteByteLength &&
      lhs.vertexCount == rhs.vertexCount &&
      lhs.paletteMatrixCount == rhs.paletteMatrixCount &&
      lhs.sourceUvLayerCount == rhs.sourceUvLayerCount &&
      lhs.outputFormat == rhs.outputFormat &&
      lhs.layoutGeneration == rhs.layoutGeneration &&
      lhs.consumerBits == rhs.consumerBits;
}

// VS-A 不直接引用 producer-only upload page。调色板先复制进由 page generation
// 和帧尾 consumer fence 共同保护的 device-local 区间，static slice 继续引用不可变图集。
struct GpuSkinInputLease {
  GpuSkinInputLeaseDesc desc;
  DxvkBufferSlice staticSource;
  DxvkBufferSlice palette;
  std::shared_ptr<GpuSkinInputLeaseReceipt> receipt;
  uint64_t storageLeaseId = 0;
  uint64_t storagePageGeneration = 0;
  uint32_t storagePageId = 0;
  GpuSkinFallbackReason fallback = GpuSkinFallbackReason::None;

  explicit operator bool() const {
    return staticSource.defined() && palette.defined() && receipt != nullptr &&
        storageLeaseId != 0u && storagePageGeneration != 0u &&
        storagePageId != 0u && fallback == GpuSkinFallbackReason::None &&
        receipt->state == GpuSkinInputLeaseReceiptState::Pending &&
        receipt->consumerFence == nullptr &&
        receipt->consumerFenceValue == 0u &&
        receipt->storageLeaseId == storageLeaseId &&
        receipt->storagePageGeneration == storagePageGeneration &&
        receipt->storagePageId == storagePageId &&
        SameGpuSkinInputLeaseDesc(receipt->desc, desc) &&
        receipt->staticSource.buffer() == staticSource.buffer() &&
        receipt->staticSource.offset() == staticSource.offset() &&
        receipt->staticSource.length() == staticSource.length() &&
        receipt->palette.buffer() == palette.buffer() &&
        receipt->palette.offset() == palette.offset() &&
        receipt->palette.length() == palette.length();
  }
};

struct GpuSkinInputCopy {
  DxvkBufferSlice source;
  DxvkBufferSlice destination;
  VkDeviceSize byteCount = 0u;
  uint32_t token = 0u;
};

class War3PersistentGpuPackageStore;

class War3GpuSkinResources {
public:
  explicit War3GpuSkinResources(Rc<DxvkDevice> device,
      const GpuSkinResourceBudgets& budgets = { });
  ~War3GpuSkinResources();

  War3GpuSkinResources(const War3GpuSkinResources&) = delete;
  War3GpuSkinResources& operator=(const War3GpuSkinResources&) = delete;

  bool beginFrame(uint64_t mapEpoch, uint64_t deviceEpoch,
                  uint64_t frameTag);
  void invalidateMapEpoch(uint64_t nextMapEpoch);
  void invalidateDevice(Rc<DxvkDevice> device, uint64_t nextDeviceEpoch);
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
  bool retireStaticUploads(
      const std::vector<GpuSkinStaticUpload>& uploads,
      Rc<DxvkFence> fence, uint64_t value);
  DxvkBufferSlice staticAtlasSlice() const;

  GpuSkinBatchUpload allocateBatchUpload(
      VkDeviceSize paletteByteCount, VkDeviceSize jobByteCount);
  // VS-B0 不创建 compute job；这里只分配一次 host-visible palette 源，
  // 其页仍由既有 producer fence 统一退休。
  GpuSkinUploadSlice allocatePaletteUpload(VkDeviceSize paletteByteCount);
  bool retireUploads(Rc<DxvkFence> fence, uint64_t value);
  void discardUploads();

  OutputLease allocateOutput(const OutputLeaseDesc& desc);
  bool retireOutput(const OutputLease& lease, Rc<DxvkFence> fence,
      uint64_t value);
  // Only use for a lease that was never submitted to the GPU.
  void discardOutput(const OutputLease& lease);
  void pollRetired();

  void recordJobFallback(uint64_t frameTag, uint32_t token,
      GpuSkinFallbackReason reason);
  std::vector<GpuSkinJobFallback> takeJobFallbacks();

  const GpuSkinResourceBudgets& budgets() const;
  GpuSkinDiagnostics diagnostics() const;
  VkDeviceSize storageBufferOffsetAlignment() const;
  uint64_t mapEpoch() const;
  uint64_t deviceEpoch() const;
  bool hasInFlightResources() const;

private:
  struct UploadPage {
    Rc<DxvkBuffer> buffer;
    resource_census::ResourceHandle residencyCensus;
    VkDeviceSize capacity = 0;
    VkDeviceSize cursor = 0;
    Rc<DxvkFence> fence;
    uint64_t retireValue = 0;
    // uploadPagesAllocated/uploadPagesReclaimed is also a quiescence
    // contract. Count a physical page as reclaimed only once even when the
    // same allocation completes multiple reuse cycles.
    bool reclamationCounted = false;
  };

  struct OutputPage {
    uint32_t id = 0;
    uint64_t generation = 1u;
    Rc<DxvkBuffer> buffer;
    resource_census::ResourceHandle residencyCensus;
    VkDeviceSize capacity = 0;
    VkDeviceSize cursor = 0;
    uint32_t outstanding = 0;
  };

  struct RetiredOutput {
    OutputLease lease;
  };

  struct ActiveOutput {
    uint32_t pageId = 0;
    uint64_t pageGeneration = 0;
    uint32_t byteOffset = 0;
    uint32_t byteLength = 0;
    bool retirementQueued = false;
  };

  bool ensureUploadPage(VkDeviceSize requiredBytes);
  void updateStorageBufferOffsetAlignment();
  void recordFallback(GpuSkinFallbackReason reason);
  void publishResidencySnapshot() const;
  void clearDynamicEpochResources();

  Rc<DxvkDevice> m_device;
  GpuSkinResourceBudgets m_budgets;
  uint64_t m_mapEpoch = 0;
  uint64_t m_deviceEpoch = 0;
  uint64_t m_lastRetirementPollMapEpoch = 0;
  uint64_t m_lastRetirementPollDeviceEpoch = 0;
  uint64_t m_lastRetirementPollFrameTag = 0;
  uint64_t m_nextOutputLeaseId = 1;
  uint32_t m_nextOutputPageId = 1;
  VkDeviceSize m_uploadBytes = 0;
  VkDeviceSize m_outputBytes = 0;
  VkDeviceSize m_storageBufferOffsetAlignment = 16u;
  GpuSkinDiagnostics m_diagnostics;
  // P0 ownership seam: immutable model/index packages live in an independent
  // store. Resources preserves its public API surface and delegates the old path; no
  // D3D9 device owner or renderer consumer is introduced by this refactor.
  std::unique_ptr<War3PersistentGpuPackageStore> m_persistentPackages;
  std::deque<GpuSkinJobFallback> m_jobFallbacks;
  std::unique_ptr<UploadPage> m_activeUpload;
  std::deque<std::unique_ptr<UploadPage>> m_pendingUploads;
  std::deque<std::unique_ptr<UploadPage>> m_retiredUploads;
  // Pages enter this pool only after their producer fence has completed.
  // Idle pages still count against m_uploadBytes but are not in flight.
  std::deque<std::unique_ptr<UploadPage>> m_idleUploads;
  std::vector<std::unique_ptr<OutputPage>> m_outputPages;
  std::unordered_map<uint64_t, ActiveOutput> m_activeOutputs;
  std::deque<RetiredOutput> m_retiredOutputs;
};

}  // namespace dxvk::war3::gpu_skin
