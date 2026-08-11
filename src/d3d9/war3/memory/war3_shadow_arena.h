// war3_shadow_arena.h
// Shadow 帧级 GPU 线性分配器（bump-pointer allocator）
//
// Arena buffer 为 DEVICE_LOCAL，无 CPU 映射。
// 分配结果（ShadowArenaAllocation）仅含 GPU offset + size + storage + info。
// 动态源通过 EmitCs(ctx->copyBuffer) 写入，GPU shadow pass 直接读取，CPU 不介入。
// 帧间轮转（默认 3 帧）避免 GPU 仍在读取上帧数据时被覆写。

#pragma once

#include "../../dxvk/dxvk_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>


namespace dxvk::war3::memory {

struct ShadowArenaAllocation {
  uint32_t offset = 0;
  uint32_t size = 0;
  Rc<DxvkBuffer> storage;
  DxvkResourceBufferInfo info = {};

  explicit operator bool() const {
    return storage != nullptr && info.buffer != VK_NULL_HANDLE && size != 0u;
  }
};

enum class ShadowArenaAllocationTag : uint8_t {
  Unknown = 0u,
  Position,
  Blend,
  Uv,
  Index,
};

enum class ShadowArenaSourceClass : uint8_t {
  Terrain = 0u,
  Model,
  Skinned,
  Up,
};

struct ShadowArenaBundleRequest {
  uint32_t size = 0u;
  uint32_t alignment = 16u;
  ShadowArenaAllocationTag tag = ShadowArenaAllocationTag::Unknown;
};

constexpr uint32_t kShadowArenaBundleMaxParts = 4u;

// Reserved before copy commands are recorded. The render thread either commits
// every position/blend/UV/index allocation or restores the exact cursor.
struct ShadowArenaBundleTransaction {
  std::array<ShadowArenaAllocation, kShadowArenaBundleMaxParts> allocations = {};
  std::array<ShadowArenaAllocationTag, kShadowArenaBundleMaxParts> tags = {};
  uint32_t allocationCount = 0u;
  uint32_t generationIndex = 0u;
  uint32_t startPage = 0u;
  uint32_t startOffset = 0u;
  uint32_t startCommittedBytes = 0u;
  uint32_t startPageTailWasteBytes = 0u;
  uint32_t endPage = 0u;
  uint32_t endOffset = 0u;
  uint32_t endCommittedBytes = 0u;
  uint32_t endPageTailWasteBytes = 0u;
  uint64_t generation = 0u;
  uint64_t requestedBytes = 0u;
  bool active = false;
};

struct ShadowArenaDiagnostics {
  uint64_t usedBytes = 0u;
  uint64_t residentBytes = 0u;
  uint64_t perGenerationCapacityBytes = 0u;
  // Effective cap from the latest Present-safe memory-budget snapshot.
  uint64_t residentLimitBytes = 0u;
  uint64_t fixedResidentLimitBytes = 0u;
  uint64_t memoryHeapSizeBytes = 0u;
  uint64_t memoryBudgetBytes = 0u;
  uint64_t memoryAllocatedBytes = 0u;
  uint64_t memoryAvailableBytes = 0u;
  uint64_t proportionalLimitBytes = 0u;
  uint64_t reserveLimitBytes = 0u;
  uint64_t budgetRefreshCount = 0u;
  uint64_t budgetGrowthRejectCount = 0u;
  uint64_t budgetSnapshotFrameSerial = 0u;
  uint64_t generation = 0u;
  uint64_t submittedSerial = 0u;
  uint64_t completedSerial = 0u;
  uint64_t busyReuseRejectCount = 0u;
  uint64_t overflowCount = 0u;
  uint64_t reservedBytes = 0u;
  uint64_t committedBundleBytes = 0u;
  uint64_t rolledBackBytes = 0u;
  uint64_t admissionRejectedCount = 0u;
  uint64_t partialTransactionCount = 0u;
  uint64_t pageTailWasteBytes = 0u;
  uint64_t positionBytes = 0u;
  uint64_t blendBytes = 0u;
  uint64_t uvBytes = 0u;
  uint64_t indexBytes = 0u;
  uint64_t terrainBytes = 0u;
  uint64_t modelBytes = 0u;
  uint64_t skinnedBytes = 0u;
  uint64_t upBytes = 0u;
  uint64_t uniqueSourceBytes = 0u;
  uint64_t duplicateBytesSaved = 0u;
  uint64_t exactIndexTrimAcceptedCount = 0u;
  uint64_t exactIndexTrimRejectedCount = 0u;
  uint64_t exactIndexTrimBytesSaved = 0u;
  uint64_t coherentUpTrimObservedCount = 0u;
  uint64_t coherentUpTrimEligibleCount = 0u;
  uint64_t coherentUpTrimWouldSaveBytes = 0u;
  uint64_t coherentUpTrimConsumedCount = 0u;
  uint64_t coherentUpTrimConsumedBytesSaved = 0u;
  uint64_t coherentRealTrimObservedCount = 0u;
  uint64_t coherentRealTrimEligibleCount = 0u;
  uint64_t coherentRealTrimWouldSaveBytes = 0u;
  uint64_t coherentRealTrimConsumedCount = 0u;
  uint64_t coherentRealTrimConsumedBytesSaved = 0u;
  uint64_t currentUpPositionReplayObservedCount = 0u;
  uint64_t currentUpPositionReplayEligibleCount = 0u;
  uint64_t currentUpPositionReplayWouldAvoidBytes = 0u;
  uint64_t currentUpPositionReplayConsumedCount = 0u;
  uint64_t currentUpPositionReplayAvoidedBytes = 0u;
  uint64_t quarantineCount = 0u;
  uint64_t lastQuarantinedGeneration = 0u;
  uint64_t lastQuarantinedRetireSerial = 0u;
  uint32_t primaryHeapIndex = 0xFFFFFFFFu;
  uint32_t memoryBudgetSupported = 0u;
  uint32_t memoryBudgetTrusted = 0u;
  uint32_t activeGenerationCount = 0u;
  uint32_t frameIncomplete = 0u;
};

/**
 * @brief 初始化 Shadow Arena 分配器。
 *
 * 创建三个 64 MiB DEVICE_LOCAL 预热页；每个 GPU 代际最多 384 MiB。
 * 总驻留量不超过 1.125 GiB，并在 VK_EXT_memory_budget 数据可信时进一步
 * 受主显存可用预算比例和固定保留量约束。无 CPU 映射；写入通过
 * EmitCs(ctx->copyBuffer) 完成。
 * 在 D3D9 设备创建后调用，不依赖 TLSF 池。
 *
 * @return 成功返回 true。
 */
bool ShadowArena_Init(DxvkDevice* device);
bool ShadowArena_IsInitialized();
bool ShadowArena_IsOwnedBy(const DxvkDevice* device);
void ShadowArena_Shutdown(DxvkDevice* device);

/**
 * @brief 切换到当前渲染帧对应的 Arena 分区。
 *
 * 优先轮转三个预热代际，但只有 retire fence 已完成的代际才允许清零复用；
 * GPU 落后时会在总驻留上限内增加 spill 代际。
 */
bool ShadowArena_BeginFrame(uint64_t frameSerial, uint64_t completedSerial);

/** Mark the active generation as owned by all shadow work for frameSerial. */
void ShadowArena_EndFrame(uint64_t frameSerial);

/** Seal the active map generation without rewinding its allocation cursor. */
bool ShadowArena_QuarantineCurrentGeneration(uint64_t retireSerial);

/**
 * @brief 在 Arena 中分配当前帧所需内存（极速无锁分配）。
 *
 * 只涉及当前代际的页游标推进。此块内存在该代际的 completion fence
 * 完成之前均不会被覆写。
 *
 * @param size      需要的内存大小。
 * @param alignment 对齐要求（必须为 2 的幂次，默认 16）。
 * @return          分配结果（含 GPU offset/size/storage/info），空间不足时 operator bool() 返回 false。
 */
ShadowArenaAllocation ShadowArena_Alloc(uint32_t size,
                                         uint32_t alignment = 16);
bool ShadowArena_BeginBundle(
    const ShadowArenaBundleRequest* requests, uint32_t requestCount,
    ShadowArenaBundleTransaction& transaction);
bool ShadowArena_CommitBundle(ShadowArenaBundleTransaction& transaction);
bool ShadowArena_RollbackBundle(ShadowArenaBundleTransaction& transaction);
void ShadowArena_NoteFreezeCatalogBytes(
    ShadowArenaAllocationTag tag, ShadowArenaSourceClass sourceClass,
    uint64_t uniqueBytes, uint64_t duplicateBytesSaved);
void ShadowArena_NoteExactIndexTrim(
    bool accepted, uint64_t bytesBefore, uint64_t bytesAfter);
void ShadowArena_NoteCoherentUpIndexTrim(
    bool eligible, bool consumed, uint64_t bytesBefore,
    uint64_t bytesAfter);
void ShadowArena_NoteCoherentRealIndexTrim(
    bool observed, bool eligible, bool consumed, uint64_t bytesBefore,
    uint64_t bytesAfter);
void ShadowArena_NoteCurrentUpPositionReplay(
    bool observed, bool eligible, bool consumed, uint64_t avoidedBytes);

/**
 * @brief 重置分配器游标。
 *
 * 仅供已经由 ShadowArena_BeginFrame 证明可复用的当前代际使用；内部只将
 * 当前页游标置零，不执行 free。
 */
void ShadowArena_Reset();

/**
 * @brief 查询当前帧 Arena 已用字节数。
 * @return 已用字节数。
 */
uint32_t ShadowArena_UsedBytes();
uint32_t ShadowArena_CapacityBytes();
uint32_t ShadowArena_RemainingBytes();
uint64_t ShadowArena_ResidentBytes();
ShadowArenaDiagnostics ShadowArena_QueryDiagnostics();

} // namespace dxvk::war3::memory
