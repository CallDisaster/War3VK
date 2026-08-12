#pragma once

#include "../../util/util_matrix.h"
#include "war3_render_objects.h"
#include "war3_render_queue_tracker.h"
#include "war3_render_state.h"
#include "war3_shadow_producer_policy.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk::war3::render {

enum class CurrentDrawResolveStatus : uint8_t {
  MissingContract = 0,
  MissingPalette = 1,
  MissingGroupSlots = 2,
  StaleVisibleFrame = 3,
  Ready = 4,
};

// Phase 1 Producer Packet Takeover：palette snapshot 的来源标记。
// 用于区分 PublishCurrentDrawContract 里的 trusted writer 命中 vs raw arena fallback。
// submit 端据此决定是否信任 palette 数据，以及统计 provenance 分布。
enum class PaletteProvenance : uint32_t {
  Unknown = 0,                  // 未标记（兼容旧路径）
  TrustedBlendedWriter = 1,     // 来自 Hook_RuntimeMatrixWrite (0x12E600) 同帧捕获
  RawGlobalArena = 2,           // 直接 memcpy record.paletteAddress（可能残留）
  ProducerPartPacket = 3,       // 预留：Phase 2 producer ring
  RangeCopyPoseRebuild = 4,     // 预留：Phase 4 基于 0x12FDC0 重建
  CModelFallback = 5,           // 预留：CModel + 0x60 直读
};

// The ordinary render-queue dispatchers carry a proven material layer.  The
// transparent Type0 model dispatcher is a separate native boundary and does
// not: it identifies the exact child render batch, but not a normal layer
// index.  Keep that distinction explicit so an unknown layer can never be
// forged as layer zero.
enum class CurrentDrawDispatchDomain : uint8_t {
  Unknown = 0u,
  Common = 1u,
  Special = 2u,
  TransparentType0 = 3u,
};

struct CurrentDrawContractRecord {
  bool known = false;
  void* sceneNode = nullptr;
  void* renderablePart = nullptr;
  void* meshPayloadPtr = nullptr;
  void* worldObjectEntry = nullptr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t rawcode = 0u;
  ObjectKind objectKind = ObjectKind::Unknown;
  uint32_t layerIndex = 0u;
  uint32_t paletteSlotIndex = 0xFFFFFFFFu;
  void* paletteAddress = nullptr;
  uint32_t payloadWordF0 = 0u;
  uint32_t payloadWord104 = 0u;
  uint32_t payloadWord108 = 0u;
  uint32_t payloadWord11C = 0u;
  uint32_t payloadWord48 = 0u;
  void* stream1Ptr = nullptr;
  uint32_t stream1Stride = 0u;
  uint32_t capturedPaletteCount = 0u;
  uint32_t frameTag = 0u;
  int16_t stage = -1;
  bool pathBlocker = false;
  uint64_t visibleFrameSerial = 0u;
  uint32_t renderFrameIndex = 0u;
  uint64_t captureSerial = 0u;
  // Shadow producer ownership is stage/tag based. Preserve the tag observed at
  // publish time so delayed snapshot consumers cannot reclassify an S12 range
  // indicator redraw as a physical world-object caster.
  War3BatchTag batchTag = War3BatchTag::Unknown;
  int16_t producerStage = -1;
  War3BatchTag producerGroup = War3BatchTag::Unknown;
  ShadowProducerKind sourceKind = ShadowProducerKind::CurrentDrawContract;
  bool producerFreshThisFrame = false;
  uint64_t stagePolicyRevision = 0u;
  bool fromGrace = false;
  uint32_t graceAge = 0u;
  bool alphaPayloadComplete = false;
};

struct CurrentDrawDispatchContext {
  bool valid = false;
  CurrentDrawDispatchDomain domain = CurrentDrawDispatchDomain::Unknown;
  void* sceneNode = nullptr;
  void* renderablePart = nullptr;
  void* meshPayload = nullptr;
  bool layerKnown = false;
  uint32_t layerIndex = kRenderQueueUnknownLayerIndex;
};

struct CurrentDrawPreparedSliceRecord {
  bool known = false;
  void* sceneNode = nullptr;
  void* renderablePart = nullptr;
  uint32_t layerIndex = 0u;
  uint32_t primitiveType = 0u;
  uint32_t preparedCount = 0u;
  uint64_t preparedHash = 0u;
  uint64_t captureSerial = 0u;
};

