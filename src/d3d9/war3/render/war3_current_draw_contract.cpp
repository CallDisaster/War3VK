#include "war3_current_draw_contract.h"

#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_hook.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../hooks/war3_hook_install_util.h"
#include "../tools/war3_perf_monitor.h"
#include "war3_render_state.h"
#include "war3_render_objects.h"
#include "../state/war3_render_state.h"
#include "war3_visible_renderables.h"
#include "../../util/util_bit.h"
#include "../model/war3_model_hook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

namespace dxvk::war3::render {

namespace {

static constexpr uint32_t kMaxPaletteMatrices = 256u;
static constexpr size_t kContractCacheSize = 4096u;
static constexpr size_t kPaletteSnapshotCacheSize = 512u;
static constexpr size_t kPreparedSliceCacheSize = 1024u;
static constexpr size_t kPreparedSliceInterestCacheSize = 2048u;
using RenderQueueUpdateItemWorldMatrixFn =
    int(__fastcall *)(int sceneNodePtr, int renderablePartPtr,
                      int meshPayloadPtr);
constexpr uintptr_t kRenderQueueUpdateItemWorldMatrixRva = 0x13A510u;
RenderQueueUpdateItemWorldMatrixFn g_trampolineRenderQueueUpdateItemWorldMatrix =
    nullptr;
uintptr_t g_currentDrawContractGameBase = 0u;
std::atomic<uint64_t> g_captureSerialCounter{0u};
std::atomic<uint64_t> g_preparedSliceSerialCounter{0u};

enum class ContractLookupStatus : uint32_t {
  Hit = 0u,
  MissingRecord = 1u,
  FrameTagMismatch = 2u,
  CacheCollision = 3u,
};

enum class CurrentDrawMissReason : uint32_t {
  None = 0u,
  PublishNoRenderablePart = 1u,
  PublishNoMeshPayload = 2u,
  PublishInvalidPaletteSlot = 3u,
  PublishInvalidPaletteCount = 4u,
  PublishNoGlobalPalette = 5u,
  QueryNoRecord = 6u,
  QueryFrameTagMismatch = 7u,
  QueryCacheCollision = 8u,
  PaletteNoContract = 9u,
  PaletteInvalidCount = 10u,
  PaletteNoSnapshot = 11u,
  PaletteUnreadable = 12u,
  GroupSlotNoStream = 13u,
  GroupSlotUnreadable = 14u,
  GroupSlotOutOfRange = 15u,
  StaleVisibleFrame = 16u,
  GroupSlotStreamDisabled = 17u,
  PublishSmallViewport = 18u,
};

struct PaletteSnapshotEntry {
  void* renderablePart = nullptr;
  uint32_t frameTag = 0u;
  uint64_t captureSerial = 0u;
  uint32_t matrixCount = 0u;
  PaletteProvenance paletteProvenance = PaletteProvenance::Unknown;
  std::array<uint8_t, kMaxPaletteMatrices * 48u> bytes = {};
};

struct CurrentDrawPreparedSliceInterest {
  void* renderablePart = nullptr;
  uint32_t layerIndex = 0u;
};

thread_local std::array<CurrentDrawContractRecord, kContractCacheSize>
    g_currentDrawContractCache = {};
thread_local std::array<PaletteSnapshotEntry, kPaletteSnapshotCacheSize>
    g_paletteSnapshotCache = {};
thread_local std::array<CurrentDrawPreparedSliceRecord,
                        kPreparedSliceCacheSize>
    g_preparedSliceCache = {};
thread_local std::array<CurrentDrawPreparedSliceInterest,
                        kPreparedSliceInterestCacheSize>
    g_preparedSliceInterestCache = {};
thread_local CurrentDrawContractRecord g_globalFallbackRecord = {};
thread_local CurrentDrawDispatchContext g_currentDrawDispatchContext = {};
std::mutex g_publishedCurrentDrawMutex;
std::unordered_map<void*, CurrentDrawContractRecord> g_publishedCurrentDrawByPart;
std::unordered_map<void*, CurrentDrawContractRecord> g_publishedCurrentDrawReadyByPart;
std::unordered_map<void*, PaletteSnapshotEntry> g_publishedPaletteSnapshotByPart;
// Phase 7.30 Action B：attribution-key 专用 palette snapshot 通路。
// 跨帧稳定的 palette 归属 key = objectKey + layerIndex + payload108 +
// payload11C + capturedPaletteCount。renderablePart 每帧可能换地址，但逻辑
// slice 的 attribution key 不变，snapshot 查找以它为备用主键。
std::unordered_map<uint64_t, PaletteSnapshotEntry>
    g_publishedPaletteSnapshotByAttribution;
std::unordered_map<uint64_t, CurrentDrawContractRecord>
    g_publishedCurrentDrawReadyByAttribution;

std::atomic<uint64_t> g_publishAttemptCount{0u};
std::atomic<uint64_t> g_publishReadyCount{0u};
std::atomic<uint64_t> g_publishMissNoRenderablePart{0u};
std::atomic<uint64_t> g_publishMissNoMeshPayload{0u};
std::atomic<uint64_t> g_publishMissInvalidPaletteSlot{0u};
std::atomic<uint64_t> g_publishMissInvalidPaletteCount{0u};
std::atomic<uint64_t> g_publishMissNoGlobalPalette{0u};
std::atomic<uint64_t> g_publishSkippedNonWorldContext{0u};
std::atomic<uint64_t> g_publishSkippedSmallViewport{0u};
std::atomic<uint64_t> g_queryAttemptCount{0u};
std::atomic<uint64_t> g_queryHitCount{0u};
std::atomic<uint64_t> g_queryMissNoRecord{0u};
std::atomic<uint64_t> g_queryMissFrameTagMismatch{0u};
std::atomic<uint64_t> g_queryMissCacheCollision{0u};
std::atomic<uint64_t> g_capturedPaletteQueryAttemptCount{0u};
std::atomic<uint64_t> g_capturedPaletteQueryHitCount{0u};
std::atomic<uint64_t> g_capturedPaletteMissNoContract{0u};
std::atomic<uint64_t> g_capturedPaletteMissInvalidCount{0u};
std::atomic<uint64_t> g_capturedPaletteMissNoSnapshot{0u};
std::atomic<uint64_t> g_capturedPaletteMissUnreadablePalette{0u};
// Phase 7.30 Action B：attribution snapshot 命中次数，用来直接证明它是否
// 救回了 renderablePart churn 造成的错配。
std::atomic<uint64_t> g_paletteAttributionSnapshotHitCount{0u};
// Phase 7.30 Action B 第二刀：capture 端 trusted palette 源使用情况。
// TrustedHit = Hook_RuntimeMatrixWrite 缓存命中，作为 snapshot 的真源；
// TrustedMiss = 未命中，回退到 globalPaletteBuffer raw memcpy（老路径）。
std::atomic<uint64_t> g_paletteCaptureTrustedSourceHitCount{0u};
std::atomic<uint64_t> g_paletteCaptureTrustedSourceMissCount{0u};
// Phase 7.34 线 A：严格仲裁拒绝 raw arena 时的丢弃次数。
// 数值越高说明 trusted 覆盖率越低；应配合 `runtimeMatrixRangeCopyPalettePublishHitCount`
// 观察 authoritative palette 是否被真正发布。
std::atomic<uint64_t> g_paletteRejectedNoTrustedSourceCount{0u};
// Phase 7.34 第三轮：trusted snapshot 保留次数。
// 每次 RawGlobalArena 来袭但旧 snapshot 是 trusted 时 +1。
// 数值越高说明我们成功避免了"trusted 被 raw arena 降级覆盖"的污染，
// 是"视角边缘对象闪烁"修复的核心指标。
std::atomic<uint64_t> g_paletteSnapshotTrustedPreservedCount{0u};
// Phase 7.49：per-publish provenance probe。
// 目的：在 full trace 里把 FROZEN/NON-FROZEN 窗口里的 Publish 行为拆开。
// 通过累计 counter 的跨 trace frame delta 就能直接判断：
//   - FROZEN 段里 Call 是否继续递增（分支 3 判据：= 不动说明没被调）
//   - FROZEN 段里 TrustedHit 是否继续递增（= 是命中了同一个旧 slot entry）
//   - RecordFrameTag 是否多帧卡在同一值（= 分支 1：record 不新鲜）
//   - LiveGamePaletteFrameTag 是否一直推进（= writer 侧 frameTag 推进了）
std::atomic<uint64_t> g_publishCallCumulative{0u};
std::atomic<uint64_t> g_publishTrustedHitCumulative{0u};
std::atomic<uint64_t> g_publishRawFallbackCumulative{0u};
std::atomic<uint64_t> g_publishRejectedNoTrustedCumulative{0u};
// record.frameTag 连续相同 run 的计算 scratch + max
std::atomic<uint32_t> g_publishRecordFrameTagLast{0u};
std::atomic<uint32_t> g_publishRecordFrameTagCurrentSameRun{0u};
std::atomic<uint32_t> g_publishRecordFrameTagSameRunMax{0u};
// record.frameTag 全局 min/max
std::atomic<uint32_t> g_publishRecordFrameTagMin{0u};
std::atomic<uint32_t> g_publishRecordFrameTagMax{0u};
// Publish 时读到的 Game.dll live frameTag
std::atomic<uint32_t> g_publishLiveGamePaletteFrameTagLast{0u};
std::atomic<uint32_t> g_publishLiveGamePaletteFrameTagMin{0u};
std::atomic<uint32_t> g_publishLiveGamePaletteFrameTagMax{0u};
// record.frameTag vs live 比较
std::atomic<uint32_t> g_publishRecordFrameTagBehindLiveMaxDelta{0u};
std::atomic<uint64_t> g_publishRecordFrameTagEqualsLiveCount{0u};
std::atomic<uint64_t> g_publishRecordFrameTagBehindLiveCount{0u};
std::atomic<uint64_t> g_publishRecordFrameTagAheadLiveCount{0u};
// Phase 7.35 Pose-lag 诊断：submit 端 palette 时间滞后分桶。
// 语义：`currentFrameIndex - record.renderFrameIndex`，每次 append 一次样本。
// - Lag0 代表同帧 capture→submit（理想）；
// - Lag1 表示 record 是上一帧的（1 帧滞后，视觉几乎感知不到）；
// - Lag2 对应 Phase 7.32 captureSerial diff<=2 放宽产生的最大沿用距离；
// - Lag3-5 就是视觉 "pose 停一下追帧" 的刺眼区间；
// - Lag6+ 说明 manifest/lease 机制沿用了非常旧的 palette。
// 占比 = Lag>=1Count / TotalSamples，越低越好。
std::atomic<uint64_t> g_submitPaletteFrameLag0Count{0u};
std::atomic<uint64_t> g_submitPaletteFrameLag1Count{0u};
std::atomic<uint64_t> g_submitPaletteFrameLag2Count{0u};
std::atomic<uint64_t> g_submitPaletteFrameLag3To5Count{0u};
std::atomic<uint64_t> g_submitPaletteFrameLag6PlusCount{0u};
std::atomic<uint64_t> g_submitPaletteFrameLagMax{0u};
std::atomic<uint64_t> g_submitPaletteFrameLagSampleCount{0u};
// Phase 7.39：submit 端 palette 内容年龄（Game.dll frameTag 口径）。
std::atomic<uint64_t> g_submitPaletteContentAgeLag0Count{0u};
std::atomic<uint64_t> g_submitPaletteContentAgeLag1Count{0u};
std::atomic<uint64_t> g_submitPaletteContentAgeLag2Count{0u};
std::atomic<uint64_t> g_submitPaletteContentAgeLag3To5Count{0u};
std::atomic<uint64_t> g_submitPaletteContentAgeLag6PlusCount{0u};
std::atomic<uint64_t> g_submitPaletteContentAgeMax{0u};
std::atomic<uint64_t> g_submitPaletteContentAgeSampleCount{0u};
std::atomic<uint64_t> g_submitPaletteContentAgeUnknownCount{0u};
// Phase 7.35 路径 1 诊断：从 model_hook 透传的 Exact 查询 miss 分桶。
// 这些值每帧由 model_hook summary 组装时写入（见
// PublishCaptureExactQueryCounters 的调用点）。
std::atomic<uint64_t> g_paletteCaptureExactHitCount{0u};
std::atomic<uint64_t> g_paletteCaptureBestEffortHitCount{0u};
std::atomic<uint64_t> g_paletteCaptureSlotOverflowMissCount{0u};
std::atomic<uint64_t> g_paletteCaptureInvalidEntryMissCount{0u};
std::atomic<uint64_t> g_paletteCaptureFrameTagMismatchMissCount{0u};
std::atomic<uint64_t> g_paletteCaptureShortResultMissCount{0u};
// Phase 7.35 路径 2 诊断：submit-side live palette rebuild。
// 语义：当 submit 点发现 record.renderFrameIndex 比当前帧旧 N 帧以上，
// 尝试用 PoseRegistry 重建 fresh palette。各计数器含义：
// - Attempt：进入 rebuild 路径的次数（满足 lag 阈值）；
// - Hit：`War3TryBuildLiveRuntimeGroupPalette` 返回 true；
// - Miss：返回 false（PoseRegistry 无 fresh 数据）；
// - Applied：rebuild 成功并真正覆盖 packet palette 的次数。
// 判据：Applied / Attempt 应接近 PoseRegistry publish hit rate（当前约 43%）。
std::atomic<uint64_t> g_submitLiveRebuildAttemptCount{0u};
std::atomic<uint64_t> g_submitLiveRebuildHitCount{0u};
std::atomic<uint64_t> g_submitLiveRebuildMissCount{0u};
std::atomic<uint64_t> g_submitLiveRebuildAppliedCount{0u};
std::atomic<uint64_t> g_groupSlotDecodeAttemptCount{0u};
std::atomic<uint64_t> g_groupSlotDecodeHitCount{0u};
std::atomic<uint64_t> g_groupSlotDecodeMissDisabledStream{0u};
std::atomic<uint64_t> g_groupSlotDecodeMissNoStream{0u};
std::atomic<uint64_t> g_groupSlotDecodeMissUnreadableStream{0u};
std::atomic<uint64_t> g_groupSlotDecodeMissGroupOutOfRange{0u};
std::atomic<uint64_t> g_preparedSliceProbeAttemptCount{0u};
std::atomic<uint64_t> g_preparedSliceProbeContextReadyCount{0u};
std::atomic<uint64_t> g_preparedSliceProbeBackingReadableCount{0u};
std::atomic<uint64_t> g_preparedSliceRecordedCount{0u};
std::atomic<uint64_t> g_preparedSliceQueryAttemptCount{0u};
std::atomic<uint64_t> g_preparedSliceQueryHitCount{0u};
std::atomic<uint64_t> g_preparedSliceQueryMissCount{0u};
std::atomic<uint64_t> g_stream1PublishNoStreamCount{0u};
std::atomic<uint64_t> g_stream1PublishStride0Count{0u};
std::atomic<uint64_t> g_stream1PublishStride1Count{0u};
std::atomic<uint64_t> g_stream1PublishStride8Count{0u};
std::atomic<uint64_t> g_stream1PublishStride12Count{0u};
std::atomic<uint64_t> g_stream1PublishStride16Count{0u};
std::atomic<uint64_t> g_stream1PublishStride20Count{0u};
std::atomic<uint64_t> g_stream1PublishStrideOtherCount{0u};
std::atomic<uint32_t> g_stream1PublishLastRawStride{0u};
std::atomic<uint32_t> g_stream1PublishMaxRawStride{0u};
std::atomic<uint64_t> g_lastRenderablePart{0u};
std::atomic<uint64_t> g_lastSceneNode{0u};
std::atomic<uint64_t> g_lastMeshPayloadPtr{0u};
std::atomic<uint64_t> g_lastPaletteAddress{0u};
std::atomic<uint64_t> g_lastStream1Ptr{0u};
std::atomic<uint64_t> g_lastCaptureSerial{0u};
std::atomic<uint32_t> g_lastPaletteSlotIndex{0u};
std::atomic<uint32_t> g_lastCapturedPaletteCount{0u};
std::atomic<uint32_t> g_lastStream1Stride{0u};
std::atomic<uint32_t> g_lastFrameTag{0u};
std::atomic<uint64_t> g_lastVisibleFrameSerial{0u};
std::atomic<uint64_t> g_lastRenderFrameIndex{0u};
std::atomic<uint32_t> g_lastSmallViewportWidth{0u};
std::atomic<uint32_t> g_lastSmallViewportHeight{0u};
std::atomic<uint32_t> g_lastMissReason{0u};

uint32_t ReadEnvU32(const char* name, uint32_t fallback) {
  char buffer[32] = {};
  const DWORD len = ::GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (len == 0u || len >= sizeof(buffer))
    return fallback;
  char* end = nullptr;
  const unsigned long value = std::strtoul(buffer, &end, 0);
  return end != buffer ? static_cast<uint32_t>(value) : fallback;
}

bool PreserveTrustedPaletteOnRawMissEnabled() {
  static const bool s_enabled =
      ReadEnvU32("DXVK_WAR3_PRESERVE_TRUSTED_PALETTE_ON_RAW_MISS", 0u) != 0u;
  return s_enabled;
}

// Phase 7.30 Action B：按"逻辑 slice 身份"计算稳定的 palette attribution key。
// 该 key 跨帧稳定（不依赖 renderablePart 地址），可作为 snapshot 查找的主键。
// 组成：objectKey（Manifest object key）+ layerIndex + payload108 +
// payload11C + capturedPaletteCount。前两者定 slice，后三者共同排除
// cull/variant/结构性差异，避免错读到同 object 另一 slice 的 palette。
uint64_t ComputePaletteAttributionKey(const CurrentDrawContractRecord& r) {
  // objectKey 与 VisibleRenderableRegistry::computeShadowManifestObjectKey
  // 保持同一规则：jHandle 优于 unitPtr 优于 worldObjectEntry。
  uint64_t objectKey = 0u;
  auto mix = [](uint32_t tag, uint64_t value) {
    uint64_t h = bit::fnv1a_init();
    h = bit::fnv1a_iter(h, tag);
    h = bit::fnv1a_iter(h, value);
    return h;
  };
  if (r.jHandle != 0u) {
    objectKey = mix(0x72110002u, uint64_t(r.jHandle));
  } else if (r.unitPtr != nullptr) {
    objectKey = mix(0x72110001u, uint64_t(reinterpret_cast<uintptr_t>(r.unitPtr)));
  } else if (r.worldObjectEntry != nullptr) {
    objectKey = mix(0x72110004u,
                    uint64_t(reinterpret_cast<uintptr_t>(r.worldObjectEntry)));
  } else if (r.sceneNode != nullptr) {
    objectKey = mix(0x72110005u,
                    uint64_t(reinterpret_cast<uintptr_t>(r.sceneNode)));
  } else {
    return 0u;
  }

  uint64_t h = bit::fnv1a_init();
  h = bit::fnv1a_iter(h, 0x72120000u);
  h = bit::fnv1a_iter(h, objectKey);
  h = bit::fnv1a_iter(h, r.layerIndex);
  h = bit::fnv1a_iter(h, r.payloadWord108);
  h = bit::fnv1a_iter(h, r.payloadWord11C);
  h = bit::fnv1a_iter(h, r.capturedPaletteCount);
  return h;
}

bool PaletteAttributionSnapshotEnabled() {
  // Phase 7.30 Action B：默认开启 attribution-key snapshot 通路；提供 env
  // 开关便于回归。
  static const bool enabled =
      ReadEnvU32("DXVK_WAR3_SEMANTIC_PALETTE_ATTRIBUTION_SNAPSHOT", 1u) != 0u;
  return enabled;
}

bool GlobalCurrentDrawPublishEnabled() {
  static const bool enabled =
      ReadEnvU32("DXVK_WAR3_CURRENT_DRAW_GLOBAL_PUBLISH", 0u) != 0u;
  return enabled;
}

bool PublishCurrentDrawContractAfterOriginalRuntime() {
  // RenderQueue_UpdateItemWorldMatrix (0x6F13A510) is the authoritative
  // palette-slot consumer/update point. Capture after the original by default
  // so the owned palette snapshot reflects the draw War3 just bound.
  static const bool enabled =
      ReadEnvU32("DXVK_WAR3_CURRENT_DRAW_PUBLISH_AFTER_ORIGINAL", 1u) != 0u;
  return enabled;
}

bool KeepReadySnapshotOnInvalidCurrentDrawEnabled() {
  // Some native draw paths for the same renderablePart can publish a transient
  // non-authoritative palette tuple. Dropping the previous ready snapshot there
  // turns a one-frame producer miss into an empty semantic replay frame; stale
  // records are still bounded by visible-frame pruning during snapshot.
  static const bool enabled =
      ReadEnvU32("DXVK_WAR3_SEMANTIC_KEEP_READY_ON_INVALID_CURRENT_DRAW", 1u) !=
      0u;
  return enabled;
}

bool RequireMainWorldObjectStageForPublish() {
  static const bool enabled =
      ReadEnvU32("DXVK_WAR3_CURRENT_DRAW_REQUIRE_MAIN_WORLD_STAGE", 0u) != 0u;
  return enabled;
}

bool RejectSmallViewportForPublish() {
  static const bool enabled =
      ReadEnvU32("DXVK_WAR3_CURRENT_DRAW_REJECT_SMALL_VIEWPORT", 1u) != 0u;
  return enabled;
}

uint32_t SmallViewportPublishMinSize() {
  static const uint32_t minSize =
      std::max<uint32_t>(1u, ReadEnvU32("DXVK_WAR3_CURRENT_DRAW_SMALL_VIEWPORT_MIN", 300u));
  return minSize;
}

bool IsMainWorldObjectStageForCurrentDrawPublish() {
  if (!dxvk::War3RenderState::IsMainWorldStageActive())
    return false;

  const int stage = dxvk::War3RenderState::GetStage();
  return stage == 11 || stage == 12 || stage == 13;
}

bool IsWorldCurrentDrawPublishContext() {
  if (dxvk::War3RenderState::CurrentLayer() == dxvk::War3RenderLayer::UI ||
      dxvk::War3RenderState::IsUiPhase()) {
    return false;
  }

  if (RejectSmallViewportForPublish()) {
    const auto viewport = dxvk::War3RenderState::GetCurrentViewportSnapshot();
    const uint32_t minSize = SmallViewportPublishMinSize();
    if (viewport.valid &&
        (viewport.width < minSize || viewport.height < minSize)) {
      g_publishSkippedSmallViewport.fetch_add(1u, std::memory_order_relaxed);
      g_lastSmallViewportWidth.store(viewport.width, std::memory_order_relaxed);
      g_lastSmallViewportHeight.store(viewport.height, std::memory_order_relaxed);
      g_lastMissReason.store(uint32_t(CurrentDrawMissReason::PublishSmallViewport),
                             std::memory_order_relaxed);
      return false;
    }
  }

  if (RequireMainWorldObjectStageForPublish() &&
      !IsMainWorldObjectStageForCurrentDrawPublish()) {
    return false;
  }

  const auto& semantic = dxvk::War3RenderState::GetTlsShadowSemanticState();
  if (semantic.tag == dxvk::War3BatchTag::WorldObjects)
    return true;

  const dxvk::War3BatchTag tag =
      dxvk::War3RenderState::GetCurrentBatchTag();
  if (tag == dxvk::War3BatchTag::WorldObjects)
    return true;

  return dxvk::War3RenderState::IsWorldObjectPhase();
}

bool SafePaletteRangeCheckEnabled() {
  static const bool enabled =
      ReadEnvU32("DXVK_WAR3_CURRENT_DRAW_SAFE_PALETTE_READ", 0u) != 0u;
  return enabled;
}

bool LastSampleDiagnosticsEnabled() {
  static const bool enabled =
      ReadEnvU32("DXVK_WAR3_CURRENT_DRAW_LAST_SAMPLE", 0u) != 0u;
  return enabled;
}

// Phase 7.77：Phase 7.49 用来诊断 Pose stutter / FROZEN 段的 publish-side
// probe 在 PublishCurrentDrawContract 主路径里**始终运行**，每次 publish 至少
// 5 次 atomic CAS-loop + frameTag 内存读，每帧 10K-30K publish 调用累计 1-3
// ms 纯诊断开销。Phase 7.55 v4 + Phase 7.57 producer 已闭环 stutter 根因，
// 所以默认关掉这一组诊断，只有 `DXVK_WAR3_PUBLISH_PROBE=1` 显式开启时才执行。
bool Phase749PublishProbeEnabled() {
  static const bool enabled =
      ReadEnvU32("DXVK_WAR3_PUBLISH_PROBE", 0u) != 0u;
  return enabled;
}

void NotePublishedStream1Layout(const CurrentDrawContractRecord& record) {
  if (record.stream1Ptr == nullptr) {
    g_stream1PublishNoStreamCount.fetch_add(1u, std::memory_order_relaxed);
    return;
  }

  g_stream1PublishLastRawStride.store(record.stream1Stride,
                                      std::memory_order_relaxed);
  uint32_t previousMax =
      g_stream1PublishMaxRawStride.load(std::memory_order_relaxed);
  while (record.stream1Stride > previousMax &&
         !g_stream1PublishMaxRawStride.compare_exchange_weak(
             previousMax, record.stream1Stride, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }

  switch (record.stream1Stride) {
  case 0u:
    g_stream1PublishStride0Count.fetch_add(1u, std::memory_order_relaxed);
    break;
  case 1u:
    g_stream1PublishStride1Count.fetch_add(1u, std::memory_order_relaxed);
    break;
  case 8u:
    g_stream1PublishStride8Count.fetch_add(1u, std::memory_order_relaxed);
    break;
  case 12u:
    g_stream1PublishStride12Count.fetch_add(1u, std::memory_order_relaxed);
    break;
  case 16u:
    g_stream1PublishStride16Count.fetch_add(1u, std::memory_order_relaxed);
    break;
  case 20u:
    g_stream1PublishStride20Count.fetch_add(1u, std::memory_order_relaxed);
    break;
  default:
    g_stream1PublishStrideOtherCount.fetch_add(1u, std::memory_order_relaxed);
    break;
  }
}

bool RecordHasReadyShape(const CurrentDrawContractRecord& record) {
  return record.renderablePart != nullptr && record.meshPayloadPtr != nullptr &&
         record.paletteSlotIndex != 0xFFFFFFFFu &&
         record.paletteSlotIndex < 0x3A98u && record.paletteAddress != nullptr &&
         record.capturedPaletteCount != 0u &&
         record.capturedPaletteCount <= kMaxPaletteMatrices;
}

bool RecordHasLocalPaletteSnapshot(const CurrentDrawContractRecord& record) {
  if (!RecordHasReadyShape(record))
    return false;
  const size_t snapshotSlot =
      (uintptr_t(record.renderablePart) >> 4u) % kPaletteSnapshotCacheSize;
  const auto& snapshot = g_paletteSnapshotCache[snapshotSlot];
  if (snapshot.renderablePart != record.renderablePart)
    return false;
  // Phase 7.32: 放宽 captureSerial 匹配条件。
  // 原先要求 snapshot.captureSerial == record.captureSerial，但在高频渲染中
  // record 可能在本帧被 PublishCurrentDrawContract 更新了 captureSerial，
  // 而 palette snapshot cache 还保留着上一次 capture 的 serial。
  // 这种相位差导致所有 record 被 readyOnly 过滤掉，populate 返回空帧。
  // 修复：允许 snapshot serial 比 record serial 旧最多 2 个版本。
  const bool captureSerialMatch =
      snapshot.captureSerial == record.captureSerial ||
      (record.captureSerial != 0u && snapshot.captureSerial != 0u &&
       record.captureSerial > snapshot.captureSerial &&
       record.captureSerial - snapshot.captureSerial <= 2u);
  if (!captureSerialMatch)
    return false;
  // Phase 7.31 P0: 放宽 matrixCount 匹配（capturedPaletteCount 来自整模型，
  // snapshot.matrixCount 来自单 geoset groupCount，两者天然不同）。
  if (snapshot.matrixCount == 0u)
    return false;
  if (!(record.frameTag == 0u || snapshot.frameTag == 0u ||
        snapshot.frameTag == record.frameTag ||
        (record.frameTag > snapshot.frameTag &&
         record.frameTag - snapshot.frameTag <= 1u)))
    return false;
  return true;
}

bool TryReadCurrentPaletteFrameTag(uint32_t& outFrameTag) {
  outFrameTag = 0u;
  uintptr_t gameDllBase = g_currentDrawContractGameBase;
  if (gameDllBase == 0u)
    gameDllBase = reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
  if (gameDllBase == 0u)
    return false;

  return dxvk::war3::SafeReadU32Fast(
      reinterpret_cast<const void*>(gameDllBase + 0xBDA4CCu), 0u, outFrameTag);
}

uint32_t CachedCurrentPaletteFrameTag(uint32_t renderFrameIndex) {
  thread_local uint32_t cachedRenderFrameIndex = 0xFFFFFFFFu;
  thread_local uint32_t cachedFrameTag = 0u;
  if (cachedRenderFrameIndex == renderFrameIndex)
    return cachedFrameTag;
  uint32_t frameTag = 0u;
  TryReadCurrentPaletteFrameTag(frameTag);
  cachedRenderFrameIndex = renderFrameIndex;
  cachedFrameTag = frameTag;
  return cachedFrameTag;
}

ContractLookupStatus LookupCurrentDrawContractRecord(
    void* renderablePart, const CurrentDrawContractRecord*& out) {
  out = nullptr;
  if (renderablePart == nullptr)
    return ContractLookupStatus::MissingRecord;

  const size_t slot =
      (uintptr_t(renderablePart) >> 4u) % kContractCacheSize;
  const CurrentDrawContractRecord& entry = g_currentDrawContractCache[slot];
  if (entry.renderablePart != renderablePart) {
    std::lock_guard<std::mutex> lock(g_publishedCurrentDrawMutex);
    const auto globalIt = g_publishedCurrentDrawByPart.find(renderablePart);
    if (globalIt == g_publishedCurrentDrawByPart.end()) {
      if (entry.renderablePart != nullptr)
        return ContractLookupStatus::CacheCollision;
      return ContractLookupStatus::MissingRecord;
    }
    g_globalFallbackRecord = globalIt->second;
    out = &g_globalFallbackRecord;
    return ContractLookupStatus::Hit;
  }

  uint32_t currentFrameTag = 0u;
  const bool hasCurrentFrameTag = TryReadCurrentPaletteFrameTag(currentFrameTag);
  if (hasCurrentFrameTag && entry.frameTag != 0u &&
      entry.frameTag != currentFrameTag) {
    return ContractLookupStatus::FrameTagMismatch;
  }

  out = &entry;
  return ContractLookupStatus::Hit;
}

uint32_t SnapshotPriorityOf(const CurrentDrawContractRecord& record,
                            bool unitsOnly) {
  if (unitsOnly) {
    if (record.objectKind == ObjectKind::Unit)
      return 4u;
    if (record.objectKind == ObjectKind::Building ||
        record.objectKind == ObjectKind::Destructible) {
      return 0u;
    }
    if (record.unitPtr != nullptr)
      return 3u;
    if (record.jHandle != 0u || record.rawcode != 0u)
      return 2u;
    const bool hasSkinnedCurrentDrawShape =
        record.sceneNode != nullptr && record.meshPayloadPtr != nullptr &&
        record.capturedPaletteCount != 0u &&
        (record.payloadWord104 != 0u ||
         (record.payloadWord48 != 0u && record.stream1Ptr != nullptr));
    if (hasSkinnedCurrentDrawShape)
      return 1u;
    return 0u;
  }

  if (record.objectKind == ObjectKind::Unit)
    return 4u;
  if (record.objectKind == ObjectKind::Building ||
      record.objectKind == ObjectKind::Destructible)
    return 3u;
  if (record.unitPtr != nullptr || record.worldObjectEntry != nullptr ||
      record.sceneNode != nullptr)
    return 2u;
  if (record.jHandle != 0u || record.rawcode != 0u)
    return 1u;
  return 0u;
}

uint64_t SnapshotStableIdentityKey(const CurrentDrawContractRecord& record) {
  auto ptrValue = [](const void* ptr) -> uint64_t {
    return uint64_t(reinterpret_cast<uintptr_t>(ptr));
  };
  auto makeKey = [](uint32_t tag, uint64_t value) -> uint64_t {
    uint64_t hash = bit::fnv1a_init();
    hash = bit::fnv1a_iter(hash, tag);
    hash = bit::fnv1a_iter(hash, value);
    return hash;
  };

  if (record.unitPtr != nullptr)
    return makeKey(1u, ptrValue(record.unitPtr));
  if (record.jHandle != 0u)
    return makeKey(2u, record.jHandle);
  if (record.worldObjectEntry != nullptr)
    return makeKey(4u, ptrValue(record.worldObjectEntry));
  if (record.sceneNode != nullptr)
    return makeKey(5u, ptrValue(record.sceneNode));
  if (record.meshPayloadPtr != nullptr) {
    uint64_t hash = bit::fnv1a_init();
    hash = bit::fnv1a_iter(hash, 6u);
    hash = bit::fnv1a_iter(hash, ptrValue(record.meshPayloadPtr));
    hash = bit::fnv1a_iter(hash, record.payloadWord108);
    hash = bit::fnv1a_iter(hash, record.payloadWord11C);
    return hash;
  }
  if (record.renderablePart != nullptr)
    return makeKey(7u, ptrValue(record.renderablePart));
  return 0u;
}

uint64_t SnapshotStablePartKey(const CurrentDrawContractRecord& record) {
  uint64_t hash = bit::fnv1a_init();
  auto mixPtr = [&](uint32_t tag, const void* ptr) {
    hash = bit::fnv1a_iter(hash, tag);
    hash = bit::fnv1a_iter(
        hash, uint64_t(reinterpret_cast<uintptr_t>(ptr)));
  };

  if (record.unitPtr != nullptr)
    mixPtr(1u, record.unitPtr);
  else if (record.jHandle != 0u) {
    hash = bit::fnv1a_iter(hash, 2u);
    hash = bit::fnv1a_iter(hash, record.jHandle);
  } else if (record.worldObjectEntry != nullptr)
    mixPtr(4u, record.worldObjectEntry);
  else if (record.sceneNode != nullptr)
    mixPtr(5u, record.sceneNode);
  else if (record.renderablePart != nullptr)
    mixPtr(7u, record.renderablePart);
  else if (record.meshPayloadPtr != nullptr)
    mixPtr(8u, record.meshPayloadPtr);

  hash = bit::fnv1a_iter(hash, record.layerIndex);
  hash = bit::fnv1a_iter(hash, record.payloadWord108);
  hash = bit::fnv1a_iter(hash, record.payloadWord11C);
  return hash;
}

bool BetterSnapshotRecord(const CurrentDrawContractRecord& a,
                          const CurrentDrawContractRecord& b,
                          bool unitsOnly) {
  const uint32_t pa = SnapshotPriorityOf(a, unitsOnly);
  const uint32_t pb = SnapshotPriorityOf(b, unitsOnly);
  if (pa != pb)
    return pa > pb;
  const uint64_t ia = SnapshotStableIdentityKey(a);
  const uint64_t ib = SnapshotStableIdentityKey(b);
  if (ia != ib)
    return ia < ib;
  const uint64_t partA = SnapshotStablePartKey(a);
  const uint64_t partB = SnapshotStablePartKey(b);
  if (partA != partB)
    return partA < partB;
  if (a.visibleFrameSerial != b.visibleFrameSerial)
    return a.visibleFrameSerial > b.visibleFrameSerial;
  if (a.renderFrameIndex != b.renderFrameIndex)
    return a.renderFrameIndex > b.renderFrameIndex;
  return a.captureSerial > b.captureSerial;
}

bool SnapshotRecordVisibleEnough(const CurrentDrawContractRecord& record,
                                 uint64_t minVisibleFrameSerial) {
  if (record.renderablePart == nullptr)
    return false;
  if (minVisibleFrameSerial == 0u || record.visibleFrameSerial == 0u)
    return true;
  return record.visibleFrameSerial >= minVisibleFrameSerial;
}

void PrunePublishedCurrentDrawContractsLocked(uint64_t minVisibleFrameSerial) {
  if (minVisibleFrameSerial == 0u)
    return;

  for (auto it = g_publishedCurrentDrawByPart.begin();
       it != g_publishedCurrentDrawByPart.end();) {
    if (it->second.visibleFrameSerial != 0u &&
        it->second.visibleFrameSerial < minVisibleFrameSerial) {
      it = g_publishedCurrentDrawByPart.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_publishedCurrentDrawReadyByPart.begin();
       it != g_publishedCurrentDrawReadyByPart.end();) {
    if (it->second.visibleFrameSerial != 0u &&
        it->second.visibleFrameSerial < minVisibleFrameSerial) {
      g_publishedPaletteSnapshotByPart.erase(it->first);
      it = g_publishedCurrentDrawReadyByPart.erase(it);
    } else {
      ++it;
    }
  }
  // Phase 7.30 Action B：attribution 表与 ByPart 表遵循同一 visibleFrameSerial
  // 过期规则，防止跨帧条目无限堆积。key 是 uint64，不是指针，但用 record 内
  // 自己携带的 visibleFrameSerial 判断。
  for (auto it = g_publishedCurrentDrawReadyByAttribution.begin();
       it != g_publishedCurrentDrawReadyByAttribution.end();) {
    if (it->second.visibleFrameSerial != 0u &&
        it->second.visibleFrameSerial < minVisibleFrameSerial) {
      g_publishedPaletteSnapshotByAttribution.erase(it->first);
      it = g_publishedCurrentDrawReadyByAttribution.erase(it);
    } else {
      ++it;
    }
  }
}

void ClearPublishedCurrentDrawReadyRecord(void* renderablePart) {
  if (renderablePart == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_publishedCurrentDrawMutex);
  auto readyIt = g_publishedCurrentDrawReadyByPart.find(renderablePart);
  uint64_t attrKey = 0u;
  if (readyIt != g_publishedCurrentDrawReadyByPart.end()) {
    // 在移除 byPart 条目前，先算出它对应的 attribution key，以便同步清理
    // attribution 表，避免"byPart 清了但 attribution 还挂着旧 record"。
    attrKey = ComputePaletteAttributionKey(readyIt->second);
  }
  g_publishedCurrentDrawReadyByPart.erase(renderablePart);
  g_publishedPaletteSnapshotByPart.erase(renderablePart);
  if (attrKey != 0u) {
    g_publishedCurrentDrawReadyByAttribution.erase(attrKey);
    g_publishedPaletteSnapshotByAttribution.erase(attrKey);
  }
}

bool DecodeCapturedPaletteForRecord(const CurrentDrawContractRecord& record,
                                    std::vector<Matrix4>& outPalette,
                                    uint32_t& outGroupCount,
                                    bool countAttempt) {
  outPalette.clear();
  outGroupCount = 0u;
  if (countAttempt)
    g_capturedPaletteQueryAttemptCount.fetch_add(1u,
                                                 std::memory_order_relaxed);

  if (record.renderablePart == nullptr || !record.known) {
    g_capturedPaletteMissNoContract.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(uint32_t(CurrentDrawMissReason::PaletteNoContract),
                           std::memory_order_relaxed);
    return false;
  }

  if (record.capturedPaletteCount == 0u ||
      record.capturedPaletteCount > kMaxPaletteMatrices) {
    g_capturedPaletteMissInvalidCount.fetch_add(1u,
                                                std::memory_order_relaxed);
    g_lastMissReason.store(uint32_t(CurrentDrawMissReason::PaletteInvalidCount),
                           std::memory_order_relaxed);
    return false;
  }

  outPalette.resize(record.capturedPaletteCount);
  const auto decodePaletteBytes = [&](const uint8_t* paletteBytes) {
    for (uint32_t i = 0u; i < record.capturedPaletteCount; ++i) {
      const float* m =
          reinterpret_cast<const float*>(paletteBytes + size_t(i) * 48u);
      outPalette[i] =
          Matrix4(Vector4(m[0], m[1], m[2], 0.0f),
                  Vector4(m[3], m[4], m[5], 0.0f),
                  Vector4(m[6], m[7], m[8], 0.0f),
                  Vector4(m[9], m[10], m[11], 1.0f));
    }
  };

  const size_t snapshotSlot =
      (uintptr_t(record.renderablePart) >> 4u) % kPaletteSnapshotCacheSize;
  const auto& snapshot = g_paletteSnapshotCache[snapshotSlot];
  if (snapshot.renderablePart == record.renderablePart &&
      snapshot.captureSerial == record.captureSerial &&
      snapshot.matrixCount == record.capturedPaletteCount &&
      (record.frameTag == 0u || snapshot.frameTag == 0u ||
       snapshot.frameTag == record.frameTag)) {
    decodePaletteBytes(snapshot.bytes.data());
    outGroupCount = record.capturedPaletteCount;
    g_capturedPaletteQueryHitCount.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(uint32_t(CurrentDrawMissReason::None),
                           std::memory_order_relaxed);
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(g_publishedCurrentDrawMutex);
    const auto globalIt =
        g_publishedPaletteSnapshotByPart.find(record.renderablePart);
    if (globalIt != g_publishedPaletteSnapshotByPart.end()) {
      const auto& globalSnapshot = globalIt->second;
      if (globalSnapshot.captureSerial == record.captureSerial &&
          globalSnapshot.matrixCount == record.capturedPaletteCount &&
          (record.frameTag == 0u || globalSnapshot.frameTag == 0u ||
           globalSnapshot.frameTag == record.frameTag)) {
        decodePaletteBytes(globalSnapshot.bytes.data());
        outGroupCount = record.capturedPaletteCount;
        g_capturedPaletteQueryHitCount.fetch_add(1u,
                                                 std::memory_order_relaxed);
        g_lastMissReason.store(uint32_t(CurrentDrawMissReason::None),
                               std::memory_order_relaxed);
        return true;
      }
    }
    // Phase 7.30 Action B：renderablePart 指针 miss/mismatch 时，按
    // attribution key 找上一帧同 slice 的 snapshot。这条分支只在前两级
    // （per-thread cache、global by-part）都匹配不上时才走；命中时依旧校验
    // captureSerial/matrixCount/frameTag，所以不会把旧帧的 palette 套到
    // 当前帧。目标是救那种"逻辑 slice 不变但 renderablePart 指针变了"
    // 导致的归属丢失；而不是延长 palette 生命周期。
    if (PaletteAttributionSnapshotEnabled()) {
      const uint64_t attrKey = ComputePaletteAttributionKey(record);
      if (attrKey != 0u) {
        const auto attrIt =
            g_publishedPaletteSnapshotByAttribution.find(attrKey);
        if (attrIt != g_publishedPaletteSnapshotByAttribution.end()) {
          const auto& attrSnapshot = attrIt->second;
          if (attrSnapshot.captureSerial == record.captureSerial &&
              attrSnapshot.matrixCount == record.capturedPaletteCount &&
              (record.frameTag == 0u || attrSnapshot.frameTag == 0u ||
               attrSnapshot.frameTag == record.frameTag)) {
            decodePaletteBytes(attrSnapshot.bytes.data());
            outGroupCount = record.capturedPaletteCount;
            g_capturedPaletteQueryHitCount.fetch_add(
                1u, std::memory_order_relaxed);
            g_paletteAttributionSnapshotHitCount.fetch_add(
                1u, std::memory_order_relaxed);
            g_lastMissReason.store(uint32_t(CurrentDrawMissReason::None),
                                   std::memory_order_relaxed);
            return true;
          }
        }
      }
    }
  }

  outPalette.clear();
  g_capturedPaletteMissNoSnapshot.fetch_add(1u, std::memory_order_relaxed);
  g_lastMissReason.store(uint32_t(CurrentDrawMissReason::PaletteNoSnapshot),
                         std::memory_order_relaxed);
  return false;
}

} // namespace

int __fastcall Hook_RenderQueueUpdateItemWorldMatrix(int sceneNodePtr,
                                                     int renderablePartPtr,
                                                     int meshPayloadPtr) {
  if (!g_trampolineRenderQueueUpdateItemWorldMatrix)
    return 0;

  // Phase 7.89：退出地图后 producer 降级为纯透传。
  if (!dxvk::g_war3_runtime_activated.load(std::memory_order_relaxed)) {
    return g_trampolineRenderQueueUpdateItemWorldMatrix(
        sceneNodePtr, renderablePartPtr, meshPayloadPtr);
  }

  // Phase 7.94：诊断用 — 完全禁用 publish contract hook 的数据层工作。
  // 用于隔离"是 publish hook 导致卡顿还是其他 hook"。
  static const bool s_disablePublish =
      ReadEnvU32("DXVK_WAR3_DISABLE_PUBLISH_CONTRACT", 0u) != 0u;
  if (s_disablePublish) {
    return g_trampolineRenderQueueUpdateItemWorldMatrix(
        sceneNodePtr, renderablePartPtr, meshPayloadPtr);
  }

  const bool allowWorldPublish = IsWorldCurrentDrawPublishContext();

  CurrentDrawContractRecord entry = {};
  entry.sceneNode = reinterpret_cast<void*>(uintptr_t(uint32_t(sceneNodePtr)));
  entry.renderablePart =
      reinterpret_cast<void*>(uintptr_t(uint32_t(renderablePartPtr)));
  entry.meshPayloadPtr =
      reinterpret_cast<void*>(uintptr_t(uint32_t(meshPayloadPtr)));
  const auto& tlsSemantic = dxvk::War3RenderState::GetTlsShadowSemanticState();
  entry.worldObjectEntry = tlsSemantic.worldObjectEntry;
  entry.jHandle = tlsSemantic.jHandle;
  entry.rawcode = tlsSemantic.rawcode;
  entry.objectKind = tlsSemantic.objectKind;
  if (tlsSemantic.object != nullptr)
    entry.unitPtr = tlsSemantic.object->unitPtr;
  const auto dispatchContext = g_currentDrawDispatchContext;
  if (dispatchContext.valid &&
      (dispatchContext.renderablePart == nullptr ||
       dispatchContext.renderablePart == entry.renderablePart)) {
    entry.layerIndex = dispatchContext.layerIndex;
  }
  const bool needsVisibleIdentityBackfill =
      entry.worldObjectEntry == nullptr || entry.unitPtr == nullptr ||
      entry.jHandle == 0u || entry.rawcode == 0u ||
      entry.objectKind == ObjectKind::Unknown ||
      entry.sceneNode == nullptr || entry.meshPayloadPtr == nullptr;
  if (needsVisibleIdentityBackfill && entry.renderablePart != nullptr) {
    VisibleRenderableRecord visible = {};
    if (VisibleRenderableRegistry::instance().queryByRenderablePartAndLayer(
            entry.renderablePart, entry.layerIndex, visible)) {
      if (entry.worldObjectEntry == nullptr)
        entry.worldObjectEntry = visible.identity.worldObjectEntry;
      if (entry.unitPtr == nullptr)
        entry.unitPtr = visible.identity.unitPtr;
      if (entry.jHandle == 0u)
        entry.jHandle = visible.identity.jHandle != 0u
                            ? visible.identity.jHandle
                            : visible.identity.handleId;
      if (entry.rawcode == 0u)
        entry.rawcode = visible.identity.rawcode;
      if (entry.objectKind == ObjectKind::Unknown &&
          visible.identity.kind != ObjectKind::Unknown)
        entry.objectKind = visible.identity.kind;
      if (entry.sceneNode == nullptr)
        entry.sceneNode = visible.sceneNode;
      if (entry.meshPayloadPtr == nullptr)
        entry.meshPayloadPtr = visible.meshData;
    }
  }
  entry.visibleFrameSerial = VisibleRenderableRegistry::instance().getFrameNumber();
  entry.renderFrameIndex = dxvk::war3::state::RenderState::instance().getFrameIndex();
  entry.frameTag = CachedCurrentPaletteFrameTag(entry.renderFrameIndex);
  entry.captureSerial =
      g_captureSerialCounter.fetch_add(1u, std::memory_order_relaxed) + 1u;

  auto refreshDrawBindFieldsAndPublish = [&]() {
    if (!allowWorldPublish) {
      g_publishSkippedNonWorldContext.fetch_add(1u,
                                                std::memory_order_relaxed);
      return;
    }

    entry.paletteSlotIndex = 0xFFFFFFFFu;
    entry.paletteAddress = nullptr;
    entry.capturedPaletteCount = 0u;

    if (entry.renderablePart != nullptr) {
      entry.paletteSlotIndex =
          *reinterpret_cast<uint32_t*>(uintptr_t(uint32_t(renderablePartPtr)) +
                                       0x08u);
    }

    if (entry.meshPayloadPtr != nullptr) {
      const uintptr_t payload = uintptr_t(uint32_t(meshPayloadPtr));
      entry.payloadWordF0 = *reinterpret_cast<uint32_t*>(payload + 0xF0u);
      entry.payloadWord104 = *reinterpret_cast<uint32_t*>(payload + 0x104u);
      entry.payloadWord108 = *reinterpret_cast<uint32_t*>(payload + 0x108u);
      entry.payloadWord11C = *reinterpret_cast<uint32_t*>(payload + 0x11Cu);
      entry.payloadWord48 = *reinterpret_cast<uint32_t*>(payload + 0x48u);
      entry.stream1Ptr = *reinterpret_cast<void**>(payload + 0x4Cu);
      entry.stream1Stride = *reinterpret_cast<uint32_t*>(payload + 0x58u);
      NotePublishedStream1Layout(entry);
    }

    if (entry.renderablePart != nullptr && entry.meshPayloadPtr != nullptr &&
        entry.paletteSlotIndex != 0xFFFFFFFFu &&
        entry.paletteSlotIndex < 0x3A98u && entry.payloadWordF0 != 0u &&
        entry.payloadWordF0 <= kMaxPaletteMatrices &&
        entry.paletteSlotIndex + entry.payloadWordF0 <= 0x3A98u) {
      const uintptr_t globalPaletteBuffer =
          g_currentDrawContractGameBase != 0u
              ? uintptr_t(*reinterpret_cast<uint32_t*>(
                    g_currentDrawContractGameBase + 0xBC6BD0u))
              : 0u;
      if (globalPaletteBuffer != 0u) {
        entry.paletteAddress = reinterpret_cast<void*>(
            globalPaletteBuffer + size_t(entry.paletteSlotIndex) * 48u);
        entry.capturedPaletteCount = entry.payloadWordF0;
      }
    }

    PublishCurrentDrawContract(entry);
  };

  if (PublishCurrentDrawContractAfterOriginalRuntime()) {
    const int result = g_trampolineRenderQueueUpdateItemWorldMatrix(
        sceneNodePtr, renderablePartPtr, meshPayloadPtr);
    refreshDrawBindFieldsAndPublish();
    return result;
  }

  refreshDrawBindFieldsAndPublish();
  return g_trampolineRenderQueueUpdateItemWorldMatrix(
      sceneNodePtr, renderablePartPtr, meshPayloadPtr);
}

void ResetCurrentDrawContractCache() {
  for (auto& entry : g_currentDrawContractCache)
    entry = {};
  for (auto& snapshot : g_paletteSnapshotCache)
    snapshot = {};
  for (auto& prepared : g_preparedSliceCache)
    prepared = {};
  for (auto& interest : g_preparedSliceInterestCache)
    interest = {};
  g_globalFallbackRecord = {};
  g_currentDrawDispatchContext = {};
  g_captureSerialCounter.store(0u, std::memory_order_relaxed);
  g_preparedSliceSerialCounter.store(0u, std::memory_order_relaxed);
  g_publishAttemptCount.store(0u, std::memory_order_relaxed);
  g_publishReadyCount.store(0u, std::memory_order_relaxed);
  g_publishMissNoRenderablePart.store(0u, std::memory_order_relaxed);
  g_publishMissNoMeshPayload.store(0u, std::memory_order_relaxed);
  g_publishMissInvalidPaletteSlot.store(0u, std::memory_order_relaxed);
  g_publishMissInvalidPaletteCount.store(0u, std::memory_order_relaxed);
  g_publishMissNoGlobalPalette.store(0u, std::memory_order_relaxed);
  g_publishSkippedNonWorldContext.store(0u, std::memory_order_relaxed);
  g_publishSkippedSmallViewport.store(0u, std::memory_order_relaxed);
  g_queryAttemptCount.store(0u, std::memory_order_relaxed);
  g_queryHitCount.store(0u, std::memory_order_relaxed);
  g_queryMissNoRecord.store(0u, std::memory_order_relaxed);
  g_queryMissFrameTagMismatch.store(0u, std::memory_order_relaxed);
  g_queryMissCacheCollision.store(0u, std::memory_order_relaxed);
  g_capturedPaletteQueryAttemptCount.store(0u, std::memory_order_relaxed);
  g_capturedPaletteQueryHitCount.store(0u, std::memory_order_relaxed);
  g_capturedPaletteMissNoContract.store(0u, std::memory_order_relaxed);
  g_capturedPaletteMissInvalidCount.store(0u, std::memory_order_relaxed);
  g_capturedPaletteMissNoSnapshot.store(0u, std::memory_order_relaxed);
  g_capturedPaletteMissUnreadablePalette.store(0u, std::memory_order_relaxed);
  g_groupSlotDecodeAttemptCount.store(0u, std::memory_order_relaxed);
  g_groupSlotDecodeHitCount.store(0u, std::memory_order_relaxed);
  g_groupSlotDecodeMissDisabledStream.store(0u, std::memory_order_relaxed);
  g_groupSlotDecodeMissNoStream.store(0u, std::memory_order_relaxed);
  g_groupSlotDecodeMissUnreadableStream.store(0u, std::memory_order_relaxed);
  g_groupSlotDecodeMissGroupOutOfRange.store(0u, std::memory_order_relaxed);
  g_preparedSliceProbeAttemptCount.store(0u, std::memory_order_relaxed);
  g_preparedSliceProbeContextReadyCount.store(0u, std::memory_order_relaxed);
  g_preparedSliceProbeBackingReadableCount.store(0u,
                                                 std::memory_order_relaxed);
  g_preparedSliceRecordedCount.store(0u, std::memory_order_relaxed);
  g_preparedSliceQueryAttemptCount.store(0u, std::memory_order_relaxed);
  g_preparedSliceQueryHitCount.store(0u, std::memory_order_relaxed);
  g_preparedSliceQueryMissCount.store(0u, std::memory_order_relaxed);
  g_stream1PublishNoStreamCount.store(0u, std::memory_order_relaxed);
  g_stream1PublishStride0Count.store(0u, std::memory_order_relaxed);
  g_stream1PublishStride1Count.store(0u, std::memory_order_relaxed);
  g_stream1PublishStride8Count.store(0u, std::memory_order_relaxed);
  g_stream1PublishStride12Count.store(0u, std::memory_order_relaxed);
  g_stream1PublishStride16Count.store(0u, std::memory_order_relaxed);
  g_stream1PublishStride20Count.store(0u, std::memory_order_relaxed);
  g_stream1PublishStrideOtherCount.store(0u, std::memory_order_relaxed);
  g_stream1PublishLastRawStride.store(0u, std::memory_order_relaxed);
  g_stream1PublishMaxRawStride.store(0u, std::memory_order_relaxed);
  g_lastRenderablePart.store(0u, std::memory_order_relaxed);
  g_lastSceneNode.store(0u, std::memory_order_relaxed);
  g_lastMeshPayloadPtr.store(0u, std::memory_order_relaxed);
  g_lastPaletteAddress.store(0u, std::memory_order_relaxed);
  g_lastStream1Ptr.store(0u, std::memory_order_relaxed);
  g_lastCaptureSerial.store(0u, std::memory_order_relaxed);
  g_lastPaletteSlotIndex.store(0u, std::memory_order_relaxed);
  g_lastCapturedPaletteCount.store(0u, std::memory_order_relaxed);
  g_lastStream1Stride.store(0u, std::memory_order_relaxed);
  g_lastFrameTag.store(0u, std::memory_order_relaxed);
  g_lastVisibleFrameSerial.store(0u, std::memory_order_relaxed);
  g_lastRenderFrameIndex.store(0u, std::memory_order_relaxed);
  g_lastSmallViewportWidth.store(0u, std::memory_order_relaxed);
  g_lastSmallViewportHeight.store(0u, std::memory_order_relaxed);
  g_lastMissReason.store(uint32_t(CurrentDrawMissReason::None),
                         std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_publishedCurrentDrawMutex);
    g_publishedCurrentDrawByPart.clear();
    g_publishedCurrentDrawReadyByPart.clear();
    g_publishedPaletteSnapshotByPart.clear();
    // Phase 7.30 Action B：attribution snapshot 表随 reset 一起清。
    g_publishedPaletteSnapshotByAttribution.clear();
    g_publishedCurrentDrawReadyByAttribution.clear();
  }
}

CurrentDrawDispatchContext PushCurrentDrawDispatchContext(
    void* sceneNode,
    void* renderablePart,
    uint32_t layerIndex) {
  CurrentDrawDispatchContext previous = g_currentDrawDispatchContext;
  g_currentDrawDispatchContext.valid = true;
  g_currentDrawDispatchContext.sceneNode = sceneNode;
  g_currentDrawDispatchContext.renderablePart = renderablePart;
  g_currentDrawDispatchContext.layerIndex = layerIndex;
  return previous;
}

void RestoreCurrentDrawDispatchContext(
    const CurrentDrawDispatchContext& previous) {
  g_currentDrawDispatchContext = previous;
}

CurrentDrawDispatchContext GetCurrentDrawDispatchContext() {
  return g_currentDrawDispatchContext;
}

void NoteCurrentDrawPreparedSliceProbe(bool contextReady,
                                       bool backingReadable) {
  g_preparedSliceProbeAttemptCount.fetch_add(1u, std::memory_order_relaxed);
  if (contextReady)
    g_preparedSliceProbeContextReadyCount.fetch_add(1u,
                                                    std::memory_order_relaxed);
  if (backingReadable)
    g_preparedSliceProbeBackingReadableCount.fetch_add(
        1u, std::memory_order_relaxed);
}

void PublishCurrentDrawPreparedSlice(
    const CurrentDrawPreparedSliceRecord& record) {
  if (!record.known || record.renderablePart == nullptr ||
      record.preparedCount == 0u || record.primitiveType == 0u) {
    return;
  }

  CurrentDrawPreparedSliceRecord stored = record;
  stored.captureSerial =
      g_preparedSliceSerialCounter.fetch_add(1u, std::memory_order_relaxed) +
      1u;
  const uintptr_t key = reinterpret_cast<uintptr_t>(stored.renderablePart);
  const size_t slot =
      ((key >> 4u) ^ uint64_t(stored.layerIndex)) &
      (g_preparedSliceCache.size() - 1u);
  g_preparedSliceCache[slot] = stored;
  g_preparedSliceRecordedCount.fetch_add(1u, std::memory_order_relaxed);
}

void MarkCurrentDrawPreparedSliceInterest(void* renderablePart,
                                          uint32_t layerIndex) {
  if (renderablePart == nullptr)
    return;
  const uintptr_t key = reinterpret_cast<uintptr_t>(renderablePart);
  const size_t slot =
      ((key >> 4u) ^ uint64_t(layerIndex)) &
      (g_preparedSliceInterestCache.size() - 1u);
  g_preparedSliceInterestCache[slot] = {renderablePart, layerIndex};
  const size_t partSlot =
      ((key >> 4u) ^ uint64_t(0xFFFFFFFFu)) &
      (g_preparedSliceInterestCache.size() - 1u);
  g_preparedSliceInterestCache[partSlot] = {renderablePart, 0xFFFFFFFFu};
}

bool IsCurrentDrawPreparedSliceInterested(void* renderablePart,
                                          uint32_t layerIndex) {
  if (renderablePart == nullptr)
    return false;
  const uintptr_t key = reinterpret_cast<uintptr_t>(renderablePart);
  const size_t slot =
      ((key >> 4u) ^ uint64_t(layerIndex)) &
      (g_preparedSliceInterestCache.size() - 1u);
  const auto& interest = g_preparedSliceInterestCache[slot];
  if (interest.renderablePart == renderablePart &&
      interest.layerIndex == layerIndex)
    return true;
  const size_t partSlot =
      ((key >> 4u) ^ uint64_t(0xFFFFFFFFu)) &
      (g_preparedSliceInterestCache.size() - 1u);
  const auto& partInterest = g_preparedSliceInterestCache[partSlot];
  return partInterest.renderablePart == renderablePart &&
         partInterest.layerIndex == 0xFFFFFFFFu;
}

bool QueryCurrentDrawPreparedSlice(void* renderablePart,
                                   uint32_t layerIndex,
                                   CurrentDrawPreparedSliceRecord& out) {
  out = {};
  g_preparedSliceQueryAttemptCount.fetch_add(1u, std::memory_order_relaxed);
  if (renderablePart == nullptr) {
    g_preparedSliceQueryMissCount.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  MarkCurrentDrawPreparedSliceInterest(renderablePart, layerIndex);

  const uintptr_t key = reinterpret_cast<uintptr_t>(renderablePart);
  const size_t slot =
      ((key >> 4u) ^ uint64_t(layerIndex)) &
      (g_preparedSliceCache.size() - 1u);
  const auto& record = g_preparedSliceCache[slot];
  if (!record.known || record.renderablePart != renderablePart ||
      record.layerIndex != layerIndex || record.preparedCount == 0u ||
      record.primitiveType == 0u) {
    const CurrentDrawPreparedSliceRecord* best = nullptr;
    for (const auto& candidate : g_preparedSliceCache) {
      if (!candidate.known || candidate.renderablePart != renderablePart ||
          candidate.preparedCount == 0u || candidate.primitiveType == 0u)
        continue;
      if (best == nullptr || candidate.captureSerial > best->captureSerial)
        best = &candidate;
    }
    if (best != nullptr) {
      out = *best;
      g_preparedSliceQueryHitCount.fetch_add(1u, std::memory_order_relaxed);
      return true;
    }
    g_preparedSliceQueryMissCount.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }

  out = record;
  g_preparedSliceQueryHitCount.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

void PublishCurrentDrawContract(const CurrentDrawContractRecord& record) {
  // Phase 7.74：把 Publish 的 CPU 时间显式归类。constexpr 门控；release 默认
  // 编译期 strip。该函数每帧约 10K-30K 次（每个 RenderQueue_UpdateItemWorldMatrix
  // 都会触发一次），是 OutsideMainLoop/Tracked 的潜在大头。
  auto publishScope = [&]() -> dxvk::war3::War3PerfMonitor::ScopedCpuScope {
    if constexpr (dxvk::war3::internal::kNativeOptimizationPerfTrackingEnabled) {
      return dxvk::war3::War3PerfMonitor::instance().cpuScope(
          "Shadow/Publish/CurrentDraw");
    }
    return {};
  }();
  g_publishAttemptCount.fetch_add(1u, std::memory_order_relaxed);
  if (record.renderablePart == nullptr) {
    g_publishMissNoRenderablePart.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(
        uint32_t(CurrentDrawMissReason::PublishNoRenderablePart),
        std::memory_order_relaxed);
    return;
  }

  // Phase 7.49：per-publish provenance probe（极低成本，纯 relaxed atomics）。
  // 核心思路：Publish 进入这一点之后 record 已经是本次要发布的材料；
  // 把它和 Game.dll 当前 frameTag 做对比，能直接告诉我们：
  //   - record.frameTag 是否多帧不变（FROZEN 的 record-side 假设）
  //   - live writer frameTag 是否推进（producer-side 对照）
  //   - 二者差值是否在 FROZEN 段里扩大（record 落后 writer 多帧）
  // Phase 7.77：默认关闭这一组诊断（每帧 10K-30K 次 publish × 5+ atomic
  // CAS-loop 累计 1-3ms）。Phase 7.55 v4 + Phase 7.57 producer 已闭环
  // stutter 根因，probe 不再属于主路径所需。需要重启诊断时设
  // `DXVK_WAR3_PUBLISH_PROBE=1`。
  if (Phase749PublishProbeEnabled()) {
    g_publishCallCumulative.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t recordTag = record.frameTag;
    // 更新 record.frameTag same-run 与 min/max。注意 relaxed 即可，这是
    // 统计用，不构成发布屏障。
    const uint32_t prevRecordTag = g_publishRecordFrameTagLast.exchange(
        recordTag, std::memory_order_relaxed);
    if (recordTag == prevRecordTag && recordTag != 0u) {
      const uint32_t newRun = g_publishRecordFrameTagCurrentSameRun.fetch_add(
          1u, std::memory_order_relaxed) + 1u;
      uint32_t curMax = g_publishRecordFrameTagSameRunMax.load(
          std::memory_order_relaxed);
      while (newRun > curMax &&
             !g_publishRecordFrameTagSameRunMax.compare_exchange_weak(
                 curMax, newRun, std::memory_order_relaxed,
                 std::memory_order_relaxed)) {
        // retry
      }
    } else {
      g_publishRecordFrameTagCurrentSameRun.store(1u,
                                                   std::memory_order_relaxed);
    }
    if (recordTag != 0u) {
      uint32_t curMin = g_publishRecordFrameTagMin.load(
          std::memory_order_relaxed);
      while ((curMin == 0u || recordTag < curMin) &&
             !g_publishRecordFrameTagMin.compare_exchange_weak(
                 curMin, recordTag, std::memory_order_relaxed,
                 std::memory_order_relaxed)) {}
      uint32_t curMax = g_publishRecordFrameTagMax.load(
          std::memory_order_relaxed);
      while (recordTag > curMax &&
             !g_publishRecordFrameTagMax.compare_exchange_weak(
                 curMax, recordTag, std::memory_order_relaxed,
                 std::memory_order_relaxed)) {}
    }
    // 读 Game.dll live frameTag（同一个 TryReadCurrentPaletteFrameTag，
    // Hook_RuntimeMatrixWrite/Wrapper/Simple 里也是读的同一个指针）。
    uint32_t liveTag = 0u;
    if (dxvk::war3::model::QueryCurrentPaletteFrameTag(liveTag) &&
        liveTag != 0u) {
      g_publishLiveGamePaletteFrameTagLast.store(
          liveTag, std::memory_order_relaxed);
      uint32_t curMin = g_publishLiveGamePaletteFrameTagMin.load(
          std::memory_order_relaxed);
      while ((curMin == 0u || liveTag < curMin) &&
             !g_publishLiveGamePaletteFrameTagMin.compare_exchange_weak(
                 curMin, liveTag, std::memory_order_relaxed,
                 std::memory_order_relaxed)) {}
      uint32_t curMax = g_publishLiveGamePaletteFrameTagMax.load(
          std::memory_order_relaxed);
      while (liveTag > curMax &&
             !g_publishLiveGamePaletteFrameTagMax.compare_exchange_weak(
                 curMax, liveTag, std::memory_order_relaxed,
                 std::memory_order_relaxed)) {}
      if (recordTag != 0u) {
        if (recordTag == liveTag) {
          g_publishRecordFrameTagEqualsLiveCount.fetch_add(
              1u, std::memory_order_relaxed);
        } else if (recordTag < liveTag) {
          g_publishRecordFrameTagBehindLiveCount.fetch_add(
              1u, std::memory_order_relaxed);
          const uint32_t delta = liveTag - recordTag;
          uint32_t curD = g_publishRecordFrameTagBehindLiveMaxDelta.load(
              std::memory_order_relaxed);
          while (delta > curD &&
                 !g_publishRecordFrameTagBehindLiveMaxDelta
                      .compare_exchange_weak(curD, delta,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {}
        } else {
          g_publishRecordFrameTagAheadLiveCount.fetch_add(
              1u, std::memory_order_relaxed);
        }
      }
    }
  }

  if (LastSampleDiagnosticsEnabled()) {
    g_lastRenderablePart.store(reinterpret_cast<uintptr_t>(record.renderablePart),
                               std::memory_order_relaxed);
    g_lastSceneNode.store(reinterpret_cast<uintptr_t>(record.sceneNode),
                          std::memory_order_relaxed);
    g_lastMeshPayloadPtr.store(reinterpret_cast<uintptr_t>(record.meshPayloadPtr),
                               std::memory_order_relaxed);
    g_lastPaletteAddress.store(reinterpret_cast<uintptr_t>(record.paletteAddress),
                               std::memory_order_relaxed);
    g_lastStream1Ptr.store(reinterpret_cast<uintptr_t>(record.stream1Ptr),
                           std::memory_order_relaxed);
    g_lastCaptureSerial.store(record.captureSerial, std::memory_order_relaxed);
    g_lastPaletteSlotIndex.store(record.paletteSlotIndex,
                                 std::memory_order_relaxed);
    g_lastCapturedPaletteCount.store(record.capturedPaletteCount,
                                     std::memory_order_relaxed);
    g_lastStream1Stride.store(record.stream1Stride,
                              std::memory_order_relaxed);
    g_lastFrameTag.store(record.frameTag, std::memory_order_relaxed);
    g_lastVisibleFrameSerial.store(record.visibleFrameSerial,
                                   std::memory_order_relaxed);
    g_lastRenderFrameIndex.store(record.renderFrameIndex,
                                 std::memory_order_relaxed);
  }

  if (record.meshPayloadPtr == nullptr) {
    g_publishMissNoMeshPayload.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(uint32_t(CurrentDrawMissReason::PublishNoMeshPayload),
                           std::memory_order_relaxed);
  }

  const size_t slot =
      (uintptr_t(record.renderablePart) >> 4u) % kContractCacheSize;
  CurrentDrawContractRecord& localCacheRecord = g_currentDrawContractCache[slot];
  const bool preserveLocalReadyRecord =
      KeepReadySnapshotOnInvalidCurrentDrawEnabled() &&
      !RecordHasReadyShape(record) &&
      localCacheRecord.renderablePart == record.renderablePart &&
      RecordHasLocalPaletteSnapshot(localCacheRecord);
  if (!preserveLocalReadyRecord)
    localCacheRecord = record;

  const bool publishGlobal = GlobalCurrentDrawPublishEnabled();
  if (publishGlobal) {
    std::lock_guard<std::mutex> lock(g_publishedCurrentDrawMutex);
    g_publishedCurrentDrawByPart[record.renderablePart] = record;
  }

  if (record.paletteSlotIndex == 0xFFFFFFFFu || record.paletteSlotIndex >= 0x3A98u) {
    // Phase 7.52 根因说明：FROZEN 段里 War3 的 8-帧 cadence 让 record.paletteSlotIndex
    // 临时 = 0xFFFFFFFFu。这里仍然早退 publish，但不是 submit 端吃旧 palette 的根因：
    //   1. `preserveLocalReadyRecord` 让 localCacheRecord 保留上次 ready 的 record
    //   2. Resolve 端拿到旧 record（和 `g_publishedPaletteSnapshotByPart` 的旧 snapshot）
    //   3. 下游 Phase 7.51 每帧 live rebuild 调用 `War3TryBuildLiveRuntimeGroupPalette`
    //      第一优先级查 Phase 7.46 `QueryRenderablePartPaletteSnapshot`
    //   4. Phase 7.52 第一刀修复让这个 snapshot 每帧从 arena 读 fresh bytes
    //   5. rebuild 成功就用 fresh palette 覆盖 drawTimeCapturedPalette
    // 所以这里保持原语义，fresh-palette 供给走 snapshot → live rebuild 链路。
    if (publishGlobal && !KeepReadySnapshotOnInvalidCurrentDrawEnabled())
      ClearPublishedCurrentDrawReadyRecord(record.renderablePart);
    g_publishMissInvalidPaletteSlot.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(
        uint32_t(CurrentDrawMissReason::PublishInvalidPaletteSlot),
        std::memory_order_relaxed);
    return;
  }

  if (record.capturedPaletteCount == 0u ||
      record.capturedPaletteCount > kMaxPaletteMatrices) {
    if (publishGlobal && !KeepReadySnapshotOnInvalidCurrentDrawEnabled())
      ClearPublishedCurrentDrawReadyRecord(record.renderablePart);
    g_publishMissInvalidPaletteCount.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(
        uint32_t(CurrentDrawMissReason::PublishInvalidPaletteCount),
        std::memory_order_relaxed);
    return;
  }

  if (record.paletteAddress == nullptr) {
    if (publishGlobal && !KeepReadySnapshotOnInvalidCurrentDrawEnabled())
      ClearPublishedCurrentDrawReadyRecord(record.renderablePart);
    g_publishMissNoGlobalPalette.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(
        uint32_t(CurrentDrawMissReason::PublishNoGlobalPalette),
        std::memory_order_relaxed);
    return;
  }

  const size_t requiredBytes = size_t(record.capturedPaletteCount) * 48u;
  if (SafePaletteRangeCheckEnabled() &&
      !dxvk::war3::IsReadableRangeFast(record.paletteAddress, requiredBytes)) {
    if (publishGlobal && !KeepReadySnapshotOnInvalidCurrentDrawEnabled())
      ClearPublishedCurrentDrawReadyRecord(record.renderablePart);
    g_publishMissNoGlobalPalette.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(
        uint32_t(CurrentDrawMissReason::PublishNoGlobalPalette),
        std::memory_order_relaxed);
    return;
  }

  // Phase 7.30 Action B（第二刀）：优先用 Hook_RuntimeMatrixWrite 当场捕获的
  // trusted palette 作为 snapshot 源，而不是从 globalPaletteBuffer +
  // slotIndex*48 raw memcpy。原因：engine 的 palette arena 在 hook 触发时机
  // 可能还残留上一个对象的数据；Hook_RuntimeMatrixWrite 是真正的
  // writer-side 捕获，以 slotIndex 为 key 存完整 palette + frameTag。
  // 该路径命中时，bytes 是"本帧本 slice 的写入"，根除 arena 复用带来的
  // 跨帧 palette 跳变。失败时回退到原 memcpy 路径（保留兼容）。
  //
  // Phase 7.34 线 A：强化仲裁
  //   1. QueryBlendedPaletteBySlotIndexExact 现在严格要求 size == expected；
  //      partial 结果在 query 层已被拒绝，调用端防御性再验一次。
  //   2. trusted miss 时不再自动回退到 raw arena snapshot。
  //      `DXVK_WAR3_PALETTE_ARBITRATION_STRICT=1`（默认）模式下：
  //        - trusted miss → 整条 publish 被丢弃，不写任何 snapshot，不推
  //          `renderablePart`/`attribution` 两张 map，不计 publishReady。
  //        - 该 renderablePart 的旧 snapshot/record 保留，允许后续帧补上。
  //        - counter `g_paletteRejectedNoTrustedSourceCount` 记录丢弃次数。
  //      `DXVK_WAR3_PALETTE_ARBITRATION_STRICT=0` 兼容模式：回退到原 raw arena
  //      行为，供临时回滚调试。
  //   3. raw arena 永远标记 `PaletteProvenance::RawGlobalArena`，仅做诊断。
  std::array<uint8_t, kMaxPaletteMatrices * 48u> trustedPaletteBytes = {};
  const uint8_t* paletteSource = nullptr;
  PaletteProvenance provenance = PaletteProvenance::Unknown;
  {
    std::vector<Matrix4> trustedPalette;
    if (PaletteAttributionSnapshotEnabled() &&
        dxvk::war3::model::QueryBlendedPaletteBySlotIndexExact(
            record.paletteSlotIndex, record.capturedPaletteCount,
            record.frameTag, &trustedPalette) &&
        // Phase 7.34 防御：Exact 版本已经保证 size == expected，这里再 double
        // check 避免任何下游跳变。
        trustedPalette.size() == record.capturedPaletteCount) {
      for (uint32_t i = 0u; i < record.capturedPaletteCount; ++i) {
        const Matrix4& m = trustedPalette[i];
        float* dst = reinterpret_cast<float*>(
            trustedPaletteBytes.data() + size_t(i) * 48u);
        dst[0] = m[0][0]; dst[1] = m[0][1]; dst[2] = m[0][2];
        dst[3] = m[1][0]; dst[4] = m[1][1]; dst[5] = m[1][2];
        dst[6] = m[2][0]; dst[7] = m[2][1]; dst[8] = m[2][2];
        dst[9] = m[3][0]; dst[10] = m[3][1]; dst[11] = m[3][2];
      }
      paletteSource = trustedPaletteBytes.data();
      provenance = PaletteProvenance::TrustedBlendedWriter;
      g_paletteCaptureTrustedSourceHitCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_publishTrustedHitCumulative.fetch_add(1u, std::memory_order_relaxed);
    } else if (PaletteAttributionSnapshotEnabled()) {
      g_paletteCaptureTrustedSourceMissCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
  }

  // Phase 7.34：严格仲裁模式默认开启。
  // 环境变量 DXVK_WAR3_PALETTE_ARBITRATION_STRICT=0 可回滚到旧的 raw-arena
  // fallback 行为（仅调试用）。
  //
  // Phase 7.34 软化（2026-05-11 21:35）：首轮 A2 把 trusted miss 整条丢弃，
  // 用户观察到"视角移动/压力大时阴影一帧消失→视觉闪烁"。根因：
  //   - `visibleFrameSerial` grace 8 帧，连续多帧 trusted miss 后旧 record
  //     超窗口被剔除，下游 populate 看不到对象，阴影消失。
  //   - 严格拒绝 publish 没解决 palette 正确性，反而让对象缺失更严重。
  // 正确策略（接手计划原话）：RawGlobalArena "只保留诊断，不作为 Ready palette"
  //   - 不是在 publish 端丢弃，而是在发布时**打上 RawGlobalArena 标签**，
  //     让下游 submit 根据 provenance 决定是否接受。
  //   - 本轮先恢复 publish（不丢），只做标记+counter。下一轮补下游 provenance
  //     感知过滤（让 submit 对 RawGlobalArena 退化优雅处理）。
  // 回滚：`DXVK_WAR3_PALETTE_ARBITRATION_STRICT=2` 恢复到 A2 首轮的"直接丢弃"
  // 行为供诊断对比，`=0` 恢复完全老 raw arena 兼容。
  static const uint32_t s_paletteArbitrationStrictMode =
      ReadEnvU32("DXVK_WAR3_PALETTE_ARBITRATION_STRICT", 1u);

  if (paletteSource == nullptr) {
    if (s_paletteArbitrationStrictMode >= 2u) {
      // 严格模式 2：trusted miss 直接丢弃，保留旧 record 给 grace 窗口。
      // 此路径仅作诊断对比使用。
      g_paletteRejectedNoTrustedSourceCount.fetch_add(
          1u, std::memory_order_relaxed);
      g_publishRejectedNoTrustedCumulative.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastMissReason.store(
          uint32_t(CurrentDrawMissReason::PaletteNoSnapshot),
          std::memory_order_relaxed);
      return;
    }
    // 默认路径（Strict mode 1 或 0）：使用 raw arena 作为 snapshot 源，
    // 但 provenance 明确标记为 RawGlobalArena 以便下游识别。
    // 注意：这里的数据可能带 arena 残留（跨对象污染），下游 submit 端
    // 需要根据 provenance 决定是否信任。
    paletteSource = reinterpret_cast<const uint8_t*>(record.paletteAddress);
    provenance = PaletteProvenance::RawGlobalArena;
    g_publishRawFallbackCumulative.fetch_add(1u, std::memory_order_relaxed);
    if (s_paletteArbitrationStrictMode >= 1u) {
      // Strict mode 1（默认）：标记但仍发布。记录 counter 以观察
      // RawGlobalArena 在总 publish 中的占比。
      g_paletteRejectedNoTrustedSourceCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
  }
  // Phase 1：记录 provenance 分桶计数。
  static std::atomic<uint64_t> g_paletteProvenanceTrustedBlendedWriterCount{0u};
  static std::atomic<uint64_t> g_paletteProvenanceRawGlobalArenaCount{0u};
  static std::atomic<uint64_t> g_paletteProvenanceUnknownCount{0u};
  switch (provenance) {
    case PaletteProvenance::TrustedBlendedWriter:
      g_paletteProvenanceTrustedBlendedWriterCount.fetch_add(1u, std::memory_order_relaxed);
      break;
    case PaletteProvenance::RawGlobalArena:
      g_paletteProvenanceRawGlobalArenaCount.fetch_add(1u, std::memory_order_relaxed);
      break;
    default:
      g_paletteProvenanceUnknownCount.fetch_add(1u, std::memory_order_relaxed);
      break;
  }

  const size_t snapshotSlot =
      (uintptr_t(record.renderablePart) >> 4u) % kPaletteSnapshotCacheSize;
  auto& snapshot = g_paletteSnapshotCache[snapshotSlot];

  // Phase 7.34 第三轮曾尝试不让 RawGlobalArena 降级覆盖已有 trusted snapshot。
  // 根因：用户观察到"视角边缘对象（门）在压力下闪烁"。
  // 机制：某帧视锥边缘对象的 trusted cache miss → 若允许 raw arena 覆写
  // snapshot，当前帧的 snapshot 变成 arena 残留数据 → 下游 populate 拿到
  // 错位的 palette → 阴影跳变 → 视觉闪烁。
  //
  // Phase 7.43 复核发现这会制造更隐蔽的问题：旧 trusted bytes 被保留，
  // 但 snapshot 的 frameTag/captureSerial 仍更新成当前帧，content-age 诊断会
  // 显示 fresh，实际 shadow pose 却可连续多帧冻结。默认关闭该保护，让
  // RawGlobalArena 覆写当前帧 bytes；若需要回滚对比，可显式设置
  // DXVK_WAR3_PRESERVE_TRUSTED_PALETTE_ON_RAW_MISS=1。
  const bool snapshotWasTrusted =
      snapshot.renderablePart == record.renderablePart &&
      snapshot.paletteProvenance == PaletteProvenance::TrustedBlendedWriter;
  const bool preserveTrustedSnapshot =
      PreserveTrustedPaletteOnRawMissEnabled() && snapshotWasTrusted &&
      provenance == PaletteProvenance::RawGlobalArena;

  snapshot.renderablePart = record.renderablePart;
  snapshot.frameTag = record.frameTag;
  snapshot.captureSerial = record.captureSerial;
  snapshot.matrixCount = record.capturedPaletteCount;
  if (!preserveTrustedSnapshot) {
    snapshot.paletteProvenance = provenance;
    std::memcpy(snapshot.bytes.data(), paletteSource, requiredBytes);
  } else {
    g_paletteSnapshotTrustedPreservedCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (publishGlobal) {
    std::lock_guard<std::mutex> lock(g_publishedCurrentDrawMutex);
    auto& globalSnapshot = g_publishedPaletteSnapshotByPart[record.renderablePart];
    // 对 global snapshot 做同样的"不降级" 保护。
    const bool globalWasTrusted =
        globalSnapshot.renderablePart == record.renderablePart &&
        globalSnapshot.paletteProvenance ==
            PaletteProvenance::TrustedBlendedWriter;
    const bool preserveGlobalTrusted =
        PreserveTrustedPaletteOnRawMissEnabled() && globalWasTrusted &&
        provenance == PaletteProvenance::RawGlobalArena;
    globalSnapshot.renderablePart = record.renderablePart;
    globalSnapshot.frameTag = record.frameTag;
    globalSnapshot.captureSerial = record.captureSerial;
    globalSnapshot.matrixCount = record.capturedPaletteCount;
    if (!preserveGlobalTrusted) {
      globalSnapshot.paletteProvenance = provenance;
      std::memcpy(globalSnapshot.bytes.data(), paletteSource, requiredBytes);
    }
    g_publishedCurrentDrawReadyByPart[record.renderablePart] = record;
    // Phase 7.30 Action B：按 attribution key 并行存一份 snapshot 与 record，
    // 作为 renderablePart 换地址时的跨帧真源。这一份的 key 是纯数值，不会
    // 因为 renderablePart 指针被回收/重分配而失效。
    if (PaletteAttributionSnapshotEnabled()) {
      const uint64_t attrKey = ComputePaletteAttributionKey(record);
      if (attrKey != 0u) {
        auto& attrSnapshot = g_publishedPaletteSnapshotByAttribution[attrKey];
        const bool attrWasTrusted =
            attrSnapshot.renderablePart != nullptr &&
            attrSnapshot.paletteProvenance ==
                PaletteProvenance::TrustedBlendedWriter;
        const bool preserveAttrTrusted =
            PreserveTrustedPaletteOnRawMissEnabled() && attrWasTrusted &&
            provenance == PaletteProvenance::RawGlobalArena;
        attrSnapshot.renderablePart = record.renderablePart;
        attrSnapshot.frameTag = record.frameTag;
        attrSnapshot.captureSerial = record.captureSerial;
        attrSnapshot.matrixCount = record.capturedPaletteCount;
        if (!preserveAttrTrusted) {
          attrSnapshot.paletteProvenance = provenance;
          std::memcpy(attrSnapshot.bytes.data(), paletteSource, requiredBytes);
        }
        g_publishedCurrentDrawReadyByAttribution[attrKey] = record;
      }
    }
  }
  g_publishReadyCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastMissReason.store(uint32_t(CurrentDrawMissReason::None),
                         std::memory_order_relaxed);
}

CurrentDrawContractDiagnosticsSummary
QueryCurrentDrawContractDiagnosticsSummary() {
  CurrentDrawContractDiagnosticsSummary summary = {};
  summary.publishAttemptCount =
      g_publishAttemptCount.load(std::memory_order_relaxed);
  summary.publishReadyCount =
      g_publishReadyCount.load(std::memory_order_relaxed);
  summary.publishMissNoRenderablePart =
      g_publishMissNoRenderablePart.load(std::memory_order_relaxed);
  summary.publishMissNoMeshPayload =
      g_publishMissNoMeshPayload.load(std::memory_order_relaxed);
  summary.publishMissInvalidPaletteSlot =
      g_publishMissInvalidPaletteSlot.load(std::memory_order_relaxed);
  summary.publishMissInvalidPaletteCount =
      g_publishMissInvalidPaletteCount.load(std::memory_order_relaxed);
  summary.publishMissNoGlobalPalette =
      g_publishMissNoGlobalPalette.load(std::memory_order_relaxed);
  summary.publishSkippedNonWorldContext =
      g_publishSkippedNonWorldContext.load(std::memory_order_relaxed);
  summary.publishSkippedSmallViewport =
      g_publishSkippedSmallViewport.load(std::memory_order_relaxed);
  summary.queryAttemptCount =
      g_queryAttemptCount.load(std::memory_order_relaxed);
  summary.queryHitCount =
      g_queryHitCount.load(std::memory_order_relaxed);
  summary.queryMissNoRecord =
      g_queryMissNoRecord.load(std::memory_order_relaxed);
  summary.queryMissFrameTagMismatch =
      g_queryMissFrameTagMismatch.load(std::memory_order_relaxed);
  summary.queryMissCacheCollision =
      g_queryMissCacheCollision.load(std::memory_order_relaxed);
  summary.capturedPaletteQueryAttemptCount =
      g_capturedPaletteQueryAttemptCount.load(std::memory_order_relaxed);
  summary.capturedPaletteQueryHitCount =
      g_capturedPaletteQueryHitCount.load(std::memory_order_relaxed);
  summary.capturedPaletteMissNoContract =
      g_capturedPaletteMissNoContract.load(std::memory_order_relaxed);
  summary.capturedPaletteMissInvalidCount =
      g_capturedPaletteMissInvalidCount.load(std::memory_order_relaxed);
  summary.capturedPaletteMissNoSnapshot =
      g_capturedPaletteMissNoSnapshot.load(std::memory_order_relaxed);
  summary.capturedPaletteMissUnreadablePalette =
      g_capturedPaletteMissUnreadablePalette.load(std::memory_order_relaxed);
  summary.paletteAttributionSnapshotHitCount =
      g_paletteAttributionSnapshotHitCount.load(std::memory_order_relaxed);
  summary.paletteCaptureTrustedSourceHitCount =
      g_paletteCaptureTrustedSourceHitCount.load(std::memory_order_relaxed);
  summary.paletteCaptureTrustedSourceMissCount =
      g_paletteCaptureTrustedSourceMissCount.load(std::memory_order_relaxed);
  // Phase 7.34：严格仲裁下 raw-arena 拒绝次数。
  summary.paletteRejectedNoTrustedSourceCount =
      g_paletteRejectedNoTrustedSourceCount.load(std::memory_order_relaxed);
  // Phase 7.34 第三轮：trusted snapshot 保留次数。
  summary.paletteSnapshotTrustedPreservedCount =
      g_paletteSnapshotTrustedPreservedCount.load(std::memory_order_relaxed);
  // Phase 7.35 Pose-lag 诊断：submit 端 palette 时间滞后分桶。
  summary.submitPaletteFrameLag0Count =
      g_submitPaletteFrameLag0Count.load(std::memory_order_relaxed);
  summary.submitPaletteFrameLag1Count =
      g_submitPaletteFrameLag1Count.load(std::memory_order_relaxed);
  summary.submitPaletteFrameLag2Count =
      g_submitPaletteFrameLag2Count.load(std::memory_order_relaxed);
  summary.submitPaletteFrameLag3To5Count =
      g_submitPaletteFrameLag3To5Count.load(std::memory_order_relaxed);
  summary.submitPaletteFrameLag6PlusCount =
      g_submitPaletteFrameLag6PlusCount.load(std::memory_order_relaxed);
  summary.submitPaletteFrameLagMax =
      g_submitPaletteFrameLagMax.load(std::memory_order_relaxed);
  summary.submitPaletteFrameLagSampleCount =
      g_submitPaletteFrameLagSampleCount.load(std::memory_order_relaxed);
  // Phase 7.39：submit 端实际 palette 内容年龄分桶。
  summary.submitPaletteContentAgeLag0Count =
      g_submitPaletteContentAgeLag0Count.load(std::memory_order_relaxed);
  summary.submitPaletteContentAgeLag1Count =
      g_submitPaletteContentAgeLag1Count.load(std::memory_order_relaxed);
  summary.submitPaletteContentAgeLag2Count =
      g_submitPaletteContentAgeLag2Count.load(std::memory_order_relaxed);
  summary.submitPaletteContentAgeLag3To5Count =
      g_submitPaletteContentAgeLag3To5Count.load(std::memory_order_relaxed);
  summary.submitPaletteContentAgeLag6PlusCount =
      g_submitPaletteContentAgeLag6PlusCount.load(std::memory_order_relaxed);
  summary.submitPaletteContentAgeMax =
      g_submitPaletteContentAgeMax.load(std::memory_order_relaxed);
  summary.submitPaletteContentAgeSampleCount =
      g_submitPaletteContentAgeSampleCount.load(std::memory_order_relaxed);
  summary.submitPaletteContentAgeUnknownCount =
      g_submitPaletteContentAgeUnknownCount.load(std::memory_order_relaxed);
  // Phase 7.35 路径 1 诊断：capture 端 Exact 查询命中/miss 分布。
  summary.paletteCaptureExactHitCount =
      g_paletteCaptureExactHitCount.load(std::memory_order_relaxed);
  summary.paletteCaptureBestEffortHitCount =
      g_paletteCaptureBestEffortHitCount.load(std::memory_order_relaxed);
  summary.paletteCaptureSlotOverflowMissCount =
      g_paletteCaptureSlotOverflowMissCount.load(std::memory_order_relaxed);
  summary.paletteCaptureInvalidEntryMissCount =
      g_paletteCaptureInvalidEntryMissCount.load(std::memory_order_relaxed);
  summary.paletteCaptureFrameTagMismatchMissCount =
      g_paletteCaptureFrameTagMismatchMissCount.load(std::memory_order_relaxed);
  summary.paletteCaptureShortResultMissCount =
      g_paletteCaptureShortResultMissCount.load(std::memory_order_relaxed);
  // Phase 7.35 路径 2：submit 端 live rebuild 分桶。
  summary.submitLiveRebuildAttemptCount =
      g_submitLiveRebuildAttemptCount.load(std::memory_order_relaxed);
  summary.submitLiveRebuildHitCount =
      g_submitLiveRebuildHitCount.load(std::memory_order_relaxed);
  summary.submitLiveRebuildMissCount =
      g_submitLiveRebuildMissCount.load(std::memory_order_relaxed);
  summary.submitLiveRebuildAppliedCount =
      g_submitLiveRebuildAppliedCount.load(std::memory_order_relaxed);
  summary.groupSlotDecodeAttemptCount =
      g_groupSlotDecodeAttemptCount.load(std::memory_order_relaxed);
  summary.groupSlotDecodeHitCount =
      g_groupSlotDecodeHitCount.load(std::memory_order_relaxed);
  summary.groupSlotDecodeMissDisabledStream =
      g_groupSlotDecodeMissDisabledStream.load(std::memory_order_relaxed);
  summary.groupSlotDecodeMissNoStream =
      g_groupSlotDecodeMissNoStream.load(std::memory_order_relaxed);
  summary.groupSlotDecodeMissUnreadableStream =
      g_groupSlotDecodeMissUnreadableStream.load(std::memory_order_relaxed);
  summary.groupSlotDecodeMissGroupOutOfRange =
      g_groupSlotDecodeMissGroupOutOfRange.load(std::memory_order_relaxed);
  summary.preparedSliceProbeAttemptCount =
      g_preparedSliceProbeAttemptCount.load(std::memory_order_relaxed);
  summary.preparedSliceProbeContextReadyCount =
      g_preparedSliceProbeContextReadyCount.load(std::memory_order_relaxed);
  summary.preparedSliceProbeBackingReadableCount =
      g_preparedSliceProbeBackingReadableCount.load(
          std::memory_order_relaxed);
  summary.preparedSliceRecordedCount =
      g_preparedSliceRecordedCount.load(std::memory_order_relaxed);
  summary.preparedSliceQueryAttemptCount =
      g_preparedSliceQueryAttemptCount.load(std::memory_order_relaxed);
  summary.preparedSliceQueryHitCount =
      g_preparedSliceQueryHitCount.load(std::memory_order_relaxed);
  summary.preparedSliceQueryMissCount =
      g_preparedSliceQueryMissCount.load(std::memory_order_relaxed);
  summary.stream1PublishNoStreamCount =
      g_stream1PublishNoStreamCount.load(std::memory_order_relaxed);
  summary.stream1PublishStride0Count =
      g_stream1PublishStride0Count.load(std::memory_order_relaxed);
  summary.stream1PublishStride1Count =
      g_stream1PublishStride1Count.load(std::memory_order_relaxed);
  summary.stream1PublishStride8Count =
      g_stream1PublishStride8Count.load(std::memory_order_relaxed);
  summary.stream1PublishStride12Count =
      g_stream1PublishStride12Count.load(std::memory_order_relaxed);
  summary.stream1PublishStride16Count =
      g_stream1PublishStride16Count.load(std::memory_order_relaxed);
  summary.stream1PublishStride20Count =
      g_stream1PublishStride20Count.load(std::memory_order_relaxed);
  summary.stream1PublishStrideOtherCount =
      g_stream1PublishStrideOtherCount.load(std::memory_order_relaxed);
  summary.stream1PublishLastRawStride =
      g_stream1PublishLastRawStride.load(std::memory_order_relaxed);
  summary.stream1PublishMaxRawStride =
      g_stream1PublishMaxRawStride.load(std::memory_order_relaxed);
  summary.lastRenderablePart =
      g_lastRenderablePart.load(std::memory_order_relaxed);
  summary.lastSceneNode = g_lastSceneNode.load(std::memory_order_relaxed);
  summary.lastMeshPayloadPtr =
      g_lastMeshPayloadPtr.load(std::memory_order_relaxed);
  summary.lastPaletteAddress =
      g_lastPaletteAddress.load(std::memory_order_relaxed);
  summary.lastStream1Ptr =
      g_lastStream1Ptr.load(std::memory_order_relaxed);
  summary.lastCaptureSerial =
      g_lastCaptureSerial.load(std::memory_order_relaxed);
  summary.lastPaletteSlotIndex =
      g_lastPaletteSlotIndex.load(std::memory_order_relaxed);
  summary.lastCapturedPaletteCount =
      g_lastCapturedPaletteCount.load(std::memory_order_relaxed);
  summary.lastStream1Stride =
      g_lastStream1Stride.load(std::memory_order_relaxed);
  summary.lastFrameTag = g_lastFrameTag.load(std::memory_order_relaxed);
  summary.lastVisibleFrameSerial =
      g_lastVisibleFrameSerial.load(std::memory_order_relaxed);
  summary.lastRenderFrameIndex =
      g_lastRenderFrameIndex.load(std::memory_order_relaxed);
  summary.lastSmallViewportWidth =
      g_lastSmallViewportWidth.load(std::memory_order_relaxed);
  summary.lastSmallViewportHeight =
      g_lastSmallViewportHeight.load(std::memory_order_relaxed);
  summary.lastMissReason = g_lastMissReason.load(std::memory_order_relaxed);
  // Phase 7.49：per-publish provenance probe
  summary.publishCallCumulative =
      g_publishCallCumulative.load(std::memory_order_relaxed);
  summary.publishTrustedHitCumulative =
      g_publishTrustedHitCumulative.load(std::memory_order_relaxed);
  summary.publishRawFallbackCumulative =
      g_publishRawFallbackCumulative.load(std::memory_order_relaxed);
  summary.publishRejectedNoTrustedCumulative =
      g_publishRejectedNoTrustedCumulative.load(std::memory_order_relaxed);
  summary.publishRecordFrameTagSameRunMax =
      g_publishRecordFrameTagSameRunMax.load(std::memory_order_relaxed);
  summary.publishRecordFrameTagCurrentSameRun =
      g_publishRecordFrameTagCurrentSameRun.load(std::memory_order_relaxed);
  summary.publishRecordFrameTagLast =
      g_publishRecordFrameTagLast.load(std::memory_order_relaxed);
  summary.publishLiveGamePaletteFrameTagLast =
      g_publishLiveGamePaletteFrameTagLast.load(std::memory_order_relaxed);
  summary.publishLiveGamePaletteFrameTagMin =
      g_publishLiveGamePaletteFrameTagMin.load(std::memory_order_relaxed);
  summary.publishLiveGamePaletteFrameTagMax =
      g_publishLiveGamePaletteFrameTagMax.load(std::memory_order_relaxed);
  summary.publishRecordFrameTagMin =
      g_publishRecordFrameTagMin.load(std::memory_order_relaxed);
  summary.publishRecordFrameTagMax =
      g_publishRecordFrameTagMax.load(std::memory_order_relaxed);
  summary.publishRecordFrameTagBehindLiveMaxDelta =
      g_publishRecordFrameTagBehindLiveMaxDelta.load(
          std::memory_order_relaxed);
  summary.publishRecordFrameTagEqualsLiveCount =
      g_publishRecordFrameTagEqualsLiveCount.load(
          std::memory_order_relaxed);
  summary.publishRecordFrameTagBehindLiveCount =
      g_publishRecordFrameTagBehindLiveCount.load(
          std::memory_order_relaxed);
  summary.publishRecordFrameTagAheadLiveCount =
      g_publishRecordFrameTagAheadLiveCount.load(
          std::memory_order_relaxed);
  return summary;
}

const char* DescribeCurrentDrawMissReason(uint32_t reason) {
  switch (CurrentDrawMissReason(reason)) {
  case CurrentDrawMissReason::None:
    return "none";
  case CurrentDrawMissReason::PublishNoRenderablePart:
    return "publish_no_renderable_part";
  case CurrentDrawMissReason::PublishNoMeshPayload:
    return "publish_no_mesh_payload";
  case CurrentDrawMissReason::PublishInvalidPaletteSlot:
    return "publish_invalid_palette_slot";
  case CurrentDrawMissReason::PublishInvalidPaletteCount:
    return "publish_invalid_palette_count";
  case CurrentDrawMissReason::PublishNoGlobalPalette:
    return "publish_no_global_palette";
  case CurrentDrawMissReason::QueryNoRecord:
    return "query_no_record";
  case CurrentDrawMissReason::QueryFrameTagMismatch:
    return "query_frame_tag_mismatch";
  case CurrentDrawMissReason::QueryCacheCollision:
    return "query_cache_collision";
  case CurrentDrawMissReason::PaletteNoContract:
    return "palette_no_contract";
  case CurrentDrawMissReason::PaletteInvalidCount:
    return "palette_invalid_count";
  case CurrentDrawMissReason::PaletteNoSnapshot:
    return "palette_no_snapshot";
  case CurrentDrawMissReason::PaletteUnreadable:
    return "palette_unreadable";
  case CurrentDrawMissReason::GroupSlotNoStream:
    return "group_slot_no_stream";
  case CurrentDrawMissReason::GroupSlotUnreadable:
    return "group_slot_unreadable";
  case CurrentDrawMissReason::GroupSlotOutOfRange:
    return "group_slot_out_of_range";
  case CurrentDrawMissReason::StaleVisibleFrame:
    return "stale_visible_frame";
  case CurrentDrawMissReason::GroupSlotStreamDisabled:
    return "group_slot_stream_disabled";
  case CurrentDrawMissReason::PublishSmallViewport:
    return "publish_small_viewport";
  }
  return "unknown";
}

bool InstallCurrentDrawContractHook(uintptr_t gameBase, bool logEnabled) {
  g_currentDrawContractGameBase = gameBase;
  const uintptr_t target = gameBase + kRenderQueueUpdateItemWorldMatrixRva;
  if (!IsExecutableRange(reinterpret_cast<const void*>(target), 16)) {
    war3dbg::Print(
        "DXVK_CurrentDraw: RenderQueue_UpdateItemWorldMatrix not executable, "
        "skip Hook (addr=%p)\n",
        reinterpret_cast<void*>(target));
    return false;
  }

  return hooks::InstallMinHook(
      reinterpret_cast<LPVOID>(target),
      reinterpret_cast<LPVOID>(&Hook_RenderQueueUpdateItemWorldMatrix),
      reinterpret_cast<LPVOID*>(
          &g_trampolineRenderQueueUpdateItemWorldMatrix),
      "CurrentDraw", "RenderQueueUpdateItemWorldMatrix", true, logEnabled);
}

bool QueryCurrentDrawContract(void* renderablePart,
                              CurrentDrawContractRecord& out) {
  out = {};
  g_queryAttemptCount.fetch_add(1u, std::memory_order_relaxed);

  const CurrentDrawContractRecord* entry = nullptr;
  const auto status = LookupCurrentDrawContractRecord(renderablePart, entry);
  if (status != ContractLookupStatus::Hit) {
    switch (status) {
    case ContractLookupStatus::MissingRecord:
      g_queryMissNoRecord.fetch_add(1u, std::memory_order_relaxed);
      g_lastMissReason.store(uint32_t(CurrentDrawMissReason::QueryNoRecord),
                             std::memory_order_relaxed);
      break;
    case ContractLookupStatus::FrameTagMismatch:
      g_queryMissFrameTagMismatch.fetch_add(1u, std::memory_order_relaxed);
      g_lastMissReason.store(
          uint32_t(CurrentDrawMissReason::QueryFrameTagMismatch),
          std::memory_order_relaxed);
      break;
    case ContractLookupStatus::CacheCollision:
      g_queryMissCacheCollision.fetch_add(1u, std::memory_order_relaxed);
      g_lastMissReason.store(uint32_t(CurrentDrawMissReason::QueryCacheCollision),
                             std::memory_order_relaxed);
      break;
    case ContractLookupStatus::Hit:
      break;
    }
    return false;
  }

  out = *entry;
  out.known = true;
  g_queryHitCount.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

bool QueryCurrentDrawContractCapturedPalette(
    void* renderablePart,
    std::vector<Matrix4>& outPalette,
    uint32_t& outGroupCount) {
  outPalette.clear();
  outGroupCount = 0u;

  const CurrentDrawContractRecord* entry = nullptr;
  if (LookupCurrentDrawContractRecord(renderablePart, entry) !=
      ContractLookupStatus::Hit) {
    g_capturedPaletteQueryAttemptCount.fetch_add(1u,
                                                 std::memory_order_relaxed);
    g_capturedPaletteMissNoContract.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(uint32_t(CurrentDrawMissReason::PaletteNoContract),
                           std::memory_order_relaxed);
    return false;
  }
  CurrentDrawContractRecord record = *entry;
  record.known = true;
  return DecodeCapturedPaletteForRecord(record, outPalette, outGroupCount,
                                        true);
}

bool DecodeCurrentDrawGroupSlots(const CurrentDrawContractRecord& record,
                                 uint32_t vertexCount,
                                 uint32_t paletteCount,
                                 std::vector<uint8_t>& outGroupSlots,
                                 uint64_t& outGroupHash) {
  outGroupSlots.clear();
  outGroupHash = 0u;
  g_groupSlotDecodeAttemptCount.fetch_add(1u, std::memory_order_relaxed);

  if (!record.known || record.stream1Ptr == nullptr || vertexCount == 0u ||
      paletteCount == 0u) {
    g_groupSlotDecodeMissNoStream.fetch_add(1u, std::memory_order_relaxed);
    g_lastMissReason.store(uint32_t(CurrentDrawMissReason::GroupSlotNoStream),
                           std::memory_order_relaxed);
    return false;
  }

  if (record.payloadWord48 == 0u) {
    g_groupSlotDecodeMissDisabledStream.fetch_add(
        1u, std::memory_order_relaxed);
    g_lastMissReason.store(
        uint32_t(CurrentDrawMissReason::GroupSlotStreamDisabled),
        std::memory_order_relaxed);
    return false;
  }

  // 0x6F138EE0 binds +0x4C with the enable value from +0x48. The separate
  // +0x58 field is another 12-byte bind input, not the stride of +0x4C.
  constexpr uint32_t kCurrentDrawGroupSlotStep = 1u;

  const auto* streamBase =
      reinterpret_cast<const uint8_t*>(record.stream1Ptr);
  const size_t requiredBytes =
      size_t(vertexCount - 1u) * size_t(kCurrentDrawGroupSlotStep) + 1u;
  if (!dxvk::war3::IsReadableRange(streamBase, requiredBytes)) {
    g_groupSlotDecodeMissUnreadableStream.fetch_add(
        1u, std::memory_order_relaxed);
    g_lastMissReason.store(
        uint32_t(CurrentDrawMissReason::GroupSlotUnreadable),
        std::memory_order_relaxed);
    return false;
  }

  outGroupSlots.resize(vertexCount);
  uint64_t hash = bit::fnv1a_init();
  for (uint32_t i = 0u; i < vertexCount; ++i) {
    const uint32_t groupSlot =
        uint32_t(streamBase[size_t(i) * size_t(kCurrentDrawGroupSlotStep)]);
    if (groupSlot >= paletteCount || groupSlot >= 256u) {
      outGroupSlots.clear();
      g_groupSlotDecodeMissGroupOutOfRange.fetch_add(
          1u, std::memory_order_relaxed);
      g_lastMissReason.store(
          uint32_t(CurrentDrawMissReason::GroupSlotOutOfRange),
          std::memory_order_relaxed);
      return false;
    }

    outGroupSlots[i] = uint8_t(groupSlot);
    hash = bit::fnv1a_iter(hash, groupSlot);
  }

  hash = bit::fnv1a_iter(hash,
                         reinterpret_cast<uintptr_t>(record.stream1Ptr));
  hash = bit::fnv1a_iter(hash, kCurrentDrawGroupSlotStep);
  hash = bit::fnv1a_iter(hash, record.payloadWord48);
  hash = bit::fnv1a_iter(hash, record.payloadWord108);
  hash = bit::fnv1a_iter(hash, record.payloadWord11C);
  hash = bit::fnv1a_iter(hash, record.layerIndex);
  outGroupHash = hash;
  g_groupSlotDecodeHitCount.fetch_add(1u, std::memory_order_relaxed);
  g_lastMissReason.store(uint32_t(CurrentDrawMissReason::None),
                         std::memory_order_relaxed);
  return true;
}

uint64_t ComputeStableGroupContentHash(const CurrentDrawContractRecord& record,
                                       const std::vector<uint8_t>& groupSlots) {
  using namespace ::dxvk::bit;
  uint64_t hash = fnv1a_init();
  for (uint8_t slot : groupSlots)
    hash = fnv1a_iter(hash, uint32_t(slot));
  hash = fnv1a_iter(hash, 1u);
  hash = fnv1a_iter(hash, record.payloadWord48);
  hash = fnv1a_iter(hash, record.payloadWord108);
  hash = fnv1a_iter(hash, record.payloadWord11C);
  hash = fnv1a_iter(hash, record.layerIndex);
  return hash;
}

std::vector<CurrentDrawContractRecord> SnapshotPublishedCurrentDrawContracts(
    uint64_t minVisibleFrameSerial,
    bool readyOnly) {
  CurrentDrawContractSnapshotOptions options = {};
  options.minVisibleFrameSerial = minVisibleFrameSerial;
  options.readyOnly = readyOnly;
  options.maxRecords = 0u;
  options.unitsOnly = false;
  options.pruneOlderThanMinVisibleFrame = true;
  return SnapshotPublishedCurrentDrawContracts(options);
}

std::vector<CurrentDrawContractRecord> SnapshotPublishedCurrentDrawContracts(
    const CurrentDrawContractSnapshotOptions& options) {
  std::vector<CurrentDrawContractRecord> out;
  const size_t reserveCount =
      options.maxRecords != 0u
          ? std::min<size_t>(kContractCacheSize, size_t(options.maxRecords))
          : kContractCacheSize;
  out.reserve(reserveCount);
  const std::unordered_set<uint64_t> preferredKeySet(
      options.preferredSelectionKeys.begin(),
      options.preferredSelectionKeys.end());
  // Phase 7.81：thread_local 复用 preferredVisibleKeyCache。
  static thread_local std::unordered_map<uint64_t, uint64_t>
      s_preferredVisibleKeyCache;
  s_preferredVisibleKeyCache.clear();
  auto& preferredVisibleKeyCache = s_preferredVisibleKeyCache;
  const auto makePreferredKey = [](uint32_t tag, uint64_t value) -> uint64_t {
    uint64_t hash = bit::fnv1a_init();
    hash = bit::fnv1a_iter(hash, tag);
    hash = bit::fnv1a_iter(hash, value);
    return hash;
  };
  const auto preferredVisibleKey =
      [&](const CurrentDrawContractRecord& record) -> uint64_t {
    if (record.renderablePart == nullptr)
      return 0u;
    uint64_t cacheKey = bit::fnv1a_init();
    cacheKey = bit::fnv1a_iter(
        cacheKey, uint64_t(reinterpret_cast<uintptr_t>(record.renderablePart)));
    cacheKey = bit::fnv1a_iter(cacheKey, record.layerIndex);
    const auto cached = preferredVisibleKeyCache.find(cacheKey);
    if (cached != preferredVisibleKeyCache.end())
      return cached->second;

    uint64_t key = 0u;
    VisibleRenderableRecord visible = {};
    if (VisibleRenderableRegistry::instance().queryByRenderablePartAndLayer(
            record.renderablePart, record.layerIndex, visible)) {
      const auto ptrValue = [](const void* ptr) -> uint64_t {
        return uint64_t(reinterpret_cast<uintptr_t>(ptr));
      };
      if (visible.identity.unitPtr != nullptr)
        key = makePreferredKey(1u, ptrValue(visible.identity.unitPtr));
      else if (visible.identity.jHandle != 0u)
        key = makePreferredKey(2u, visible.identity.jHandle);
      else if (visible.identity.handleId != 0u)
        key = makePreferredKey(2u, visible.identity.handleId | 0x100000u);
      else if (visible.identity.worldObjectEntry != nullptr)
        key = makePreferredKey(
            4u, ptrValue(visible.identity.worldObjectEntry));
      else if (visible.sceneNode != nullptr)
        key = makePreferredKey(5u, ptrValue(visible.sceneNode));
    }
    preferredVisibleKeyCache.emplace(cacheKey, key);
    return key;
  };
  const auto isPreferred = [&](const CurrentDrawContractRecord& record) {
    if (preferredKeySet.empty())
      return false;
    const uint64_t key = SnapshotStableIdentityKey(record);
    if (key != 0u && preferredKeySet.find(key) != preferredKeySet.end()) {
      return true;
    }
    const uint64_t visibleKey = preferredVisibleKey(record);
    return visibleKey != 0u &&
           preferredKeySet.find(visibleKey) != preferredKeySet.end();
  };
  const auto betterRecord = [&](const CurrentDrawContractRecord& a,
                                const CurrentDrawContractRecord& b) {
    const bool pa = isPreferred(a);
    const bool pb = isPreferred(b);
    if (pa != pb)
      return pa;
    return BetterSnapshotRecord(a, b, options.unitsOnly);
  };
  // Phase 7.81：thread_local 复用 unlimitedDedupeIndex，避免每帧多次调用
  // SnapshotPublishedCurrentDrawContracts 时反复 alloc/free + reserve(4096)。
  static thread_local std::unordered_map<uint64_t, size_t>
      s_unlimitedDedupeIndex;
  s_unlimitedDedupeIndex.clear();
  auto& unlimitedDedupeIndex = s_unlimitedDedupeIndex;
  auto unlimitedDedupeKey = [](const CurrentDrawContractRecord& r) -> uint64_t {
    uint64_t h = bit::fnv1a_init();
    h = bit::fnv1a_iter(h, uint64_t(reinterpret_cast<uintptr_t>(r.renderablePart)));
    h = bit::fnv1a_iter(h, r.layerIndex);
    h = bit::fnv1a_iter(h, r.payloadWord108);
    h = bit::fnv1a_iter(h, r.payloadWord11C);
    return h;
  };
  if (options.maxRecords == 0u)
    unlimitedDedupeIndex.reserve(kContractCacheSize);

  auto considerRecord = [&](const CurrentDrawContractRecord& record) {
    if (!SnapshotRecordVisibleEnough(record, options.minVisibleFrameSerial))
      return;
    if (options.readyOnly && !RecordHasLocalPaletteSnapshot(record))
      return;
    if (options.maxRecords != 0u &&
        SnapshotPriorityOf(record, options.unitsOnly) == 0u)
      return;
    if (options.maxRecords == 0u) {
      const uint64_t dedupeKey = unlimitedDedupeKey(record);
      const auto duplicate = unlimitedDedupeIndex.find(dedupeKey);
      if (duplicate != unlimitedDedupeIndex.end() &&
          duplicate->second < out.size()) {
        CurrentDrawContractRecord& existing = out[duplicate->second];
        if (betterRecord(record, existing))
          existing = record;
        return;
      }
      unlimitedDedupeIndex.emplace(dedupeKey, out.size());
      out.push_back(record);
      return;
    }

    auto duplicate = std::find_if(
        out.begin(), out.end(), [&](const CurrentDrawContractRecord& existing) {
          // 使用 part-slice key 去重：同 renderablePart 的不同 layer/slice 
          // 不再互相覆盖
          return existing.renderablePart == record.renderablePart &&
                 existing.layerIndex == record.layerIndex &&
                 existing.payloadWord108 == record.payloadWord108 &&
                 existing.payloadWord11C == record.payloadWord11C;
        });
    if (duplicate != out.end()) {
      if (!betterRecord(record, *duplicate))
        return;
      out.erase(duplicate);
    }

    auto insertAt = std::find_if(
        out.begin(), out.end(), [&](const CurrentDrawContractRecord& existing) {
          return betterRecord(record, existing);
        });
    out.insert(insertAt, record);
    if (out.size() > size_t(options.maxRecords))
      out.pop_back();
  };

  for (const auto& record : g_currentDrawContractCache)
    considerRecord(record);

  if (GlobalCurrentDrawPublishEnabled()) {
    std::lock_guard<std::mutex> lock(g_publishedCurrentDrawMutex);
    if (options.pruneOlderThanMinVisibleFrame)
      PrunePublishedCurrentDrawContractsLocked(options.minVisibleFrameSerial);

    const auto& source =
        options.readyOnly ? g_publishedCurrentDrawReadyByPart
                          : g_publishedCurrentDrawByPart;
    for (const auto& [_, record] : source)
      considerRecord(record);
  }
  return out;
}

CurrentDrawResolveStatus ResolveCurrentDrawAuthoritativeSample(
    void* renderablePart,
    uint32_t vertexCount,
    uint64_t expectedVisibleFrameSerial,
    CurrentDrawAuthoritativeSample& out) {
  out = {};
  if (!QueryCurrentDrawContract(renderablePart, out.contract)) {
    out.status = CurrentDrawResolveStatus::MissingContract;
    return out.status;
  }

  if (expectedVisibleFrameSerial != 0u && out.contract.visibleFrameSerial != 0u &&
      out.contract.visibleFrameSerial < expectedVisibleFrameSerial) {
    g_lastMissReason.store(uint32_t(CurrentDrawMissReason::StaleVisibleFrame),
                           std::memory_order_relaxed);
    out = {};
    out.status = CurrentDrawResolveStatus::StaleVisibleFrame;
    return out.status;
  }

  QueryCurrentDrawContractCapturedPalette(renderablePart, out.palette,
                                          out.paletteCount);
  if (out.paletteCount != 0u && out.palette.size() >= out.paletteCount) {
    if (out.palette.size() > out.paletteCount)
      out.palette.resize(out.paletteCount);
    out.paletteHash =
        bit::fnv1a_hash(reinterpret_cast<const uint8_t*>(out.palette.data()),
                        out.palette.size() * sizeof(Matrix4));
    // Phase 1：从 thread-local snapshot 读取 provenance。
    const size_t snapshotSlot =
        (uintptr_t(renderablePart) >> 4u) % kPaletteSnapshotCacheSize;
    const auto& snapshot = g_paletteSnapshotCache[snapshotSlot];
    if (snapshot.renderablePart == renderablePart)
      out.paletteProvenance = snapshot.paletteProvenance;
  }

  if (!out.paletteReady()) {
    out.status = CurrentDrawResolveStatus::MissingPalette;
    return out.status;
  }

  if (!DecodeCurrentDrawGroupSlots(out.contract, vertexCount, out.paletteCount,
                                   out.groupSlots, out.groupHash)) {
    out.status = CurrentDrawResolveStatus::MissingGroupSlots;
    return out.status;
  }

  out.stableGroupHash =
      ComputeStableGroupContentHash(out.contract, out.groupSlots);
  out.status = CurrentDrawResolveStatus::Ready;
  return out.status;
}

CurrentDrawResolveStatus ResolveCurrentDrawAuthoritativeSampleFromRecord(
    const CurrentDrawContractRecord& record,
    uint32_t vertexCount,
    uint64_t expectedVisibleFrameSerial,
    CurrentDrawAuthoritativeSample& out) {
  out = {};
  out.contract = record;
  out.contract.known =
      record.known || (record.renderablePart != nullptr &&
                       record.meshPayloadPtr != nullptr);
  if (!out.contract.known) {
    out.status = CurrentDrawResolveStatus::MissingContract;
    return out.status;
  }

  if (expectedVisibleFrameSerial != 0u &&
      out.contract.visibleFrameSerial != 0u &&
      out.contract.visibleFrameSerial < expectedVisibleFrameSerial) {
    g_lastMissReason.store(uint32_t(CurrentDrawMissReason::StaleVisibleFrame),
                           std::memory_order_relaxed);
    out = {};
    out.status = CurrentDrawResolveStatus::StaleVisibleFrame;
    return out.status;
  }

  DecodeCapturedPaletteForRecord(out.contract, out.palette, out.paletteCount,
                                 true);
  if (out.paletteCount != 0u && out.palette.size() >= out.paletteCount) {
    if (out.palette.size() > out.paletteCount)
      out.palette.resize(out.paletteCount);
    out.paletteHash =
        bit::fnv1a_hash(reinterpret_cast<const uint8_t*>(out.palette.data()),
                        out.palette.size() * sizeof(Matrix4));
  }

  if (!out.paletteReady()) {
    out.status = CurrentDrawResolveStatus::MissingPalette;
    return out.status;
  }

  if (!DecodeCurrentDrawGroupSlots(out.contract, vertexCount, out.paletteCount,
                                   out.groupSlots, out.groupHash)) {
    out.status = CurrentDrawResolveStatus::MissingGroupSlots;
    return out.status;
  }

  out.stableGroupHash =
      ComputeStableGroupContentHash(out.contract, out.groupSlots);
  out.status = CurrentDrawResolveStatus::Ready;
  return out.status;
}

// Phase 7.35：submit 端观察到 palette 时间滞后时记录一次分桶。
// 分桶阈值见 header 的注释；这里用 std::atomic 的 fetch_add relaxed
// 写入，不保证跨 counter 原子一致（汇报时会在同一帧 summary 内采样，
// 单样本可能落在不同分桶上但对分布统计几乎没影响）。
// fetch_max CAS 循环保证 LagMax 单调递增。
void NoteSubmitPaletteFrameLag(uint32_t recordRenderFrameIndex) {
  const uint64_t current =
      dxvk::war3::state::RenderState::instance().getFrameIndex();
  // record 没有被 publish（第一次见到），视为 Lag0。
  uint64_t lag = 0u;
  if (recordRenderFrameIndex != 0u && current >= recordRenderFrameIndex)
    lag = current - static_cast<uint64_t>(recordRenderFrameIndex);
  // 分桶写入。
  if (lag == 0u) {
    g_submitPaletteFrameLag0Count.fetch_add(1u, std::memory_order_relaxed);
  } else if (lag == 1u) {
    g_submitPaletteFrameLag1Count.fetch_add(1u, std::memory_order_relaxed);
  } else if (lag == 2u) {
    g_submitPaletteFrameLag2Count.fetch_add(1u, std::memory_order_relaxed);
  } else if (lag <= 5u) {
    g_submitPaletteFrameLag3To5Count.fetch_add(1u, std::memory_order_relaxed);
  } else {
    g_submitPaletteFrameLag6PlusCount.fetch_add(1u, std::memory_order_relaxed);
  }
  g_submitPaletteFrameLagSampleCount.fetch_add(1u,
                                               std::memory_order_relaxed);
  // 简单 fetch_max（不存在 std::atomic::fetch_max<uint64_t> 的原子版本），
  // 用 CAS 循环保证单调递增；采样率较低，竞争可忽略。
  uint64_t prev = g_submitPaletteFrameLagMax.load(std::memory_order_relaxed);
  while (lag > prev && !g_submitPaletteFrameLagMax.compare_exchange_weak(
                          prev, lag, std::memory_order_relaxed,
                          std::memory_order_relaxed)) {
    // 更新失败：prev 已被 CAS 更新为最新值，循环重试。
  }
}

namespace {
void NoteSubmitPaletteContentAgeBucket(uint64_t age) {
  if (age == 0u) {
    g_submitPaletteContentAgeLag0Count.fetch_add(1u,
                                                 std::memory_order_relaxed);
  } else if (age == 1u) {
    g_submitPaletteContentAgeLag1Count.fetch_add(1u,
                                                 std::memory_order_relaxed);
  } else if (age == 2u) {
    g_submitPaletteContentAgeLag2Count.fetch_add(1u,
                                                 std::memory_order_relaxed);
  } else if (age <= 5u) {
    g_submitPaletteContentAgeLag3To5Count.fetch_add(
        1u, std::memory_order_relaxed);
  } else {
    g_submitPaletteContentAgeLag6PlusCount.fetch_add(
        1u, std::memory_order_relaxed);
  }

  uint64_t prev =
      g_submitPaletteContentAgeMax.load(std::memory_order_relaxed);
  while (age > prev &&
         !g_submitPaletteContentAgeMax.compare_exchange_weak(
             prev, age, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}
} // namespace

void NoteSubmitPaletteContentAge(uint32_t contentFrameTag,
                                 uint32_t currentFrameTag) {
  if (contentFrameTag == 0u || currentFrameTag == 0u) {
    NoteSubmitPaletteContentAgeUnknown();
    return;
  }

  const uint64_t age = currentFrameTag >= contentFrameTag
                           ? uint64_t(currentFrameTag - contentFrameTag)
                           : 0u;
  g_submitPaletteContentAgeSampleCount.fetch_add(
      1u, std::memory_order_relaxed);
  NoteSubmitPaletteContentAgeBucket(age);
}

void NoteSubmitPaletteContentAgeUnknown() {
  g_submitPaletteContentAgeUnknownCount.fetch_add(
      1u, std::memory_order_relaxed);
  g_submitPaletteContentAgeSampleCount.fetch_add(
      1u, std::memory_order_relaxed);
}

// Phase 7.35：model_hook 在组装 summary 时每帧调用一次，把 Exact 查询的
// 命中/miss 分桶镜像过来。这样 submit 端的诊断 summary 一次拿全
// capture+submit 两端数据，不用跨模块查询两个 summary。
void PublishCaptureExactQueryCounters(uint64_t exactHitCount,
                                      uint64_t bestEffortHitCount,
                                      uint64_t slotOverflowMissCount,
                                      uint64_t invalidEntryMissCount,
                                      uint64_t frameTagMismatchMissCount,
                                      uint64_t shortResultMissCount) {
  g_paletteCaptureExactHitCount.store(exactHitCount,
                                      std::memory_order_relaxed);
  g_paletteCaptureBestEffortHitCount.store(bestEffortHitCount,
                                           std::memory_order_relaxed);
  g_paletteCaptureSlotOverflowMissCount.store(slotOverflowMissCount,
                                              std::memory_order_relaxed);
  g_paletteCaptureInvalidEntryMissCount.store(invalidEntryMissCount,
                                              std::memory_order_relaxed);
  g_paletteCaptureFrameTagMismatchMissCount.store(frameTagMismatchMissCount,
                                                  std::memory_order_relaxed);
  g_paletteCaptureShortResultMissCount.store(shortResultMissCount,
                                             std::memory_order_relaxed);
}

// Phase 7.35 路径 2：submit 端 live palette rebuild 计数器。
// 语义见 header 注释与 g_submitLiveRebuild* 定义处。
// 全部使用 relaxed fetch_add：单次 submit 开销 < 10ns，不需要内存序语义。
void NoteSubmitLiveRebuildAttempt() {
  g_submitLiveRebuildAttemptCount.fetch_add(1u, std::memory_order_relaxed);
}

void NoteSubmitLiveRebuildHit() {
  g_submitLiveRebuildHitCount.fetch_add(1u, std::memory_order_relaxed);
}

void NoteSubmitLiveRebuildMiss() {
  g_submitLiveRebuildMissCount.fetch_add(1u, std::memory_order_relaxed);
}

void NoteSubmitLiveRebuildApplied() {
  g_submitLiveRebuildAppliedCount.fetch_add(1u, std::memory_order_relaxed);
}

} // namespace dxvk::war3::render
