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
#include "../tools/war3_resource_residency_census.h"

namespace dxvk::war3::gpu_skin {

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

struct GpuSkinStaticResource {
  GpuSkinStaticResourceKey key;
  model::ShadowGeosetResourceSnapshot record;
  uint64_t indexContentHash = 0;
  uint32_t maxVertexGroupSlot = 0;
  GpuSkinStaticSourceLayout sourceLayout;
  DxvkBufferSlice staticSource;
  GpuSkinStaticUpload pendingUpload;
  GpuSkinStaticResourceState state = GpuSkinStaticResourceState::PendingUpload;
  VkDeviceSize allocatedBytes = 0;
  resource_census::ResourceHandle residencyCensus;
};

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

  struct RetiredStaticUpload {
    GpuSkinStaticResourceKey key;
    DxvkBufferSlice source;
    Rc<DxvkFence> fence;
    uint64_t retireValue = 0;
    resource_census::ResourceHandle residencyCensus;
  };

  struct ActiveOutput {
    uint32_t pageId = 0;
    uint64_t pageGeneration = 0;
    uint32_t byteOffset = 0;
    uint32_t byteLength = 0;
    bool retirementQueued = false;
  };

  struct QueuedStaticMiss {
    GpuSkinStaticResourceKey key;
    model::ShadowGeosetResourceSnapshot record;
  };

  GpuSkinStaticResourceKey makeKey(
      const model::ShadowGeosetResourceRecord& record,
      uint32_t layoutGeneration) const;
  std::shared_ptr<GpuSkinStaticResource> createStaticResource(
      const QueuedStaticMiss& miss);
  bool ensureUploadPage(VkDeviceSize requiredBytes);
  void updateStorageBufferOffsetAlignment();
  void recordFallback(GpuSkinFallbackReason reason);
  void publishResidencySnapshot() const;
  void clearEpochResources();

  Rc<DxvkDevice> m_device;
  GpuSkinResourceBudgets m_budgets;
  uint64_t m_mapEpoch = 0;
  uint64_t m_deviceEpoch = 0;
  uint64_t m_lastRetirementPollMapEpoch = 0;
  uint64_t m_lastRetirementPollDeviceEpoch = 0;
  uint64_t m_lastRetirementPollFrameTag = 0;
  uint64_t m_nextOutputLeaseId = 1;
  uint32_t m_nextOutputPageId = 1;
  VkDeviceSize m_staticBytes = 0;
  VkDeviceSize m_staticCursor = 0;
  VkDeviceSize m_uploadBytes = 0;
  VkDeviceSize m_outputBytes = 0;
  uint64_t m_queuedStaticMissHostBytes = 0;
  uint64_t m_peakQueuedStaticMissRecords = 0;
  uint64_t m_peakQueuedStaticMissHostBytes = 0;
  VkDeviceSize m_storageBufferOffsetAlignment = 16u;
  GpuSkinDiagnostics m_diagnostics;
  std::unordered_map<GpuSkinStaticResourceKey,
      std::shared_ptr<GpuSkinStaticResource>, GpuSkinStaticResourceKeyHash>
      m_staticResources;
  std::deque<QueuedStaticMiss> m_staticMisses;
  std::deque<GpuSkinJobFallback> m_jobFallbacks;
  std::vector<GpuSkinStaticUpload> m_readyStaticUploads;
  Rc<DxvkBuffer> m_staticAtlas;
  resource_census::ResourceHandle m_staticAtlasCensus;
  std::unique_ptr<UploadPage> m_activeUpload;
  std::deque<std::unique_ptr<UploadPage>> m_pendingUploads;
  std::deque<std::unique_ptr<UploadPage>> m_retiredUploads;
  // Pages enter this pool only after their producer fence has completed.
  // Idle pages still count against m_uploadBytes but are not in flight.
  std::deque<std::unique_ptr<UploadPage>> m_idleUploads;
  std::vector<std::unique_ptr<OutputPage>> m_outputPages;
  std::unordered_map<uint64_t, ActiveOutput> m_activeOutputs;
  std::deque<RetiredStaticUpload> m_retiredStaticUploads;
  std::deque<RetiredOutput> m_retiredOutputs;
};

}  // namespace dxvk::war3::gpu_skin