struct CurrentDrawAuthoritativeSample {
  CurrentDrawContractRecord contract = {};
  std::vector<Matrix4> palette;
  uint32_t paletteCount = 0u;
  uint64_t paletteHash = 0u;
  std::vector<uint8_t> groupSlots;
  uint64_t groupHash = 0u;         // 诊断 hash（含 stream1Ptr）
  uint64_t stableGroupHash = 0u;   // 稳定 hash（不含 stream1Ptr），用于 geometry key
  CurrentDrawResolveStatus status = CurrentDrawResolveStatus::MissingContract;
  // Phase 1：palette bytes 的来源标记。
  PaletteProvenance paletteProvenance = PaletteProvenance::Unknown;

  bool contractKnown() const {
    return contract.known;
  }

  bool paletteReady() const {
    return paletteCount != 0u && palette.size() >= paletteCount &&
           paletteHash != 0u;
  }

  bool groupSlotsReady() const {
    return !groupSlots.empty();
  }
};

// Optional low-overhead phase observer for the authoritative decode hot path.
// The caller owns timing policy and sampling; production calls pass nullptr.
enum class CurrentDrawResolveTracePhase : uint8_t {
  PaletteDecode = 0u,
  PaletteHash,
  GroupGate,
  GroupRangeCheck,
  GroupDecodeLoop,
  GroupFinalize,
  StableHash,
};

struct CurrentDrawResolveTrace {
  void* context = nullptr;
  void (*enter)(void* context, CurrentDrawResolveTracePhase phase) = nullptr;

  inline void note(CurrentDrawResolveTracePhase phase) const {
    if (enter != nullptr)
      enter(context, phase);
  }
};

// Optional caller-owned readable-range validator. Populate uses this to keep
// VirtualQuery results only for the lifetime of one scene build; callers that
// do not provide it retain the original IsReadableRange behavior.
struct CurrentDrawRangeValidator {
  void* context = nullptr;
  bool (*validate)(void* context, const void* address, size_t size) = nullptr;

  inline bool readable(const void* address, size_t size) const {
    return validate != nullptr && validate(context, address, size);
  }
};

struct CurrentDrawContractDiagnosticsSummary {
  uint64_t publishAttemptCount = 0u;
  uint64_t publishReadyCount = 0u;
  uint64_t publishMissNoRenderablePart = 0u;
  uint64_t publishMissNoMeshPayload = 0u;
  uint64_t publishMissInvalidPaletteSlot = 0u;
  uint64_t publishMissInvalidPaletteCount = 0u;
  uint64_t publishMissNoGlobalPalette = 0u;
  uint64_t publishSkippedNonWorldContext = 0u;
  uint64_t publishSkippedSmallViewport = 0u;
  uint64_t queryAttemptCount = 0u;
  uint64_t queryHitCount = 0u;
  uint64_t queryMissNoRecord = 0u;
  uint64_t queryMissFrameTagMismatch = 0u;
  uint64_t queryMissCacheCollision = 0u;
  uint64_t capturedPaletteQueryAttemptCount = 0u;
  uint64_t capturedPaletteQueryHitCount = 0u;
  uint64_t capturedPaletteMissNoContract = 0u;
  uint64_t capturedPaletteMissInvalidCount = 0u;
  uint64_t capturedPaletteMissNoSnapshot = 0u;
  uint64_t capturedPaletteMissUnreadablePalette = 0u;
  // Phase 7.30 Action B：attribution-key snapshot 命中计数。
  uint64_t paletteAttributionSnapshotHitCount = 0u;
  uint64_t paletteCaptureTrustedSourceHitCount = 0u;
  uint64_t paletteCaptureTrustedSourceMissCount = 0u;
  // Phase 7.34：严格仲裁下丢弃 raw arena 的次数。
  // 高值说明 trusted 覆盖率不足，需要补 range-copy 发布或 best-effort 通道。
  uint64_t paletteRejectedNoTrustedSourceCount = 0u;
  // Phase 7.34 第三轮：trusted snapshot 保留次数。
  // 每次 RawGlobalArena 来袭但旧 snapshot 是 trusted 时 +1。
  // 用于验证"不让 raw arena 污染已有 trusted 数据"的修复生效。
  uint64_t paletteSnapshotTrustedPreservedCount = 0u;
  // Phase 7.35 Pose-lag 诊断：submit 端 palette 时间滞后分布。
  // 语义：submit 时比较 `currentFrameIndex - record.renderFrameIndex`。
  // - Lag0：record 是本帧新 publish（理想情况，时间对齐）。
  // - Lag1：record 是上一帧 publish（可能 capture miss 被沿用一次）。
  // - Lag2：落后 2 帧（Phase 7.32 放宽的 captureSerial diff<=2 会让 submit 沿用）。
  // - Lag3To5：路径 2 所谓 "stutter" 的集中区间（3-5 帧冻结）。
  // - Lag6Plus：已经严重滞后，manifest/lease 设计预期最坏 6 帧。
  // Max：单帧内最大观察到的滞后值（用于定位最严重的 stutter 时刻）。
  // 占比算法：Lag>=1 / TotalSamples 就是 "submit 用旧 palette" 的比例。
  uint64_t submitPaletteFrameLag0Count = 0u;
  uint64_t submitPaletteFrameLag1Count = 0u;
  uint64_t submitPaletteFrameLag2Count = 0u;
  uint64_t submitPaletteFrameLag3To5Count = 0u;
  uint64_t submitPaletteFrameLag6PlusCount = 0u;
  uint64_t submitPaletteFrameLagMax = 0u;
  uint64_t submitPaletteFrameLagSampleCount = 0u;
  // Phase 7.39：submit 端实际 palette 内容年龄分布。
  // 语义：比较当前 Game.dll palette frameTag 与 packet palette 内容的最旧
  // frameTag，区别于上面的 record-age lag。Unknown 表示 packet 没有 slot/
  // frameTag 元数据，或当前 Game.dll frameTag 不可读。
  uint64_t submitPaletteContentAgeLag0Count = 0u;
  uint64_t submitPaletteContentAgeLag1Count = 0u;
  uint64_t submitPaletteContentAgeLag2Count = 0u;
  uint64_t submitPaletteContentAgeLag3To5Count = 0u;
  uint64_t submitPaletteContentAgeLag6PlusCount = 0u;
  uint64_t submitPaletteContentAgeMax = 0u;
  uint64_t submitPaletteContentAgeSampleCount = 0u;
  uint64_t submitPaletteContentAgeUnknownCount = 0u;
  // Phase 7.35 路径 2 诊断：submit-side live palette rebuild。
  // 语义：当 submit 点发现 record 的 `renderFrameIndex` 比当前帧旧 N 帧以上时，
  // 尝试用 PoseRegistry (0x12FDC0 publish) 重建 fresh palette 并覆盖原 palette。
  // - AttemptCount：满足 lag 阈值、进入 rebuild 路径的次数；
  // - HitCount：`War3TryBuildLiveRuntimeGroupPalette` 返回 true（PoseRegistry 命中）；
  // - MissCount：返回 false（PoseRegistry 没有 fresh 数据）；
  // - AppliedCount：rebuild 成功 + 覆盖 packet palette 成功的次数。
  // 对比：Attempt vs Lag>=3 总和 → 覆盖完整度；Hit vs Attempt → PoseRegistry 覆盖率；
  // Applied vs Hit → 覆盖链路是否打通。
  uint64_t submitLiveRebuildAttemptCount = 0u;
  uint64_t submitLiveRebuildHitCount = 0u;
  uint64_t submitLiveRebuildMissCount = 0u;
  uint64_t submitLiveRebuildAppliedCount = 0u;
  // Phase 7.35 路径 1 诊断：capture 端 QueryBlendedPaletteBySlotIndexExact 的
  // miss 分桶。这些值从 war3_model_hook 透传上来，让一份 summary 同时能看到
  // capture miss 分布和 submit lag 分布。
  uint64_t paletteCaptureSlotOverflowMissCount = 0u;
  uint64_t paletteCaptureInvalidEntryMissCount = 0u;
  uint64_t paletteCaptureFrameTagMismatchMissCount = 0u;
  uint64_t paletteCaptureShortResultMissCount = 0u;
  uint64_t paletteCaptureBestEffortHitCount = 0u;
  uint64_t paletteCaptureExactHitCount = 0u;
  // Phase 1：palette provenance 分桶计数。
  uint64_t paletteProvenanceTrustedBlendedWriterCount = 0u;
  uint64_t paletteProvenanceRawGlobalArenaCount = 0u;
  uint64_t paletteProvenanceProducerPartPacketCount = 0u;
  uint64_t paletteProvenanceRangeCopyPoseRebuildCount = 0u;
  uint64_t paletteProvenanceCModelFallbackCount = 0u;
  uint64_t paletteProvenanceUnknownCount = 0u;
  uint64_t groupSlotDecodeAttemptCount = 0u;
  uint64_t groupSlotDecodeHitCount = 0u;
  uint64_t groupSlotDecodeMissDisabledStream = 0u;
  uint64_t groupSlotDecodeMissNoStream = 0u;
  uint64_t groupSlotDecodeMissUnreadableStream = 0u;
  uint64_t groupSlotDecodeMissGroupOutOfRange = 0u;
  uint64_t preparedSliceProbeAttemptCount = 0u;
  uint64_t preparedSliceProbeContextReadyCount = 0u;
  uint64_t preparedSliceProbeBackingReadableCount = 0u;
  uint64_t preparedSliceRecordedCount = 0u;
  uint64_t preparedSliceQueryAttemptCount = 0u;
  uint64_t preparedSliceQueryHitCount = 0u;
  uint64_t preparedSliceQueryMissCount = 0u;
  uint64_t stream1PublishNoStreamCount = 0u;
  uint64_t stream1PublishStride0Count = 0u;
  uint64_t stream1PublishStride1Count = 0u;
  uint64_t stream1PublishStride8Count = 0u;
  uint64_t stream1PublishStride12Count = 0u;
  uint64_t stream1PublishStride16Count = 0u;
  uint64_t stream1PublishStride20Count = 0u;
  uint64_t stream1PublishStrideOtherCount = 0u;
  uint32_t stream1PublishLastRawStride = 0u;
  uint32_t stream1PublishMaxRawStride = 0u;
  uint64_t lastRenderablePart = 0u;
  uint64_t lastSceneNode = 0u;
  uint64_t lastMeshPayloadPtr = 0u;
  uint64_t lastPaletteAddress = 0u;
  uint64_t lastStream1Ptr = 0u;
  uint64_t lastCaptureSerial = 0u;
  uint32_t lastPaletteSlotIndex = 0u;
  uint32_t lastCapturedPaletteCount = 0u;
  uint32_t lastStream1Stride = 0u;
  uint32_t lastFrameTag = 0u;
  uint64_t lastVisibleFrameSerial = 0u;
  uint64_t lastRenderFrameIndex = 0u;
  uint32_t lastSmallViewportWidth = 0u;
  uint32_t lastSmallViewportHeight = 0u;
  uint32_t lastMissReason = 0u;
  // Phase 7.49：per-publish provenance probe
  // 目的：在 full trace 里把 FROZEN/NON-FROZEN 窗口里的 Publish 行为拆开。
  //   publishCallCumulative：Publish 被调的累计次数
  //   publishTrustedHitCumulative：trusted path 命中累计（与 g_paletteCaptureTrustedSourceHitCount 对齐）
  //   publishRecordFrameTagSameRunMax：Publish 时 record.frameTag 连续相同的最长 run
  //     如果在 FROZEN 段里它 ≥ FROZEN 长度 → record.frameTag 真的多帧不推进（分支 1）
  //   publishRecordFrameTagDistinctInWindowCount：本累计窗口内 record.frameTag 出现过的不同值数量
  //   publishLiveGamePaletteFrameTagLast：最近一次 Publish 时读到的 Game.dll frameTag
  //   publishLiveGamePaletteFrameTagMin/Max：累计最小/最大
  //   publishRecordFrameTagBehindLiveMaxDelta：Publish 时 `live - record.frameTag` 的最大差值
  //     如果 FROZEN 段里这个差值飙升 → writer 在推进但 record 卡住
  uint64_t publishCallCumulative = 0u;
  uint64_t publishTrustedHitCumulative = 0u;
  uint64_t publishRawFallbackCumulative = 0u;
  uint64_t publishRejectedNoTrustedCumulative = 0u;
  uint32_t publishRecordFrameTagSameRunMax = 0u;
  uint32_t publishRecordFrameTagCurrentSameRun = 0u;
  uint32_t publishRecordFrameTagLast = 0u;
  uint32_t publishLiveGamePaletteFrameTagLast = 0u;
  uint32_t publishLiveGamePaletteFrameTagMin = 0u;
  uint32_t publishLiveGamePaletteFrameTagMax = 0u;
  uint32_t publishRecordFrameTagMin = 0u;
  uint32_t publishRecordFrameTagMax = 0u;
  uint32_t publishRecordFrameTagBehindLiveMaxDelta = 0u;
  uint64_t publishRecordFrameTagEqualsLiveCount = 0u;
  uint64_t publishRecordFrameTagBehindLiveCount = 0u;
  uint64_t publishRecordFrameTagAheadLiveCount = 0u;
};

struct CurrentDrawContractSnapshotOptions {
  uint64_t minVisibleFrameSerial = 0u;
  bool readyOnly = true;
  uint32_t maxRecords = 0u;
  bool unitsOnly = false;
  bool pruneOlderThanMinVisibleFrame = true;
  std::vector<uint64_t> preferredSelectionKeys;
};

struct CurrentDrawRetireResult {
  uint64_t localContractCount = 0u;
  uint64_t localPaletteCount = 0u;
  uint64_t localPreparedSliceCount = 0u;
  uint64_t globalContractCount = 0u;
  uint64_t globalPaletteCount = 0u;
};

void ResetCurrentDrawContractCache();

CurrentDrawRetireResult RetireCurrentDrawContracts(
    const struct ShadowCasterTombstone& tombstone);

// breakdownSampleWeight is an internal profiler token selected by the
// RenderQueue_UpdateItemWorldMatrix Hook. Zero keeps the production path free
// of nested timing; non-zero is used only by the opt-in fixed-ID detail tree.
void PublishCurrentDrawContract(const CurrentDrawContractRecord& record,
                                uint32_t breakdownSampleWeight = 0u);

CurrentDrawDispatchContext PushCurrentDrawDispatchContext(
    void* sceneNode,
    void* renderablePart,
    uint32_t layerIndex,
    CurrentDrawDispatchDomain domain = CurrentDrawDispatchDomain::Common,
    bool layerKnown = true,
    void* meshPayload = nullptr);

void RestoreCurrentDrawDispatchContext(
    const CurrentDrawDispatchContext& previous);

CurrentDrawDispatchContext GetCurrentDrawDispatchContext();

void NoteCurrentDrawPreparedSliceProbe(bool contextReady,
                                       bool backingReadable);

void PublishCurrentDrawPreparedSlice(
    const CurrentDrawPreparedSliceRecord& record);

void MarkCurrentDrawPreparedSliceInterest(void* renderablePart,
                                          uint32_t layerIndex);

bool IsCurrentDrawPreparedSliceInterested(void* renderablePart,
                                          uint32_t layerIndex);

bool QueryCurrentDrawPreparedSlice(void* renderablePart,
                                   uint32_t layerIndex,
                                   CurrentDrawPreparedSliceRecord& out);

CurrentDrawContractDiagnosticsSummary
QueryCurrentDrawContractDiagnosticsSummary();

const char* DescribeCurrentDrawMissReason(uint32_t reason);

bool InstallCurrentDrawContractHook(uintptr_t gameBase, bool logEnabled);

bool QueryCurrentDrawContract(void* renderablePart,
                              CurrentDrawContractRecord& out);

// Collision-resistant, current-frame-only lookup for Stage11 geometry
// capture. This ledger is independent from the historical one-way palette
// cache and never feeds palette/snapshot consumers.
bool QueryCurrentDrawGeometryContract(
    void* renderablePart,
    void* sceneNode,
    void* worldObjectEntry,
    void* unitPtr,
    uint32_t jHandle,
    uint32_t expectedRenderFrameIndex,
    CurrentDrawContractRecord& out);

bool QueryCurrentDrawContractCapturedPalette(
    void* renderablePart,
    std::vector<Matrix4>& outPalette,
    uint32_t& outGroupCount);

bool DecodeCurrentDrawGroupSlots(const CurrentDrawContractRecord& record,
                                 uint32_t vertexCount,
                                 uint32_t paletteCount,
                                 std::vector<uint8_t>& outGroupSlots,
                                 uint64_t& outGroupHash,
                                 const CurrentDrawResolveTrace* trace = nullptr,
                                 const CurrentDrawRangeValidator* rangeValidator =
                                     nullptr);

/**
 * @brief Legacy diagnostic normalization for the raw +0x58 bind value.
 *
 * IDA shows +0x58 is bound as a separate 12-byte input stream, while +0x48
 * gates +0x4C. Do not use this helper for current-draw group-slot decoding.
 */
inline uint32_t NormalizeCurrentDrawStreamStride(uint32_t stride) {
  if (stride == 0u || stride > 64u)
    return 1u;
  return stride;
}

/**
 * @brief 计算稳定的 group content hash（不混入 stream1Ptr 地址）
 *
 * stream1Ptr 在 War3 的环形/临时 stream buffer 中可能每帧变化，
 * 将其混入 groupHash 会导致同一 caster 每帧被误判为 blend/geometry contract 变化。
 * 此函数仅 hash 实际 group slot 内容、归一化 stride、payloadWords、layerIndex，
 * 用于 geometry/source hash 稳定身份，与包含 stream1Ptr 的诊断 hash 并行保留。
 */
uint64_t ComputeStableGroupContentHash(const CurrentDrawContractRecord& record,
                                       const std::vector<uint8_t>& groupSlots);

std::vector<CurrentDrawContractRecord> SnapshotPublishedCurrentDrawContracts(
    uint64_t minVisibleFrameSerial = 0u,
    bool readyOnly = true);

std::vector<CurrentDrawContractRecord> SnapshotPublishedCurrentDrawContracts(
    const CurrentDrawContractSnapshotOptions& options);

// Caller-owned output form for render-thread hot paths. The function clears
// and rebuilds out from the same immutable snapshot policy; retaining vector
// capacity never authorizes record identity or cross-frame publication.
void SnapshotPublishedCurrentDrawContracts(
    const CurrentDrawContractSnapshotOptions& options,
    std::vector<CurrentDrawContractRecord>& out);

CurrentDrawResolveStatus ResolveCurrentDrawAuthoritativeSample(
    void* renderablePart,
    uint32_t vertexCount,
    uint64_t expectedVisibleFrameSerial,
    CurrentDrawAuthoritativeSample& out);

CurrentDrawResolveStatus ResolveCurrentDrawAuthoritativeSampleFromRecord(
    const CurrentDrawContractRecord& record,
    uint32_t vertexCount,
    uint64_t expectedVisibleFrameSerial,
    CurrentDrawAuthoritativeSample& out,
    const CurrentDrawResolveTrace* trace = nullptr,
    const CurrentDrawRangeValidator* rangeValidator = nullptr);

/**
 * @brief Phase 7.35：记录一次 submit 观察到的 palette 时间滞后。
 *
 * 在 PopulateDirectSceneShadow 的 append 点调用，衡量从 record publish
 * （capture 时的 renderFrameIndex）到当前 frame 的 lag。该函数不改变
 * 行为，只更新诊断 counter。分桶边界见 `Diagnostics::submitPaletteFrameLag*`。
 *
 * @param recordRenderFrameIndex record 被 publish 时的帧号
 * @thread_safety 仅限渲染线程 submit 阶段
 * @performance 两个 atomic relaxed 加 + 一次 CAS max，单次 submit 开销 <100ns
 */
void NoteSubmitPaletteFrameLag(uint32_t recordRenderFrameIndex);

/**
 * @brief Phase 7.39：记录一次 submit 实际 palette 内容年龄。
 *
 * contentFrameTag 来自 packet palette 元数据（producer slot 写入 frameTag 的
 * min 值），currentFrameTag 来自 Game.dll 当前 palette frameTag。该计数器
 * 用来验证“record 很旧但 palette 内容是否已经每帧刷新”。
 */
void NoteSubmitPaletteContentAge(uint32_t contentFrameTag,
                                 uint32_t currentFrameTag);
void NoteSubmitPaletteContentAgeUnknown();

/**
 * @brief Phase 7.35 路径 2：记录 submit 端 live palette rebuild 的四类事件。
 *
 * - Attempt：发现 lag >= 阈值，进入 rebuild 路径；
 * - Hit：PoseRegistry 命中，拿到 fresh palette；
 * - Miss：PoseRegistry 没有 fresh 数据，沿用原 palette；
 * - Applied：rebuild 成功且真正覆盖了 packet 用的 palette。
 *
 * Attempt vs (Lag3-5 + Lag6+) 观察覆盖完整度；
 * Hit / Attempt 观察 PoseRegistry 覆盖率（应接近 43%）；
 * Applied / Hit 观察覆盖链路完整度（应为 100%）。
 */
void NoteSubmitLiveRebuildAttempt();
void NoteSubmitLiveRebuildHit();
void NoteSubmitLiveRebuildMiss();
void NoteSubmitLiveRebuildApplied();

/**
 * @brief Phase 7.35：从 war3_model_hook 透传 capture 端 miss 分桶。
 *
 * 单独定义在 current_draw_contract 里避免 submit diagnostics 和
 * capture diagnostics 的 header 双向依赖；by_value 填入全部字段。
 */
void PublishCaptureExactQueryCounters(uint64_t exactHitCount,
                                      uint64_t bestEffortHitCount,
                                      uint64_t slotOverflowMissCount,
                                      uint64_t invalidEntryMissCount,
                                      uint64_t frameTagMismatchMissCount,
                                      uint64_t shortResultMissCount);

} // namespace dxvk::war3::render
