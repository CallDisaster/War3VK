// war3_perf_monitor.cpp - War3 性能监控器实现（增强版）

#include "war3_perf_monitor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <functional>
#include <string_view>

#include "../hooks/war3_hook_lifecycle.h"
#include "../core/war3_runtime_profile.h"
#include "../model/war3_model_registry.h"
#include "../render/war3_shadow_object_registry.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../../util/util_env.h"
namespace dxvk::war3 {

using Clock = std::chrono::steady_clock;

static double toMs(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

static bool startsWith(const std::string &text, const char *prefix) {
  return text.rfind(prefix, 0) == 0;
}

static bool endsWith(const std::string &text, const char *suffix) {
  if (!suffix)
    return false;
  const size_t suffixLen = std::strlen(suffix);
  if (suffixLen == 0 || text.size() < suffixLen)
    return false;
  return text.compare(text.size() - suffixLen, suffixLen, suffix) == 0;
}

// 判定“空转/等待”段：用于将主循环等待时间与有效计算时间分离展示。
static bool isIdleWaitSectionPath(const std::string &path) {
  if (path == "War3MainLoop/Engine/WaitGate")
    return true;
  if (path == "War3MainLoop/Engine/SleepGate")
    return true;
  if (path == "War3MainLoop/Engine/SleepGateInner")
    return true;
  if (startsWith(path, "War3MainLoop/Wait/"))
    return true;
  return false;
}

static std::string getWarVkBaseDir() {
  char exePath[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) <= 0)
    return {};
  std::string exeDir(exePath);
  size_t lastSlash = exeDir.find_last_of("\\/");
  if (lastSlash == std::string::npos)
    return {};
  exeDir = exeDir.substr(0, lastSlash + 1);
  return exeDir + "WarVK\\";
}

static uint64_t combineFileTimes100ns(const FILETIME& kernel,
                                      const FILETIME& user) {
  ULARGE_INTEGER k = {};
  k.LowPart = kernel.dwLowDateTime;
  k.HighPart = kernel.dwHighDateTime;
  ULARGE_INTEGER u = {};
  u.LowPart = user.dwLowDateTime;
  u.HighPart = user.dwHighDateTime;
  return k.QuadPart + u.QuadPart;
}

static void decrementPendingJobs(std::atomic<uint32_t>& value) {
  uint32_t current = value.load(std::memory_order_relaxed);
  while (current > 0 &&
         !value.compare_exchange_weak(current, current - 1,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
  }
}

// ============================================================================
// ScopedSection
// ============================================================================

War3PerfMonitor::ScopedSection::ScopedSection(War3PerfMonitor *monitor,
                                              uint32_t id,
                                              const Rc<DxvkCommandList> &ctx)
    : m_monitor(monitor), m_id(id), m_cpuStart(Clock::now()), m_ctx(ctx) {
  if (m_monitor) {
    m_gpuStart = m_monitor->writeTimestamp(ctx);
  }
}

War3PerfMonitor::ScopedSection::~ScopedSection() {
  if (!m_monitor)
    return;

  const auto cpuEnd = Clock::now();
  const double cpuMs = toMs(cpuEnd - m_cpuStart);
  Rc<DxvkGpuQuery> gpuEnd = m_monitor->writeTimestamp(m_ctx);
  m_monitor->submitSample(m_id, cpuMs, std::move(m_gpuStart),
                          std::move(gpuEnd));
}

War3PerfMonitor::ScopedSection::ScopedSection(ScopedSection &&other) noexcept
    : m_monitor(other.m_monitor), m_id(other.m_id),
      m_cpuStart(other.m_cpuStart), m_gpuStart(std::move(other.m_gpuStart)),
      m_ctx(std::move(other.m_ctx)) {
  other.m_monitor = nullptr;
}

War3PerfMonitor::ScopedSection &
War3PerfMonitor::ScopedSection::operator=(ScopedSection &&other) noexcept {
  if (this != &other) {
    m_monitor = other.m_monitor;
    m_id = other.m_id;
    m_cpuStart = other.m_cpuStart;
    m_gpuStart = std::move(other.m_gpuStart);
    m_ctx = std::move(other.m_ctx);
    other.m_monitor = nullptr;
  }
  return *this;
}

// ============================================================================
// FrameGuard
// ============================================================================

War3PerfMonitor::FrameGuard::FrameGuard(War3PerfMonitor *monitor)
    : m_monitor(monitor) {
  if (m_monitor) {
    m_monitor->beginFrame();
  }
}

War3PerfMonitor::FrameGuard::~FrameGuard() {
  if (m_monitor) {
    m_monitor->endFrame();
  }
}

// ============================================================================
// ScopedCpuScope
// ============================================================================

War3PerfMonitor::ScopedCpuScope::ScopedCpuScope(War3PerfMonitor *monitor,
                                                const char *name)
    : m_monitor(monitor) {
  if (m_monitor) {
    m_monitor->pushScope(name);
  }
}

War3PerfMonitor::ScopedCpuScope::~ScopedCpuScope() {
  if (m_monitor) {
    m_monitor->popScope();
  }
}

War3PerfMonitor::ScopedCpuScope::ScopedCpuScope(ScopedCpuScope &&other) noexcept
    : m_monitor(other.m_monitor) {
  other.m_monitor = nullptr;
}

War3PerfMonitor::ScopedCpuScope &
War3PerfMonitor::ScopedCpuScope::operator=(ScopedCpuScope &&other) noexcept {
  if (this != &other) {
    m_monitor = other.m_monitor;
    other.m_monitor = nullptr;
  }
  return *this;
}

// ============================================================================
// War3PerfMonitor 核心
// ============================================================================

War3PerfMonitor &War3PerfMonitor::instance() {
  static War3PerfMonitor s_instance;
  return s_instance;
}

War3PerfMonitor::War3PerfMonitor()
    : m_lastReport(Clock::now()), m_lastAutoExport(Clock::now()) {
  const std::string historyFrames =
      env::getEnvVar("DXVK_WAR3_PERF_HISTORY_FRAMES");
  if (!historyFrames.empty()) {
    const int frames = std::max(0, std::atoi(historyFrames.c_str()));
    if (frames > 0)
      m_maxHistorySize = static_cast<size_t>(frames);
  } else {
    const std::string historySec = env::getEnvVar("DXVK_WAR3_PERF_HISTORY_SEC");
    if (!historySec.empty()) {
      const int sec = std::max(0, std::atoi(historySec.c_str()));
      if (sec > 0) {
        const int maxFrames = std::clamp(sec * 120, 600, 200000);
        m_maxHistorySize = static_cast<size_t>(maxFrames);
      }
    }
  }

  const std::string pendingMax = env::getEnvVar("DXVK_WAR3_PERF_PENDING_MAX");
  if (!pendingMax.empty()) {
    const int maxPending = std::max(0, std::atoi(pendingMax.c_str()));
    if (maxPending > 0) {
      m_maxPendingSamples = static_cast<size_t>(maxPending);
    }
  }

  // 自动录制开关：便于无人值守压测/自动化回归。
  const std::string autoRecord =
      env::getEnvVar("DXVK_WAR3_PERF_RECORD_ON_START");
  if (!autoRecord.empty() &&
      std::strtoul(autoRecord.c_str(), nullptr, 10) != 0) {
    m_recording.store(true, std::memory_order_relaxed);
  }

  // 周期自动导出（秒）：用于无人值守自动化采样。
  const std::string autoExportSec =
      env::getEnvVar("DXVK_WAR3_PERF_AUTO_EXPORT_SEC");
  if (!autoExportSec.empty()) {
    const int sec = std::max(0, std::atoi(autoExportSec.c_str()));
    if (sec > 0) {
      m_autoExportInterval = std::chrono::seconds(sec);
    }
  }
}

void War3PerfMonitor::noteShadowBudgetFrame(
    const War3ShadowCaptureStats& stats) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  auto& agg = m_shadowBudgetAggregate;
  agg.framesObserved++;
  agg.framesBudgetExceeded += stats.budgetExceeded != 0 ? 1u : 0u;
  agg.totalBudgetBytes += stats.fallbackBudgetBytes;
  agg.totalUsedBytes += stats.fallbackBudgetUsedBytes;
  agg.maxBudgetBytes =
      (std::max)(agg.maxBudgetBytes, stats.fallbackBudgetBytes);
  agg.maxUsedBytes =
      (std::max)(agg.maxUsedBytes, stats.fallbackBudgetUsedBytes);
  agg.totalArenaBytes += stats.fallbackArenaBytes;
  agg.maxArenaBytes =
      (std::max)(agg.maxArenaBytes, stats.fallbackArenaBytes);
  agg.legacyCaptured += stats.captured;
  agg.skippedUpload += stats.skippedMissingPerDrawUpload;
  agg.arenaRejectUnsupported += stats.skippedPosFormat;
  agg.arenaRejectValidation += stats.skippedNoPosition;
  agg.arenaRejectUninitialized += stats.skippedNoZTest;
  agg.arenaRejectOverflow += stats.skippedVertexShader;
  agg.skippedCasterCap += stats.skippedCasterCap;
  agg.skippedDistanceCull += stats.skippedPositionT;
  agg.skippedFreezeBudget += stats.skippedFreezeBudget;
  agg.skippedPriorityBudget += stats.skippedPriorityBudget;
  agg.degradedAlphaBudget += stats.degradedAlphaBudget;
  agg.uniqueGeometryCount += stats.uniqueGeometryCount;
  agg.uniqueInstanceableGeometryCount += stats.persistentGeometryCount;
  agg.duplicateGeometryInstances += stats.duplicateGeometryInstances;
  agg.reuseEligibleDuplicates += stats.reuseEligibleDuplicates;
  agg.potentialFreezeReuseHits += stats.potentialFreezeReuseHits;
  agg.staticPersistentCount += stats.staticPersistentCount;
  agg.dynamicPoseCount += stats.dynamicPoseCount;
  agg.dynamicSkinnedOutputCount += stats.dynamicSkinnedOutputCount;
  agg.fallbackDrawCount += stats.fallbackDrawCount;
  agg.fallbackDrawCountTerrain += stats.fallbackDrawCountTerrain;
  agg.fallbackDrawCountWorldObject += stats.fallbackDrawCountWorldObject;
  agg.fallbackDrawCountUnitObject += stats.fallbackDrawCountUnitObject;
  agg.objectFallbackDrawCount +=
      stats.fallbackDrawCountWorldObject + stats.fallbackDrawCountUnitObject;
  agg.semanticBridgeHit += stats.semanticBridgeHit;
  agg.semanticBridgeMiss += stats.semanticBridgeMiss;
  agg.semanticBridgeBypassed += stats.semanticBridgeBypassed;
  agg.semanticSceneSubmitted += stats.semanticSceneSubmitted;
  agg.semanticSceneSubmittedUnit += stats.semanticSceneSubmittedUnit;
  agg.semanticSceneSubmittedSkinned += stats.semanticSceneSubmittedSkinned;
  agg.semanticSceneSubmittedSkinnedNonUnitResolvedCount +=
      stats.semanticSceneSubmittedSkinnedNonUnitResolvedCount;
  agg.semanticSceneSubmittedSkinnedUnknownPacketKindCount +=
      stats.semanticSceneSubmittedSkinnedUnknownPacketKindCount;
  agg.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount +=
      stats.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount;
  agg.semanticSceneSubmittedSkinnedGroupNonZeroCount +=
      stats.semanticSceneSubmittedSkinnedGroupNonZeroCount;
  agg.semanticSceneSubmittedSkinnedTransparentQueueCount +=
      stats.semanticSceneSubmittedSkinnedTransparentQueueCount;
  agg.semanticSceneSubmittedSkinnedMissingUnitPtrCount +=
      stats.semanticSceneSubmittedSkinnedMissingUnitPtrCount;
  agg.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount +=
      stats.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount;
  agg.semanticSceneSubmittedBuilding += stats.semanticSceneSubmittedBuilding;
  agg.semanticSceneSubmittedDestructible +=
      stats.semanticSceneSubmittedDestructible;
  agg.semanticSceneSubmittedCutout += stats.semanticSceneSubmittedCutout;
  agg.semanticSceneSubmittedAlphaBlend +=
      stats.semanticSceneSubmittedAlphaBlend;
  agg.semanticSceneMaterialObservedCutoutCount +=
      stats.semanticSceneMaterialObservedCutoutCount;
  agg.semanticSceneMaterialObservedAlphaBlendCount +=
      stats.semanticSceneMaterialObservedAlphaBlendCount;
  agg.semanticSceneRejectedCutoutSkinnedContract +=
      stats.semanticSceneRejectedCutoutSkinnedContract;
  agg.semanticSceneRejectedAlphaBlendSkinnedContract +=
      stats.semanticSceneRejectedAlphaBlendSkinnedContract;
  agg.semanticSceneRejectedCutoutGeometry +=
      stats.semanticSceneRejectedCutoutGeometry;
  agg.semanticSceneRejectedAlphaBlendGeometry +=
      stats.semanticSceneRejectedAlphaBlendGeometry;
  agg.semanticSceneRejectedCutoutVisualPolicy +=
      stats.semanticSceneRejectedCutoutVisualPolicy;
  agg.semanticSceneRejectedAlphaBlendVisualPolicy +=
      stats.semanticSceneRejectedAlphaBlendVisualPolicy;
  agg.semanticSceneMaterialLayerContractResolvedCount +=
      stats.semanticSceneMaterialLayerContractResolvedCount;
  agg.semanticSceneMaterialLayerContractFailedCount +=
      stats.semanticSceneMaterialLayerContractFailedCount;
  agg.semanticSceneMaterialBlendMode0Count +=
      stats.semanticSceneMaterialBlendMode0Count;
  agg.semanticSceneMaterialBlendMode1Count +=
      stats.semanticSceneMaterialBlendMode1Count;
  agg.semanticSceneMaterialBlendMode2PlusCount +=
      stats.semanticSceneMaterialBlendMode2PlusCount;
  agg.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount +=
      stats.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount;
  agg.semanticSceneLivePaletteRefreshAttemptCount +=
      stats.semanticSceneLivePaletteRefreshAttemptCount;
  agg.semanticSceneLivePaletteRefreshHitCount +=
      stats.semanticSceneLivePaletteRefreshHitCount;
  agg.semanticSceneLivePaletteRefreshMissCount +=
      stats.semanticSceneLivePaletteRefreshMissCount;
  agg.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount +=
      stats.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount;
  agg.semanticScenePaletteOverrideNoComposeCount +=
      stats.semanticScenePaletteOverrideNoComposeCount;
  agg.semanticScenePaletteOverrideWouldComposeCount +=
      stats.semanticScenePaletteOverrideWouldComposeCount;
  agg.semanticScenePalettePacketWorldComposeCount +=
      stats.semanticScenePalettePacketWorldComposeCount;
  agg.semanticSceneLivePaletteMotionSampleCount +=
      stats.semanticSceneLivePaletteMotionSampleCount;
  agg.semanticSceneLivePaletteMotionNewRuntimeCount +=
      stats.semanticSceneLivePaletteMotionNewRuntimeCount;
  agg.semanticSceneLivePaletteMotionRawChangedCount +=
      stats.semanticSceneLivePaletteMotionRawChangedCount;
  agg.semanticSceneLivePaletteMotionRawStableCount +=
      stats.semanticSceneLivePaletteMotionRawStableCount;
  agg.semanticSceneLivePaletteMotionGroupChangedCount +=
      stats.semanticSceneLivePaletteMotionGroupChangedCount;
  agg.semanticSceneLivePaletteMotionGroupStableCount +=
      stats.semanticSceneLivePaletteMotionGroupStableCount;
  agg.semanticSceneDrawTimePoseAttemptCount +=
      stats.semanticSceneDrawTimePoseAttemptCount;
  agg.semanticSceneDrawTimePosePublishedCount +=
      stats.semanticSceneDrawTimePosePublishedCount;
  agg.semanticSceneDrawTimePoseRejectUiOrEffectCount +=
      stats.semanticSceneDrawTimePoseRejectUiOrEffectCount;
  agg.semanticSceneDrawTimePoseRejectVertexShaderCount +=
      stats.semanticSceneDrawTimePoseRejectVertexShaderCount;
  agg.semanticSceneDrawTimePoseRejectNoVertexBlendCount +=
      stats.semanticSceneDrawTimePoseRejectNoVertexBlendCount;
  agg.semanticSceneDrawTimePoseRejectNoContextCount +=
      stats.semanticSceneDrawTimePoseRejectNoContextCount;
  agg.semanticSceneDrawTimePoseRejectNoRuntimeModelCount +=
      stats.semanticSceneDrawTimePoseRejectNoRuntimeModelCount;
  agg.semanticSceneDrawTimePoseDedupedCount +=
      stats.semanticSceneDrawTimePoseDedupedCount;
  agg.semanticSceneDrawTimePoseChangedCount +=
      stats.semanticSceneDrawTimePoseChangedCount;
  agg.semanticSceneDrawTimePoseStableCount +=
      stats.semanticSceneDrawTimePoseStableCount;
  agg.semanticSceneSubmittedPaletteMotionSampleCount +=
      stats.semanticSceneSubmittedPaletteMotionSampleCount;
  agg.semanticSceneSubmittedPaletteMotionNewRuntimeCount +=
      stats.semanticSceneSubmittedPaletteMotionNewRuntimeCount;
  agg.semanticSceneSubmittedPaletteMotionChangedCount +=
      stats.semanticSceneSubmittedPaletteMotionChangedCount;
  agg.semanticSceneSubmittedPaletteMotionStableCount +=
      stats.semanticSceneSubmittedPaletteMotionStableCount;
  agg.semanticSceneSkinnedDynamicIndexSliceCount +=
      stats.semanticSceneSkinnedDynamicIndexSliceCount;
  agg.semanticSceneSubmittedOwnedGroupSlots +=
      stats.semanticSceneSubmittedOwnedGroupSlots;
  agg.semanticSceneCurrentDrawContractKnownCount +=
      stats.semanticSceneCurrentDrawContractKnownCount;
  agg.semanticSceneCurrentDrawPaletteReadyCount +=
      stats.semanticSceneCurrentDrawPaletteReadyCount;
  agg.semanticSceneCurrentDrawGroupSlotReadyCount +=
      stats.semanticSceneCurrentDrawGroupSlotReadyCount;
  agg.semanticSceneCurrentDrawResolveReadyCount +=
      stats.semanticSceneCurrentDrawResolveReadyCount;
  agg.semanticSceneCurrentDrawMissNoContract +=
      stats.semanticSceneCurrentDrawMissNoContract;
  agg.semanticSceneCurrentDrawMissNoPalette +=
      stats.semanticSceneCurrentDrawMissNoPalette;
  agg.semanticSceneCurrentDrawMissNoGroupSlots +=
      stats.semanticSceneCurrentDrawMissNoGroupSlots;
  agg.semanticSceneCurrentDrawMissStaleVisibleFrame +=
      stats.semanticSceneCurrentDrawMissStaleVisibleFrame;
  agg.semanticSceneCurrentDrawResolveReadyRejectedCount +=
      stats.semanticSceneCurrentDrawResolveReadyRejectedCount;
  agg.semanticSceneCanonicalReadyCount +=
      stats.semanticSceneCanonicalReadyCount;
  agg.semanticSceneCanonicalReadyCutoutCount +=
      stats.semanticSceneCanonicalReadyCutoutCount;
  agg.semanticSceneCanonicalReadyAlphaBlendCount +=
      stats.semanticSceneCanonicalReadyAlphaBlendCount;
  agg.semanticSceneCanonicalRejectNoStableIdentity +=
      stats.semanticSceneCanonicalRejectNoStableIdentity;
  agg.semanticSceneCanonicalRejectNoMesh +=
      stats.semanticSceneCanonicalRejectNoMesh;
  agg.semanticSceneCanonicalRejectNoWorldTransform +=
      stats.semanticSceneCanonicalRejectNoWorldTransform;
  agg.semanticSceneCanonicalRejectNoPalette +=
      stats.semanticSceneCanonicalRejectNoPalette;
  agg.semanticSceneCanonicalRejectNoSlotContract +=
      stats.semanticSceneCanonicalRejectNoSlotContract;
  agg.semanticSceneCanonicalRejectStaleProducer +=
      stats.semanticSceneCanonicalRejectStaleProducer;
  agg.semanticSceneCanonicalRejectInvalidVertexIndex +=
      stats.semanticSceneCanonicalRejectInvalidVertexIndex;
  agg.semanticSceneCanonicalRejectExplicitBlendIncomplete +=
      stats.semanticSceneCanonicalRejectExplicitBlendIncomplete;
  agg.semanticSceneCanonicalRejectAfterReadyCount +=
      stats.semanticSceneCanonicalRejectAfterReadyCount;
  agg.semanticSceneSubmittedExplicitBlendContract +=
      stats.semanticSceneSubmittedExplicitBlendContract;
  agg.semanticSceneSubmittedSingleMatrixGroupSkinning +=
      stats.semanticSceneSubmittedSingleMatrixGroupSkinning;
  agg.semanticSceneSubmittedMultiGroupSlotSkinning +=
      stats.semanticSceneSubmittedMultiGroupSlotSkinning;
  if (stats.semanticSceneSkinnedMinUniqueGroupSlots != 0u &&
      (agg.semanticSceneSkinnedMinUniqueGroupSlots == 0u ||
       stats.semanticSceneSkinnedMinUniqueGroupSlots <
           agg.semanticSceneSkinnedMinUniqueGroupSlots)) {
    agg.semanticSceneSkinnedMinUniqueGroupSlots =
        stats.semanticSceneSkinnedMinUniqueGroupSlots;
  }
  agg.semanticSceneSkinnedMaxUniqueGroupSlots = std::max<uint64_t>(
      agg.semanticSceneSkinnedMaxUniqueGroupSlots,
      stats.semanticSceneSkinnedMaxUniqueGroupSlots);
  agg.semanticSceneSkinnedGroupSlotsUnique1Count +=
      stats.semanticSceneSkinnedGroupSlotsUnique1Count;
  agg.semanticSceneSkinnedGroupSlotsUnique2To4Count +=
      stats.semanticSceneSkinnedGroupSlotsUnique2To4Count;
  agg.semanticSceneSkinnedGroupSlotsUnique5To8Count +=
      stats.semanticSceneSkinnedGroupSlotsUnique5To8Count;
  agg.semanticSceneSkinnedGroupSlotsUnique9To16Count +=
      stats.semanticSceneSkinnedGroupSlotsUnique9To16Count;
  agg.semanticSceneSkinnedGroupSlotsUnique17PlusCount +=
      stats.semanticSceneSkinnedGroupSlotsUnique17PlusCount;
  agg.semanticSceneExplicitBlendUnavailableCurrentDraw +=
      stats.semanticSceneExplicitBlendUnavailableCurrentDraw;
  agg.semanticSceneSkinnedFullIndexFallbackCount +=
      stats.semanticSceneSkinnedFullIndexFallbackCount;
  agg.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount +=
      stats.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount;
  agg.semanticSceneSubmittedFrameLocal +=
      stats.semanticSceneSubmittedFrameLocal;
  agg.semanticSceneSubmittedPersistent +=
      stats.semanticSceneSubmitted > stats.semanticSceneSubmittedFrameLocal
          ? (stats.semanticSceneSubmitted -
             stats.semanticSceneSubmittedFrameLocal)
          : 0u;
  agg.semanticScenePopulateAttemptCount +=
      stats.semanticScenePopulateAttemptCount;
  agg.semanticScenePopulateUnitsOnlyCount +=
      stats.semanticScenePopulateUnitsOnlyCount;
  agg.semanticScenePopulateLastReturnReason =
      stats.semanticScenePopulateLastReturnReason;
  agg.semanticScenePopulateLastProducerPublishAttemptDelta =
      stats.semanticScenePopulateLastProducerPublishAttemptDelta;
  agg.semanticScenePopulateLastProducerPublishReadyDelta =
      stats.semanticScenePopulateLastProducerPublishReadyDelta;
  agg.semanticScenePopulateLastProducerQueryAttemptDelta =
      stats.semanticScenePopulateLastProducerQueryAttemptDelta;
  agg.semanticScenePopulateLastProducerQueryHitDelta =
      stats.semanticScenePopulateLastProducerQueryHitDelta;
  agg.semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta =
      stats.semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta;
  agg.semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta =
      stats.semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta;
  agg.semanticScenePopulateLastProducerGroupDecodeAttemptDelta =
      stats.semanticScenePopulateLastProducerGroupDecodeAttemptDelta;
  agg.semanticScenePopulateLastProducerGroupDecodeHitDelta =
      stats.semanticScenePopulateLastProducerGroupDecodeHitDelta;
  agg.semanticSceneDirectCurrentDrawRecordCount +=
      stats.semanticSceneDirectCurrentDrawRecordCount;
  agg.semanticSceneDirectCurrentDrawBuiltPacketCount +=
      stats.semanticSceneDirectCurrentDrawBuiltPacketCount;
  agg.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount +=
      stats.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount;
  agg.semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount +=
      stats.semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount;
  agg.semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount +=
      stats.semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount;
  agg.semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount +=
      stats
          .semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount;
  // Phase 7.2: flicker diagnostics + reconciliation
  agg.semanticSceneDirectLastRawRecordCount = std::max(
      agg.semanticSceneDirectLastRawRecordCount,
      uint64_t(stats.semanticSceneDirectLastRawRecordCount));
  agg.semanticSceneDirectLastEligibleRecordCount = std::max(
      agg.semanticSceneDirectLastEligibleRecordCount,
      uint64_t(stats.semanticSceneDirectLastEligibleRecordCount));
  agg.semanticSceneDirectLastSubmittedRecordCount = std::max(
      agg.semanticSceneDirectLastSubmittedRecordCount,
      uint64_t(stats.semanticSceneDirectLastSubmittedRecordCount));
  agg.semanticSceneDirectLastUniqueObjectCount = std::max(
      agg.semanticSceneDirectLastUniqueObjectCount,
      uint64_t(stats.semanticSceneDirectLastUniqueObjectCount));
  agg.semanticSceneDirectLastSubmittedObjectCount = std::max(
      agg.semanticSceneDirectLastSubmittedObjectCount,
      uint64_t(stats.semanticSceneDirectLastSubmittedObjectCount));
  agg.semanticSceneDirectLastRecordCapPartialObjectCount = std::max(
      agg.semanticSceneDirectLastRecordCapPartialObjectCount,
      uint64_t(stats.semanticSceneDirectLastRecordCapPartialObjectCount));
  agg.semanticSceneDirectLastScanCapPartialObjectCount = std::max(
      agg.semanticSceneDirectLastScanCapPartialObjectCount,
      uint64_t(stats.semanticSceneDirectLastScanCapPartialObjectCount));
  agg.semanticSceneDirectLastMinGeosetsPerObject = std::max(
      agg.semanticSceneDirectLastMinGeosetsPerObject,
      uint64_t(stats.semanticSceneDirectLastMinGeosetsPerObject));
  agg.semanticSceneDirectLastMaxGeosetsPerObject = std::max(
      agg.semanticSceneDirectLastMaxGeosetsPerObject,
      uint64_t(stats.semanticSceneDirectLastMaxGeosetsPerObject));
  agg.semanticSceneDirectLastSubmittedIdentityHash = stats.semanticSceneDirectLastSubmittedIdentityHash;
  agg.semanticSceneDirectIdentityChurnCount += stats.semanticSceneDirectIdentityChurnCount;
  agg.semanticSceneDirectRecordCapHitCount += stats.semanticSceneDirectRecordCapHitCount;
  agg.semanticSceneDirectRecordCapTruncatedRecordCount +=
      stats.semanticSceneDirectRecordCapTruncatedRecordCount;
  agg.semanticSceneDirectScanCapHitCount +=
      stats.semanticSceneDirectScanCapHitCount;
  agg.semanticSceneDirectObjectGroupedSubmitCount +=
      stats.semanticSceneDirectObjectGroupedSubmitCount;
  agg.semanticSceneDirectObjectGroupedSkipCount +=
      stats.semanticSceneDirectObjectGroupedSkipCount;
  agg.semanticSceneDirectRecordCapSkipObjectCount += stats.semanticSceneDirectRecordCapSkipObjectCount;
  agg.semanticSceneDirectRecordCapAppendFailCount += stats.semanticSceneDirectRecordCapAppendFailCount;
  agg.semanticSceneDirectSelectionLeaseActiveKeyCount = std::max(
      agg.semanticSceneDirectSelectionLeaseActiveKeyCount,
      uint64_t(stats.semanticSceneDirectSelectionLeaseActiveKeyCount));
  agg.semanticSceneDirectSelectionLeasePrunedKeyCount +=
      stats.semanticSceneDirectSelectionLeasePrunedKeyCount;
  agg.semanticSceneDirectSelectionLeaseSubmittedKeyCount +=
      stats.semanticSceneDirectSelectionLeaseSubmittedKeyCount;
  agg.semanticSceneDirectStickyFillBudgetRecordCount = std::max(
      agg.semanticSceneDirectStickyFillBudgetRecordCount,
      uint64_t(stats.semanticSceneDirectStickyFillBudgetRecordCount));
  agg.semanticSceneDirectStickyFillAppendedCount +=
      stats.semanticSceneDirectStickyFillAppendedCount;
  agg.semanticSceneDirectStickyFillSubmittedCount +=
      stats.semanticSceneDirectStickyFillSubmittedCount;
  agg.semanticSceneDirectStickyFillMissedCount +=
      stats.semanticSceneDirectStickyFillMissedCount;
  agg.semanticSceneDirectPartLeaseRestoredCount +=
      stats.semanticSceneDirectPartLeaseRestoredCount;
  agg.semanticSceneDirectPartLeaseUpdatedCount +=
      stats.semanticSceneDirectPartLeaseUpdatedCount;
  agg.semanticSceneDirectPartLeaseExpiredCount +=
      stats.semanticSceneDirectPartLeaseExpiredCount;
  agg.semanticSceneDirectPartLeaseRejectedDynamicMeshCount +=
      stats.semanticSceneDirectPartLeaseRejectedDynamicMeshCount;
  agg.semanticSceneDirectPartLeaseRejectedNotSelfContainedCount +=
      stats.semanticSceneDirectPartLeaseRejectedNotSelfContainedCount;
  agg.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount +=
      stats.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount;
  agg.semanticSceneDirectPartLeaseRejectedSelfRenewCount +=
      stats.semanticSceneDirectPartLeaseRejectedSelfRenewCount;
  agg.semanticSceneDirectPartLeaseBudgetLimitCount +=
      stats.semanticSceneDirectPartLeaseBudgetLimitCount;
  agg.semanticSceneShadowManifestPartLeaseRestoredCount +=
      stats.semanticSceneShadowManifestPartLeaseRestoredCount;
  agg.semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount +=
      stats.semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount;
  agg.semanticSceneShadowManifestPartLeaseExpiredCount +=
      stats.semanticSceneShadowManifestPartLeaseExpiredCount;
  agg.semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount +=
      stats.semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount;
  agg.semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount +=
      stats.semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount;
  agg.semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount +=
      stats.semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount;
  agg
      .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount +=
      stats
          .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount;
  agg.semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount +=
      stats.semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount;
  agg.semanticSceneShadowManifestPartLeaseBudgetLimitCount +=
      stats.semanticSceneShadowManifestPartLeaseBudgetLimitCount;
  agg.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount +=
      stats.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount;
  agg.semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount +=
      stats.semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount;
  agg.semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount +=
      stats.semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount;
  agg.semanticSceneShadowManifestObjectCoreCompleteCount +=
      stats.semanticSceneShadowManifestObjectCoreCompleteCount;
  agg.semanticSceneShadowManifestObjectCoreIncompleteSkipCount +=
      stats.semanticSceneShadowManifestObjectCoreIncompleteSkipCount;
  agg.semanticSceneShadowManifestPartOmittedIncompleteCoreCount +=
      stats.semanticSceneShadowManifestPartOmittedIncompleteCoreCount;
  // Phase 7.25 core epoch planner 专属计数器聚合（累加语义，便于整测窗口对比）。
  agg.semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount;
  agg.semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount;
  agg.semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount;
  agg.semanticSceneShadowManifestObjectCoreEpochMissingPartCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochMissingPartCount;
  agg.semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount;
  // Phase 7.28：skinned palette content stability probe（累加 / max）。
  agg.semanticSceneSubmittedSkinnedPaletteSourceNoneCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceNoneCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount;
  agg.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount +=
      stats.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount;
  agg.semanticSceneSubmittedSkinnedPaletteHashChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteHashChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax = std::max(
      agg.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax,
      uint64_t(stats.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax));
  agg.semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax = std::max(
      agg.semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax,
      uint64_t(
          stats.semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax));
  agg.semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount +=
      stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount +=
      stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount +=
      stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteCountChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteCountChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount;
  agg.semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount +=
      stats.semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount;
  agg.semanticSceneDirectPaletteAttributionSnapshotHitCount +=
      stats.semanticSceneDirectPaletteAttributionSnapshotHitCount;
  agg.semanticSceneDirectPaletteCaptureTrustedSourceHitCount +=
      stats.semanticSceneDirectPaletteCaptureTrustedSourceHitCount;
  agg.semanticSceneDirectPaletteCaptureTrustedSourceMissCount +=
      stats.semanticSceneDirectPaletteCaptureTrustedSourceMissCount;
  // Phase 7.30 Step A：stale→live 过渡归因。
  agg.semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount;
  agg.semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount +=
      stats.semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount;
  agg.semanticSceneDirectManifestObjectCount = std::max(
      agg.semanticSceneDirectManifestObjectCount,
      uint64_t(stats.semanticSceneDirectManifestObjectCount));
  agg.semanticSceneDirectManifestObservedPartCount = std::max(
      agg.semanticSceneDirectManifestObservedPartCount,
      uint64_t(stats.semanticSceneDirectManifestObservedPartCount));
  agg.semanticSceneDirectManifestShadowEligiblePartCount = std::max(
      agg.semanticSceneDirectManifestShadowEligiblePartCount,
      uint64_t(stats.semanticSceneDirectManifestShadowEligiblePartCount));
  agg.semanticSceneDirectObjectCompleteEligibleCount = std::max(
      agg.semanticSceneDirectObjectCompleteEligibleCount,
      uint64_t(stats.semanticSceneDirectObjectCompleteEligibleCount));
  agg.semanticSceneDirectObjectIncompleteByScanCapCount +=
      stats.semanticSceneDirectObjectIncompleteByScanCapCount;
  agg.semanticSceneDirectObjectIncompleteByAlphaPolicyCount +=
      stats.semanticSceneDirectObjectIncompleteByAlphaPolicyCount;
  agg.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount +=
      stats.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount;
  agg.semanticSceneDirectObjectIncompleteByPacketBuildFailCount +=
      stats.semanticSceneDirectObjectIncompleteByPacketBuildFailCount;
  agg.semanticSceneDirectObjectIncompleteByAppendFailCount +=
      stats.semanticSceneDirectObjectIncompleteByAppendFailCount;
  agg.semanticSceneDirectSubmittedCompleteObjectCount = std::max(
      agg.semanticSceneDirectSubmittedCompleteObjectCount,
      uint64_t(stats.semanticSceneDirectSubmittedCompleteObjectCount));
  agg.semanticSceneDirectSubmittedPartialObjectCount = std::max(
      agg.semanticSceneDirectSubmittedPartialObjectCount,
      uint64_t(stats.semanticSceneDirectSubmittedPartialObjectCount));
  agg.semanticSceneDirectPreparedSliceAuthoritativeCount +=
      stats.semanticSceneDirectPreparedSliceAuthoritativeCount;
  agg.semanticSceneDirectPreparedSliceFallbackLayerIndexCount +=
      stats.semanticSceneDirectPreparedSliceFallbackLayerIndexCount;
  agg.semanticSceneDirectPreparedSliceMissingCount +=
      stats.semanticSceneDirectPreparedSliceMissingCount;
  agg.semanticScenePreparedProbeAttemptCount +=
      stats.semanticScenePreparedProbeAttemptCount;
  agg.semanticScenePreparedProbeContextReadyCount +=
      stats.semanticScenePreparedProbeContextReadyCount;
  agg.semanticScenePreparedProbeBackingReadableCount +=
      stats.semanticScenePreparedProbeBackingReadableCount;
  agg.semanticScenePreparedSliceRecordedCount +=
      stats.semanticScenePreparedSliceRecordedCount;
  agg.semanticScenePreparedSliceQueryAttemptCount +=
      stats.semanticScenePreparedSliceQueryAttemptCount;
  agg.semanticScenePreparedSliceQueryHitCount +=
      stats.semanticScenePreparedSliceQueryHitCount;
  agg.semanticScenePreparedSliceQueryMissCount +=
      stats.semanticScenePreparedSliceQueryMissCount;
  agg.semanticSceneShadowManifestObjectCount = std::max(
      agg.semanticSceneShadowManifestObjectCount,
      uint64_t(stats.semanticSceneShadowManifestObjectCount));
  agg.semanticSceneShadowManifestPartCount = std::max(
      agg.semanticSceneShadowManifestPartCount,
      uint64_t(stats.semanticSceneShadowManifestPartCount));
  agg.semanticSceneShadowManifestStableObjectCount = std::max(
      agg.semanticSceneShadowManifestStableObjectCount,
      uint64_t(stats.semanticSceneShadowManifestStableObjectCount));
  agg.semanticSceneShadowManifestNewObjectCount +=
      stats.semanticSceneShadowManifestNewObjectCount;
  agg.semanticSceneShadowManifestExpiredObjectCount +=
      stats.semanticSceneShadowManifestExpiredObjectCount;
  agg.semanticSceneShadowManifestFreshPartCount = std::max(
      agg.semanticSceneShadowManifestFreshPartCount,
      uint64_t(stats.semanticSceneShadowManifestFreshPartCount));
  agg.semanticSceneShadowManifestLeaseablePartCount = std::max(
      agg.semanticSceneShadowManifestLeaseablePartCount,
      uint64_t(stats.semanticSceneShadowManifestLeaseablePartCount));
  agg.semanticSceneShadowManifestPoseStalePartCount = std::max(
      agg.semanticSceneShadowManifestPoseStalePartCount,
      uint64_t(stats.semanticSceneShadowManifestPoseStalePartCount));
  agg.semanticSceneShadowManifestSliceStalePartCount = std::max(
      agg.semanticSceneShadowManifestSliceStalePartCount,
      uint64_t(stats.semanticSceneShadowManifestSliceStalePartCount));
  agg.semanticSceneShadowManifestExpiredPartCount +=
      stats.semanticSceneShadowManifestExpiredPartCount;
  agg.semanticSceneShadowManifestMultiSlicePartCount = std::max(
      agg.semanticSceneShadowManifestMultiSlicePartCount,
      uint64_t(stats.semanticSceneShadowManifestMultiSlicePartCount));
  agg.semanticSceneShadowManifestPayload11CChurnCount +=
      stats.semanticSceneShadowManifestPayload11CChurnCount;
  agg.semanticSceneShadowManifestRenderablePartChurnCount +=
      stats.semanticSceneShadowManifestRenderablePartChurnCount;
  agg.semanticSceneShadowManifestCModelPoseHitCount = std::max(
      agg.semanticSceneShadowManifestCModelPoseHitCount,
      uint64_t(stats.semanticSceneShadowManifestCModelPoseHitCount));
  agg.semanticSceneShadowManifestCModelPoseMissCount = std::max(
      agg.semanticSceneShadowManifestCModelPoseMissCount,
      uint64_t(stats.semanticSceneShadowManifestCModelPoseMissCount));
  agg.semanticSceneShadowManifestCModelPoseNoRuntimeCount = std::max(
      agg.semanticSceneShadowManifestCModelPoseNoRuntimeCount,
      uint64_t(stats.semanticSceneShadowManifestCModelPoseNoRuntimeCount));
  agg.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr =
      stats.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr;
  agg.semanticSceneShadowManifestCModelPoseLastMatrixCount =
      stats.semanticSceneShadowManifestCModelPoseLastMatrixCount;
  agg.semanticSceneShadowManifestCModelPoseLastMatrixHash =
      stats.semanticSceneShadowManifestCModelPoseLastMatrixHash;
  agg.semanticSceneSubmittedObjectJaccardMilli = std::max(
      agg.semanticSceneSubmittedObjectJaccardMilli,
      uint64_t(stats.semanticSceneSubmittedObjectJaccardMilli));
  agg.semanticSceneSubmittedPartJaccardMilli = std::max(
      agg.semanticSceneSubmittedPartJaccardMilli,
      uint64_t(stats.semanticSceneSubmittedPartJaccardMilli));
  agg.semanticSceneVisibleLookupPartLayerHitCount =
      stats.semanticSceneVisibleLookupPartLayerHitCount;
  agg.semanticSceneVisibleLookupSingleFallbackCount =
      stats.semanticSceneVisibleLookupSingleFallbackCount;
  agg.semanticSceneVisibleLookupMissCount =
      stats.semanticSceneVisibleLookupMissCount;
  agg.semanticSceneDirectMainWorldBackingNotCheckedCount +=
      stats.semanticSceneDirectMainWorldBackingNotCheckedCount;
  agg.semanticSceneDirectMainWorldBackingPassCount +=
      stats.semanticSceneDirectMainWorldBackingPassCount;
  agg.semanticSceneDirectMainWorldBackingFailNoRenderablePartCount +=
      stats.semanticSceneDirectMainWorldBackingFailNoRenderablePartCount;
  agg.semanticSceneDirectMainWorldBackingFailLookupMissCount +=
      stats.semanticSceneDirectMainWorldBackingFailLookupMissCount;
  agg.semanticSceneDirectMainWorldBackingFailNonMainQueueCount +=
      stats.semanticSceneDirectMainWorldBackingFailNonMainQueueCount;
  agg.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount +=
      stats.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount;
  agg.semanticSceneDirectMainWorldBackingFailIdentityMismatchCount +=
      stats.semanticSceneDirectMainWorldBackingFailIdentityMismatchCount;
  agg.semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount +=
      stats.semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount;
  agg.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount +=
      stats.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount;
  agg.semanticSceneDirectPaletteHashChurnCount += stats.semanticSceneDirectPaletteHashChurnCount;
  agg.semanticSceneDirectGroupHashChurnCount += stats.semanticSceneDirectGroupHashChurnCount;
  agg.semanticSceneDirectStableGroupHashChurnCount += stats.semanticSceneDirectStableGroupHashChurnCount;
  agg.semanticSceneDirectStream1PtrChurnCount += stats.semanticSceneDirectStream1PtrChurnCount;
  agg.semanticSceneDirectGeometrySourceHashChurnCount += stats.semanticSceneDirectGeometrySourceHashChurnCount;
  agg.semanticSceneDirectSameCasterComparisonCount +=
      stats.semanticSceneDirectSameCasterComparisonCount;
  agg.semanticSceneDirectIdentitySkippedChurnCount +=
      stats.semanticSceneDirectIdentitySkippedChurnCount;
  agg.semanticSceneDirectPaletteRootDeltaSampleCount +=
      stats.semanticSceneDirectPaletteRootDeltaSampleCount;
  agg.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount +=
      stats.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount;
  agg.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount +=
      stats.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount;
  agg.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount +=
      stats.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount;
  agg.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount +=
      stats.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount;
  agg.semanticSceneDirectPaletteRootMaxDeltaMilli = std::max(
      agg.semanticSceneDirectPaletteRootMaxDeltaMilli,
      uint64_t(stats.semanticSceneDirectPaletteRootMaxDeltaMilli));
  agg.semanticSceneDirectSelectionKeyUnitPtrCount +=
      stats.semanticSceneDirectSelectionKeyUnitPtrCount;
  agg.semanticSceneDirectSelectionKeyJHandleCount +=
      stats.semanticSceneDirectSelectionKeyJHandleCount;
  agg.semanticSceneDirectSelectionKeyRuntimeModelCount +=
      stats.semanticSceneDirectSelectionKeyRuntimeModelCount;
  agg.semanticSceneDirectSelectionKeyWorldObjectCount +=
      stats.semanticSceneDirectSelectionKeyWorldObjectCount;
  agg.semanticSceneDirectSelectionKeySceneNodeCount +=
      stats.semanticSceneDirectSelectionKeySceneNodeCount;
  agg.semanticSceneDirectSelectionKeyModelMeshCount +=
      stats.semanticSceneDirectSelectionKeyModelMeshCount;
  agg.semanticSceneDirectSelectionKeyRenderablePartCount +=
      stats.semanticSceneDirectSelectionKeyRenderablePartCount;
  agg.semanticSceneLastAppendedGeometrySourceHash =
      stats.semanticSceneLastAppendedGeometrySourceHash;
  agg.semanticSceneLastAppendedGeometryId =
      stats.semanticSceneLastAppendedGeometryId;
  agg.semanticSceneShadowCastersCount = std::max(
      agg.semanticSceneShadowCastersCount,
      uint64_t(stats.semanticSceneShadowCastersCount));
  agg.semanticSceneReplayDrawsCount = std::max(
      agg.semanticSceneReplayDrawsCount,
      uint64_t(stats.semanticSceneReplayDrawsCount));
  agg.semanticSceneShadowMapDrawnCasters = std::max(
      agg.semanticSceneShadowMapDrawnCasters,
      uint64_t(stats.semanticSceneShadowMapDrawnCasters));
  agg.semanticSceneShadowMapCascadeCulledCount += stats.semanticSceneShadowMapCascadeCulledCount;
  agg.semanticSceneShadowMapSkinnedCasterCount = std::max(
      agg.semanticSceneShadowMapSkinnedCasterCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedCasterCount));
  agg.semanticSceneShadowMapSkinnedPreparedCount = std::max(
      agg.semanticSceneShadowMapSkinnedPreparedCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedPreparedCount));
  agg.semanticSceneShadowMapSkinnedInvalidBufferCount = std::max(
      agg.semanticSceneShadowMapSkinnedInvalidBufferCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedInvalidBufferCount));
  agg.semanticSceneShadowMapSkinnedInvalidPipelineCount = std::max(
      agg.semanticSceneShadowMapSkinnedInvalidPipelineCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedInvalidPipelineCount));
  agg.semanticSceneShadowMapSkinnedDrawnCount = std::max(
      agg.semanticSceneShadowMapSkinnedDrawnCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedDrawnCount));
  agg.semanticSceneShadowTaaActive = std::max(
      agg.semanticSceneShadowTaaActive,
      uint64_t(stats.semanticSceneShadowTaaActive));
  agg.semanticSceneReceiverReuseShadowMap = std::max(
      agg.semanticSceneReceiverReuseShadowMap,
      uint64_t(stats.semanticSceneReceiverReuseShadowMap));
  agg.semanticSceneReceiverInputValid = std::max(
      agg.semanticSceneReceiverInputValid,
      uint64_t(stats.semanticSceneReceiverInputValid));
  agg.semanticSceneReceiverInputRejectReason = std::max(
      agg.semanticSceneReceiverInputRejectReason,
      uint64_t(stats.semanticSceneReceiverInputRejectReason));
  agg.semanticSceneReceiverNeedPass = std::max(
      agg.semanticSceneReceiverNeedPass,
      uint64_t(stats.semanticSceneReceiverNeedPass));
  agg.semanticSceneReceiverNeedShadowMap = std::max(
      agg.semanticSceneReceiverNeedShadowMap,
      uint64_t(stats.semanticSceneReceiverNeedShadowMap));
  agg.semanticSceneReceiverHasCompleteShadowMap = std::max(
      agg.semanticSceneReceiverHasCompleteShadowMap,
      uint64_t(stats.semanticSceneReceiverHasCompleteShadowMap));
  agg.semanticSceneReceiverHasUsableDirectionalShadow = std::max(
      agg.semanticSceneReceiverHasUsableDirectionalShadow,
      uint64_t(stats.semanticSceneReceiverHasUsableDirectionalShadow));
  agg.semanticSceneReceiverActiveStrengthMilli = std::max(
      agg.semanticSceneReceiverActiveStrengthMilli,
      uint64_t(stats.semanticSceneReceiverActiveStrengthMilli));
  agg.semanticSceneReceiverUboStrengthMilli = std::max(
      agg.semanticSceneReceiverUboStrengthMilli,
      uint64_t(stats.semanticSceneReceiverUboStrengthMilli));
  agg.semanticSceneReceiverDebugMode = std::max(
      agg.semanticSceneReceiverDebugMode,
      uint64_t(stats.semanticSceneReceiverDebugMode));
  agg.semanticSceneReceiverCsmCascadeCount = std::max(
      agg.semanticSceneReceiverCsmCascadeCount,
      uint64_t(stats.semanticSceneReceiverCsmCascadeCount));
  agg.semanticSceneReceiverZeroStrengthFrameCount +=
      stats.semanticSceneReceiverZeroStrengthFrameCount;
  agg.semanticSceneReceiverDrawnWithZeroStrengthCount +=
      stats.semanticSceneReceiverDrawnWithZeroStrengthCount;
  agg.semanticSceneReceiverNoCompleteShadowMapCount +=
      stats.semanticSceneReceiverNoCompleteShadowMapCount;
  agg.semanticSceneReceiverNoShadowMapImageCount +=
      stats.semanticSceneReceiverNoShadowMapImageCount;
  agg.semanticSceneReceiverNoShadowMapSampleViewCount +=
      stats.semanticSceneReceiverNoShadowMapSampleViewCount;
  agg.semanticSceneReceiverNoCandidateCsmCount +=
      stats.semanticSceneReceiverNoCandidateCsmCount;
  agg.semanticSceneReceiverCsmFallbackToLastGoodCount +=
      stats.semanticSceneReceiverCsmFallbackToLastGoodCount;
  agg.semanticSceneReceiverHoldInvalidCsmCount +=
      stats.semanticSceneReceiverHoldInvalidCsmCount;
  agg.semanticSceneReceiverHoldEmptyReplayCount +=
      stats.semanticSceneReceiverHoldEmptyReplayCount;
  agg.semanticSceneReceiverHoldIdentityChurnCount +=
      stats.semanticSceneReceiverHoldIdentityChurnCount;
  agg.semanticSceneReceiverReuseInvalidatedAfterEnsureCount +=
      stats.semanticSceneReceiverReuseInvalidatedAfterEnsureCount;
  agg.semanticSceneShadowMapRenderSkippedNoResourcesCount +=
      stats.semanticSceneShadowMapRenderSkippedNoResourcesCount;
  agg.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount +=
      stats.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount;
  agg.semanticSceneReceiverViewportX = std::max(
      agg.semanticSceneReceiverViewportX,
      uint64_t(stats.semanticSceneReceiverViewportX));
  agg.semanticSceneReceiverViewportY = std::max(
      agg.semanticSceneReceiverViewportY,
      uint64_t(stats.semanticSceneReceiverViewportY));
  agg.semanticSceneReceiverViewportWidth = std::max(
      agg.semanticSceneReceiverViewportWidth,
      uint64_t(stats.semanticSceneReceiverViewportWidth));
  agg.semanticSceneReceiverViewportHeight = std::max(
      agg.semanticSceneReceiverViewportHeight,
      uint64_t(stats.semanticSceneReceiverViewportHeight));
  agg.semanticSceneSkippedUnitsOnlyFilter +=
      stats.semanticSceneSkippedUnitsOnlyFilter;
  agg.semanticSceneAcceptedExplicitResourceOwnerRigid +=
      stats.semanticSceneAcceptedExplicitResourceOwnerRigid;
  agg.semanticSceneRejectedNoVertex += stats.semanticSceneRejectedNoVertex;
  agg.semanticSceneRejectedSkinnedContract +=
      stats.semanticSceneRejectedSkinnedContract;
  agg.semanticSceneRejectedGeometry += stats.semanticSceneRejectedGeometry;
  agg.semanticSceneRejectedGeometryFrameLocal +=
      stats.semanticSceneRejectedGeometryFrameLocal;
  agg.semanticSceneRejectedGeometryPersistent +=
      stats.semanticSceneRejectedGeometryPersistent;
  agg.semanticFallbackPruned += stats.semanticFallbackPruned;
  agg.semanticFallbackPrunedByHandle +=
      stats.semanticFallbackPrunedByHandle;
  agg.semanticFallbackPrunedByWorldObjectEntry +=
      stats.semanticFallbackPrunedByWorldObjectEntry;
  agg.semanticFallbackPrunedBySceneNode +=
      stats.semanticFallbackPrunedBySceneNode;
  agg.semanticFallbackPrunedByRuntimeModel +=
      stats.semanticFallbackPrunedByRuntimeModel;
  agg.instancedGeometryInstances += stats.persistentInstanceCount;
  agg.instancedGeometryDrawsSaved += stats.instancedGeometryDrawsSaved;
}

void War3PerfMonitor::noteShadowMapFallback(bool reusedLastComplete,
                                            bool renderedCurrentPartial) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  if (reusedLastComplete)
    m_shadowBudgetAggregate.framesReuseLastComplete++;
  if (renderedCurrentPartial)
    m_shadowBudgetAggregate.framesRenderCurrentPartial++;
}

void War3PerfMonitor::noteShadowReceiverFrame(
    uint32_t replayCasterCount,
    uint64_t replayGeometryWork,
    uint32_t requestedShadowResolution,
    uint32_t effectiveShadowResolution) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  auto& agg = m_shadowBudgetAggregate;
  agg.shadowReceiverFrames++;
  agg.shadowReceiverReplayCasterCountTotal += replayCasterCount;
  agg.shadowReceiverReplayCasterCountMax =
      std::max<uint64_t>(agg.shadowReceiverReplayCasterCountMax,
                         replayCasterCount);
  agg.shadowReceiverReplayGeometryWorkTotal += replayGeometryWork;
  agg.shadowReceiverReplayGeometryWorkMax =
      std::max<uint64_t>(agg.shadowReceiverReplayGeometryWorkMax,
                         replayGeometryWork);
  if (requestedShadowResolution != effectiveShadowResolution)
    agg.shadowReceiverAdaptiveResolutionFrames++;
  agg.shadowReceiverRequestedResolutionLast = requestedShadowResolution;
  agg.shadowReceiverEffectiveResolutionLast = effectiveShadowResolution;
}

War3PerfMonitor::~War3PerfMonitor() { shutdown(); }

std::vector<War3PerfMonitor::CpuOnlyScope> &War3PerfMonitor::scopeStack() {
  static thread_local std::vector<CpuOnlyScope> stack;
  return stack;
}

War3PerfMonitor::ThreadCpuState &War3PerfMonitor::threadCpuState() {
  static thread_local ThreadCpuState state;
  return state;
}

bool War3PerfMonitor::readProcessCpu100ns(uint64_t& out100ns) const {
  FILETIME creation = {};
  FILETIME exit = {};
  FILETIME kernel = {};
  FILETIME user = {};
  if (!::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user))
    return false;
  out100ns = combineFileTimes100ns(kernel, user);
  return true;
}

bool War3PerfMonitor::readThreadCpu100ns(HANDLE threadHandle,
                                         uint64_t& out100ns) const {
  if (!threadHandle)
    return false;
  FILETIME creation = {};
  FILETIME exit = {};
  FILETIME kernel = {};
  FILETIME user = {};
  if (!::GetThreadTimes(threadHandle, &creation, &exit, &kernel, &user))
    return false;
  out100ns = combineFileTimes100ns(kernel, user);
  return true;
}

HANDLE War3PerfMonitor::ensureMainThreadHandleLocked(DWORD tid) {
  if (tid == 0)
    return nullptr;
  if (m_mainThreadHandle && m_mainThreadHandleTid == tid)
    return m_mainThreadHandle;

  closeMainThreadHandleLocked();

  DWORD access = THREAD_QUERY_INFORMATION;
#ifdef THREAD_QUERY_LIMITED_INFORMATION
  access |= THREAD_QUERY_LIMITED_INFORMATION;
#endif
  HANDLE threadHandle = ::OpenThread(access, FALSE, tid);
  if (!threadHandle)
    return nullptr;

  m_mainThreadHandle = threadHandle;
  m_mainThreadHandleTid = tid;
  return m_mainThreadHandle;
}

void War3PerfMonitor::closeMainThreadHandleLocked() {
  if (m_mainThreadHandle) {
    ::CloseHandle(m_mainThreadHandle);
    m_mainThreadHandle = nullptr;
  }
  m_mainThreadHandleTid = 0;
}

War3PerfMonitor::CpuProbeSnapshot War3PerfMonitor::captureCpuProbeLocked() {
  CpuProbeSnapshot probe = {};

  uint64_t process100ns = 0;
  if (readProcessCpu100ns(process100ns)) {
    probe.hasProcess = true;
    probe.processCpu100ns = process100ns;
  }

  const DWORD mainTid = dxvk::war3::hooks::GetMainLoopThreadId();
  probe.mainThreadId = mainTid;
  if (mainTid != 0) {
    HANDLE mainHandle = ensureMainThreadHandleLocked(mainTid);
    uint64_t main100ns = 0;
    if (readThreadCpu100ns(mainHandle, main100ns)) {
      probe.hasMainThread = true;
      probe.mainThreadCpu100ns = main100ns;
    }
  }

  return probe;
}

void War3PerfMonitor::setDevice(DxvkDevice *device) {
  std::lock_guard lock(m_mutex);
  m_device = device;
  if (m_device != nullptr) {
    m_timestampPeriodNs = static_cast<double>(
        m_device->properties().core.properties.limits.timestampPeriod);
  } else {
    m_timestampPeriodNs = 0.0;
  }
}

void War3PerfMonitor::shutdown() {
  if (m_shutdownStarted.exchange(true, std::memory_order_acq_rel))
    return;

  flushCurrentThreadCpuDeltas();
  const bool drained = waitForPendingExports(15000);
  if (!drained) {
    Logger::warn("War3PerfMonitor: timeout waiting pending exports, forcing "
                 "export worker stop");
  }

  {
    std::lock_guard exportLock(m_exportMutex);
    m_exportStopRequested = true;
    m_exportAbortRequested = !drained;
    if (!drained) {
      while (!m_exportQueue.empty()) {
        m_exportQueue.pop_front();
      }
      m_pendingExportJobs.store(0, std::memory_order_relaxed);
    }
  }
  m_exportCv.notify_all();

  if (m_exportThread.joinable()) {
    m_exportThread.join();
  }
  m_exportWorkerStarted = false;

  {
    std::lock_guard lock(m_mutex);
    m_enabled.store(false, std::memory_order_relaxed);
    m_recording.store(false, std::memory_order_relaxed);
    m_device = nullptr;
    m_timestampPeriodNs = 0.0;
    m_pending.clear();
    m_sections.clear();
    m_sectionIds.clear();
    m_frameHistory.clear();
    m_shadowBudgetAggregate = {};
    m_lastReport = Clock::now();
    m_frameCpuProbeStart = {};
    closeMainThreadHandleLocked();
  }
}

void War3PerfMonitor::beginFrame() {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return;

  flushCurrentThreadCpuDeltas();

  std::lock_guard lock(m_mutex);
  m_frameStart = Clock::now();
  m_frameCpuProbeStart = captureCpuProbeLocked();
  m_inFrame = true;

  // 重置当前帧的计数器
  for (auto &s : m_sections) {
    s.cpuCount = 0;
    s.cpuSumMs = 0.0;
    s.gpuCount = 0;
    s.gpuSumMs = 0.0;
  }
}

void War3PerfMonitor::endFrame() {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return;

  flushCurrentThreadCpuDeltas();
  tick();
  archiveFrame();

  const auto now = Clock::now();
  if (now - m_lastReport >= m_reportInterval) {
    report(now);
  }

  if (m_autoExportInterval.count() > 0 &&
      now - m_lastAutoExport >= m_autoExportInterval) {
    m_lastAutoExport = now;
    exportHtmlReport("war3_perf_report_auto.html");
  }
}

War3PerfMonitor::FrameGuard War3PerfMonitor::makeFrameGuard() {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return FrameGuard(nullptr);
  return FrameGuard(this);
}

War3PerfMonitor::ScopedSection
War3PerfMonitor::scope(const char *name, const Rc<DxvkCommandList> &ctx) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0]) {
    return ScopedSection();
  }

  std::string parentPath;
  auto &stack = scopeStack();
  if (!stack.empty()) {
    parentPath = stack.back().path;
  }

  uint32_t id = 0;
  {
    std::lock_guard lock(m_mutex);
    const char *parentPtr = parentPath.empty() ? nullptr : parentPath.c_str();
    id = getSectionIdLocked(name, parentPtr);
  }
  return ScopedSection(this, id, ctx);
}

War3PerfMonitor::ScopedCpuScope War3PerfMonitor::cpuScope(const char *name) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0]) {
    return ScopedCpuScope();
  }
  return ScopedCpuScope(this, name);
}

void War3PerfMonitor::addCpuSample(const char *name, double cpuMs,
                                   const char *parentPath, uint32_t calls) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0] ||
      cpuMs <= 0.0 || calls == 0) {
    return;
  }

  ThreadCpuState &state = threadCpuState();
  const uint32_t id = resolveSectionIdWithTlsCache(state, name, parentPath);
  queueCpuDelta(state, id, cpuMs, calls);
}

void War3PerfMonitor::pushScope(const char *name) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0])
    return;

  auto &stack = scopeStack();
  CpuOnlyScope scope;
  scope.name = name;
  if (!stack.empty()) {
    scope.path = stack.back().path + "/" + scope.name;
  } else {
    scope.path = scope.name;
  }
  scope.start = Clock::now();
  stack.push_back(std::move(scope));
}

void War3PerfMonitor::popScope() {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return;

  auto &stack = scopeStack();
  if (stack.empty())
    return;

  auto scope = std::move(stack.back());
  stack.pop_back();
  const double cpuMs = toMs(Clock::now() - scope.start);

  // 获取父级名称
  std::string parentPath;
  if (!stack.empty()) {
    parentPath = stack.back().path;
  }

  const char *parentPtr = parentPath.empty() ? nullptr : parentPath.c_str();
  ThreadCpuState &state = threadCpuState();
  const uint32_t id =
      resolveSectionIdWithTlsCache(state, scope.name.c_str(), parentPtr);
  queueCpuDelta(state, id, cpuMs, 1);
}

uint32_t War3PerfMonitor::getSectionId(const char *name,
                                       const char *parentPath) {
  std::lock_guard lock(m_mutex);
  return getSectionIdLocked(name, parentPath);
}

uint32_t War3PerfMonitor::resolveSectionIdWithTlsCache(ThreadCpuState &state,
                                                       const char *name,
                                                       const char *parentPath) {
  const size_t nameHash = std::hash<std::string_view>{}(name);
  const size_t parentHash =
      parentPath ? std::hash<std::string_view>{}(parentPath) : 0;
  for (const auto &entry : state.sectionIdCache) {
    if (entry.name == name && entry.parentPath == parentPath &&
        entry.nameHash == nameHash && entry.parentHash == parentHash) {
      return entry.id;
    }
  }

  uint32_t id = 0;
  {
    std::lock_guard lock(m_mutex);
    id = getSectionIdLocked(name, parentPath);
  }

  const size_t slot = static_cast<size_t>(state.nextCacheSlot++) %
                      state.sectionIdCache.size();
  state.sectionIdCache[slot].name = name;
  state.sectionIdCache[slot].parentPath = parentPath;
  state.sectionIdCache[slot].nameHash = nameHash;
  state.sectionIdCache[slot].parentHash = parentHash;
  state.sectionIdCache[slot].id = id;
  return id;
}

uint32_t War3PerfMonitor::getSectionIdLocked(const char *name,
                                             const char *parentPath) {

  // 创建唯一键：parentPath/name 或 name
  std::string key = parentPath && parentPath[0]
                        ? (std::string(parentPath) + "/" + name)
                        : std::string(name);

  auto it = m_sectionIds.find(key);
  if (it != m_sectionIds.end())
    return it->second;

  const uint32_t id = static_cast<uint32_t>(m_sections.size());
  SectionStats stats = {};
  stats.name = name;
  stats.path = key;
  stats.parentPath = parentPath ? parentPath : "";
  m_sections.emplace_back(std::move(stats));
  m_sectionIds.emplace(key, id);
  return id;
}

void War3PerfMonitor::queueCpuDelta(ThreadCpuState &state, uint32_t id,
                                    double cpuMs, uint32_t calls) {
  CpuDelta delta = {};
  delta.id = id;
  delta.cpuMs = cpuMs;
  delta.calls = calls;
  state.pendingDeltas.emplace_back(std::move(delta));
  if (state.pendingDeltas.size() >= state.sectionIdCache.size()) {
    flushThreadCpuDeltas(state);
  }
}

void War3PerfMonitor::flushThreadCpuDeltas(ThreadCpuState &state) {
  if (state.pendingDeltas.empty())
    return;

  std::lock_guard lock(m_mutex);
  for (const CpuDelta &delta : state.pendingDeltas) {
    if (delta.id >= m_sections.size())
      continue;
    m_sections[delta.id].cpuCount += delta.calls;
    m_sections[delta.id].cpuSumMs += delta.cpuMs;
  }
  state.pendingDeltas.clear();
}

void War3PerfMonitor::flushCurrentThreadCpuDeltas() {
  ThreadCpuState &state = threadCpuState();
  flushThreadCpuDeltas(state);
}

Rc<DxvkGpuQuery>
War3PerfMonitor::writeTimestamp(const Rc<DxvkCommandList> &ctx) {
  if (!m_device || !ctx || m_timestampPeriodNs <= 0.0) {
    return nullptr;
  }

  Rc<DxvkGpuQuery> gpuQuery = m_device->createRawQuery(VK_QUERY_TYPE_TIMESTAMP);
  if (!gpuQuery) {
    return nullptr;
  }

  const auto handle = gpuQuery->getQuery();
  ctx->resetQuery(handle.first, handle.second);
  ctx->cmdWriteTimestamp(DxvkCmdBuffer::ExecBuffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, handle.first,
                         handle.second);

  ctx->track(gpuQuery);
  return gpuQuery;
}

void War3PerfMonitor::submitSample(uint32_t id, double cpuMs,
                                   Rc<DxvkGpuQuery> gpuBegin,
                                   Rc<DxvkGpuQuery> gpuEnd) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  ThreadCpuState &state = threadCpuState();
  if (cpuMs > 0.0) {
    queueCpuDelta(state, id, cpuMs, 1);
  }

  if (!gpuBegin || !gpuEnd)
    return;

  std::lock_guard lock(m_mutex);
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }
  if (m_pending.size() >= m_maxPendingSamples) {
    static bool s_loggedPendingDrop = false;
    if (!s_loggedPendingDrop) {
      s_loggedPendingDrop = true;
      Logger::warn("War3PerfMonitor: pending samples overflow, dropping GPU "
                   "timing samples");
    }
    return;
  }
  PendingSample sample = {};
  sample.id = id;
  sample.cpuMs = cpuMs;
  sample.gpuBegin = std::move(gpuBegin);
  sample.gpuEnd = std::move(gpuEnd);
  m_pending.emplace_back(std::move(sample));
}

void War3PerfMonitor::tick() {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return;

  std::lock_guard lock(m_mutex);
  if (!m_device || m_timestampPeriodNs <= 0.0) {
    m_pending.clear();
    return;
  }

  for (size_t i = 0; i < m_pending.size();) {
    auto &sample = m_pending[i];
    if (!sample.gpuBegin || !sample.gpuEnd) {
      i++;
      continue;
    }

    auto vk = m_device->vkd();
    uint64_t beginTs = 0;
    uint64_t endTs = 0;

    const auto beginHandle = sample.gpuBegin->getQuery();
    const auto endHandle = sample.gpuEnd->getQuery();

    VkResult beginRes = vk->vkGetQueryPoolResults(
        vk->device(), beginHandle.first, beginHandle.second, 1,
        sizeof(uint64_t), &beginTs, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

    if (beginRes == VK_NOT_READY) {
      i++;
      continue;
    }

    VkResult endRes = vk->vkGetQueryPoolResults(
        vk->device(), endHandle.first, endHandle.second, 1, sizeof(uint64_t),
        &endTs, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

    if (endRes == VK_NOT_READY) {
      i++;
      continue;
    }

    if (beginRes != VK_SUCCESS || endRes != VK_SUCCESS) {
      m_pending[i] = std::move(m_pending.back());
      m_pending.pop_back();
      continue;
    }

    if (endTs > beginTs && sample.id < m_sections.size()) {
      const double deltaNs = double(endTs - beginTs) * m_timestampPeriodNs;
      const double gpuMs = deltaNs / 1.0e6;
      m_sections[sample.id].gpuCount += 1;
      m_sections[sample.id].gpuSumMs += gpuMs;
    }

    m_pending[i] = std::move(m_pending.back());
    m_pending.pop_back();
  }
}

void War3PerfMonitor::archiveFrame() {
  std::lock_guard lock(m_mutex);
  if (!m_inFrame)
    return;

  m_inFrame = false;

  FrameSnapshot snapshot;
  snapshot.frameIndex = m_frameIndex++;
  snapshot.timestamp = Clock::now();
  snapshot.totalCpuMs = toMs(snapshot.timestamp - m_frameStart);
  CpuProbeSnapshot endProbe = captureCpuProbeLocked();

  if (m_frameCpuProbeStart.hasProcess && endProbe.hasProcess &&
      endProbe.processCpu100ns >= m_frameCpuProbeStart.processCpu100ns) {
    const uint64_t delta100ns =
        endProbe.processCpu100ns - m_frameCpuProbeStart.processCpu100ns;
    snapshot.processCpuMs = static_cast<double>(delta100ns) / 10000.0;
    snapshot.hasProcessCpu = true;
  }

  if (m_frameCpuProbeStart.hasMainThread && endProbe.hasMainThread &&
      m_frameCpuProbeStart.mainThreadId != 0 &&
      m_frameCpuProbeStart.mainThreadId == endProbe.mainThreadId &&
      endProbe.mainThreadCpu100ns >= m_frameCpuProbeStart.mainThreadCpu100ns) {
    const uint64_t delta100ns =
        endProbe.mainThreadCpu100ns - m_frameCpuProbeStart.mainThreadCpu100ns;
    snapshot.mainThreadCpuMs = static_cast<double>(delta100ns) / 10000.0;
    snapshot.hasMainThreadCpu = true;
  }

  if (snapshot.hasProcessCpu && snapshot.hasMainThreadCpu) {
    snapshot.workerThreadsCpuMs =
        std::max(0.0, snapshot.processCpuMs - snapshot.mainThreadCpuMs);
  } else if (snapshot.hasProcessCpu) {
    snapshot.workerThreadsCpuMs = snapshot.processCpuMs;
  }

  double totalGpu = 0.0;
  for (size_t i = 0; i < m_sections.size(); ++i) {
    const auto &s = m_sections[i];
    if (s.cpuCount > 0 || s.gpuCount > 0) {
      SectionTiming timing;
      timing.id = static_cast<uint32_t>(i);
      timing.cpuMs = s.cpuSumMs;
      timing.gpuMs = s.gpuSumMs;
      timing.callCount = static_cast<uint32_t>(s.cpuCount);
      snapshot.sections.push_back(std::move(timing));
      totalGpu += s.gpuSumMs;
    }
  }
  snapshot.totalGpuMs = totalGpu;

  m_frameHistory.push_back(std::move(snapshot));

  // 保持环形缓冲区大小
  while (m_frameHistory.size() > m_maxHistorySize) {
    m_frameHistory.pop_front();
  }
}

void War3PerfMonitor::resetHistory() {
  flushCurrentThreadCpuDeltas();
  std::lock_guard lock(m_mutex);
  m_frameHistory.clear();
  m_pending.clear();
  m_shadowBudgetAggregate = {};
  m_frameIndex = 0;
  m_inFrame = false;
  m_frameCpuProbeStart = {};
  m_lastReport = Clock::now();
  m_lastAutoExport = Clock::now();
}

uint32_t War3PerfMonitor::getPendingExportCount() const {
  return m_pendingExportJobs.load(std::memory_order_relaxed);
}

bool War3PerfMonitor::waitForPendingExports(uint32_t timeoutMs) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  std::unique_lock lock(m_exportMutex);
  auto done = [this]() {
    return m_exportQueue.empty() && m_exportInFlight == 0;
  };

  if (done())
    return true;

  if (timeoutMs == 0)
    return false;

  return m_exportCv.wait_until(lock, deadline, done);
}

void War3PerfMonitor::report(std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(m_mutex);
  if (m_sections.empty())
    return;

  bool hasAny = false;
  for (const auto &s : m_sections) {
    if (s.cpuCount > 0 || s.gpuCount > 0) {
      hasAny = true;
      break;
    }
  }

  if (!hasAny) {
    m_lastReport = now;
    return;
  }

  const double intervalSec =
      std::chrono::duration<double>(now - m_lastReport).count();

  war3dbg::Print("DXVK War3Perf: %.1fs, %zu frames in history\n", intervalSec,
                 m_frameHistory.size());

  m_lastReport = now;
}

War3PerfMonitor::ExportSnapshot
War3PerfMonitor::captureExportSnapshotLocked() const {
  ExportSnapshot snapshot;
  snapshot.frameHistory = m_frameHistory;
  snapshot.sections = m_sections;
  snapshot.shadowBudgetAggregate = m_shadowBudgetAggregate;
  return snapshot;
}

std::string War3PerfMonitor::resolveExportPath(const std::string &outputPath) const {
  std::string finalPath = outputPath;
  const std::string baseDir = getWarVkBaseDir();
  if (baseDir.empty()) {
    return finalPath;
  }

  const std::string tempDir = baseDir + "Temp\\";
  const std::string logDir = baseDir + "Log\\";
  CreateDirectoryA(baseDir.c_str(), nullptr);
  CreateDirectoryA(tempDir.c_str(), nullptr);
  CreateDirectoryA(logDir.c_str(), nullptr);

  std::string fileName = outputPath.empty() ? "war3_perf_report.html" : outputPath;
  const size_t slashPos = fileName.find_last_of("\\/");
  if (slashPos == std::string::npos) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char timeBuf[64] = {};
    std::snprintf(timeBuf, sizeof(timeBuf), "_%04u_%02u_%02u_%02u_%02u_%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    const size_t dotPos = fileName.find_last_of('.');
    if (dotPos != std::string::npos) {
      fileName = fileName.substr(0, dotPos) + timeBuf + fileName.substr(dotPos);
    } else {
      fileName += timeBuf;
    }
    finalPath = logDir + fileName;
  } else {
    finalPath = baseDir + fileName;
  }
  return finalPath;
}

double War3PerfMonitor::resolveReportWindowSec() const {
  double windowSec = 0.0;
  const std::string windowVar = env::getEnvVar("DXVK_WAR3_PERF_WINDOW_SEC");
  if (!windowVar.empty()) {
    windowSec = std::strtod(windowVar.c_str(), nullptr);
  }
  if (windowSec <= 0.0) {
    windowSec = 1200.0;
  }
  return windowSec;
}

void War3PerfMonitor::ensureExportWorkerLocked() {
  if (m_exportWorkerStarted)
    return;

  m_exportStopRequested = false;
  m_exportAbortRequested = false;
  m_exportInFlight = 0;
  m_exportThread = std::thread(&War3PerfMonitor::exportWorkerLoop, this);
  m_exportWorkerStarted = true;
}

void War3PerfMonitor::enqueueExportJob(ExportJob &&job) {
  std::unique_lock lock(m_exportMutex);
  if (m_exportStopRequested || m_exportAbortRequested)
    return;

  ensureExportWorkerLocked();

  if (!job.isAuto) {
    while (m_exportQueue.size() >= 2) {
      auto autoIt = std::find_if(
          m_exportQueue.begin(), m_exportQueue.end(),
          [](const ExportJob &queued) { return queued.isAuto; });
      if (autoIt != m_exportQueue.end()) {
        m_exportQueue.erase(autoIt);
        decrementPendingJobs(m_pendingExportJobs);
        continue;
      }
      m_exportCv.wait(lock, [this]() {
        return m_exportQueue.size() < 2 || m_exportAbortRequested ||
               m_exportStopRequested;
      });
      if (m_exportAbortRequested || m_exportStopRequested) {
        return;
      }
    }
  } else if (m_exportQueue.size() >= 2) {
    auto autoIt = std::find_if(
        m_exportQueue.begin(), m_exportQueue.end(),
        [](const ExportJob &queued) { return queued.isAuto; });
    if (autoIt != m_exportQueue.end()) {
      m_exportQueue.erase(autoIt);
      decrementPendingJobs(m_pendingExportJobs);
      Logger::warn("War3PerfMonitor: dropped stale auto export job");
    } else {
      Logger::warn(
          "War3PerfMonitor: auto export skipped because queue is occupied by "
          "manual exports");
      return;
    }
  }

  m_exportQueue.emplace_back(std::move(job));
  m_pendingExportJobs.fetch_add(1, std::memory_order_relaxed);
  lock.unlock();
  m_exportCv.notify_all();
}

void War3PerfMonitor::exportWorkerLoop() {
  for (;;) {
    ExportJob job;
    {
      std::unique_lock lock(m_exportMutex);
      m_exportCv.wait(lock, [this]() {
        return m_exportAbortRequested || !m_exportQueue.empty() ||
               m_exportStopRequested;
      });

      if (m_exportAbortRequested) {
        break;
      }

      if (m_exportQueue.empty()) {
        if (m_exportStopRequested) {
          break;
        }
        continue;
      }

      job = std::move(m_exportQueue.front());
      m_exportQueue.pop_front();
      m_exportInFlight += 1;
    }

    try {
      processExportJob(job);
    } catch (const std::exception &e) {
      Logger::err("War3PerfMonitor: export worker exception");
      Logger::err(e.what());
    } catch (...) {
      Logger::err("War3PerfMonitor: export worker unknown exception");
    }

    {
      std::lock_guard lock(m_exportMutex);
      if (m_exportInFlight > 0) {
        m_exportInFlight -= 1;
      }
    }
    decrementPendingJobs(m_pendingExportJobs);
    m_exportCv.notify_all();
  }

  {
    std::lock_guard lock(m_exportMutex);
    m_exportInFlight = 0;
  }
  m_exportCv.notify_all();
}

std::string War3PerfMonitor::generateJsonData(double windowSec) const {
  ExportSnapshot snapshot;
  {
    std::lock_guard lock(const_cast<std::mutex &>(m_mutex));
    snapshot = captureExportSnapshotLocked();
  }
  return generateJsonDataFromSnapshot(snapshot, windowSec);
}

std::string War3PerfMonitor::generateJsonDataFromSnapshot(
    const ExportSnapshot &snapshot, double windowSec) const {
  const auto now = Clock::now();
  std::vector<const FrameSnapshot *> frames;
  frames.reserve(snapshot.frameHistory.size());

  if (windowSec > 0.0) {
    for (const auto &f : snapshot.frameHistory) {
      const double ageSec =
          std::chrono::duration<double>(now - f.timestamp).count();
      if (ageSec <= windowSec) {
        frames.push_back(&f);
      }
    }
  } else {
    for (const auto &f : snapshot.frameHistory) {
      frames.push_back(&f);
    }
  }

  if (frames.empty()) {
    for (const auto &f : snapshot.frameHistory) {
      frames.push_back(&f);
    }
  }

  std::ostringstream json;
  json << std::fixed << std::setprecision(3);
  json << "{\n";
  json << "  \"frameCount\": " << frames.size() << ",\n";
  if (!frames.empty()) {
    const double windowUsed =
        std::chrono::duration<double>(now - frames.front()->timestamp).count();
    json << "  \"windowSec\": " << windowUsed << ",\n";
  } else {
    json << "  \"windowSec\": 0.0,\n";
  }

  // 计算帧级统计
  double avgCpu = 0.0, avgGpu = 0.0, maxCpu = 0.0, minCpu = 999999.0;
  double totalCpu = 0.0;
  double totalProcessCpuMs = 0.0;
  double totalMainThreadCpuMs = 0.0;
  double totalWorkerThreadsCpuMs = 0.0;
  uint32_t processCpuSamples = 0;
  uint32_t mainThreadCpuSamples = 0;
  uint32_t workerThreadsCpuSamples = 0;
  std::vector<double> cpuFrameTimes;
  std::vector<double> gpuFrameTimes;
  cpuFrameTimes.reserve(frames.size());
  gpuFrameTimes.reserve(frames.size());
  uint32_t jank16 = 0; // > 16.67ms (60fps budget)
  uint32_t jank33 = 0; // > 33.33ms (30fps budget)
  uint32_t jank50 = 0; // > 50.00ms (severe stutter)
  for (const auto *f : frames) {
    avgCpu += f->totalCpuMs;
    avgGpu += f->totalGpuMs;
    maxCpu = std::max(maxCpu, f->totalCpuMs);
    minCpu = std::min(minCpu, f->totalCpuMs);
    totalCpu += f->totalCpuMs;
    cpuFrameTimes.push_back(f->totalCpuMs);
    gpuFrameTimes.push_back(f->totalGpuMs);
    if (f->hasProcessCpu) {
      totalProcessCpuMs += f->processCpuMs;
      processCpuSamples++;
    }
    if (f->hasMainThreadCpu) {
      totalMainThreadCpuMs += f->mainThreadCpuMs;
      mainThreadCpuSamples++;
    }
    if (f->hasProcessCpu) {
      totalWorkerThreadsCpuMs += f->workerThreadsCpuMs;
      workerThreadsCpuSamples++;
    }
    if (f->totalCpuMs > 16.67)
      ++jank16;
    if (f->totalCpuMs > 33.33)
      ++jank33;
    if (f->totalCpuMs > 50.0)
      ++jank50;
  }
  if (!frames.empty()) {
    avgCpu /= frames.size();
    avgGpu /= frames.size();
  }
  const double avgProcessCpuMs =
      processCpuSamples ? (totalProcessCpuMs / static_cast<double>(processCpuSamples)) : 0.0;
  const double avgMainThreadCpuMs =
      mainThreadCpuSamples ? (totalMainThreadCpuMs / static_cast<double>(mainThreadCpuSamples))
                           : 0.0;
  const double avgWorkerThreadsCpuMs =
      workerThreadsCpuSamples
          ? (totalWorkerThreadsCpuMs / static_cast<double>(workerThreadsCpuSamples))
          : std::max(0.0, avgProcessCpuMs - avgMainThreadCpuMs);
  const double processCpuCoveragePct =
      frames.empty() ? 0.0
                     : (static_cast<double>(processCpuSamples) /
                        static_cast<double>(frames.size()) * 100.0);
  const double mainThreadCpuCoveragePct =
      frames.empty() ? 0.0
                     : (static_cast<double>(mainThreadCpuSamples) /
                        static_cast<double>(frames.size()) * 100.0);
  const double processParallelismCores =
      (avgCpu > 1e-6) ? (avgProcessCpuMs / avgCpu) : 0.0;
  const double mainThreadShareOfProcessPct =
      (avgProcessCpuMs > 1e-6)
          ? std::clamp(avgMainThreadCpuMs / avgProcessCpuMs * 100.0, 0.0, 100.0)
          : 0.0;
  const double workerThreadsShareOfProcessPct =
      (avgProcessCpuMs > 1e-6)
          ? std::clamp(avgWorkerThreadsCpuMs / avgProcessCpuMs * 100.0, 0.0, 100.0)
          : 0.0;

  auto percentile = [](std::vector<double> samples, double p) -> double {
    if (samples.empty())
      return 0.0;
    p = std::clamp(p, 0.0, 100.0);
    std::sort(samples.begin(), samples.end());
    const double rank = (p / 100.0) * static_cast<double>(samples.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(rank));
    const size_t hi = static_cast<size_t>(std::ceil(rank));
    if (lo == hi)
      return samples[lo];
    const double t = rank - static_cast<double>(lo);
    return samples[lo] * (1.0 - t) + samples[hi] * t;
  };

  double varCpu = 0.0;
  for (double v : cpuFrameTimes) {
    const double d = v - avgCpu;
    varCpu += d * d;
  }
  const double stddevCpu =
      cpuFrameTimes.empty() ? 0.0 : std::sqrt(varCpu / cpuFrameTimes.size());
  const double p50Cpu = percentile(cpuFrameTimes, 50.0);
  const double p95Cpu = percentile(cpuFrameTimes, 95.0);
  const double p99Cpu = percentile(cpuFrameTimes, 99.0);
  const double p50Gpu = percentile(gpuFrameTimes, 50.0);
  const double p95Gpu = percentile(gpuFrameTimes, 95.0);
  const double p99Gpu = percentile(gpuFrameTimes, 99.0);

  json << "  \"avgFrameTimeMs\": " << avgCpu << ",\n";
  json << "  \"avgGpuTimeMs\": " << avgGpu << ",\n";
  json << "  \"maxFrameTimeMs\": " << maxCpu << ",\n";
  json << "  \"minFrameTimeMs\": " << (minCpu < 999999.0 ? minCpu : 0.0)
       << ",\n";
  json << "  \"p50CpuMs\": " << p50Cpu << ",\n";
  json << "  \"p95CpuMs\": " << p95Cpu << ",\n";
  json << "  \"p99CpuMs\": " << p99Cpu << ",\n";
  json << "  \"p50GpuMs\": " << p50Gpu << ",\n";
  json << "  \"p95GpuMs\": " << p95Gpu << ",\n";
  json << "  \"p99GpuMs\": " << p99Gpu << ",\n";
  json << "  \"stddevCpuMs\": " << stddevCpu << ",\n";
  json << "  \"avgProcessCpuMs\": " << avgProcessCpuMs << ",\n";
  json << "  \"avgMainThreadCpuMs\": " << avgMainThreadCpuMs << ",\n";
  json << "  \"avgWorkerThreadsCpuMs\": " << avgWorkerThreadsCpuMs << ",\n";
  json << "  \"processCpuCoveragePct\": " << processCpuCoveragePct << ",\n";
  json << "  \"mainThreadCpuCoveragePct\": " << mainThreadCpuCoveragePct
       << ",\n";
  json << "  \"processParallelismCores\": " << processParallelismCores
       << ",\n";
  json << "  \"mainThreadShareOfProcessPct\": " << mainThreadShareOfProcessPct
       << ",\n";
  json << "  \"workerThreadsShareOfProcessPct\": "
       << workerThreadsShareOfProcessPct << ",\n";
  json << "  \"mainLoopThreadSplit\": [\n";
  json << "    {\"name\":\"Process\", \"avgCpuMs\": " << avgProcessCpuMs
       << ", \"shareOfFramePct\": "
       << ((avgCpu > 1e-6) ? (avgProcessCpuMs / avgCpu * 100.0) : 0.0)
       << ", \"shareOfProcessPct\": 100.0},\n";
  json << "    {\"name\":\"MainThread\", \"avgCpuMs\": " << avgMainThreadCpuMs
       << ", \"shareOfFramePct\": "
       << ((avgCpu > 1e-6) ? (avgMainThreadCpuMs / avgCpu * 100.0) : 0.0)
       << ", \"shareOfProcessPct\": " << mainThreadShareOfProcessPct
       << "},\n";
  json << "    {\"name\":\"Workers\", \"avgCpuMs\": " << avgWorkerThreadsCpuMs
       << ", \"shareOfFramePct\": "
       << ((avgCpu > 1e-6) ? (avgWorkerThreadsCpuMs / avgCpu * 100.0) : 0.0)
       << ", \"shareOfProcessPct\": " << workerThreadsShareOfProcessPct
       << "}\n";
  json << "  ],\n";
  json << "  \"jank16\": " << jank16 << ",\n";
  json << "  \"jank33\": " << jank33 << ",\n";
  json << "  \"jank50\": " << jank50 << ",\n";
  json << "  \"avgFps\": " << (avgCpu > 0.0 ? 1000.0 / avgCpu : 0.0) << ",\n";
  json << "  \"runtimeProfile\": {\n";
  json << "    \"name\": \"" << dxvk::war3::runtime::GetWar3RuntimeProfileName()
       << "\",\n";
  json << "    \"disabledModules\": \""
       << dxvk::war3::runtime::GetWar3RuntimeDisabledModulesCsv() << "\",\n";
  json << "    \"enabledModules\": \""
       << dxvk::war3::runtime::GetWar3RuntimeEnabledModulesCsv() << "\"\n";
  json << "  },\n";
  json << "  \"moduleMatrix\": [\n";
  json << "    {\"profile\": \""
       << dxvk::war3::runtime::GetWar3RuntimeProfileName()
       << "\", \"disabledModules\": \""
       << dxvk::war3::runtime::GetWar3RuntimeDisabledModulesCsv()
       << "\", \"enabledModules\": \""
       << dxvk::war3::runtime::GetWar3RuntimeEnabledModulesCsv() << "\"}\n";
  json << "  ],\n";

  const auto& shadowAgg = snapshot.shadowBudgetAggregate;
  const double shadowFrames =
      static_cast<double>(std::max<uint64_t>(shadowAgg.framesObserved, 1u));
  json << "  \"shadowBudgetSummary\": {\n";
  json << "    \"framesObserved\": " << shadowAgg.framesObserved << ",\n";
  json << "    \"framesIncomplete\": " << shadowAgg.framesIncomplete << ",\n";
  json << "    \"framesBudgetExceeded\": " << shadowAgg.framesBudgetExceeded
       << ",\n";
  json << "    \"framesReuseLastComplete\": "
       << shadowAgg.framesReuseLastComplete << ",\n";
  json << "    \"framesRenderCurrentPartial\": "
       << shadowAgg.framesRenderCurrentPartial << ",\n";
  const double shadowReceiverAvgCasters =
      shadowAgg.shadowReceiverFrames != 0u
          ? static_cast<double>(shadowAgg.shadowReceiverReplayCasterCountTotal) /
                static_cast<double>(shadowAgg.shadowReceiverFrames)
          : 0.0;
  const double shadowReceiverAvgGeometryWork =
      shadowAgg.shadowReceiverFrames != 0u
          ? static_cast<double>(shadowAgg.shadowReceiverReplayGeometryWorkTotal) /
                static_cast<double>(shadowAgg.shadowReceiverFrames)
          : 0.0;
  json << "    \"shadowReceiverFrames\": "
       << shadowAgg.shadowReceiverFrames << ",\n";
  json << "    \"shadowReceiverReplayCasterCountAvg\": "
       << shadowReceiverAvgCasters << ",\n";
  json << "    \"shadowReceiverReplayCasterCountMax\": "
       << shadowAgg.shadowReceiverReplayCasterCountMax << ",\n";
  json << "    \"shadowReceiverReplayGeometryWorkAvg\": "
       << shadowReceiverAvgGeometryWork << ",\n";
  json << "    \"shadowReceiverReplayGeometryWorkMax\": "
       << shadowAgg.shadowReceiverReplayGeometryWorkMax << ",\n";
  json << "    \"shadowReceiverAdaptiveResolutionFrames\": "
       << shadowAgg.shadowReceiverAdaptiveResolutionFrames << ",\n";
  json << "    \"shadowReceiverRequestedResolutionLast\": "
       << shadowAgg.shadowReceiverRequestedResolutionLast << ",\n";
  json << "    \"shadowReceiverEffectiveResolutionLast\": "
       << shadowAgg.shadowReceiverEffectiveResolutionLast << ",\n";
  json << "    \"avgBudgetMb\": "
       << (static_cast<double>(shadowAgg.totalBudgetBytes) / shadowFrames /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"avgUsedMb\": "
       << (static_cast<double>(shadowAgg.totalUsedBytes) / shadowFrames /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"avgArenaMb\": "
       << (static_cast<double>(shadowAgg.totalArenaBytes) / shadowFrames /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"maxBudgetMb\": "
       << (static_cast<double>(shadowAgg.maxBudgetBytes) / (1024.0 * 1024.0))
       << ",\n";
  json << "    \"maxUsedMb\": "
       << (static_cast<double>(shadowAgg.maxUsedBytes) / (1024.0 * 1024.0))
       << ",\n";
  json << "    \"maxArenaMb\": "
       << (static_cast<double>(shadowAgg.maxArenaBytes) / (1024.0 * 1024.0))
       << ",\n";
  json << "    \"arenaClaimCount\": " << shadowAgg.arenaClaimCount << ",\n";
  json << "    \"arenaDispatchClaims\": " << shadowAgg.arenaDispatchClaims
       << ",\n";
  json << "    \"arenaDispatchClaimMisses\": "
       << shadowAgg.arenaDispatchClaimMisses << ",\n";
  json << "    \"arenaCaptured\": " << shadowAgg.arenaCaptured << ",\n";
  json << "    \"legacyCaptured\": " << shadowAgg.legacyCaptured << ",\n";
  json << "    \"directRefCaptured\": " << shadowAgg.directRefCaptured << ",\n";
  json << "    \"arenaFallbackToFreeze\": "
       << shadowAgg.arenaFallbackToFreeze << ",\n";
  json << "    \"arenaDedup\": " << shadowAgg.arenaDedup << ",\n";
  json << "    \"vbLevelCacheHits\": " << shadowAgg.vbLevelCacheHits << ",\n";
  json << "    \"directRefRejectedUnstable\": "
       << shadowAgg.directRefRejectedUnstable << ",\n";
  json << "    \"arenaOverflowCount\": " << shadowAgg.arenaOverflowCount
       << ",\n";
  json << "    \"arenaEmitRejects\": " << shadowAgg.arenaEmitRejects << ",\n";
  json << "    \"arenaRebasedIndexed\": " << shadowAgg.arenaRebasedIndexed
       << ",\n";
  json << "    \"arenaRebasedNonIndexed\": "
       << shadowAgg.arenaRebasedNonIndexed << ",\n";
  json << "    \"arenaRejectUnsupported\": "
       << shadowAgg.arenaRejectUnsupported << ",\n";
  json << "    \"arenaRejectValidation\": "
       << shadowAgg.arenaRejectValidation << ",\n";
  json << "    \"arenaRejectUninitialized\": "
       << shadowAgg.arenaRejectUninitialized << ",\n";
  json << "    \"arenaRejectOverflow\": " << shadowAgg.arenaRejectOverflow
       << ",\n";
  json << "    \"skippedFreezeBudget\": " << shadowAgg.skippedFreezeBudget
       << ",\n";
  json << "    \"skippedPriorityBudget\": " << shadowAgg.skippedPriorityBudget
       << ",\n";
  json << "    \"skippedUpload\": " << shadowAgg.skippedUpload << ",\n";
  json << "    \"skippedCasterCap\": " << shadowAgg.skippedCasterCap << ",\n";
  json << "    \"skippedDistanceCull\": " << shadowAgg.skippedDistanceCull
       << ",\n";
  json << "    \"degradedAlphaBudget\": " << shadowAgg.degradedAlphaBudget
       << ",\n";
  json << "    \"reusedFreezeHits\": " << shadowAgg.reusedFreezeHits
       << ",\n";
  json << "    \"actualFreezeReuseHits\": " << shadowAgg.actualFreezeReuseHits
       << ",\n";
  json << "    \"uniqueGeometryCount\": " << shadowAgg.uniqueGeometryCount
       << ",\n";
  json << "    \"uniqueInstanceableGeometryCount\": "
       << shadowAgg.uniqueInstanceableGeometryCount << ",\n";
  json << "    \"duplicateGeometryInstances\": "
       << shadowAgg.duplicateGeometryInstances << ",\n";
  json << "    \"reuseEligibleDuplicates\": "
       << shadowAgg.reuseEligibleDuplicates << ",\n";
  json << "    \"potentialFreezeReuseHits\": "
       << shadowAgg.potentialFreezeReuseHits << ",\n";
  json << "    \"staticPersistentCount\": "
       << shadowAgg.staticPersistentCount << ",\n";
  json << "    \"dynamicPoseCount\": " << shadowAgg.dynamicPoseCount
       << ",\n";
  json << "    \"dynamicSkinnedOutputCount\": "
       << shadowAgg.dynamicSkinnedOutputCount << ",\n";
  json << "    \"fallbackDrawCount\": " << shadowAgg.fallbackDrawCount
       << ",\n";
  json << "    \"fallbackDrawCountTerrain\": "
       << shadowAgg.fallbackDrawCountTerrain << ",\n";
  json << "    \"fallbackDrawCountWorldObject\": "
       << shadowAgg.fallbackDrawCountWorldObject << ",\n";
  json << "    \"fallbackDrawCountUnitObject\": "
       << shadowAgg.fallbackDrawCountUnitObject << ",\n";
  json << "    \"objectFallbackDrawCount\": "
       << shadowAgg.objectFallbackDrawCount << ",\n";
  json << "    \"semanticBridgeHit\": " << shadowAgg.semanticBridgeHit
       << ",\n";
  json << "    \"semanticBridgeMiss\": " << shadowAgg.semanticBridgeMiss
       << ",\n";
  json << "    \"semanticBridgeBypassed\": "
       << shadowAgg.semanticBridgeBypassed << ",\n";
  json << "    \"semanticSceneSubmitted\": "
       << shadowAgg.semanticSceneSubmitted << ",\n";
  json << "    \"semanticSceneSubmittedUnit\": "
       << shadowAgg.semanticSceneSubmittedUnit << ",\n";
  json << "    \"semanticSceneSubmittedSkinned\": "
       << shadowAgg.semanticSceneSubmittedSkinned << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedNonUnitResolvedCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedNonUnitResolvedCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedUnknownPacketKindCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedUnknownPacketKindCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedGroupNonZeroCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedGroupNonZeroCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedTransparentQueueCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedTransparentQueueCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedMissingUnitPtrCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedMissingUnitPtrCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount
       << ",\n";
  json << "    \"semanticSceneSubmittedBuilding\": "
       << shadowAgg.semanticSceneSubmittedBuilding << ",\n";
  json << "    \"semanticSceneSubmittedDestructible\": "
       << shadowAgg.semanticSceneSubmittedDestructible << ",\n";
  json << "    \"semanticSceneSubmittedCutout\": "
       << shadowAgg.semanticSceneSubmittedCutout << ",\n";
  json << "    \"semanticSceneSubmittedAlphaBlend\": "
       << shadowAgg.semanticSceneSubmittedAlphaBlend << ",\n";
  json << "    \"semanticSceneMaterialObservedCutoutCount\": "
       << shadowAgg.semanticSceneMaterialObservedCutoutCount << ",\n";
  json << "    \"semanticSceneMaterialObservedAlphaBlendCount\": "
       << shadowAgg.semanticSceneMaterialObservedAlphaBlendCount << ",\n";
  json << "    \"semanticSceneRejectedCutoutSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedCutoutSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedCutoutGeometry\": "
       << shadowAgg.semanticSceneRejectedCutoutGeometry << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendGeometry\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendGeometry << ",\n";
  json << "    \"semanticSceneRejectedCutoutVisualPolicy\": "
       << shadowAgg.semanticSceneRejectedCutoutVisualPolicy << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendVisualPolicy\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendVisualPolicy << ",\n";
  json << "    \"semanticSceneMaterialLayerContractResolvedCount\": "
       << shadowAgg.semanticSceneMaterialLayerContractResolvedCount << ",\n";
  json << "    \"semanticSceneMaterialLayerContractFailedCount\": "
       << shadowAgg.semanticSceneMaterialLayerContractFailedCount << ",\n";
  json << "    \"semanticSceneMaterialBlendMode0Count\": "
       << shadowAgg.semanticSceneMaterialBlendMode0Count << ",\n";
  json << "    \"semanticSceneMaterialBlendMode1Count\": "
       << shadowAgg.semanticSceneMaterialBlendMode1Count << ",\n";
  json << "    \"semanticSceneMaterialBlendMode2PlusCount\": "
       << shadowAgg.semanticSceneMaterialBlendMode2PlusCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawLayerIndexNonZeroCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshAttemptCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshAttemptCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshHitCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshHitCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshMissCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshMissCount << ",\n";
  json << "    \"semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount\": "
       << shadowAgg.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount << ",\n";
  json << "    \"semanticScenePaletteOverrideNoComposeCount\": "
       << shadowAgg.semanticScenePaletteOverrideNoComposeCount << ",\n";
  json << "    \"semanticScenePaletteOverrideWouldComposeCount\": "
       << shadowAgg.semanticScenePaletteOverrideWouldComposeCount << ",\n";
  json << "    \"semanticScenePalettePacketWorldComposeCount\": "
       << shadowAgg.semanticScenePalettePacketWorldComposeCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionSampleCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionSampleCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionNewRuntimeCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionNewRuntimeCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionRawChangedCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionRawChangedCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionRawStableCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionRawStableCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionGroupChangedCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionGroupChangedCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionGroupStableCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionGroupStableCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseAttemptCount\": "
       << shadowAgg.semanticSceneDrawTimePoseAttemptCount << ",\n";
  json << "    \"semanticSceneDrawTimePosePublishedCount\": "
       << shadowAgg.semanticSceneDrawTimePosePublishedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectUiOrEffectCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectUiOrEffectCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectVertexShaderCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectVertexShaderCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoVertexBlendCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoVertexBlendCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoContextCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoContextCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoRuntimeModelCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoRuntimeModelCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseDedupedCount\": "
       << shadowAgg.semanticSceneDrawTimePoseDedupedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseChangedCount\": "
       << shadowAgg.semanticSceneDrawTimePoseChangedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseStableCount\": "
       << shadowAgg.semanticSceneDrawTimePoseStableCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionSampleCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionSampleCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionNewRuntimeCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionNewRuntimeCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionChangedCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionChangedCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionStableCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionStableCount << ",\n";
  json << "    \"semanticSceneSkinnedDynamicIndexSliceCount\": "
       << shadowAgg.semanticSceneSkinnedDynamicIndexSliceCount << ",\n";
  json << "    \"semanticSceneSubmittedOwnedGroupSlots\": "
       << shadowAgg.semanticSceneSubmittedOwnedGroupSlots << ",\n";
  json << "    \"semanticSceneCurrentDrawContractKnownCount\": "
       << shadowAgg.semanticSceneCurrentDrawContractKnownCount << ",\n";
  json << "    \"semanticSceneCurrentDrawPaletteReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawPaletteReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawGroupSlotReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawGroupSlotReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawResolveReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawResolveReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoContract\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoContract << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoPalette\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoPalette << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoGroupSlots\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoGroupSlots << ",\n";
  json << "    \"semanticSceneCurrentDrawMissStaleVisibleFrame\": "
       << shadowAgg.semanticSceneCurrentDrawMissStaleVisibleFrame << ",\n";
  json << "    \"semanticSceneCurrentDrawResolveReadyRejectedCount\": "
       << shadowAgg.semanticSceneCurrentDrawResolveReadyRejectedCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyCount\": "
       << shadowAgg.semanticSceneCanonicalReadyCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyCutoutCount\": "
       << shadowAgg.semanticSceneCanonicalReadyCutoutCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyAlphaBlendCount\": "
       << shadowAgg.semanticSceneCanonicalReadyAlphaBlendCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyCutoutCount\": "
       << shadowAgg.semanticSceneCanonicalReadyCutoutCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyAlphaBlendCount\": "
       << shadowAgg.semanticSceneCanonicalReadyAlphaBlendCount << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoStableIdentity\": "
       << shadowAgg.semanticSceneCanonicalRejectNoStableIdentity << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoMesh\": "
       << shadowAgg.semanticSceneCanonicalRejectNoMesh << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoWorldTransform\": "
       << shadowAgg.semanticSceneCanonicalRejectNoWorldTransform << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoPalette\": "
       << shadowAgg.semanticSceneCanonicalRejectNoPalette << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoSlotContract\": "
       << shadowAgg.semanticSceneCanonicalRejectNoSlotContract << ",\n";
  json << "    \"semanticSceneCanonicalRejectStaleProducer\": "
       << shadowAgg.semanticSceneCanonicalRejectStaleProducer << ",\n";
  json << "    \"semanticSceneCanonicalRejectInvalidVertexIndex\": "
       << shadowAgg.semanticSceneCanonicalRejectInvalidVertexIndex << ",\n";
  json << "    \"semanticSceneCanonicalRejectExplicitBlendIncomplete\": "
       << shadowAgg.semanticSceneCanonicalRejectExplicitBlendIncomplete << ",\n";
  json << "    \"semanticSceneCanonicalRejectAfterReadyCount\": "
       << shadowAgg.semanticSceneCanonicalRejectAfterReadyCount << ",\n";
  json << "    \"semanticSceneSubmittedExplicitBlendContract\": "
       << shadowAgg.semanticSceneSubmittedExplicitBlendContract << ",\n";
  json << "    \"semanticSceneSubmittedSingleMatrixGroupSkinning\": "
       << shadowAgg.semanticSceneSubmittedSingleMatrixGroupSkinning << ",\n";
  json << "    \"semanticSceneSubmittedMultiGroupSlotSkinning\": "
       << shadowAgg.semanticSceneSubmittedMultiGroupSlotSkinning << ",\n";
  json << "    \"semanticSceneSkinnedMinUniqueGroupSlots\": "
       << shadowAgg.semanticSceneSkinnedMinUniqueGroupSlots << ",\n";
  json << "    \"semanticSceneSkinnedMaxUniqueGroupSlots\": "
       << shadowAgg.semanticSceneSkinnedMaxUniqueGroupSlots << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique1Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique1Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique2To4Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique2To4Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique5To8Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique5To8Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique9To16Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique9To16Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique17PlusCount\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique17PlusCount << ",\n";
  json << "    \"semanticSceneExplicitBlendUnavailableCurrentDraw\": "
       << shadowAgg.semanticSceneExplicitBlendUnavailableCurrentDraw << ",\n";
  json << "    \"semanticSceneSkinnedFullIndexFallbackCount\": "
       << shadowAgg.semanticSceneSkinnedFullIndexFallbackCount << ",\n";
  json << "    \"semanticSceneSkinnedMissingVisibleIndexSliceRejectCount\": "
       << shadowAgg.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount
       << ",\n";
  json << "    \"semanticSceneSubmittedFrameLocal\": "
       << shadowAgg.semanticSceneSubmittedFrameLocal << ",\n";
  json << "    \"semanticSceneSubmittedPersistent\": "
       << shadowAgg.semanticSceneSubmittedPersistent << ",\n";
  json << "    \"semanticScenePopulateAttemptCount\": "
       << shadowAgg.semanticScenePopulateAttemptCount << ",\n";
  json << "    \"semanticScenePopulateUnitsOnlyCount\": "
       << shadowAgg.semanticScenePopulateUnitsOnlyCount << ",\n";
  json << "    \"semanticScenePopulateLastReturnReason\": "
       << shadowAgg.semanticScenePopulateLastReturnReason << ",\n";
  json << "    \"semanticScenePopulateLastProducerPublishAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerPublishAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerPublishReadyDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerPublishReadyDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerQueryAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerQueryAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerQueryHitDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerQueryHitDelta << ",\n";
  json << "    \"semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta\": "
       << shadowAgg
              .semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta\": "
       << shadowAgg
              .semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerGroupDecodeAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerGroupDecodeAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerGroupDecodeHitDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerGroupDecodeHitDelta
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawRecordCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawRecordCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawBuiltPacketCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawBuiltPacketCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount\": "
        << shadowAgg
               .semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount
        << ",\n";
  // Phase 7.2: flicker diagnostics + reconciliation
  json << "    \"semanticSceneDirectLastRawRecordCount\": "
       << shadowAgg.semanticSceneDirectLastRawRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastEligibleRecordCount\": "
       << shadowAgg.semanticSceneDirectLastEligibleRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedRecordCount\": "
       << shadowAgg.semanticSceneDirectLastSubmittedRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastUniqueObjectCount\": "
       << shadowAgg.semanticSceneDirectLastUniqueObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedObjectCount\": "
       << shadowAgg.semanticSceneDirectLastSubmittedObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastRecordCapPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectLastRecordCapPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastScanCapPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectLastScanCapPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastMinGeosetsPerObject\": "
       << shadowAgg.semanticSceneDirectLastMinGeosetsPerObject << ",\n";
  json << "    \"semanticSceneDirectLastMaxGeosetsPerObject\": "
       << shadowAgg.semanticSceneDirectLastMaxGeosetsPerObject << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedIdentityHash\": "
       << shadowAgg.semanticSceneDirectLastSubmittedIdentityHash << ",\n";
  json << "    \"semanticSceneDirectIdentityChurnCount\": "
       << shadowAgg.semanticSceneDirectIdentityChurnCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapHitCount\": "
       << shadowAgg.semanticSceneDirectRecordCapHitCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapTruncatedRecordCount\": "
       << shadowAgg.semanticSceneDirectRecordCapTruncatedRecordCount << ",\n";
  json << "    \"semanticSceneDirectScanCapHitCount\": "
       << shadowAgg.semanticSceneDirectScanCapHitCount << ",\n";
  json << "    \"semanticSceneDirectObjectGroupedSubmitCount\": "
       << shadowAgg.semanticSceneDirectObjectGroupedSubmitCount << ",\n";
  json << "    \"semanticSceneDirectObjectGroupedSkipCount\": "
       << shadowAgg.semanticSceneDirectObjectGroupedSkipCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapSkipObjectCount\": "
       << shadowAgg.semanticSceneDirectRecordCapSkipObjectCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapAppendFailCount\": "
       << shadowAgg.semanticSceneDirectRecordCapAppendFailCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeaseActiveKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeaseActiveKeyCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeasePrunedKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeasePrunedKeyCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeaseSubmittedKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeaseSubmittedKeyCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillBudgetRecordCount\": "
       << shadowAgg.semanticSceneDirectStickyFillBudgetRecordCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillAppendedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillAppendedCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillSubmittedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillSubmittedCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillMissedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillMissedCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRestoredCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRestoredCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseUpdatedCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseUpdatedCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseExpiredCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseExpiredCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedDynamicMeshCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedDynamicMeshCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedNotSelfContainedCount\": "
       << shadowAgg
              .semanticSceneDirectPartLeaseRejectedNotSelfContainedCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedUnsafeBackingCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedSelfRenewCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedSelfRenewCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseBudgetLimitCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseBudgetLimitCount << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRestoredCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseRestoredCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseExpiredCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseExpiredCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseBudgetLimitCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseBudgetLimitCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreCompleteCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCoreCompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreIncompleteSkipCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCoreIncompleteSkipCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartOmittedIncompleteCoreCount\": "
       << shadowAgg.semanticSceneShadowManifestPartOmittedIncompleteCoreCount
       << ",\n";
  // Phase 7.25 core epoch planner 专属计数器。
  json << "    \"semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochMissingPartCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochMissingPartCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount
       << ",\n";
  // Phase 7.28：skinned palette content stability probe。
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceNoneCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteSourceNoneCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStablePartSampleCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStablePartSampleCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteHashChurnCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteHashChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteCountChurnCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteCountChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteAttributionSnapshotHitCount\": "
       << shadowAgg
              .semanticSceneDirectPaletteAttributionSnapshotHitCount
       << ",\n";
  json << "    \"semanticSceneDirectManifestObjectCount\": "
       << shadowAgg.semanticSceneDirectManifestObjectCount << ",\n";
  json << "    \"semanticSceneDirectManifestObservedPartCount\": "
       << shadowAgg.semanticSceneDirectManifestObservedPartCount << ",\n";
  json << "    \"semanticSceneDirectManifestShadowEligiblePartCount\": "
       << shadowAgg.semanticSceneDirectManifestShadowEligiblePartCount << ",\n";
  json << "    \"semanticSceneDirectObjectCompleteEligibleCount\": "
       << shadowAgg.semanticSceneDirectObjectCompleteEligibleCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByScanCapCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByScanCapCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByAlphaPolicyCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByAlphaPolicyCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteBySliceUnresolvedCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByPacketBuildFailCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByPacketBuildFailCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByAppendFailCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByAppendFailCount << ",\n";
  json << "    \"semanticSceneDirectSubmittedCompleteObjectCount\": "
       << shadowAgg.semanticSceneDirectSubmittedCompleteObjectCount << ",\n";
  json << "    \"semanticSceneDirectSubmittedPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectSubmittedPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceAuthoritativeCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceAuthoritativeCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceFallbackLayerIndexCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceFallbackLayerIndexCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceMissingCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceMissingCount << ",\n";
  json << "    \"semanticScenePreparedProbeAttemptCount\": "
       << shadowAgg.semanticScenePreparedProbeAttemptCount << ",\n";
  json << "    \"semanticScenePreparedProbeContextReadyCount\": "
       << shadowAgg.semanticScenePreparedProbeContextReadyCount << ",\n";
  json << "    \"semanticScenePreparedProbeBackingReadableCount\": "
       << shadowAgg.semanticScenePreparedProbeBackingReadableCount << ",\n";
  json << "    \"semanticScenePreparedSliceRecordedCount\": "
       << shadowAgg.semanticScenePreparedSliceRecordedCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryAttemptCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryAttemptCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryHitCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryHitCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryMissCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryMissCount << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestPartCount\": "
       << shadowAgg.semanticSceneShadowManifestPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestStableObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestStableObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestNewObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestNewObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestExpiredObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestExpiredObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestFreshPartCount\": "
       << shadowAgg.semanticSceneShadowManifestFreshPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestLeaseablePartCount\": "
       << shadowAgg.semanticSceneShadowManifestLeaseablePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestPoseStalePartCount\": "
       << shadowAgg.semanticSceneShadowManifestPoseStalePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestSliceStalePartCount\": "
       << shadowAgg.semanticSceneShadowManifestSliceStalePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestExpiredPartCount\": "
       << shadowAgg.semanticSceneShadowManifestExpiredPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestMultiSlicePartCount\": "
       << shadowAgg.semanticSceneShadowManifestMultiSlicePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestPayload11CChurnCount\": "
       << shadowAgg.semanticSceneShadowManifestPayload11CChurnCount << ",\n";
  json << "    \"semanticSceneShadowManifestRenderablePartChurnCount\": "
       << shadowAgg.semanticSceneShadowManifestRenderablePartChurnCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseHitCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseHitCount << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseMissCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseMissCount << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseNoRuntimeCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseNoRuntimeCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastMatrixCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastMatrixCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastMatrixHash\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastMatrixHash
       << ",\n";
  json << "    \"semanticSceneSubmittedObjectJaccardMilli\": "
       << shadowAgg.semanticSceneSubmittedObjectJaccardMilli << ",\n";
  json << "    \"semanticSceneSubmittedPartJaccardMilli\": "
       << shadowAgg.semanticSceneSubmittedPartJaccardMilli << ",\n";
  json << "    \"semanticSceneVisibleLookupPartLayerHitCount\": "
       << shadowAgg.semanticSceneVisibleLookupPartLayerHitCount << ",\n";
  json << "    \"semanticSceneVisibleLookupSingleFallbackCount\": "
       << shadowAgg.semanticSceneVisibleLookupSingleFallbackCount << ",\n";
  json << "    \"semanticSceneVisibleLookupMissCount\": "
       << shadowAgg.semanticSceneVisibleLookupMissCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingNotCheckedCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingNotCheckedCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingPassCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingPassCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNoRenderablePartCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailNoRenderablePartCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailLookupMissCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailLookupMissCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNonMainQueueCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailNonMainQueueCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNonWorldGroupCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailIdentityMismatchCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailIdentityMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteHashChurnCount\": "
       << shadowAgg.semanticSceneDirectPaletteHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectGroupHashChurnCount\": "
       << shadowAgg.semanticSceneDirectGroupHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectStableGroupHashChurnCount\": "
       << shadowAgg.semanticSceneDirectStableGroupHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectStream1PtrChurnCount\": "
       << shadowAgg.semanticSceneDirectStream1PtrChurnCount << ",\n";
  json << "    \"semanticSceneDirectGeometrySourceHashChurnCount\": "
       << shadowAgg.semanticSceneDirectGeometrySourceHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectSameCasterComparisonCount\": "
       << shadowAgg.semanticSceneDirectSameCasterComparisonCount << ",\n";
  json << "    \"semanticSceneDirectIdentitySkippedChurnCount\": "
       << shadowAgg.semanticSceneDirectIdentitySkippedChurnCount << ",\n";
  json << "    \"semanticSceneDirectPaletteRootDeltaSampleCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootDeltaSampleCount << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedTinyDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedSmallDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedMediumDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedLargeDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootMaxDeltaMilli\": "
       << shadowAgg.semanticSceneDirectPaletteRootMaxDeltaMilli << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyUnitPtrCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyUnitPtrCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyJHandleCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyJHandleCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyRuntimeModelCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyRuntimeModelCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyWorldObjectCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyWorldObjectCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeySceneNodeCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeySceneNodeCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyModelMeshCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyModelMeshCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyRenderablePartCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyRenderablePartCount
       << ",\n";
  json << "    \"semanticSceneLastAppendedGeometrySourceHash\": "
       << shadowAgg.semanticSceneLastAppendedGeometrySourceHash << ",\n";
  json << "    \"semanticSceneLastAppendedGeometryId\": "
       << shadowAgg.semanticSceneLastAppendedGeometryId << ",\n";
  json << "    \"semanticSceneShadowCastersCount\": "
       << shadowAgg.semanticSceneShadowCastersCount << ",\n";
  json << "    \"semanticSceneReplayDrawsCount\": "
       << shadowAgg.semanticSceneReplayDrawsCount << ",\n";
  json << "    \"semanticSceneShadowMapDrawnCasters\": "
       << shadowAgg.semanticSceneShadowMapDrawnCasters << ",\n";
  json << "    \"semanticSceneShadowMapCascadeCulledCount\": "
       << shadowAgg.semanticSceneShadowMapCascadeCulledCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedCasterCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedCasterCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedPreparedCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedPreparedCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedInvalidBufferCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedInvalidBufferCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedInvalidPipelineCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedInvalidPipelineCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedDrawnCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedDrawnCount << ",\n";
  json << "    \"semanticSceneShadowTaaActive\": "
       << shadowAgg.semanticSceneShadowTaaActive << ",\n";
  json << "    \"semanticSceneReceiverReuseShadowMap\": "
       << shadowAgg.semanticSceneReceiverReuseShadowMap << ",\n";
  json << "    \"semanticSceneReceiverInputValid\": "
       << shadowAgg.semanticSceneReceiverInputValid << ",\n";
  json << "    \"semanticSceneReceiverInputRejectReason\": "
       << shadowAgg.semanticSceneReceiverInputRejectReason << ",\n";
  json << "    \"semanticSceneReceiverNeedPass\": "
       << shadowAgg.semanticSceneReceiverNeedPass << ",\n";
  json << "    \"semanticSceneReceiverNeedShadowMap\": "
       << shadowAgg.semanticSceneReceiverNeedShadowMap << ",\n";
  json << "    \"semanticSceneReceiverHasCompleteShadowMap\": "
       << shadowAgg.semanticSceneReceiverHasCompleteShadowMap << ",\n";
  json << "    \"semanticSceneReceiverHasUsableDirectionalShadow\": "
       << shadowAgg.semanticSceneReceiverHasUsableDirectionalShadow << ",\n";
  json << "    \"semanticSceneReceiverActiveStrengthMilli\": "
       << shadowAgg.semanticSceneReceiverActiveStrengthMilli << ",\n";
  json << "    \"semanticSceneReceiverUboStrengthMilli\": "
       << shadowAgg.semanticSceneReceiverUboStrengthMilli << ",\n";
  json << "    \"semanticSceneReceiverDebugMode\": "
       << shadowAgg.semanticSceneReceiverDebugMode << ",\n";
  json << "    \"semanticSceneReceiverCsmCascadeCount\": "
       << shadowAgg.semanticSceneReceiverCsmCascadeCount << ",\n";
  json << "    \"semanticSceneReceiverZeroStrengthFrameCount\": "
       << shadowAgg.semanticSceneReceiverZeroStrengthFrameCount << ",\n";
  json << "    \"semanticSceneReceiverDrawnWithZeroStrengthCount\": "
       << shadowAgg.semanticSceneReceiverDrawnWithZeroStrengthCount << ",\n";
  json << "    \"semanticSceneReceiverNoCompleteShadowMapCount\": "
       << shadowAgg.semanticSceneReceiverNoCompleteShadowMapCount << ",\n";
  json << "    \"semanticSceneReceiverNoShadowMapImageCount\": "
       << shadowAgg.semanticSceneReceiverNoShadowMapImageCount << ",\n";
  json << "    \"semanticSceneReceiverNoShadowMapSampleViewCount\": "
       << shadowAgg.semanticSceneReceiverNoShadowMapSampleViewCount << ",\n";
  json << "    \"semanticSceneReceiverNoCandidateCsmCount\": "
       << shadowAgg.semanticSceneReceiverNoCandidateCsmCount << ",\n";
  json << "    \"semanticSceneReceiverCsmFallbackToLastGoodCount\": "
       << shadowAgg.semanticSceneReceiverCsmFallbackToLastGoodCount << ",\n";
  json << "    \"semanticSceneReceiverHoldInvalidCsmCount\": "
       << shadowAgg.semanticSceneReceiverHoldInvalidCsmCount << ",\n";
  json << "    \"semanticSceneReceiverHoldEmptyReplayCount\": "
       << shadowAgg.semanticSceneReceiverHoldEmptyReplayCount << ",\n";
  json << "    \"semanticSceneReceiverHoldIdentityChurnCount\": "
       << shadowAgg.semanticSceneReceiverHoldIdentityChurnCount << ",\n";
  json << "    \"semanticSceneReceiverReuseInvalidatedAfterEnsureCount\": "
       << shadowAgg.semanticSceneReceiverReuseInvalidatedAfterEnsureCount
       << ",\n";
  json << "    \"semanticSceneShadowMapRenderSkippedNoResourcesCount\": "
       << shadowAgg.semanticSceneShadowMapRenderSkippedNoResourcesCount
       << ",\n";
  json << "    \"semanticSceneShadowMapRenderSkippedNoMatrixBufferCount\": "
       << shadowAgg.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount
       << ",\n";
  json << "    \"semanticSceneReceiverViewportX\": "
       << shadowAgg.semanticSceneReceiverViewportX << ",\n";
  json << "    \"semanticSceneReceiverViewportY\": "
       << shadowAgg.semanticSceneReceiverViewportY << ",\n";
  json << "    \"semanticSceneReceiverViewportWidth\": "
       << shadowAgg.semanticSceneReceiverViewportWidth << ",\n";
  json << "    \"semanticSceneReceiverViewportHeight\": "
       << shadowAgg.semanticSceneReceiverViewportHeight << ",\n";
  json << "    \"semanticSceneSkippedUnitsOnlyFilter\": "
       << shadowAgg.semanticSceneSkippedUnitsOnlyFilter << ",\n";
  json << "    \"semanticSceneAcceptedExplicitResourceOwnerRigid\": "
       << shadowAgg.semanticSceneAcceptedExplicitResourceOwnerRigid << ",\n";
  json << "    \"semanticSceneRejectedNoVertex\": "
       << shadowAgg.semanticSceneRejectedNoVertex << ",\n";
  json << "    \"semanticSceneRejectedSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedGeometry\": "
       << shadowAgg.semanticSceneRejectedGeometry << ",\n";
  json << "    \"semanticSceneRejectedGeometryFrameLocal\": "
       << shadowAgg.semanticSceneRejectedGeometryFrameLocal << ",\n";
  json << "    \"semanticSceneRejectedGeometryPersistent\": "
       << shadowAgg.semanticSceneRejectedGeometryPersistent << ",\n";
  json << "    \"semanticFallbackPruned\": "
       << shadowAgg.semanticFallbackPruned << ",\n";
  json << "    \"semanticFallbackPrunedByHandle\": "
       << shadowAgg.semanticFallbackPrunedByHandle << ",\n";
  json << "    \"semanticFallbackPrunedByWorldObjectEntry\": "
       << shadowAgg.semanticFallbackPrunedByWorldObjectEntry << ",\n";
  json << "    \"semanticFallbackPrunedBySceneNode\": "
       << shadowAgg.semanticFallbackPrunedBySceneNode << ",\n";
  json << "    \"semanticFallbackPrunedByRuntimeModel\": "
       << shadowAgg.semanticFallbackPrunedByRuntimeModel << ",\n";
  json << "    \"instancedGeometryGroups\": "
       << shadowAgg.instancedGeometryGroups << ",\n";
  json << "    \"instancedGeometryInstances\": "
       << shadowAgg.instancedGeometryInstances << ",\n";
  json << "    \"instancedGeometryDrawsSaved\": "
       << shadowAgg.instancedGeometryDrawsSaved << ",\n";
  json << "    \"reusedFreezeMb\": "
       << (static_cast<double>(shadowAgg.reusedFreezeBytes) / (1024.0 * 1024.0))
       << ",\n";
  json << "    \"actualFreezeReuseMb\": "
       << (static_cast<double>(shadowAgg.actualFreezeReuseBytes) /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"uniqueFreezeAcceptedMb\": "
       << (static_cast<double>(shadowAgg.uniqueFreezeAcceptedBytes) /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"duplicateFreezeBypassMb\": "
       << (static_cast<double>(shadowAgg.duplicateFreezeBypassBytes) /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"potentialFreezeReuseMb\": "
       << (static_cast<double>(shadowAgg.potentialFreezeReuseBytes) /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"phases\": [\n";
  for (size_t i = 0; i < shadowAgg.phases.size(); ++i) {
    const auto& phase = shadowAgg.phases[i];
    if (i != 0)
      json << ",\n";
    json << "      {\"name\": \"" << phase.name << "\", \"requestedMb\": "
         << (static_cast<double>(phase.requestedBytes) / (1024.0 * 1024.0))
         << ", \"acceptedMb\": "
         << (static_cast<double>(phase.acceptedBytes) / (1024.0 * 1024.0))
         << ", \"rejectedMb\": "
         << (static_cast<double>(phase.rejectedBytes) / (1024.0 * 1024.0))
         << ", \"requests\": " << phase.requests
         << ", \"rejects\": " << phase.rejects << "}";
  }
  json << "\n    ]\n";
  json << "  },\n";

  std::vector<War3PerfMonitor::ShadowBudgetAggregate::PhaseAggregate>
      topShadowOffenders(shadowAgg.phases.begin(), shadowAgg.phases.end());
  std::sort(topShadowOffenders.begin(), topShadowOffenders.end(),
            [](const auto& a, const auto& b) {
              if (a.rejectedBytes != b.rejectedBytes)
                return a.rejectedBytes > b.rejectedBytes;
              return a.requestedBytes > b.requestedBytes;
            });
  json << "  \"topShadowOffenders\": [\n";
  for (size_t i = 0; i < topShadowOffenders.size(); ++i) {
    const auto& offender = topShadowOffenders[i];
    if (i != 0)
      json << ",\n";
    json << "    {\"name\": \"" << offender.name << "\", \"requestedMb\": "
         << (static_cast<double>(offender.requestedBytes) / (1024.0 * 1024.0))
         << ", \"rejectedMb\": "
         << (static_cast<double>(offender.rejectedBytes) / (1024.0 * 1024.0))
         << ", \"rejects\": " << offender.rejects << "}";
  }
  json << "\n  ],\n";

  json << "  \"shadowRuntimeV2Summary\": {\n";
  const auto runtimeSummary =
      dxvk::war3::render::QueryShadowRuntimeBridgeSummary();
  json << "    \"staticPersistentCount\": "
       << shadowAgg.staticPersistentCount << ",\n";
  json << "    \"dynamicPoseCount\": " << shadowAgg.dynamicPoseCount
       << ",\n";
  json << "    \"dynamicSkinnedOutputCount\": "
       << shadowAgg.dynamicSkinnedOutputCount << ",\n";
  json << "    \"fallbackDrawCount\": " << shadowAgg.fallbackDrawCount
       << ",\n";
  json << "    \"fallbackDrawCountTerrain\": "
       << shadowAgg.fallbackDrawCountTerrain << ",\n";
  json << "    \"fallbackDrawCountWorldObject\": "
       << shadowAgg.fallbackDrawCountWorldObject << ",\n";
  json << "    \"fallbackDrawCountUnitObject\": "
       << shadowAgg.fallbackDrawCountUnitObject << ",\n";
  json << "    \"objectFallbackDrawCount\": "
       << shadowAgg.objectFallbackDrawCount << ",\n";
  json << "    \"semanticBridgeHit\": " << shadowAgg.semanticBridgeHit
       << ",\n";
  json << "    \"semanticBridgeMiss\": " << shadowAgg.semanticBridgeMiss
       << ",\n";
  json << "    \"semanticBridgeBypassed\": "
       << shadowAgg.semanticBridgeBypassed << ",\n";
  json << "    \"semanticSceneSubmitted\": "
       << shadowAgg.semanticSceneSubmitted << ",\n";
  json << "    \"semanticSceneSubmittedUnit\": "
       << shadowAgg.semanticSceneSubmittedUnit << ",\n";
  json << "    \"semanticSceneSubmittedSkinned\": "
       << shadowAgg.semanticSceneSubmittedSkinned << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedNonUnitResolvedCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedNonUnitResolvedCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedUnknownPacketKindCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedUnknownPacketKindCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedGroupNonZeroCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedGroupNonZeroCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedTransparentQueueCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedTransparentQueueCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedMissingUnitPtrCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedMissingUnitPtrCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount
       << ",\n";
  json << "    \"semanticSceneSubmittedBuilding\": "
       << shadowAgg.semanticSceneSubmittedBuilding << ",\n";
  json << "    \"semanticSceneSubmittedDestructible\": "
       << shadowAgg.semanticSceneSubmittedDestructible << ",\n";
  json << "    \"semanticSceneSubmittedCutout\": "
       << shadowAgg.semanticSceneSubmittedCutout << ",\n";
  json << "    \"semanticSceneSubmittedAlphaBlend\": "
       << shadowAgg.semanticSceneSubmittedAlphaBlend << ",\n";
  json << "    \"semanticSceneMaterialObservedCutoutCount\": "
       << shadowAgg.semanticSceneMaterialObservedCutoutCount << ",\n";
  json << "    \"semanticSceneMaterialObservedAlphaBlendCount\": "
       << shadowAgg.semanticSceneMaterialObservedAlphaBlendCount << ",\n";
  json << "    \"semanticSceneRejectedCutoutSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedCutoutSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedCutoutGeometry\": "
       << shadowAgg.semanticSceneRejectedCutoutGeometry << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendGeometry\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendGeometry << ",\n";
  json << "    \"semanticSceneRejectedCutoutVisualPolicy\": "
       << shadowAgg.semanticSceneRejectedCutoutVisualPolicy << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendVisualPolicy\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendVisualPolicy << ",\n";
  json << "    \"semanticSceneMaterialLayerContractResolvedCount\": "
       << shadowAgg.semanticSceneMaterialLayerContractResolvedCount << ",\n";
  json << "    \"semanticSceneMaterialLayerContractFailedCount\": "
       << shadowAgg.semanticSceneMaterialLayerContractFailedCount << ",\n";
  json << "    \"semanticSceneMaterialBlendMode0Count\": "
       << shadowAgg.semanticSceneMaterialBlendMode0Count << ",\n";
  json << "    \"semanticSceneMaterialBlendMode1Count\": "
       << shadowAgg.semanticSceneMaterialBlendMode1Count << ",\n";
  json << "    \"semanticSceneMaterialBlendMode2PlusCount\": "
       << shadowAgg.semanticSceneMaterialBlendMode2PlusCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawLayerIndexNonZeroCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshAttemptCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshAttemptCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshHitCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshHitCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshMissCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshMissCount << ",\n";
  json << "    \"semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount\": "
       << shadowAgg.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount << ",\n";
  json << "    \"semanticScenePaletteOverrideNoComposeCount\": "
       << shadowAgg.semanticScenePaletteOverrideNoComposeCount << ",\n";
  json << "    \"semanticScenePaletteOverrideWouldComposeCount\": "
       << shadowAgg.semanticScenePaletteOverrideWouldComposeCount << ",\n";
  json << "    \"semanticScenePalettePacketWorldComposeCount\": "
       << shadowAgg.semanticScenePalettePacketWorldComposeCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshLastRuntimeModelPtr\": "
       << runtimeSummary.semanticSceneLivePaletteRefreshLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshLastMatrixCount\": "
       << runtimeSummary.semanticSceneLivePaletteRefreshLastMatrixCount
       << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshLastMatrixHash\": "
       << runtimeSummary.semanticSceneLivePaletteRefreshLastMatrixHash
       << ",\n";
  json << "    \"semanticSceneLivePaletteMotionSampleCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionSampleCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionNewRuntimeCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionNewRuntimeCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionRawChangedCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionRawChangedCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionRawStableCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionRawStableCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionGroupChangedCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionGroupChangedCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionGroupStableCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionGroupStableCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastRuntimeModelPtr\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastPrevRawHash\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastPrevRawHash
       << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastRawHash\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastRawHash << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastPrevGroupHash\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastPrevGroupHash
       << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastGroupHash\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastGroupHash << ",\n";
  json << "    \"semanticSceneDrawTimePoseAttemptCount\": "
       << shadowAgg.semanticSceneDrawTimePoseAttemptCount << ",\n";
  json << "    \"semanticSceneDrawTimePosePublishedCount\": "
       << shadowAgg.semanticSceneDrawTimePosePublishedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectUiOrEffectCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectUiOrEffectCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectVertexShaderCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectVertexShaderCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoVertexBlendCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoVertexBlendCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoContextCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoContextCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoRuntimeModelCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoRuntimeModelCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseDedupedCount\": "
       << shadowAgg.semanticSceneDrawTimePoseDedupedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseChangedCount\": "
       << shadowAgg.semanticSceneDrawTimePoseChangedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseStableCount\": "
       << shadowAgg.semanticSceneDrawTimePoseStableCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseLastRuntimeModelPtr\": "
       << runtimeSummary.semanticSceneDrawTimePoseLastRuntimeModelPtr << ",\n";
  json << "    \"semanticSceneDrawTimePoseLastPrevHash\": "
       << runtimeSummary.semanticSceneDrawTimePoseLastPrevHash << ",\n";
  json << "    \"semanticSceneDrawTimePoseLastHash\": "
       << runtimeSummary.semanticSceneDrawTimePoseLastHash << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionSampleCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionSampleCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionNewRuntimeCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionNewRuntimeCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionChangedCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionChangedCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionStableCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionStableCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr\": "
       << runtimeSummary.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionLastPrevHash\": "
       << runtimeSummary.semanticSceneSubmittedPaletteMotionLastPrevHash
       << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionLastHash\": "
       << runtimeSummary.semanticSceneSubmittedPaletteMotionLastHash << ",\n";
  json << "    \"semanticSceneSkinnedDynamicIndexSliceCount\": "
       << shadowAgg.semanticSceneSkinnedDynamicIndexSliceCount << ",\n";
  json << "    \"semanticSceneSubmittedOwnedGroupSlots\": "
       << shadowAgg.semanticSceneSubmittedOwnedGroupSlots << ",\n";
  json << "    \"semanticSceneCurrentDrawContractKnownCount\": "
       << shadowAgg.semanticSceneCurrentDrawContractKnownCount << ",\n";
  json << "    \"semanticSceneCurrentDrawPaletteReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawPaletteReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawGroupSlotReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawGroupSlotReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawResolveReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawResolveReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoContract\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoContract << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoPalette\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoPalette << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoGroupSlots\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoGroupSlots << ",\n";
  json << "    \"semanticSceneCurrentDrawMissStaleVisibleFrame\": "
       << shadowAgg.semanticSceneCurrentDrawMissStaleVisibleFrame << ",\n";
  json << "    \"semanticSceneCurrentDrawResolveReadyRejectedCount\": "
       << shadowAgg.semanticSceneCurrentDrawResolveReadyRejectedCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyCount\": "
       << shadowAgg.semanticSceneCanonicalReadyCount << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoStableIdentity\": "
       << shadowAgg.semanticSceneCanonicalRejectNoStableIdentity << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoMesh\": "
       << shadowAgg.semanticSceneCanonicalRejectNoMesh << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoWorldTransform\": "
       << shadowAgg.semanticSceneCanonicalRejectNoWorldTransform << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoPalette\": "
       << shadowAgg.semanticSceneCanonicalRejectNoPalette << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoSlotContract\": "
       << shadowAgg.semanticSceneCanonicalRejectNoSlotContract << ",\n";
  json << "    \"semanticSceneCanonicalRejectStaleProducer\": "
       << shadowAgg.semanticSceneCanonicalRejectStaleProducer << ",\n";
  json << "    \"semanticSceneCanonicalRejectInvalidVertexIndex\": "
       << shadowAgg.semanticSceneCanonicalRejectInvalidVertexIndex << ",\n";
  json << "    \"semanticSceneCanonicalRejectExplicitBlendIncomplete\": "
       << shadowAgg.semanticSceneCanonicalRejectExplicitBlendIncomplete << ",\n";
  json << "    \"semanticSceneCanonicalRejectAfterReadyCount\": "
       << shadowAgg.semanticSceneCanonicalRejectAfterReadyCount << ",\n";
  json << "    \"semanticSceneSubmittedExplicitBlendContract\": "
       << shadowAgg.semanticSceneSubmittedExplicitBlendContract << ",\n";
  json << "    \"semanticSceneSubmittedSingleMatrixGroupSkinning\": "
       << shadowAgg.semanticSceneSubmittedSingleMatrixGroupSkinning << ",\n";
  json << "    \"semanticSceneSubmittedMultiGroupSlotSkinning\": "
       << shadowAgg.semanticSceneSubmittedMultiGroupSlotSkinning << ",\n";
  json << "    \"semanticSceneSkinnedMinUniqueGroupSlots\": "
       << shadowAgg.semanticSceneSkinnedMinUniqueGroupSlots << ",\n";
  json << "    \"semanticSceneSkinnedMaxUniqueGroupSlots\": "
       << shadowAgg.semanticSceneSkinnedMaxUniqueGroupSlots << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique1Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique1Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique2To4Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique2To4Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique5To8Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique5To8Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique9To16Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique9To16Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique17PlusCount\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique17PlusCount << ",\n";
  json << "    \"semanticSceneExplicitBlendUnavailableCurrentDraw\": "
       << shadowAgg.semanticSceneExplicitBlendUnavailableCurrentDraw << ",\n";
  json << "    \"semanticSceneSkinnedFullIndexFallbackCount\": "
       << shadowAgg.semanticSceneSkinnedFullIndexFallbackCount << ",\n";
  json << "    \"semanticSceneSkinnedMissingVisibleIndexSliceRejectCount\": "
       << shadowAgg.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount
       << ",\n";
  json << "    \"semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr\": "
       << runtimeSummary
              .semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneSkinnedFullIndexFallbackLastIndexCount\": "
       << runtimeSummary.semanticSceneSkinnedFullIndexFallbackLastIndexCount
       << ",\n";
  json << "    \"semanticSceneSubmittedFrameLocal\": "
       << shadowAgg.semanticSceneSubmittedFrameLocal << ",\n";
  json << "    \"semanticSceneSubmittedPersistent\": "
       << shadowAgg.semanticSceneSubmittedPersistent << ",\n";
  json << "    \"semanticScenePopulateAttemptCount\": "
       << shadowAgg.semanticScenePopulateAttemptCount << ",\n";
  json << "    \"semanticScenePopulateUnitsOnlyCount\": "
       << shadowAgg.semanticScenePopulateUnitsOnlyCount << ",\n";
  json << "    \"semanticScenePopulateLastReturnReason\": "
       << shadowAgg.semanticScenePopulateLastReturnReason << ",\n";
  json << "    \"semanticScenePopulateLastProducerPublishAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerPublishAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerPublishReadyDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerPublishReadyDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerQueryAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerQueryAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerQueryHitDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerQueryHitDelta << ",\n";
  json << "    \"semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta\": "
       << shadowAgg
              .semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta\": "
       << shadowAgg
              .semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerGroupDecodeAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerGroupDecodeAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerGroupDecodeHitDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerGroupDecodeHitDelta
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawRecordCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawRecordCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawBuiltPacketCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawBuiltPacketCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount\": "
        << shadowAgg
               .semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount
        << ",\n";
  // Phase 7.2: flicker diagnostics + reconciliation
  json << "    \"semanticSceneDirectLastRawRecordCount\": "
       << shadowAgg.semanticSceneDirectLastRawRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastEligibleRecordCount\": "
       << shadowAgg.semanticSceneDirectLastEligibleRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedRecordCount\": "
       << shadowAgg.semanticSceneDirectLastSubmittedRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastUniqueObjectCount\": "
       << shadowAgg.semanticSceneDirectLastUniqueObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedObjectCount\": "
       << shadowAgg.semanticSceneDirectLastSubmittedObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastRecordCapPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectLastRecordCapPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastScanCapPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectLastScanCapPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastMinGeosetsPerObject\": "
       << shadowAgg.semanticSceneDirectLastMinGeosetsPerObject << ",\n";
  json << "    \"semanticSceneDirectLastMaxGeosetsPerObject\": "
       << shadowAgg.semanticSceneDirectLastMaxGeosetsPerObject << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedIdentityHash\": "
       << shadowAgg.semanticSceneDirectLastSubmittedIdentityHash << ",\n";
  json << "    \"semanticSceneDirectIdentityChurnCount\": "
       << shadowAgg.semanticSceneDirectIdentityChurnCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapHitCount\": "
       << shadowAgg.semanticSceneDirectRecordCapHitCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapTruncatedRecordCount\": "
       << shadowAgg.semanticSceneDirectRecordCapTruncatedRecordCount << ",\n";
  json << "    \"semanticSceneDirectScanCapHitCount\": "
       << shadowAgg.semanticSceneDirectScanCapHitCount << ",\n";
  json << "    \"semanticSceneDirectObjectGroupedSubmitCount\": "
       << shadowAgg.semanticSceneDirectObjectGroupedSubmitCount << ",\n";
  json << "    \"semanticSceneDirectObjectGroupedSkipCount\": "
       << shadowAgg.semanticSceneDirectObjectGroupedSkipCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapSkipObjectCount\": "
       << shadowAgg.semanticSceneDirectRecordCapSkipObjectCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapAppendFailCount\": "
       << shadowAgg.semanticSceneDirectRecordCapAppendFailCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeaseActiveKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeaseActiveKeyCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeasePrunedKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeasePrunedKeyCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeaseSubmittedKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeaseSubmittedKeyCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillBudgetRecordCount\": "
       << shadowAgg.semanticSceneDirectStickyFillBudgetRecordCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillAppendedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillAppendedCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillSubmittedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillSubmittedCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillMissedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillMissedCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRestoredCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRestoredCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseUpdatedCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseUpdatedCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseExpiredCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseExpiredCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedDynamicMeshCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedDynamicMeshCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedNotSelfContainedCount\": "
       << shadowAgg
              .semanticSceneDirectPartLeaseRejectedNotSelfContainedCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedUnsafeBackingCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedSelfRenewCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedSelfRenewCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseBudgetLimitCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseBudgetLimitCount << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRestoredCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseRestoredCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseExpiredCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseExpiredCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseBudgetLimitCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseBudgetLimitCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreCompleteCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCoreCompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreIncompleteSkipCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCoreIncompleteSkipCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartOmittedIncompleteCoreCount\": "
       << shadowAgg.semanticSceneShadowManifestPartOmittedIncompleteCoreCount
       << ",\n";
  // Phase 7.25 core epoch planner 专属计数器。
  json << "    \"semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochMissingPartCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochMissingPartCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount
       << ",\n";
  // Phase 7.28：skinned palette content stability probe。
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceNoneCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteSourceNoneCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStablePartSampleCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStablePartSampleCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteHashChurnCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteHashChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteCountChurnCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteCountChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteAttributionSnapshotHitCount\": "
       << shadowAgg
              .semanticSceneDirectPaletteAttributionSnapshotHitCount
       << ",\n";
  json << "    \"semanticSceneDirectManifestObjectCount\": "
       << shadowAgg.semanticSceneDirectManifestObjectCount << ",\n";
  json << "    \"semanticSceneDirectManifestObservedPartCount\": "
       << shadowAgg.semanticSceneDirectManifestObservedPartCount << ",\n";
  json << "    \"semanticSceneDirectManifestShadowEligiblePartCount\": "
       << shadowAgg.semanticSceneDirectManifestShadowEligiblePartCount << ",\n";
  json << "    \"semanticSceneDirectObjectCompleteEligibleCount\": "
       << shadowAgg.semanticSceneDirectObjectCompleteEligibleCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByScanCapCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByScanCapCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByAlphaPolicyCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByAlphaPolicyCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteBySliceUnresolvedCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByPacketBuildFailCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByPacketBuildFailCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByAppendFailCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByAppendFailCount << ",\n";
  json << "    \"semanticSceneDirectSubmittedCompleteObjectCount\": "
       << shadowAgg.semanticSceneDirectSubmittedCompleteObjectCount << ",\n";
  json << "    \"semanticSceneDirectSubmittedPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectSubmittedPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceAuthoritativeCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceAuthoritativeCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceFallbackLayerIndexCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceFallbackLayerIndexCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceMissingCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceMissingCount << ",\n";
  json << "    \"semanticScenePreparedProbeAttemptCount\": "
       << shadowAgg.semanticScenePreparedProbeAttemptCount << ",\n";
  json << "    \"semanticScenePreparedProbeContextReadyCount\": "
       << shadowAgg.semanticScenePreparedProbeContextReadyCount << ",\n";
  json << "    \"semanticScenePreparedProbeBackingReadableCount\": "
       << shadowAgg.semanticScenePreparedProbeBackingReadableCount << ",\n";
  json << "    \"semanticScenePreparedSliceRecordedCount\": "
       << shadowAgg.semanticScenePreparedSliceRecordedCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryAttemptCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryAttemptCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryHitCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryHitCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryMissCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryMissCount << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestPartCount\": "
       << shadowAgg.semanticSceneShadowManifestPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestStableObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestStableObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestNewObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestNewObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestExpiredObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestExpiredObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestFreshPartCount\": "
       << shadowAgg.semanticSceneShadowManifestFreshPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestLeaseablePartCount\": "
       << shadowAgg.semanticSceneShadowManifestLeaseablePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestPoseStalePartCount\": "
       << shadowAgg.semanticSceneShadowManifestPoseStalePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestSliceStalePartCount\": "
       << shadowAgg.semanticSceneShadowManifestSliceStalePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestExpiredPartCount\": "
       << shadowAgg.semanticSceneShadowManifestExpiredPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestMultiSlicePartCount\": "
       << shadowAgg.semanticSceneShadowManifestMultiSlicePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestPayload11CChurnCount\": "
       << shadowAgg.semanticSceneShadowManifestPayload11CChurnCount << ",\n";
  json << "    \"semanticSceneShadowManifestRenderablePartChurnCount\": "
       << shadowAgg.semanticSceneShadowManifestRenderablePartChurnCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseHitCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseHitCount << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseMissCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseMissCount << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseNoRuntimeCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseNoRuntimeCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastMatrixCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastMatrixCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastMatrixHash\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastMatrixHash
       << ",\n";
  json << "    \"semanticSceneSubmittedObjectJaccardMilli\": "
       << shadowAgg.semanticSceneSubmittedObjectJaccardMilli << ",\n";
  json << "    \"semanticSceneSubmittedPartJaccardMilli\": "
       << shadowAgg.semanticSceneSubmittedPartJaccardMilli << ",\n";
  json << "    \"semanticSceneVisibleLookupPartLayerHitCount\": "
       << shadowAgg.semanticSceneVisibleLookupPartLayerHitCount << ",\n";
  json << "    \"semanticSceneVisibleLookupSingleFallbackCount\": "
       << shadowAgg.semanticSceneVisibleLookupSingleFallbackCount << ",\n";
  json << "    \"semanticSceneVisibleLookupMissCount\": "
       << shadowAgg.semanticSceneVisibleLookupMissCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingNotCheckedCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingNotCheckedCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingPassCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingPassCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNoRenderablePartCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailNoRenderablePartCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailLookupMissCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailLookupMissCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNonMainQueueCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailNonMainQueueCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNonWorldGroupCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailIdentityMismatchCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailIdentityMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteHashChurnCount\": "
       << shadowAgg.semanticSceneDirectPaletteHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectGroupHashChurnCount\": "
       << shadowAgg.semanticSceneDirectGroupHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectStableGroupHashChurnCount\": "
       << shadowAgg.semanticSceneDirectStableGroupHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectStream1PtrChurnCount\": "
       << shadowAgg.semanticSceneDirectStream1PtrChurnCount << ",\n";
  json << "    \"semanticSceneDirectGeometrySourceHashChurnCount\": "
       << shadowAgg.semanticSceneDirectGeometrySourceHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectSameCasterComparisonCount\": "
       << shadowAgg.semanticSceneDirectSameCasterComparisonCount << ",\n";
  json << "    \"semanticSceneDirectIdentitySkippedChurnCount\": "
       << shadowAgg.semanticSceneDirectIdentitySkippedChurnCount << ",\n";
  json << "    \"semanticSceneDirectPaletteRootDeltaSampleCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootDeltaSampleCount << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedTinyDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedSmallDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedMediumDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedLargeDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootMaxDeltaMilli\": "
       << shadowAgg.semanticSceneDirectPaletteRootMaxDeltaMilli << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyUnitPtrCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyUnitPtrCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyJHandleCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyJHandleCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyRuntimeModelCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyRuntimeModelCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyWorldObjectCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyWorldObjectCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeySceneNodeCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeySceneNodeCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyModelMeshCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyModelMeshCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyRenderablePartCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyRenderablePartCount
       << ",\n";
  json << "    \"semanticSceneLastAppendedGeometrySourceHash\": "
       << shadowAgg.semanticSceneLastAppendedGeometrySourceHash << ",\n";
  json << "    \"semanticSceneLastAppendedGeometryId\": "
       << shadowAgg.semanticSceneLastAppendedGeometryId << ",\n";
  json << "    \"semanticSceneShadowCastersCount\": "
       << shadowAgg.semanticSceneShadowCastersCount << ",\n";
  json << "    \"semanticSceneReplayDrawsCount\": "
       << shadowAgg.semanticSceneReplayDrawsCount << ",\n";
  json << "    \"semanticSceneShadowMapDrawnCasters\": "
       << shadowAgg.semanticSceneShadowMapDrawnCasters << ",\n";
  json << "    \"semanticSceneShadowMapCascadeCulledCount\": "
       << shadowAgg.semanticSceneShadowMapCascadeCulledCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedCasterCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedCasterCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedPreparedCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedPreparedCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedInvalidBufferCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedInvalidBufferCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedInvalidPipelineCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedInvalidPipelineCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedDrawnCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedDrawnCount << ",\n";
  json << "    \"semanticSceneShadowTaaActive\": "
       << shadowAgg.semanticSceneShadowTaaActive << ",\n";
  json << "    \"semanticSceneReceiverReuseShadowMap\": "
       << shadowAgg.semanticSceneReceiverReuseShadowMap << ",\n";
  json << "    \"semanticSceneReceiverInputValid\": "
       << shadowAgg.semanticSceneReceiverInputValid << ",\n";
  json << "    \"semanticSceneReceiverInputRejectReason\": "
       << shadowAgg.semanticSceneReceiverInputRejectReason << ",\n";
  json << "    \"semanticSceneReceiverNeedPass\": "
       << shadowAgg.semanticSceneReceiverNeedPass << ",\n";
  json << "    \"semanticSceneReceiverNeedShadowMap\": "
       << shadowAgg.semanticSceneReceiverNeedShadowMap << ",\n";
  json << "    \"semanticSceneReceiverHasCompleteShadowMap\": "
       << shadowAgg.semanticSceneReceiverHasCompleteShadowMap << ",\n";
  json << "    \"semanticSceneReceiverHasUsableDirectionalShadow\": "
       << shadowAgg.semanticSceneReceiverHasUsableDirectionalShadow << ",\n";
  json << "    \"semanticSceneReceiverActiveStrengthMilli\": "
       << shadowAgg.semanticSceneReceiverActiveStrengthMilli << ",\n";
  json << "    \"semanticSceneReceiverUboStrengthMilli\": "
       << shadowAgg.semanticSceneReceiverUboStrengthMilli << ",\n";
  json << "    \"semanticSceneReceiverDebugMode\": "
       << shadowAgg.semanticSceneReceiverDebugMode << ",\n";
  json << "    \"semanticSceneReceiverCsmCascadeCount\": "
       << shadowAgg.semanticSceneReceiverCsmCascadeCount << ",\n";
  json << "    \"semanticSceneReceiverZeroStrengthFrameCount\": "
       << shadowAgg.semanticSceneReceiverZeroStrengthFrameCount << ",\n";
  json << "    \"semanticSceneReceiverDrawnWithZeroStrengthCount\": "
       << shadowAgg.semanticSceneReceiverDrawnWithZeroStrengthCount << ",\n";
  json << "    \"semanticSceneReceiverNoCompleteShadowMapCount\": "
       << shadowAgg.semanticSceneReceiverNoCompleteShadowMapCount << ",\n";
  json << "    \"semanticSceneReceiverNoShadowMapImageCount\": "
       << shadowAgg.semanticSceneReceiverNoShadowMapImageCount << ",\n";
  json << "    \"semanticSceneReceiverNoShadowMapSampleViewCount\": "
       << shadowAgg.semanticSceneReceiverNoShadowMapSampleViewCount << ",\n";
  json << "    \"semanticSceneReceiverNoCandidateCsmCount\": "
       << shadowAgg.semanticSceneReceiverNoCandidateCsmCount << ",\n";
  json << "    \"semanticSceneReceiverCsmFallbackToLastGoodCount\": "
       << shadowAgg.semanticSceneReceiverCsmFallbackToLastGoodCount << ",\n";
  json << "    \"semanticSceneReceiverHoldInvalidCsmCount\": "
       << shadowAgg.semanticSceneReceiverHoldInvalidCsmCount << ",\n";
  json << "    \"semanticSceneReceiverHoldEmptyReplayCount\": "
       << shadowAgg.semanticSceneReceiverHoldEmptyReplayCount << ",\n";
  json << "    \"semanticSceneReceiverHoldIdentityChurnCount\": "
       << shadowAgg.semanticSceneReceiverHoldIdentityChurnCount << ",\n";
  json << "    \"semanticSceneReceiverReuseInvalidatedAfterEnsureCount\": "
       << shadowAgg.semanticSceneReceiverReuseInvalidatedAfterEnsureCount
       << ",\n";
  json << "    \"semanticSceneShadowMapRenderSkippedNoResourcesCount\": "
       << shadowAgg.semanticSceneShadowMapRenderSkippedNoResourcesCount
       << ",\n";
  json << "    \"semanticSceneShadowMapRenderSkippedNoMatrixBufferCount\": "
       << shadowAgg.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount
       << ",\n";
  json << "    \"semanticSceneReceiverViewportX\": "
       << shadowAgg.semanticSceneReceiverViewportX << ",\n";
  json << "    \"semanticSceneReceiverViewportY\": "
       << shadowAgg.semanticSceneReceiverViewportY << ",\n";
  json << "    \"semanticSceneReceiverViewportWidth\": "
       << shadowAgg.semanticSceneReceiverViewportWidth << ",\n";
  json << "    \"semanticSceneReceiverViewportHeight\": "
       << shadowAgg.semanticSceneReceiverViewportHeight << ",\n";
  json << "    \"semanticSceneSkippedUnitsOnlyFilter\": "
       << shadowAgg.semanticSceneSkippedUnitsOnlyFilter << ",\n";
  json << "    \"semanticSceneAcceptedExplicitResourceOwnerRigid\": "
       << shadowAgg.semanticSceneAcceptedExplicitResourceOwnerRigid << ",\n";
  json << "    \"semanticSceneRejectedNoVertex\": "
       << shadowAgg.semanticSceneRejectedNoVertex << ",\n";
  json << "    \"semanticSceneRejectedSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedGeometry\": "
       << shadowAgg.semanticSceneRejectedGeometry << ",\n";
  json << "    \"semanticSceneRejectedGeometryFrameLocal\": "
       << shadowAgg.semanticSceneRejectedGeometryFrameLocal << ",\n";
  json << "    \"semanticSceneRejectedGeometryPersistent\": "
       << shadowAgg.semanticSceneRejectedGeometryPersistent << ",\n";
  json << "    \"semanticFallbackPruned\": "
       << shadowAgg.semanticFallbackPruned << ",\n";
  json << "    \"semanticFallbackPrunedByHandle\": "
       << shadowAgg.semanticFallbackPrunedByHandle << ",\n";
  json << "    \"semanticFallbackPrunedByWorldObjectEntry\": "
       << shadowAgg.semanticFallbackPrunedByWorldObjectEntry << ",\n";
  json << "    \"semanticFallbackPrunedBySceneNode\": "
       << shadowAgg.semanticFallbackPrunedBySceneNode << ",\n";
  json << "    \"semanticFallbackPrunedByRuntimeModel\": "
       << shadowAgg.semanticFallbackPrunedByRuntimeModel << ",\n";
  json << "    \"modelRegistryHit\": " << runtimeSummary.modelRegistryCount
       << ",\n";
  json << "    \"modelRegistryMiss\": 0,\n";
  json << "    \"modelLoadCount\": " << runtimeSummary.modelRegistryCount
       << ",\n";
  json << "    \"modelReuseCount\": " << runtimeSummary.instanceRegistryCount
       << ",\n";
  json << "    \"runtimeBoundCount\": " << runtimeSummary.runtimeBoundCount
       << ",\n";
  json << "    \"completeIdentityCount\": "
       << runtimeSummary.completeIdentityCount
       << ",\n";
  json << "    \"shadowRuntimeBoundCount\": "
       << runtimeSummary.shadowRuntimeBoundCount
       << ",\n";
  json << "    \"shadowIdentityCount\": " << runtimeSummary.shadowIdentityCount
       << ",\n";
  json << "    \"poseUpdateCount\": " << runtimeSummary.poseReadyCount
       << ",\n";
  json << "    \"poseCacheHit\": 0,\n";
  json << "    \"poseCacheMiss\": 0,\n";
  json << "    \"bonePaletteUpdates\": " << runtimeSummary.matrixPaletteCount
       << ",\n";
  json << "    \"shadowGeosetResourceCount\": "
       << runtimeSummary.shadowGeosetResourceCount << ",\n";
  json << "    \"shadowReadyGeosetCount\": "
       << runtimeSummary.shadowReadyGeosetCount << ",\n";
  json << "    \"shadowModelResourceCount\": "
       << runtimeSummary.shadowModelResourceCount << ",\n";
  json << "    \"shadowRuntimeModelCount\": "
       << runtimeSummary.shadowRuntimeModelCount << ",\n";
  json << "    \"visibleRenderableCount\": "
       << runtimeSummary.visibleRenderableCount << ",\n";
  json << "    \"visibleRenderableMainCount\": "
       << runtimeSummary.visibleRenderableMainCount << ",\n";
  json << "    \"visibleRenderableTransparentCount\": "
       << runtimeSummary.visibleRenderableTransparentCount << ",\n";
  json << "    \"semanticVisibleDirectUnitCandidateAccepted\": "
       << runtimeSummary.semanticVisibleDirectUnitCandidateAccepted << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNotUnitLike\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNotUnitLike << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedGroup\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedGroup << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNoUnitPtr\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNoUnitPtr << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNoIdentity\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNoIdentity << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNoMesh\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNoMesh << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedBuilding\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedBuilding << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNoGeoset\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNoGeoset << ",\n";
  json << "    \"semanticStaticCandidateCount\": "
       << runtimeSummary.semanticStaticCandidateCount << ",\n";
  json << "    \"semanticStaticCandidateBuildingCount\": "
       << runtimeSummary.semanticStaticCandidateBuildingCount << ",\n";
  json << "    \"semanticStaticCandidateDestructibleCount\": "
       << runtimeSummary.semanticStaticCandidateDestructibleCount << ",\n";
  json << "    \"semanticStaticCandidateMaybeDoodadOrEffectCount\": "
       << runtimeSummary.semanticStaticCandidateMaybeDoodadOrEffectCount
       << ",\n";
  json << "    \"semanticStaticCandidateWithStableIdentity\": "
       << runtimeSummary.semanticStaticCandidateWithStableIdentity << ",\n";
  json << "    \"semanticStaticCandidateWithMeshData\": "
       << runtimeSummary.semanticStaticCandidateWithMeshData << ",\n";
  json << "    \"semanticStaticCandidateWithRuntimeModel\": "
       << runtimeSummary.semanticStaticCandidateWithRuntimeModel << ",\n";
  json << "    \"semanticStaticCandidateWithModelResource\": "
       << runtimeSummary.semanticStaticCandidateWithModelResource << ",\n";
  json << "    \"semanticStaticCandidateWithResolvedGeoset\": "
       << runtimeSummary.semanticStaticCandidateWithResolvedGeoset << ",\n";
  json << "    \"semanticStaticCandidateRejectedUnitsOnlyFilter\": "
       << runtimeSummary.semanticStaticCandidateRejectedUnitsOnlyFilter
       << ",\n";
  json << "    \"semanticStaticCandidateRejectedNoIdentity\": "
       << runtimeSummary.semanticStaticCandidateRejectedNoIdentity << ",\n";
  json << "    \"semanticStaticCandidateRejectedNoMeshData\": "
       << runtimeSummary.semanticStaticCandidateRejectedNoMeshData << ",\n";
  json << "    \"semanticStaticCandidateRejectedNoResource\": "
       << runtimeSummary.semanticStaticCandidateRejectedNoResource << ",\n";
  json << "    \"semanticStaticCandidateRejectedNoGeoset\": "
       << runtimeSummary.semanticStaticCandidateRejectedNoGeoset << ",\n";
  json << "    \"semanticStaticCandidateRejectedNonCanonicalKind\": "
       << runtimeSummary.semanticStaticCandidateRejectedNonCanonicalKind
       << ",\n";
  json << "    \"shadowPoseReadyCount\": "
       << runtimeSummary.shadowPoseReadyCount << ",\n";
  json << "    \"upperLayerResolveAttempts\": "
       << runtimeSummary.upperLayerResolveAttempts << ",\n";
  json << "    \"upperLayerResolveVisibleMiss\": "
       << runtimeSummary.upperLayerResolveVisibleMiss << ",\n";
  json << "    \"upperLayerResolveVisibleUnresolvedGeoset\": "
       << runtimeSummary.upperLayerResolveVisibleUnresolvedGeoset << ",\n";
  json << "    \"upperLayerResolveGeosetMiss\": "
       << runtimeSummary.upperLayerResolveGeosetMiss << ",\n";
  json << "    \"upperLayerResolvePoseMiss\": "
       << runtimeSummary.upperLayerResolvePoseMiss << ",\n";
  json << "    \"upperLayerResolveRuntimeGroupPaletteMiss\": "
       << runtimeSummary.upperLayerResolveRuntimeGroupPaletteMiss << ",\n";
  json << "    \"upperLayerResolveAuthoritativeRigid\": "
       << runtimeSummary.upperLayerResolveAuthoritativeRigid << ",\n";
  json << "    \"upperLayerResolveAuthoritativeSkinned\": "
       << runtimeSummary.upperLayerResolveAuthoritativeSkinned << ",\n";
  json << "    \"upperLayerResolvedAuthoritativeItems\": "
       << runtimeSummary.upperLayerResolvedAuthoritativeItems << ",\n";
  json << "    \"upperLayerEmitted\": "
       << runtimeSummary.upperLayerEmitted << ",\n";
  json << "    \"upperLayerDuplicateOrSuppressed\": "
       << runtimeSummary.upperLayerDuplicateOrSuppressed << ",\n";
  json << "    \"semanticCoreManifestFrameSerial\": "
       << runtimeSummary.semanticCoreManifestFrameSerial << ",\n";
  json << "    \"semanticCoreFrameSerial\": "
       << runtimeSummary.semanticCoreFrameSerial << ",\n";
  json << "    \"semanticCoreFrameLag\": "
       << runtimeSummary.semanticCoreFrameLag << ",\n";
  json << "    \"semanticCoreFrameFresh\": "
       << (runtimeSummary.semanticCoreFrameFresh ? "true" : "false") << ",\n";
  json << "    \"semanticCoreConsidered\": "
       << runtimeSummary.semanticCoreConsidered << ",\n";
  json << "    \"semanticCoreResolved\": "
       << runtimeSummary.semanticCoreResolved << ",\n";
  json << "    \"semanticCoreRigidResolved\": "
       << runtimeSummary.semanticCoreRigidResolved << ",\n";
  json << "    \"semanticCoreExplicitResourceOwnerRigidResolved\": "
       << runtimeSummary.semanticCoreExplicitResourceOwnerRigidResolved
       << ",\n";
  json << "    \"semanticCoreExplicitResourceOwnerRigidWorldTransformResolved\": "
       << runtimeSummary.semanticCoreExplicitResourceOwnerRigidWorldTransformResolved
       << ",\n";
  json << "    \"semanticCoreExplicitResourceOwnerRigidNoMatrixPalette\": "
       << runtimeSummary.semanticCoreExplicitResourceOwnerRigidNoMatrixPalette
       << ",\n";
  json << "    \"semanticCoreSkinnedResolved\": "
       << runtimeSummary.semanticCoreSkinnedResolved << ",\n";
  json << "    \"semanticCoreExplicitBlendAttempts\": "
       << runtimeSummary.semanticCoreExplicitBlendAttempts << ",\n";
  json << "    \"semanticCoreExplicitBlendAttemptWithSpanRemapTable\": "
       << runtimeSummary.semanticCoreExplicitBlendAttemptWithSpanRemapTable
       << ",\n";
  json << "    \"semanticCoreExplicitBlendResolved\": "
       << runtimeSummary.semanticCoreExplicitBlendResolved << ",\n";
  json << "    \"semanticCoreExplicitBlendSpanRemapResolved\": "
       << runtimeSummary.semanticCoreExplicitBlendSpanRemapResolved << ",\n";
  json << "    \"semanticCoreExplicitBlendStrideSearchMiss\": "
       << runtimeSummary.semanticCoreExplicitBlendStrideSearchMiss << ",\n";
  json << "    \"semanticCoreExplicitBlendFinalDecodeMiss\": "
       << runtimeSummary.semanticCoreExplicitBlendFinalDecodeMiss << ",\n";
  json << "    \"semanticCoreCoreDrawPacketCount\": "
       << runtimeSummary.semanticCoreCoreDrawPacketCount << ",\n";
  json << "    \"semanticCoreUpperLayerResolvedItems\": "
       << runtimeSummary.semanticCoreUpperLayerResolvedItems << ",\n";
  json << "    \"semanticCoreSupplementalUpperLayerDrawPacketCount\": "
       << runtimeSummary.semanticCoreSupplementalUpperLayerDrawPacketCount
       << ",\n";
  json << "    \"semanticCoreDrawPacketCount\": "
       << runtimeSummary.semanticCoreDrawPacketCount << ",\n";
  json << "    \"semanticCoreSubmittedDrawCount\": "
       << runtimeSummary.semanticCoreSubmittedDrawCount << ",\n";
  json << "    \"semanticCoreSkippedNoIdentity\": "
       << runtimeSummary.semanticCoreSkippedNoIdentity << ",\n";
  json << "    \"semanticCoreSkippedNoResolvedGeoset\": "
       << runtimeSummary.semanticCoreSkippedNoResolvedGeoset << ",\n";
  json << "    \"semanticCoreSkippedNoGeoset\": "
       << runtimeSummary.semanticCoreSkippedNoGeoset << ",\n";
  json << "    \"semanticCoreSkippedResourceMiss\": "
       << runtimeSummary.semanticCoreSkippedResourceMiss << ",\n";
  json << "    \"semanticCoreSkippedResourceNotReady\": "
       << runtimeSummary.semanticCoreSkippedResourceNotReady << ",\n";
  json << "    \"semanticCoreSkippedNoPose\": "
       << runtimeSummary.semanticCoreSkippedNoPose << ",\n";
  json << "    \"semanticCoreSkippedNoPoseNoContext\": "
       << runtimeSummary.semanticCoreSkippedNoPoseNoContext << ",\n";
  json << "    \"semanticCoreSkippedNoPoseAnonymousSubpart\": "
       << runtimeSummary.semanticCoreSkippedNoPoseAnonymousSubpart << ",\n";
  json << "    \"semanticCoreSkippedNoPoseLookupMiss\": "
       << runtimeSummary.semanticCoreSkippedNoPoseLookupMiss << ",\n";
  json << "    \"semanticCoreSkippedNoRuntimeGroupPalette\": "
       << runtimeSummary.semanticCoreSkippedNoRuntimeGroupPalette << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByMeshPoseContext\": "
       << runtimeSummary.semanticCoreRuntimeGroupPaletteRescueByMeshPoseContext
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByResourceMatchedPose\": "
       << runtimeSummary
              .semanticCoreRuntimeGroupPaletteRescueByResourceMatchedPose
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByRuntimeRoot\": "
       << runtimeSummary.semanticCoreRuntimeGroupPaletteRescueByRuntimeRoot
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByChildRuntime\": "
       << runtimeSummary.semanticCoreRuntimeGroupPaletteRescueByChildRuntime
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByDescendantRuntime\": "
       << runtimeSummary
              .semanticCoreRuntimeGroupPaletteRescueByDescendantRuntime
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteResourceMatchedPoseSuppressed\": "
       << runtimeSummary
              .semanticCoreRuntimeGroupPaletteResourceMatchedPoseSuppressed
       << ",\n";
  json << "    \"animationSequenceCount\": 0,\n";
  json << "    \"avgModelResolveCpuMs\": 0.0,\n";
  json << "    \"avgPoseUpdateCpuMs\": 0.0,\n";
  json << "    \"avgSkinnedOutputCpuMs\": 0.0,\n";
  json << "    \"modelResourceBytes\": 0,\n";
  json << "    \"poseResourceBytes\": 0,\n";
  json << "    \"spriteFramePoseCount\": "
       << runtimeSummary.spriteFramePoseCount << ",\n";
  json << "    \"frameSerial\": " << shadowAgg.framesObserved << ",\n";
  json << "    \"notes\": \"runtime-shadow-bridge-v1\"\n";
  json << "  },\n";

  // 帧时间线（CPU/GPU）
  json << "  \"frameTimes\": [";
  bool first = true;
  for (const auto *f : frames) {
    if (!first)
      json << ",";
    json << f->totalCpuMs;
    first = false;
  }
  json << "],\n";
  json << "  \"frameTimesGpu\": [";
  first = true;
  for (const auto *f : frames) {
    if (!first)
      json << ",";
    json << f->totalGpuMs;
    first = false;
  }
  json << "],\n";

  // 聚合分段数据（注意：分层 scope 为“包含式”统计，父节点包含子节点耗时）
  struct SectionAggregate {
    std::string name;
    std::string path;
    std::string parentPath;
    double totalCpuMs = 0.0;
    double totalGpuMs = 0.0;
    uint64_t calls = 0;
  };
  std::unordered_map<std::string, SectionAggregate> sectionTotals;
  for (const auto *f : frames) {
    for (const auto &s : f->sections) {
      if (s.id >= snapshot.sections.size())
        continue;
      const auto &meta = snapshot.sections[s.id];
      std::string key = meta.path.empty() ? meta.name : meta.path;
      auto &agg = sectionTotals[key];
      if (agg.path.empty()) {
        agg.name = meta.name;
        agg.path = key;
        agg.parentPath = meta.parentPath;
      }
      agg.totalCpuMs += s.cpuMs;
      agg.totalGpuMs += s.gpuMs;
      agg.calls += s.callCount;
    }
  }

  auto findSection = [&](const std::string &path) -> const SectionAggregate * {
    auto it = sectionTotals.find(path);
    if (it == sectionTotals.end())
      return nullptr;
    return &it->second;
  };

  const std::string cycleRootPathWaitGate =
      "War3MainLoop/Engine/WaitGate/Cycle";
  const std::string cycleRootPathPump = "War3MainLoop/Pump/Cycle";
  const SectionAggregate *cycleRoot = findSection(cycleRootPathWaitGate);
  std::string cycleRootPath = cycleRootPathWaitGate;
  if (!cycleRoot) {
    cycleRoot = findSection(cycleRootPathPump);
    cycleRootPath = cycleRootPathPump;
  }
  const std::string cycleActivePath = cycleRootPath + "/Active";
  const std::string cycleIdlePath = cycleRootPath + "/Idle";
  const SectionAggregate *cycleActive = findSection(cycleActivePath);
  const SectionAggregate *cycleIdle = findSection(cycleIdlePath);
  const double cycleTotalCpu = cycleRoot ? cycleRoot->totalCpuMs : 0.0;
  const double cycleActiveCpu = cycleActive ? cycleActive->totalCpuMs : 0.0;
  const double cycleIdleCpu = cycleIdle ? cycleIdle->totalCpuMs : 0.0;
  const double cycleAvgMs =
      frames.empty() ? 0.0
                     : (cycleTotalCpu / static_cast<double>(frames.size()));
  const double cycleActiveAvgMs =
      frames.empty() ? 0.0
                     : (cycleActiveCpu / static_cast<double>(frames.size()));
  const double cycleIdleAvgMs =
      frames.empty() ? 0.0
                     : (cycleIdleCpu / static_cast<double>(frames.size()));

  struct CyclePhaseRow {
    std::string name;
    std::string path;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double sharePct = 0.0;
    double callsPerFrame = 0.0;
  };
  std::vector<CyclePhaseRow> cyclePhases;
  if (cycleRoot) {
    cyclePhases.reserve(16);
    for (const auto &kv : sectionTotals) {
      const auto &agg = kv.second;
      if (agg.parentPath != cycleRootPath)
        continue;
      CyclePhaseRow row;
      row.name = agg.name;
      row.path = agg.path;
      row.totalCpuMs = agg.totalCpuMs;
      row.avgCpuMs =
          frames.empty()
              ? 0.0
              : (agg.totalCpuMs / static_cast<double>(frames.size()));
      row.sharePct =
          (cycleTotalCpu > 1e-6)
              ? std::clamp(agg.totalCpuMs / cycleTotalCpu * 100.0, 0.0, 100.0)
              : 0.0;
      row.callsPerFrame = frames.empty() ? 0.0
                                         : (static_cast<double>(agg.calls) /
                                            static_cast<double>(frames.size()));
      cyclePhases.push_back(std::move(row));
    }
    std::sort(cyclePhases.begin(), cyclePhases.end(),
              [](const CyclePhaseRow &a, const CyclePhaseRow &b) {
                return a.avgCpuMs > b.avgCpuMs;
              });
  }

  // 主循环阶段聚合（对路径后缀做规范化，避免嵌套路径导致同名阶段分裂）。
  struct MainLoopStagePattern {
    const char *suffix;
    const char *stageName;
  };
  static const MainLoopStagePattern kStagePatterns[] = {
      {"War3MainLoop/Dispatch/Case0", "Dispatch/Case0"},
      {"War3MainLoop/Dispatch/Case1", "Dispatch/Case1"},
      {"War3MainLoop/Dispatch/Case2", "Dispatch/Case2"},
      {"War3MainLoop/Dispatch/Case3", "Dispatch/Case3"},
      {"War3MainLoop/Dispatch/Case4", "Dispatch/Case4"},
      {"War3MainLoop/Dispatch/Case5", "Dispatch/Case5"},
      {"War3MainLoop/Dispatch/Case6", "Dispatch/Case6"},
      {"War3MainLoop/Dispatch/Case7", "Dispatch/Case7"},
      {"War3MainLoop/Dispatch/Case8", "Dispatch/Case8"},
      {"War3MainLoop/Dispatch/Case9", "Dispatch/Case9"},
      {"War3MainLoop/Dispatch/Case10", "Dispatch/Case10"},
      {"War3MainLoop/Dispatch/Case11", "Dispatch/Case11"},
      {"War3MainLoop/Dispatch/Case12", "Dispatch/Case12"},
      {"War3MainLoop/Dispatch/Case13", "Dispatch/Case13"},
      {"War3MainLoop/Dispatch/Case14", "Dispatch/Case14"},
      {"War3MainLoop/DispatchModule/StateFinalize",
       "DispatchModule/StateFinalize"},
      {"War3MainLoop/DispatchModule/LoadBlockType1_Single",
       "DispatchModule/LoadBlockType1_Single"},
      {"War3MainLoop/DispatchModule/LoadBlockType1_Batch",
       "DispatchModule/LoadBlockType1_Batch"},
      {"War3MainLoop/DispatchModule/LoadBlockType27",
       "DispatchModule/LoadBlockType27"},
      {"War3MainLoop/DispatchModule/LoadBlockType28",
       "DispatchModule/LoadBlockType28"},
      {"War3MainLoop/DispatchModule/MainCallbackGate",
       "DispatchModule/MainCallbackGate"},
      {"War3MainLoop/DispatchModule/LoadBlockType2_ResetSync",
       "DispatchModule/LoadBlockType2_ResetSync"},
      {"War3MainLoop/DispatchModule/LoadBlockType8_InputSet",
       "DispatchModule/LoadBlockType8_InputSet"},
      {"War3MainLoop/DispatchModule/LoadBlockType9_InputClear",
       "DispatchModule/LoadBlockType9_InputClear"},
      {"War3MainLoop/DispatchModule/LoadBlockType11_InputCompose",
       "DispatchModule/LoadBlockType11_InputCompose"},
      {"War3MainLoop/DispatchModule/LoadBlockType12",
       "DispatchModule/LoadBlockType12"},
      {"War3MainLoop/DispatchModule/LoadBlockType16_Command",
       "DispatchModule/LoadBlockType16_Command"},
      {"War3MainLoop/DispatchModule/LoadBlockType13_TimeState",
       "DispatchModule/LoadBlockType13_TimeState"},
      {"War3MainLoop/DispatchModule/LoadBlockType14_Finalize",
       "DispatchModule/LoadBlockType14_Finalize"},
      {"War3MainLoop/DispatchModule/MainCallbackGateAlt",
       "DispatchModule/MainCallbackGateAlt"},
      {"War3MainLoop/DispatchModule/Other", "DispatchModule/Other"},
      {"War3MainLoop/Dispatch/Game", "Dispatch/Game"},
      {"War3MainLoop/Dispatch/Input", "Dispatch/Input"},
      {"War3MainLoop/Dispatch/System", "Dispatch/System"},
      {"War3MainLoop/Dispatch/Other", "Dispatch/Other"},
      {"War3MainLoop/Engine/WaitGate", "Engine/WaitGate"},
      {"War3MainLoop/Engine/SleepGateInner", "Engine/SleepGateInner"},
      {"War3MainLoop/Engine/SleepGate", "Engine/SleepGate"},
      {"War3MainLoop/Engine/PrepareDispatch", "Engine/PrepareDispatch"},
      {"War3MainLoop/Engine/FinalizeDispatch", "Engine/FinalizeDispatch"},
      {"War3MainLoop/Engine/TickUpdate", "Engine/TickUpdate"},
      {"War3MainLoop/Engine/FinalizeWorker", "Engine/FinalizeWorker"},
      {"War3MainLoop/Engine/ComputeWakeDelta", "Engine/ComputeWakeDelta"},
      {"War3MainLoop/Engine/PrepareWait", "Engine/PrepareWait"},
      {"War3MainLoop/Engine/RunCallbacks", "Engine/RunCallbacks"},
      {"War3MainLoop/Engine/QueueFlush", "Engine/QueueFlush"},
      {"War3MainLoop/Engine/FinalizeTick", "Engine/FinalizeTick"},
      {"War3MainLoop/Engine/Reschedule", "Engine/Reschedule"},
      {"War3MainLoop/Engine/SelectWorker", "Engine/SelectWorker"},
      {"War3MainLoop/Engine/TlsPump", "Engine/TlsPump"},
      {"War3MainLoop/Dispatch", "Dispatch"},
      {"War3MainLoop/Callback", "Callback"},
      {"War3MainLoop/Pump", "Pump"},
  };

  struct MainLoopStageAggregate {
    std::string name;
    std::string suffix;
    double totalCpuMs = 0.0;
    uint64_t calls = 0;
  };
  std::unordered_map<std::string, MainLoopStageAggregate> mainLoopStageTotals;
  for (const auto &kv : sectionTotals) {
    const auto &agg = kv.second;
    for (const auto &pattern : kStagePatterns) {
      if (!endsWith(agg.path, pattern.suffix))
        continue;
      auto &row = mainLoopStageTotals[pattern.stageName];
      if (row.name.empty()) {
        row.name = pattern.stageName;
        row.suffix = pattern.suffix;
      }
      row.totalCpuMs += agg.totalCpuMs;
      row.calls += agg.calls;
      break;
    }
  }

  struct MainLoopStageRow {
    std::string name;
    std::string suffix;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double shareInFramePct = 0.0;
    double callsPerFrame = 0.0;
  };
  std::vector<MainLoopStageRow> mainLoopStages;
  mainLoopStages.reserve(mainLoopStageTotals.size());
  for (const auto &kv : mainLoopStageTotals) {
    const auto &src = kv.second;
    MainLoopStageRow row;
    row.name = src.name;
    row.suffix = src.suffix;
    row.totalCpuMs = src.totalCpuMs;
    row.avgCpuMs = frames.empty()
                       ? 0.0
                       : (src.totalCpuMs / static_cast<double>(frames.size()));
    row.shareInFramePct =
        (totalCpu > 1e-6)
            ? std::clamp(src.totalCpuMs / totalCpu * 100.0, 0.0, 100.0)
            : 0.0;
    row.callsPerFrame = frames.empty() ? 0.0
                                       : (static_cast<double>(src.calls) /
                                          static_cast<double>(frames.size()));
    mainLoopStages.push_back(std::move(row));
  }
  auto parseDispatchCaseNumber = [](const std::string& name) -> int {
    constexpr const char* kPrefix = "Dispatch/Case";
    if (!startsWith(name, kPrefix))
      return -1;
    const char* p = name.c_str() + std::strlen(kPrefix);
    if (*p == '\0')
      return -1;
    char* end = nullptr;
    long v = std::strtol(p, &end, 10);
    if (end == p || (end && *end != '\0'))
      return -1;
    if (v < 0 || v > 999)
      return -1;
    return static_cast<int>(v);
  };
  auto stageOrderKey = [&](const std::string& name) -> int {
    static const char* kOrdered[] = {
        "Engine/SelectWorker",     "Engine/PrepareWait",
        "Engine/WaitGate",         "Engine/SleepGate",
        "Engine/SleepGateInner",   "Engine/PrepareDispatch",
        "Engine/RunCallbacks",     "Pump",
        "Dispatch",                "Dispatch/Game",
        "Dispatch/Input",          "Dispatch/System",
        "Dispatch/Other",          "Engine/FinalizeDispatch",
        "Engine/QueueFlush",       "Engine/TickUpdate",
        "Engine/FinalizeTick",     "Engine/FinalizeWorker",
        "Engine/ComputeWakeDelta", "Engine/Reschedule",
        "Callback",                "Engine/TlsPump",
    };
    for (int i = 0; i < static_cast<int>(sizeof(kOrdered) / sizeof(kOrdered[0]));
         ++i) {
      if (name == kOrdered[i])
        return i;
    }
    if (startsWith(name, "Dispatch/Case")) {
      const int caseId = parseDispatchCaseNumber(name);
      return caseId >= 0 ? (100 + caseId) : 190;
    }
    if (startsWith(name, "DispatchModule/"))
      return 300;
    return 1000;
  };
  std::sort(mainLoopStages.begin(), mainLoopStages.end(),
            [&](const MainLoopStageRow &a, const MainLoopStageRow &b) {
              const int orderA = stageOrderKey(a.name);
              const int orderB = stageOrderKey(b.name);
              if (orderA != orderB)
                return orderA < orderB;
              if (orderA == 300)
                return a.name < b.name;
              return a.avgCpuMs > b.avgCpuMs;
            });

  struct MainLoopBreakdownRow {
    std::string name;
    std::string path;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double shareInParentPct = 0.0;
    double callsPerFrame = 0.0;
    std::string sourceTag;
  };

  auto collectMainLoopBreakdownRows =
      [&](const std::string &parentPath, const std::string &pathPrefix,
          const char *sourceTag) -> std::vector<MainLoopBreakdownRow> {
    std::vector<MainLoopBreakdownRow> rows;
    const SectionAggregate *parent = findSection(parentPath);
    const double parentTotalCpu = parent ? parent->totalCpuMs : 0.0;
    for (const auto &kv : sectionTotals) {
      const auto &agg = kv.second;
      if (!startsWith(agg.path, pathPrefix.c_str()))
        continue;
      if (agg.path == parentPath)
        continue;
      MainLoopBreakdownRow row;
      row.name = agg.name;
      row.path = agg.path;
      row.totalCpuMs = agg.totalCpuMs;
      row.avgCpuMs = frames.empty()
                         ? 0.0
                         : (agg.totalCpuMs / static_cast<double>(frames.size()));
      row.shareInParentPct =
          (parentTotalCpu > 1e-6)
              ? std::clamp(agg.totalCpuMs / parentTotalCpu * 100.0, 0.0, 100.0)
              : 0.0;
      row.callsPerFrame =
          frames.empty() ? 0.0
                         : (static_cast<double>(agg.calls) /
                            static_cast<double>(frames.size()));
      row.sourceTag = sourceTag ? sourceTag : "";
      rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(),
              [](const MainLoopBreakdownRow &a, const MainLoopBreakdownRow &b) {
                return a.avgCpuMs > b.avgCpuMs;
              });
    return rows;
  };

  const auto dispatchCaseRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Dispatch", "War3MainLoop/Dispatch/Case", "HookMeasured");
  const auto dispatchModuleRows =
      collectMainLoopBreakdownRows("War3MainLoop/DispatchModule",
                                   "War3MainLoop/DispatchModule/",
                                   "CaseMapped");
  const auto tickUpdateSubRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Engine/TickUpdate/Sub",
      "War3MainLoop/Engine/TickUpdate/Sub/", "HookMeasured");
  const auto runCallbacksCallerRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Engine/RunCallbacks",
      "War3MainLoop/Engine/RunCallbacks/Caller_", "HookMeasured");
  const auto functionBreakdownRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Function", "War3MainLoop/Function/", "HookMeasured");
  const auto dispatchCaseFunctionRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Function/DispatchCaseFunctions",
      "War3MainLoop/Function/DispatchCaseFunctions/", "CaseMapped");

  // 口径说明：
  // - allSectionSum：所有节点（含父子重叠）的累积，可能大于 totalCpu；
  // - rootSectionSum：仅根节点（parentPath 为空）累积；
  // - rootIdleWaitSum：根节点中被识别为“等待/空转”的时间；
  // - rootActiveSum：根节点中“有效计算”时间（rootSectionSum -
  // rootIdleWaitSum）。
  double allSectionSum = 0.0;
  double rootSectionSum = 0.0;
  double rootIdleWaitSum = 0.0;
  double rootActiveSum = 0.0;
  for (const auto &kv : sectionTotals) {
    allSectionSum += kv.second.totalCpuMs;
    if (kv.second.parentPath.empty()) {
      rootSectionSum += kv.second.totalCpuMs;
      if (isIdleWaitSectionPath(kv.second.path))
        rootIdleWaitSum += kv.second.totalCpuMs;
      else
        rootActiveSum += kv.second.totalCpuMs;
    }
  }

  // 注意：当前埋点为包含式统计，Idle/Active 可能存在重叠。
  // 这里做保守归一化，避免出现“active 被减成 0”这类误判。
  const double idleWaitClampedTotal =
      std::clamp(rootIdleWaitSum, 0.0, totalCpu);
  const double trackedActiveClampedTotal =
      std::clamp(rootActiveSum, 0.0, totalCpu);
  const double activeCpuTotalRaw =
      std::max(0.0, totalCpu - idleWaitClampedTotal);
  const double activeCpuTotal =
      std::max(activeCpuTotalRaw, trackedActiveClampedTotal);
  const double otherTotal = std::max(0.0, totalCpu - rootSectionSum);
  const double otherActiveTotal =
      std::max(0.0, activeCpuTotal - trackedActiveClampedTotal);
  const bool idleActiveOverlapLikely =
      (rootIdleWaitSum + rootActiveSum) > (totalCpu * 1.05);
  const double avgTrackedRootCpuMs =
      frames.empty() ? 0.0
                     : (rootSectionSum / static_cast<double>(frames.size()));
  const double avgTrackedActiveCpuMs =
      frames.empty()
          ? 0.0
          : (trackedActiveClampedTotal / static_cast<double>(frames.size()));
  const double avgIdleWaitCpuMs =
      frames.empty()
          ? 0.0
          : (idleWaitClampedTotal / static_cast<double>(frames.size()));
  const double avgUntrackedCpuMs =
      frames.empty() ? 0.0 : (otherTotal / static_cast<double>(frames.size()));
  const double avgUntrackedActiveCpuMs =
      frames.empty() ? 0.0
                     : (otherActiveTotal / static_cast<double>(frames.size()));
  const double avgActiveFrameMs =
      frames.empty() ? 0.0
                     : (activeCpuTotal / static_cast<double>(frames.size()));
  const double cpuCoveragePct =
      (activeCpuTotal > 1e-6)
          ? std::clamp(trackedActiveClampedTotal / activeCpuTotal * 100.0, 0.0,
                       100.0)
          : 0.0;
  const double cpuCoverageWithIdlePct =
      (totalCpu > 1e-6)
          ? std::clamp(rootSectionSum / totalCpu * 100.0, 0.0, 100.0)
          : 0.0;
  const double cpuInclusivePct =
      (totalCpu > 1e-6) ? (allSectionSum / totalCpu * 100.0) : 0.0;
  const double mainThreadUnattributedMs =
      std::max(0.0, avgMainThreadCpuMs - avgTrackedRootCpuMs);
  const double workerUnattributedMs = std::max(0.0, avgWorkerThreadsCpuMs);
  const double outsideMainLoopMs = std::max(
      0.0, avgProcessCpuMs - (avgTrackedRootCpuMs + mainThreadUnattributedMs +
                              workerUnattributedMs));

  if (otherTotal > 0.0) {
    SectionAggregate &other = sectionTotals["Other/Untracked"];
    other.name = "Other/Untracked (Outside DXVK scopes)";
    other.path = "Other/Untracked";
    other.parentPath.clear();
    other.totalCpuMs += otherTotal;
    other.calls += static_cast<uint64_t>(frames.size());
  }

  if (otherActiveTotal > 0.0) {
    SectionAggregate &otherActive = sectionTotals["Other/UntrackedActive"];
    otherActive.name = "Other/UntrackedActive (MainLoop Active Gap)";
    otherActive.path = "Other/UntrackedActive";
    otherActive.parentPath = "Other/Untracked";
    otherActive.totalCpuMs += otherActiveTotal;
    otherActive.calls += static_cast<uint64_t>(frames.size());
  }

  // 计算 self 时间（exclusive）：self = inclusive - children(inclusive)。
  std::unordered_map<std::string, double> childCpuSums;
  std::unordered_map<std::string, double> childGpuSums;
  for (const auto &kv : sectionTotals) {
    const auto &agg = kv.second;
    if (!agg.parentPath.empty()) {
      childCpuSums[agg.parentPath] += agg.totalCpuMs;
      childGpuSums[agg.parentPath] += agg.totalGpuMs;
    }
  }

  // 严格语义关系树：基于 self CPU 聚合，避免包含式统计导致父子重复计时。
  struct SemanticNodeAggregate {
    std::string name;
    std::string path;
    std::string parentPath;
    double totalCpuMs = 0.0;
    double totalGpuMs = 0.0;
    uint64_t calls = 0;
    bool hasIdleLeaf = false;
    bool hasActiveLeaf = false;
  };

  std::unordered_map<std::string, SemanticNodeAggregate> semanticNodes;
  auto ensureSemanticNode = [&](const std::string& path) -> SemanticNodeAggregate& {
    auto it = semanticNodes.find(path);
    if (it != semanticNodes.end())
      return it->second;

    SemanticNodeAggregate node = {};
    node.path = path;
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
      node.name = path;
      node.parentPath.clear();
    } else {
      node.name = path.substr(slash + 1);
      node.parentPath = path.substr(0, slash);
    }
    auto inserted = semanticNodes.emplace(path, std::move(node));
    return inserted.first->second;
  };

  constexpr const char* kSemanticRootPath = "Semantic";
  constexpr const char* kSemanticMainLoopPath = "Semantic/MainLoop";
  constexpr const char* kSemanticOutsideMainLoopPath =
      "Semantic/OutsideMainLoop";
  auto classifySemanticLeaf = [&](const std::string& path,
                                  bool idleWaitSection) -> std::string {
    // Cycle 指标属于“节拍包络”，与 Render/Logic 统计存在重叠。
    // 为了 strict 语义闭合关系，语义树内不纳入 Cycle 路径。
    if (path.find("/Cycle/") != std::string::npos || endsWith(path, "/Cycle"))
      return {};

    // Untracked 走独立归因表，不纳入 strict 语义树分母。
    if (path == "Other/Untracked" || startsWith(path, "Other/Untracked"))
      return {};

    if (startsWith(path, "JassVM/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/JassVM";

    if (startsWith(path, "War3MainLoop/Dispatch/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";
    if (startsWith(path, "War3MainLoop/DispatchModule/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";
    if (startsWith(path, "War3MainLoop/Function/DispatchCaseFunctions/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";
    if (startsWith(path, "War3MainLoop/Function/sub_6F05A310"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";
    if (startsWith(path, "War3MainLoop/Function/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Engine";
    if (path == "War3MainLoop/Dispatch" ||
        endsWith(path, "War3MainLoop/Dispatch"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";

    if (startsWith(path, "War3MainLoop/Callback") ||
        endsWith(path, "War3MainLoop/Callback"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Callback";
    if (startsWith(path, "War3MainLoop/Pump") ||
        endsWith(path, "War3MainLoop/Pump"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Pump";

    if (idleWaitSection)
      return std::string(kSemanticMainLoopPath) + "/Idle/WaitApi";

    if (startsWith(path, "War3MainLoop/Engine/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Engine";

    if (startsWith(path, "Draw") || startsWith(path, "Present") ||
        startsWith(path, "ShaderPack") || startsWith(path, "PostFX") ||
        startsWith(path, "AA") || startsWith(path, "SSAO")) {
      return std::string(kSemanticMainLoopPath) + "/Render/Pipeline";
    }
    if (startsWith(path, "Hook_Ui"))
      return std::string(kSemanticMainLoopPath) + "/Render/UI";
    if (startsWith(path, "Hook_WorldDispatch") ||
        startsWith(path, "Hook_WorldObjects_RenderGroup") ||
        startsWith(path, "Hook_Dispatch_") || startsWith(path, "SceneCollector")) {
      return std::string(kSemanticMainLoopPath) + "/Render/World";
    }
    if (startsWith(path, "Hook_FlushAndReset") ||
        startsWith(path, "Hook_FlushSortedItems") || startsWith(path, "FQ_")) {
      return std::string(kSemanticMainLoopPath) + "/Render/RenderQueue";
    }
    if (startsWith(path, "Shadow") || startsWith(path, "Outline") ||
        path.find("/Shadow") != std::string::npos ||
        path.find("/Outline") != std::string::npos) {
      return std::string(kSemanticMainLoopPath) + "/Render/Shadow";
    }

    if (startsWith(path, "War3MainLoop/") || startsWith(path, "Hook_") ||
        startsWith(path, "FQ_")) {
      return std::string(kSemanticMainLoopPath) + "/Unknown/OtherTracked";
    }
    return std::string(kSemanticOutsideMainLoopPath) + "/Tracked";
  };

  auto addSemanticLeaf = [&](const std::string& leafPath, double cpuMs, double gpuMs,
                             uint64_t calls, bool idleLeaf) {
    if (leafPath.empty())
      return;
    std::string path = leafPath;
    while (!path.empty()) {
      auto& node = ensureSemanticNode(path);
      node.totalCpuMs += cpuMs;
      node.totalGpuMs += gpuMs;
      node.calls += calls;
      if (idleLeaf)
        node.hasIdleLeaf = true;
      else
        node.hasActiveLeaf = true;

      if (node.parentPath.empty())
        break;
      path = node.parentPath;
    }
  };

  for (const auto &kv : sectionTotals) {
    const auto &agg = kv.second;
    const double selfCpu = std::max(0.0, agg.totalCpuMs - childCpuSums[agg.path]);
    const double selfGpu = std::max(0.0, agg.totalGpuMs - childGpuSums[agg.path]);
    if (selfCpu <= 1e-9 && selfGpu <= 1e-9)
      continue;

    const bool idleLeaf = isIdleWaitSectionPath(agg.path);
    const std::string semanticLeafPath = classifySemanticLeaf(agg.path, idleLeaf);
    addSemanticLeaf(semanticLeafPath, selfCpu, selfGpu, agg.calls, idleLeaf);
  }

  // 计算语义树 self 值与导出列表。
  std::unordered_map<std::string, double> semanticChildCpuSums;
  std::unordered_map<std::string, double> semanticChildGpuSums;
  for (const auto& kv : semanticNodes) {
    const auto& node = kv.second;
    if (!node.parentPath.empty()) {
      semanticChildCpuSums[node.parentPath] += node.totalCpuMs;
      semanticChildGpuSums[node.parentPath] += node.totalGpuMs;
    }
  }

  struct SemanticExportRow {
    std::string name;
    std::string path;
    std::string parentPath;
    bool isIdleWait = false;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double totalSelfCpuMs = 0.0;
    double avgSelfCpuMs = 0.0;
    double totalGpuMs = 0.0;
    double avgGpuMs = 0.0;
    double totalSelfGpuMs = 0.0;
    double avgSelfGpuMs = 0.0;
    double avgCpuPerCycleMs = 0.0;
    double semanticSharePct = 0.0;
    uint64_t calls = 0;
    double callsPerFrame = 0.0;
    double framePct = 0.0;
  };

  const auto semanticRootIt = semanticNodes.find(kSemanticRootPath);
  const double semanticRootTotal =
      (semanticRootIt != semanticNodes.end()) ? semanticRootIt->second.totalCpuMs
                                              : 0.0;
  const double cycleCountTotal =
      (cycleRoot && cycleRoot->calls > 0)
          ? static_cast<double>(cycleRoot->calls)
          : static_cast<double>(frames.size());

  std::vector<SemanticExportRow> semanticRows;
  semanticRows.reserve(semanticNodes.size());
  for (const auto& kv : semanticNodes) {
    const auto& node = kv.second;
    SemanticExportRow row = {};
    row.name = node.name;
    row.path = node.path;
    row.parentPath = node.parentPath;
    row.isIdleWait = node.hasIdleLeaf && !node.hasActiveLeaf;
    row.totalCpuMs = node.totalCpuMs;
    row.avgCpuMs =
        frames.empty() ? 0.0 : (node.totalCpuMs / static_cast<double>(frames.size()));
    row.totalSelfCpuMs =
        std::max(0.0, node.totalCpuMs - semanticChildCpuSums[node.path]);
    row.avgSelfCpuMs =
        frames.empty() ? 0.0 : (row.totalSelfCpuMs / static_cast<double>(frames.size()));
    row.totalGpuMs = node.totalGpuMs;
    row.avgGpuMs =
        frames.empty() ? 0.0 : (node.totalGpuMs / static_cast<double>(frames.size()));
    row.totalSelfGpuMs =
        std::max(0.0, node.totalGpuMs - semanticChildGpuSums[node.path]);
    row.avgSelfGpuMs =
        frames.empty() ? 0.0 : (row.totalSelfGpuMs / static_cast<double>(frames.size()));
    row.avgCpuPerCycleMs =
        (cycleCountTotal > 1e-6) ? (node.totalCpuMs / cycleCountTotal) : row.avgCpuMs;
    row.semanticSharePct =
        (semanticRootTotal > 1e-6)
            ? std::clamp(node.totalCpuMs / semanticRootTotal * 100.0, 0.0, 100.0)
            : 0.0;

    const bool hasSemanticChildren =
        (semanticChildCpuSums[node.path] > 1e-9) ||
        (semanticChildGpuSums[node.path] > 1e-9);
    row.calls = node.calls;
    row.callsPerFrame =
        (frames.empty() || hasSemanticChildren)
            ? 0.0
            : (static_cast<double>(node.calls) / static_cast<double>(frames.size()));
    row.framePct =
        (totalCpu > 1e-6) ? std::clamp(node.totalCpuMs / totalCpu * 100.0, 0.0, 100.0) : 0.0;
    semanticRows.push_back(std::move(row));
  }

  std::sort(semanticRows.begin(), semanticRows.end(),
            [](const SemanticExportRow& a, const SemanticExportRow& b) {
              if (a.path == b.path)
                return a.avgCpuMs > b.avgCpuMs;
              return a.path < b.path;
            });

  json << "  \"avgTrackedRootCpuMs\": " << avgTrackedRootCpuMs << ",\n";
  json << "  \"avgTrackedActiveCpuMs\": " << avgTrackedActiveCpuMs << ",\n";
  json << "  \"avgIdleWaitCpuMs\": " << avgIdleWaitCpuMs << ",\n";
  json << "  \"avgUntrackedCpuMs\": " << avgUntrackedCpuMs << ",\n";
  json << "  \"avgUntrackedActiveCpuMs\": " << avgUntrackedActiveCpuMs << ",\n";
  json << "  \"activeFrameTimeMs\": " << avgActiveFrameMs << ",\n";
  json << "  \"idleActiveOverlapLikely\": "
       << (idleActiveOverlapLikely ? "true" : "false") << ",\n";
  json << "  \"cpuCoveragePct\": " << cpuCoveragePct << ",\n";
  json << "  \"cpuCoverageWithIdlePct\": " << cpuCoverageWithIdlePct << ",\n";
  json << "  \"cpuInclusivePct\": " << cpuInclusivePct << ",\n";
  json << "  \"mainLoopUnknownAttribution\": {\n";
  json << "    \"mainThreadTrackedMs\": " << avgTrackedRootCpuMs << ",\n";
  json << "    \"mainThreadGapMs\": " << mainThreadUnattributedMs << ",\n";
  json << "    \"workerGapMs\": " << workerUnattributedMs << ",\n";
  json << "    \"outsideMainLoopMs\": " << outsideMainLoopMs << "\n";
  json << "  },\n";
  json << "  \"mainLoopCycle\": {\n";
  json << "    \"present\": " << (cycleRoot ? "true" : "false") << ",\n";
  json << "    \"avgCycleMs\": " << cycleAvgMs << ",\n";
  json << "    \"avgActiveMs\": " << cycleActiveAvgMs << ",\n";
  json << "    \"avgIdleMs\": " << cycleIdleAvgMs << ",\n";
  json << "    \"phases\": [\n";
  first = true;
  for (const auto &row : cyclePhases) {
    if (!first)
      json << ",\n";
    json << "      {\"name\": \"" << row.name << "\", \"path\": \"" << row.path
         << "\", \"totalCpuMs\": " << row.totalCpuMs
         << ", \"avgCpuMs\": " << row.avgCpuMs
         << ", \"sharePct\": " << row.sharePct
         << ", \"callsPerFrame\": " << row.callsPerFrame << "}";
    first = false;
  }
  json << "\n    ]\n";
  json << "  },\n";
  json << "  \"mainLoopStages\": [\n";
  first = true;
  for (const auto &row : mainLoopStages) {
    if (!first)
      json << ",\n";
    json << "    {\"name\": \"" << row.name << "\", \"suffix\": \""
         << row.suffix << "\", \"totalCpuMs\": " << row.totalCpuMs
         << ", \"avgCpuMs\": " << row.avgCpuMs
         << ", \"shareInFramePct\": " << row.shareInFramePct
         << ", \"callsPerFrame\": " << row.callsPerFrame << "}";
    first = false;
  }
  json << "\n  ],\n";

  auto emitMainLoopBreakdownRows =
      [&](const char *fieldName, const std::vector<MainLoopBreakdownRow> &rows) {
        json << "  \"" << fieldName << "\": [\n";
        bool localFirst = true;
        for (const auto &row : rows) {
          if (!localFirst)
            json << ",\n";
          json << "    {\"name\": \"" << row.name << "\", \"path\": \""
               << row.path << "\", \"totalCpuMs\": " << row.totalCpuMs
               << ", \"avgCpuMs\": " << row.avgCpuMs
               << ", \"shareInParentPct\": " << row.shareInParentPct
               << ", \"callsPerFrame\": " << row.callsPerFrame
               << ", \"sourceTag\": \"" << row.sourceTag << "\"}";
          localFirst = false;
        }
        json << "\n  ],\n";
      };

  emitMainLoopBreakdownRows("mainLoopDispatchCases", dispatchCaseRows);
  emitMainLoopBreakdownRows("mainLoopDispatchModules", dispatchModuleRows);
  emitMainLoopBreakdownRows("mainLoopTickUpdateSub", tickUpdateSubRows);
  emitMainLoopBreakdownRows("mainLoopRunCallbacksCallers",
                            runCallbacksCallerRows);
  emitMainLoopBreakdownRows("mainLoopFunctionBreakdown", functionBreakdownRows);
  emitMainLoopBreakdownRows("mainLoopDispatchCaseFunctions",
                            dispatchCaseFunctionRows);

  json << "  \"semanticSections\": [\n";
  first = true;
  for (const auto& row : semanticRows) {
    if (!first)
      json << ",\n";
    json << "    {\"name\": \"" << row.name
         << "\", \"path\": \"" << row.path
         << "\", \"parentPath\": \"" << row.parentPath
         << "\", \"isIdleWait\": " << (row.isIdleWait ? "true" : "false")
         << ", \"totalCpuMs\": " << row.totalCpuMs
         << ", \"avgCpuMs\": " << row.avgCpuMs
         << ", \"totalSelfCpuMs\": " << row.totalSelfCpuMs
         << ", \"avgSelfCpuMs\": " << row.avgSelfCpuMs
         << ", \"totalGpuMs\": " << row.totalGpuMs
         << ", \"avgGpuMs\": " << row.avgGpuMs
         << ", \"totalSelfGpuMs\": " << row.totalSelfGpuMs
         << ", \"avgSelfGpuMs\": " << row.avgSelfGpuMs
         << ", \"avgCpuPerCycleMs\": " << row.avgCpuPerCycleMs
         << ", \"semanticSharePct\": " << row.semanticSharePct
         << ", \"calls\": " << row.calls
         << ", \"callsPerFrame\": " << row.callsPerFrame
         << ", \"framePct\": " << row.framePct
         << "}";
    first = false;
  }
  json << "\n  ],\n";

  json << "  \"semanticRootSections\": [\n";
  first = true;
  for (const auto& row : semanticRows) {
    if (row.parentPath != kSemanticRootPath)
      continue;
    if (!first)
      json << ",\n";
    json << "    {\"name\": \"" << row.name
         << "\", \"path\": \"" << row.path
         << "\", \"isIdleWait\": " << (row.isIdleWait ? "true" : "false")
         << ", \"totalCpuMs\": " << row.totalCpuMs
         << ", \"avgCpuMs\": " << row.avgCpuMs
         << ", \"trackedPct\": " << row.semanticSharePct
         << "}";
    first = false;
  }
  json << "\n  ],\n";

  json << "  \"sections\": [\n";
  first = true;
  for (const auto &kv : sectionTotals) {
    if (!first)
      json << ",\n";
    const auto &agg = kv.second;
    double avgCpu = frames.empty() ? 0.0 : agg.totalCpuMs / frames.size();
    double avgGpu = frames.empty() ? 0.0 : agg.totalGpuMs / frames.size();
    const double selfCpu =
        std::max(0.0, agg.totalCpuMs - childCpuSums[agg.path]);
    const double selfGpu =
        std::max(0.0, agg.totalGpuMs - childGpuSums[agg.path]);
    const double avgSelfCpu = frames.empty() ? 0.0 : selfCpu / frames.size();
    const double avgSelfGpu = frames.empty() ? 0.0 : selfGpu / frames.size();
    const double callsPerFrame = frames.empty()
                                     ? 0.0
                                     : static_cast<double>(agg.calls) /
                                           static_cast<double>(frames.size());
    const double trackedPct =
        (rootSectionSum > 1e-6)
            ? std::clamp(agg.totalCpuMs / rootSectionSum * 100.0, 0.0, 100.0)
            : 0.0;
    json << "    {\"name\": \"" << agg.name << "\", \"path\": \"" << agg.path
         << "\", \"parentPath\": \"" << agg.parentPath << "\", \"isIdleWait\": "
         << (isIdleWaitSectionPath(agg.path) ? "true" : "false")
         << ", \"totalCpuMs\": " << agg.totalCpuMs
         << ", \"avgCpuMs\": " << avgCpu << ", \"totalSelfCpuMs\": " << selfCpu
         << ", \"avgSelfCpuMs\": " << avgSelfCpu
         << ", \"totalGpuMs\": " << agg.totalGpuMs
         << ", \"avgGpuMs\": " << avgGpu << ", \"totalSelfGpuMs\": " << selfGpu
         << ", \"avgSelfGpuMs\": " << avgSelfGpu << ", \"calls\": " << agg.calls
         << ", \"callsPerFrame\": " << callsPerFrame
         << ", \"trackedPct\": " << trackedPct << "}";
    first = false;
  }
  json << "\n  ],\n";

  // 父子关系边：用于前端展示函数关系与调用层级热度。
  struct SectionEdge {
    std::string parentPath;
    std::string parentName;
    std::string childPath;
    std::string childName;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double parentAvgCpuMs = 0.0;
    double sharePct = 0.0;
    double callsPerFrame = 0.0;
  };
  std::vector<SectionEdge> edges;
  edges.reserve(sectionTotals.size());
  for (const auto &kv : sectionTotals) {
    const auto &child = kv.second;
    if (child.parentPath.empty())
      continue;
    auto parentIt = sectionTotals.find(child.parentPath);
    if (parentIt == sectionTotals.end())
      continue;

    const auto &parent = parentIt->second;
    SectionEdge edge;
    edge.parentPath = parent.path;
    edge.parentName = parent.name;
    edge.childPath = child.path;
    edge.childName = child.name;
    edge.totalCpuMs = child.totalCpuMs;
    edge.avgCpuMs =
        frames.empty()
            ? 0.0
            : (child.totalCpuMs / static_cast<double>(frames.size()));
    edge.parentAvgCpuMs =
        frames.empty()
            ? 0.0
            : (parent.totalCpuMs / static_cast<double>(frames.size()));
    edge.sharePct =
        (parent.totalCpuMs > 1e-6)
            ? std::clamp(child.totalCpuMs / parent.totalCpuMs * 100.0, 0.0,
                         100.0)
            : 0.0;
    edge.callsPerFrame = frames.empty() ? 0.0
                                        : (static_cast<double>(child.calls) /
                                           static_cast<double>(frames.size()));
    edges.push_back(std::move(edge));
  }
  std::sort(edges.begin(), edges.end(),
            [](const SectionEdge &a, const SectionEdge &b) {
              return a.avgCpuMs > b.avgCpuMs;
            });

  json << "  \"sectionEdges\": [\n";
  first = true;
  for (const auto &edge : edges) {
    if (!first)
      json << ",\n";
    json << "    {\"parentPath\": \"" << edge.parentPath
         << "\", \"parentName\": \"" << edge.parentName
         << "\", \"childPath\": \"" << edge.childPath << "\", \"childName\": \""
         << edge.childName << "\", \"totalCpuMs\": " << edge.totalCpuMs
         << ", \"avgCpuMs\": " << edge.avgCpuMs
         << ", \"parentAvgCpuMs\": " << edge.parentAvgCpuMs
         << ", \"sharePct\": " << edge.sharePct
         << ", \"callsPerFrame\": " << edge.callsPerFrame << "}";
    first = false;
  }
  json << "\n  ],\n";

  // 根节点分布：用于总览图（模块占比）。
  json << "  \"rootSections\": [\n";
  first = true;
  for (const auto &kv : sectionTotals) {
    const auto &agg = kv.second;
    if (!agg.parentPath.empty())
      continue;
    if (!first)
      json << ",\n";
    const double avgCpu = frames.empty() ? 0.0 : agg.totalCpuMs / frames.size();
    const double trackedPct =
        (rootSectionSum > 1e-6)
            ? std::clamp(agg.totalCpuMs / rootSectionSum * 100.0, 0.0, 100.0)
            : 0.0;
    json << "    {\"name\": \"" << agg.name << "\", \"path\": \"" << agg.path
         << "\", \"isIdleWait\": "
         << (isIdleWaitSectionPath(agg.path) ? "true" : "false")
         << ", \"totalCpuMs\": " << agg.totalCpuMs
         << ", \"avgCpuMs\": " << avgCpu << ", \"trackedPct\": " << trackedPct
         << "}";
    first = false;
  }
  json << "\n  ]\n";

  json << "}";
  return json.str();
}

void War3PerfMonitor::exportHtmlReport(const std::string &outputPath) {
  if (m_shutdownStarted.load(std::memory_order_acquire))
    return;

  flushCurrentThreadCpuDeltas();

  ExportJob job = {};
  job.finalPath = resolveExportPath(outputPath);
  job.windowSec = resolveReportWindowSec();
  job.isAuto = outputPath == "war3_perf_report_auto.html";
  {
    std::lock_guard lock(m_mutex);
    job.snapshot = captureExportSnapshotLocked();
    war3dbg::Print("DXVK War3Perf: exportHtmlReport called, frames=%zu\n",
                   job.snapshot.frameHistory.size());
  }

  enqueueExportJob(std::move(job));
}

void War3PerfMonitor::processExportJob(const ExportJob &job) {
  war3dbg::Print("DXVK War3Perf: Writing to %s\n", job.finalPath.c_str());

  std::string jsonData = generateJsonDataFromSnapshot(job.snapshot, job.windowSec);
  std::ostringstream html;
  html << R"(<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>War3 Reforge Performance Report</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<style>
        :root {
            --bg-1: #1a1a2e;
            --bg-2: #16213e;
            --ink: #e0e0e0;
            --ink-soft: #8a8a9a;
            --accent: #e2b93b;
            --accent-2: #2a9d8f;
            --panel: rgba(25, 25, 45, 0.88);
            --line: rgba(255, 255, 255, 0.08);
            --glow: rgba(226, 185, 59, 0.18);
        }

        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: "Space Grotesk", "Noto Sans SC", sans-serif;
            background:
                radial-gradient(1200px 800px at 10% 0%, rgba(226,185,59,0.06), transparent 60%),
                radial-gradient(900px 700px at 90% 10%, rgba(42,157,143,0.08), transparent 55%),
                linear-gradient(180deg, var(--bg-1), var(--bg-2));
            color: var(--ink);
            min-height: 100vh;
            padding: 24px;
        }
        .app { max-width: 1400px; margin: 0 auto; display: grid; gap: 24px; }
        header {
            display: flex; align-items: center; justify-content: space-between;
            padding: 16px 20px;
            background: var(--panel); border: 1px solid var(--line);
            border-radius: 16px;
            box-shadow: 0 8px 32px rgba(0,0,0,0.3);
            backdrop-filter: blur(12px);
        }
        header h1 {
            font-size: 22px; margin: 0;
            font-family: "Noto Serif SC","Source Serif 4",serif;
            letter-spacing: 0.02em; color: var(--accent);
        }
        header .meta { display: flex; gap: 12px; align-items: center; color: var(--ink-soft); font-size: 13px; }
        .badge {
            padding: 4px 10px; border-radius: 999px;
            border: 1px solid var(--line);
            background: rgba(255,255,255,0.06); color: var(--ink-soft);
        }
        .panel {
            background: var(--panel); border: 1px solid var(--line);
            border-radius: 16px; padding: 20px;
            box-shadow: 0 8px 32px rgba(0,0,0,0.25);
            backdrop-filter: blur(12px);
        }
        .panel h2 { font-size: 16px; margin-bottom: 12px; color: var(--accent); letter-spacing: 0.02em; }
        .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 16px; }
        .split { display: grid; grid-template-columns: minmax(0,2fr) minmax(0,1fr); gap: 16px; }
        .split-item {
            background: rgba(255,255,255,0.03); border: 1px solid var(--line);
            border-radius: 12px; padding: 14px;
        }
        .split-item h2 { margin-bottom: 10px; }
        .card {
            background: rgba(255,255,255,0.04); border-radius: 14px; padding: 18px;
            text-align: center; border: 1px solid var(--line);
            box-shadow: 0 4px 16px rgba(0,0,0,0.2);
            transition: border-color 0.2s, box-shadow 0.2s;
        }
        .card:hover { border-color: rgba(226,185,59,0.3); box-shadow: 0 4px 20px rgba(226,185,59,0.1); }
        .card-value { font-size: 26px; font-weight: 700; color: var(--accent); }
        .card-label { font-size: 12px; color: var(--ink-soft); margin-top: 6px; }
        table {
            width: 100%; border-collapse: collapse; overflow: hidden;
            border-radius: 12px; border: 1px solid var(--line); table-layout: fixed;
        }
        th, td { padding: 10px 12px; text-align: left; border-bottom: 1px solid var(--line); }
        th { background: rgba(226,185,59,0.08); color: var(--accent); font-weight: 600; }
        thead th { position: sticky; top: 0; z-index: 2; }
        tr:hover { background: rgba(255,255,255,0.03); }
        .section-tools {
            display: flex; gap: 8px; margin-bottom: 12px; flex-wrap: wrap; align-items: center;
        }
        .section-tools button {
            border: 1px solid var(--line); background: rgba(255,255,255,0.06);
            color: var(--ink); border-radius: 999px; padding: 6px 12px;
            font-size: 12px; cursor: pointer; transition: all 0.2s;
        }
        .section-tools button:hover { border-color: var(--accent); color: var(--accent); background: rgba(226,185,59,0.1); }
        .section-tools input, .section-tools select {
            border: 1px solid var(--line); background: rgba(255,255,255,0.06);
            color: var(--ink); border-radius: 999px; padding: 6px 12px;
            font-size: 12px; outline: none; min-width: 180px;
        }
        .section-tools select option { background: #1a1a2e; color: var(--ink); }
        .section-tools input:focus, .section-tools select:focus {
            border-color: var(--accent); box-shadow: 0 0 0 3px rgba(226,185,59,0.15);
        }
        .hint { margin-top: 10px; color: var(--ink-soft); font-size: 12px; line-height: 1.5; }
        .relation-tools {
            display: flex; gap: 8px; margin-bottom: 12px; flex-wrap: wrap; align-items: center;
        }
        .relation-tools input, .relation-tools select {
            border: 1px solid var(--line); background: rgba(255,255,255,0.06);
            color: var(--ink); border-radius: 999px; padding: 6px 12px;
            font-size: 12px; outline: none; min-width: 180px;
        }
        .relation-tools select option { background: #1a1a2e; color: var(--ink); }
        .relation-tools input:focus, .relation-tools select:focus {
            border-color: var(--accent); box-shadow: 0 0 0 3px rgba(226,185,59,0.15);
        }
        .relation-parent, .relation-child {
            display: inline-block;
            font-family: "Consolas","JetBrains Mono",monospace;
            font-size: 12px; line-height: 1.3; max-width: 100%;
            white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
            vertical-align: bottom;
        }
        .relation-parent { color: #e2b93b; }
        .relation-child  { color: #2a9d8f; }
        .relation-share { display: grid; gap: 4px; }
        .relation-share .bar { background: linear-gradient(90deg, #2a9d8f, #55c070); }

        /* ── Spark-style Section Tree ── */
        .tree-container {
            font-family: "Consolas","JetBrains Mono",monospace;
            font-size: 13px; line-height: 1.1;
        }
        .tree-node-header {
            display: flex; align-items: center; padding: 3px 8px;
            border-radius: 4px; cursor: default; gap: 2px;
            transition: background 0.15s;
        }
        .tree-node-header:hover { background: rgba(255,255,255,0.04); }
        .tree-toggle {
            width: 16px; height: 16px;
            display: inline-flex; align-items: center; justify-content: center;
            cursor: pointer; user-select: none; color: var(--ink-soft);
            flex-shrink: 0; font-size: 10px; border-radius: 3px;
            transition: all 0.15s;
        }
        .tree-toggle:hover { background: rgba(255,255,255,0.08); color: var(--accent); }
        .tree-spacer { width: 16px; flex-shrink: 0; }
        .tree-children {
            margin-left: 8px; padding-left: 12px;
            border-left: 1px solid rgba(255,255,255,0.06);
        }
        .tree-children:hover { border-left-color: rgba(255,255,255,0.12); }
        .tree-name      { color: var(--ink); font-weight: 600; white-space: nowrap; }
        .tree-name-idle { color: #2a9d8f;   font-weight: 600; white-space: nowrap; }
        .tree-pct  { margin-left: 8px; font-weight: 700; font-size: 12px; white-space: nowrap; }
        .tree-ms   { margin-left: 4px; color: var(--ink-soft); font-size: 12px; white-space: nowrap; }
        .tree-gpu  { margin-left: 6px; color: #6a7fdb; font-size: 11px; white-space: nowrap; }
        .tree-calls{ margin-left: 6px; color: var(--ink-soft); font-size: 11px; white-space: nowrap; opacity: 0.7; }
        .tree-idle-tag {
            margin-left: 6px; padding: 0 5px; border-radius: 3px; font-size: 10px;
            color: #2a9d8f; border: 1px solid rgba(42,157,143,0.3);
            background: rgba(42,157,143,0.08);
        }
        .tree-self-bar {
            margin-left: auto; width: 120px; height: 4px;
            background: rgba(255,255,255,0.04); border-radius: 2px;
            flex-shrink: 0; overflow: hidden;
        }
        .tree-self-bar-fill { height: 100%; border-radius: 2px; background: linear-gradient(90deg, #e2b93b, #d62828); }

        .idle-tag {
            display: inline-block; margin-left: 8px; padding: 1px 6px;
            border-radius: 999px; font-size: 10px; color: #2a9d8f;
            border: 1px solid rgba(42,157,143,0.3); background: rgba(42,157,143,0.08);
            vertical-align: middle;
        }
        .dist { display: grid; gap: 4px; }
        .bar { height: 8px; border-radius: 4px; }
        .bar.incl { background: linear-gradient(90deg, var(--accent), var(--accent-2)); }
        .bar.self { background: linear-gradient(90deg, #d62828, #f77f00); }
        .bar-label { font-size: 11px; color: var(--ink-soft); }
        @media (max-width: 1200px) { .split { grid-template-columns: 1fr; } }
        footer { text-align: center; color: var(--ink-soft); font-size: 12px; }
    </style>
</head>
<body>
    <div class="app">
        <header>
            <h1>War3 Reforge Performance Report</h1>
            <div class="meta">
                <span class="badge">DXVK</span>
                <span class="badge">Perf</span>
            </div>
        </header>

        <section class="panel">
            <div class="cards" id="summaryCards"></div>
        </section>

        <section class="panel">
            <h2>MainLoop Cycle Breakdown</h2>
            <table id="cycleTable">
                <thead>
                    <tr>
                        <th>Phase</th>
                        <th>Avg CPU (ms)</th>
                        <th>Calls/Frame</th>
                        <th>Share In Cycle</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
            <div class="hint">
                说明：该表只展示 `War3MainLoop/Pump/Cycle/*` 的单轮主循环拆解，避免被其它嵌套路径干扰。
            </div>
        </section>

        <section class="panel">
            <h2>MainLoop Stage Breakdown</h2>
            <table id="mainLoopStageTable">
                <thead>
                    <tr>
                        <th>Stage</th>
                        <th>Avg CPU (ms)</th>
                        <th>Calls/Frame</th>
                        <th>Share In Avg Frame</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
            <div class="hint">
                说明：按主循环执行顺序聚合展示（非按耗时排序），用于直接观察单轮循环的阶段时序与开销。
            </div>
        </section>

        <section class="panel">
            <h2>Dispatch Case Breakdown</h2>
            <table id="dispatchCaseTable">
                <thead>
                    <tr>
                        <th>Case</th>
                        <th>Avg CPU (ms)</th>
                        <th>Calls/Frame</th>
                        <th>Share In Dispatch</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
        </section>

        <section class="panel">
            <h2>Dispatch Module Breakdown</h2>
            <table id="dispatchModuleTable">
                <thead>
                    <tr>
                        <th>Module</th>
                        <th>Avg CPU (ms)</th>
                        <th>Calls/Frame</th>
                        <th>Share In DispatchModule</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
            <div class="hint">
                说明：当前 `DispatchModule/*` 为 case 语义映射（`sourceTag=CaseMapped`），用于逻辑模块定位，不代表每个子函数已独立 Hook。
            </div>
        </section>

        <section class="panel">
            <h2>TickUpdate Sub Breakdown</h2>
            <table id="tickUpdateSubTable">
                <thead>
                    <tr>
                        <th>Sub Stage</th>
                        <th>Avg CPU (ms)</th>
                        <th>Calls/Frame</th>
                        <th>Share In TickUpdate/Sub</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
        </section>

        <section class="panel">
            <h2>RunCallbacks Caller Breakdown</h2>
            <table id="runCallbacksCallerTable">
                <thead>
                    <tr>
                        <th>Caller</th>
                        <th>Avg CPU (ms)</th>
                        <th>Calls/Frame</th>
                        <th>Share In RunCallbacks</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
        </section>

        <section class="panel">
            <h2>MainLoop Function Breakdown</h2>
            <table id="mainLoopFunctionTable">
                <thead>
                    <tr>
                        <th>Function</th>
                        <th>Avg CPU (ms)</th>
                        <th>Calls/Frame</th>
                        <th>Share In Function Root</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
        </section>

        <section class="panel">
            <h2>Dispatch Case Function Mapping</h2>
            <table id="dispatchCaseFunctionTable">
                <thead>
                    <tr>
                        <th>Function</th>
                        <th>Avg CPU (ms)</th>
                        <th>Calls/Frame</th>
                        <th>Share In DispatchCaseFunctions</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
            <div class="hint">
                说明：该表来自 `EventDispatch case -> callee` 语义映射归因，用于在“子函数独立 Hook”前提供稳定函数级证据。
            </div>
        </section>

        <section class="panel">
            <h2>MainLoop Unknown Attribution</h2>
            <table id="unknownAttributionTable">
                <thead>
                    <tr>
                        <th>Bucket</th>
                        <th>Avg CPU (ms/frame)</th>
                        <th>说明</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
        </section>

        <section class="panel">
            <h2>Process/Thread CPU Breakdown</h2>
            <table id="threadCpuTable">
                <thead>
                    <tr>
                        <th>Scope</th>
                        <th>Avg CPU (ms/frame)</th>
                        <th>Share Of Avg Frame</th>
                        <th>Share Of Process</th>
                        <th>Data Coverage</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
            <div class="hint">
                说明：`Process` 为全进程所有线程 CPU 时间；`MainThread` 为已识别主循环线程；`Workers` = Process - MainThread。
            </div>
        </section>

        <section class="panel split">
            <div class="split-item">
                <h2>Frame Time Timeline (ms)</h2>
                <canvas id="frameTimeChart" height="120"></canvas>
            </div>
            <div class="split-item">
                <h2>Root CPU Breakdown</h2>
                <canvas id="rootCpuChart" height="120"></canvas>
            </div>
        </section>

        <section class="panel">
            <h2>Section Breakdown</h2>
            <div class="section-tools">
                <select id="sectionViewMode">
                    <option value="semantic">严格语义树（推荐）</option>
                    <option value="raw">原始调用树</option>
                </select>
                <button id="expandAll">全部展开</button>
                <button id="collapseAll">全部折叠</button>
                <select id="sortMode">
                    <option value="self">按 Self CPU 排序</option>
                    <option value="inclusive">按 Inclusive CPU 排序</option>
                    <option value="calls">按 Calls/Frame 排序</option>
                </select>
                <input id="sectionFilter" type="text" placeholder="搜索函数（支持路径关键字）" />
            </div>
            <div class="tree-container" id="sectionTree"></div>
            <div class="hint">
                语义树采用 strict 口径（基于 Self CPU 聚合），仅统计“已追踪且可归类”的 active scope；`Other/Untracked*` 不进入该树。语义视图百分比 = 节点 / Semantic 根（总和=100%）；未追踪时间请看上方 `MainLoop Unknown Attribution` 与 Coverage 指标。颜色编码：<span style="color:#55c070">■</span>&lt;1% <span style="color:#a3c23b">■</span>1-3% <span style="color:#e2b93b">■</span>3-10% <span style="color:#e28c3b">■</span>10-30% <span style="color:#d62828">■</span>&gt;30%。右侧小条 = Self CPU 占比。
            </div>
        </section>

        <section class="panel">
            <h2>Function Relationship</h2>
            <div class="relation-tools">
                <select id="relationSortMode">
                    <option value="avg">按 Child Avg CPU 排序</option>
                    <option value="share">按 Child/Parent 占比排序</option>
                    <option value="calls">按 Calls/Frame 排序</option>
                </select>
                <select id="relationTopN">
                    <option value="40">Top 40</option>
                    <option value="80">Top 80</option>
                    <option value="120">Top 120</option>
                    <option value="200">Top 200</option>
                </select>
                <input id="relationFilter" type="text" placeholder="搜索父子路径（Parent/Child）" />
            </div>
            <table id="relationTable">
                <thead>
                    <tr>
                        <th>Parent</th>
                        <th>Child</th>
                        <th>Avg CPU (ms)</th>
                        <th>Calls/Frame</th>
                        <th>Child / Parent</th>
                    </tr>
                </thead>
                <tbody></tbody>
            </table>
            <div class="hint">
                说明：该表直接展示父子函数关系边，优先观察 `Avg CPU` 与 `Child/Parent` 同时偏高的链路。
            </div>
        </section>

        <footer>
            Generated by War3 Reforge DXVK | )"
       << __DATE__ << " " << __TIME__ << R"(
        </footer>
    </div>

    <script>
        const data = )"
       << jsonData << R"(;
        const f2 = (v) => Number(v || 0).toFixed(2);
        const f3 = (v) => Number(v || 0).toFixed(3);

        // Summary Cards
        const cycle = (data && data.mainLoopCycle) ? data.mainLoopCycle : null;
        const runtimeV2 = (data && data.shadowRuntimeV2Summary)
            ? data.shadowRuntimeV2Summary
            : {};
        const cardsHtml = `
            <div class="card"><div class="card-value">${f2(data.avgFps)}</div><div class="card-label">Average FPS</div></div>
            <div class="card"><div class="card-value">${f2(data.avgFrameTimeMs)}</div><div class="card-label">Avg CPU Frame (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.activeFrameTimeMs)}</div><div class="card-label">Avg Active CPU Frame (ms)</div></div>
            <div class="card"><div class="card-value">${f2(cycle?.avgCycleMs || 0)}</div><div class="card-label">Avg MainLoop Cycle (ms)</div></div>
            <div class="card"><div class="card-value">${f2(cycle?.avgActiveMs || 0)}</div><div class="card-label">Cycle Active (ms)</div></div>
            <div class="card"><div class="card-value">${f2(cycle?.avgIdleMs || 0)}</div><div class="card-label">Cycle Idle (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.p95CpuMs)}</div><div class="card-label">P95 CPU Frame (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.p99CpuMs)}</div><div class="card-label">P99 CPU Frame (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.stddevCpuMs)}</div><div class="card-label">CPU StdDev (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.avgGpuTimeMs)}</div><div class="card-label">Avg GPU Frame (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.p95GpuMs)}</div><div class="card-label">P95 GPU Frame (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.avgProcessCpuMs)}</div><div class="card-label">Avg Process CPU (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.avgMainThreadCpuMs)}</div><div class="card-label">Avg MainThread CPU (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.avgWorkerThreadsCpuMs)}</div><div class="card-label">Avg Worker CPU (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.processParallelismCores)}</div><div class="card-label">Process Parallelism (cores)</div></div>
            <div class="card"><div class="card-value">${f2(data.mainThreadShareOfProcessPct)}%</div><div class="card-label">MainThread Share Of Process</div></div>
            <div class="card"><div class="card-value">${f2(data.avgTrackedActiveCpuMs)}</div><div class="card-label">Avg Tracked Active CPU (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.avgIdleWaitCpuMs)}</div><div class="card-label">Avg Idle Wait CPU (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.avgUntrackedActiveCpuMs)}</div><div class="card-label">Avg Untracked Active CPU (ms)</div></div>
            <div class="card"><div class="card-value">${f2(data.cpuCoveragePct)}%</div><div class="card-label">CPU Coverage (Active Root)</div></div>
            <div class="card"><div class="card-value">${f2(data.cpuCoverageWithIdlePct)}%</div><div class="card-label">CPU Coverage (Incl Idle)</div></div>
            <div class="card"><div class="card-value">${data.idleActiveOverlapLikely ? 'YES' : 'NO'}</div><div class="card-label">Idle/Active Overlap Hint</div></div>
            <div class="card"><div class="card-value">${data.jank16 || 0}</div><div class="card-label">Jank Frames >16.67ms</div></div>
            <div class="card"><div class="card-value">${data.jank33 || 0}</div><div class="card-label">Jank Frames >33.33ms</div></div>
            <div class="card"><div class="card-value">${data.frameCount || 0}</div><div class="card-label">Frames Recorded</div></div>
            <div class="card"><div class="card-value">${f2(data.windowSec)}s</div><div class="card-label">统计窗口</div></div>
            <div class="card"><div class="card-value">${runtimeV2.shadowModelResourceCount || 0}</div><div class="card-label">Shadow Model Resources</div></div>
            <div class="card"><div class="card-value">${runtimeV2.shadowRuntimeModelCount || 0}</div><div class="card-label">Shadow Runtime Models</div></div>
            <div class="card"><div class="card-value">${runtimeV2.shadowPoseReadyCount || 0}</div><div class="card-label">Shadow Pose Ready</div></div>
            <div class="card"><div class="card-value">${runtimeV2.upperLayerResolveAttempts || 0}</div><div class="card-label">Upper Resolve Attempts</div></div>
            <div class="card"><div class="card-value">${runtimeV2.upperLayerResolvedAuthoritativeItems || 0}</div><div class="card-label">Upper Resolved Items</div></div>
            <div class="card"><div class="card-value">${runtimeV2.upperLayerEmitted || 0}</div><div class="card-label">Upper Emitted</div></div>
            <div class="card"><div class="card-value">${runtimeV2.upperLayerResolveAuthoritativeRigid || 0}</div><div class="card-label">Upper Rigid Authoritative</div></div>
            <div class="card"><div class="card-value">${runtimeV2.upperLayerResolveAuthoritativeSkinned || 0}</div><div class="card-label">Upper Skinned Authoritative</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreResolved || 0}</div><div class="card-label">Semantic Core Resolved</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreFrameLag || 0}</div><div class="card-label">Semantic Frame Lag</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreSkinnedResolved || 0}</div><div class="card-label">Semantic Core Skinned</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreExplicitBlendAttempts || 0}</div><div class="card-label">Semantic Explicit Attempts</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreExplicitBlendResolved || 0}</div><div class="card-label">Semantic Explicit Blend</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreExplicitBlendSpanRemapResolved || 0}</div><div class="card-label">Semantic Span Remap</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreExplicitBlendStrideSearchMiss || 0}</div><div class="card-label">Semantic Explicit Stride Miss</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreExplicitBlendFinalDecodeMiss || 0}</div><div class="card-label">Semantic Explicit Final Miss</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreUpperLayerResolvedItems || 0}</div><div class="card-label">Semantic Upper Resolved</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreSupplementalUpperLayerDrawPacketCount || 0}</div><div class="card-label">Semantic Upper Added</div></div>
            <div class="card"><div class="card-value">${runtimeV2.semanticCoreSubmittedDrawCount || 0}</div><div class="card-label">Semantic Core Submitted</div></div>
        `;
        document.getElementById('summaryCards').innerHTML = cardsHtml;

        // MainLoop Cycle Table
        const cycleTbody = document.querySelector('#cycleTable tbody');
        if (cycleTbody) {
            cycleTbody.innerHTML = '';
            const cycleRows = Array.isArray(cycle?.phases) ? cycle.phases : [];
            if (!cycle || !cycle.present || cycleRows.length === 0) {
                const row = document.createElement('tr');
                row.innerHTML = `<td colspan="4">暂无 MainLoop Cycle 聚合数据（请确认已启用生命周期 Hook 并完成一轮采样）。</td>`;
                cycleTbody.appendChild(row);
            } else {
                const maxPhaseAvg = Math.max(0.001, ...cycleRows.map((r) => Number(r.avgCpuMs || 0)));
                cycleRows.forEach((r) => {
                    const avg = Number(r.avgCpuMs || 0);
                    const calls = Number(r.callsPerFrame || 0);
                    const share = Number(r.sharePct || 0);
                    const width = Math.max(0, Math.min(100, (avg / maxPhaseAvg) * 100));
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td><span class="relation-child" title="${r.path || ''}">${r.name || ''}</span></td>
                        <td>${f3(avg)}</td>
                        <td>${f2(calls)}</td>
                        <td>
                            <div class="relation-share">
                                <div class="bar" style="width:${width}%"></div>
                                <div class="bar-label">${f2(share)}% of Cycle</div>
                            </div>
                        </td>
                    `;
                    cycleTbody.appendChild(tr);
                });
            }
        }

        // MainLoop Stage Table
        const mainLoopStageTbody = document.querySelector('#mainLoopStageTable tbody');
        if (mainLoopStageTbody) {
            mainLoopStageTbody.innerHTML = '';
            const stageRows = Array.isArray(data.mainLoopStages) ? data.mainLoopStages : [];
            if (stageRows.length === 0) {
                const row = document.createElement('tr');
                row.innerHTML = `<td colspan="4">暂无主循环阶段聚合数据。</td>`;
                mainLoopStageTbody.appendChild(row);
            } else {
                const maxStageAvg = Math.max(0.001, ...stageRows.map((r) => Number(r.avgCpuMs || 0)));
                stageRows.forEach((r) => {
                    const avg = Number(r.avgCpuMs || 0);
                    const calls = Number(r.callsPerFrame || 0);
                    const share = Number(r.shareInFramePct || 0);
                    const width = Math.max(0, Math.min(100, (avg / maxStageAvg) * 100));
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td><span class="relation-parent" title="${r.suffix || ''}">${r.name || ''}</span></td>
                        <td>${f3(avg)}</td>
                        <td>${f2(calls)}</td>
                        <td>
                            <div class="relation-share">
                                <div class="bar" style="width:${width}%"></div>
                                <div class="bar-label">${f2(share)}% of AvgFrame</div>
                            </div>
                        </td>
                    `;
                    mainLoopStageTbody.appendChild(tr);
                });
            }
        }

        const renderMainLoopBreakdownTable = (tableSelector, rows, emptyText, shareLabel) => {
            const tbody = document.querySelector(tableSelector);
            if (!tbody) return;
            tbody.innerHTML = '';
            const srcRows = Array.isArray(rows) ? rows : [];
            if (srcRows.length === 0) {
                const tr = document.createElement('tr');
                tr.innerHTML = `<td colspan="4">${emptyText}</td>`;
                tbody.appendChild(tr);
                return;
            }

            const maxAvg = Math.max(0.001, ...srcRows.map((r) => Number(r.avgCpuMs || 0)));
            srcRows.forEach((r) => {
                const avg = Number(r.avgCpuMs || 0);
                const calls = Number(r.callsPerFrame || 0);
                const share = Number(r.shareInParentPct || 0);
                const width = Math.max(0, Math.min(100, (avg / maxAvg) * 100));
                const sourceTag = (r.sourceTag && r.sourceTag !== 'HookMeasured')
                    ? `<span class="idle-tag">${r.sourceTag}</span>`
                    : '';
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td><span class="relation-parent" title="${r.path || ''}">${r.name || ''}</span>${sourceTag}</td>
                    <td>${f3(avg)}</td>
                    <td>${f2(calls)}</td>
                    <td>
                        <div class="relation-share">
                            <div class="bar" style="width:${width}%"></div>
                            <div class="bar-label">${f2(share)}% of ${shareLabel}</div>
                        </div>
                    </td>
                `;
                tbody.appendChild(tr);
            });
        };

        renderMainLoopBreakdownTable(
            '#dispatchCaseTable tbody',
            data.mainLoopDispatchCases,
            '暂无 Dispatch Case 分解数据。',
            'Dispatch');
        renderMainLoopBreakdownTable(
            '#dispatchModuleTable tbody',
            data.mainLoopDispatchModules,
            '暂无 Dispatch Module 分解数据。',
            'DispatchModule');
        renderMainLoopBreakdownTable(
            '#tickUpdateSubTable tbody',
            data.mainLoopTickUpdateSub,
            '暂无 TickUpdate 子阶段分解数据。',
            'TickUpdate/Sub');
        renderMainLoopBreakdownTable(
            '#runCallbacksCallerTable tbody',
            data.mainLoopRunCallbacksCallers,
            '暂无 RunCallbacks Caller 分解数据。',
            'RunCallbacks');
        renderMainLoopBreakdownTable(
            '#mainLoopFunctionTable tbody',
            data.mainLoopFunctionBreakdown,
            '暂无 MainLoop 函数级分解数据。',
            'Function');
        renderMainLoopBreakdownTable(
            '#dispatchCaseFunctionTable tbody',
            data.mainLoopDispatchCaseFunctions,
            '暂无 Dispatch Case -> Function 映射分解数据。',
            'DispatchCaseFunctions');

        const unknownTbody = document.querySelector('#unknownAttributionTable tbody');
        if (unknownTbody) {
            unknownTbody.innerHTML = '';
            const unknown = data.mainLoopUnknownAttribution || {};
            const rows = [
                {
                    name: 'MainThreadTracked',
                    ms: Number(unknown.mainThreadTrackedMs || 0),
                    desc: '主线程已落入 DXVK 统计根节点的 CPU 时间。'
                },
                {
                    name: 'MainThreadGap',
                    ms: Number(unknown.mainThreadGapMs || 0),
                    desc: '主线程 CPU 与已追踪根节点差值（优先补齐对象）。'
                },
                {
                    name: 'WorkerGap',
                    ms: Number(unknown.workerGapMs || 0),
                    desc: '工作线程 CPU（当前未细分到函数级 Hook）。'
                },
                {
                    name: 'OutsideMainLoop',
                    ms: Number(unknown.outsideMainLoopMs || 0),
                    desc: '进程剩余差值（通常是采样误差或系统线程）。'
                }
            ];
            rows.forEach((r) => {
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td>${r.name}</td>
                    <td>${f3(r.ms)}</td>
                    <td>${r.desc}</td>
                `;
                unknownTbody.appendChild(tr);
            });
        }

        // Process/Thread CPU Breakdown
        const threadCpuTbody = document.querySelector('#threadCpuTable tbody');
        if (threadCpuTbody) {
            threadCpuTbody.innerHTML = '';
            const avgFrameCpu = Math.max(0.001, Number(data.avgFrameTimeMs || 0));
            const avgProcessCpu = Number(data.avgProcessCpuMs || 0);
            const avgMainThreadCpu = Number(data.avgMainThreadCpuMs || 0);
            const avgWorkerCpu = Number(data.avgWorkerThreadsCpuMs || 0);
            const processCoverage = Number(data.processCpuCoveragePct || 0);
            const mainCoverage = Number(data.mainThreadCpuCoveragePct || 0);
            const workerCoverage = processCoverage;

            const rows = [
                {
                    name: 'Process (All Threads)',
                    avgMs: avgProcessCpu,
                    frameShare: (avgFrameCpu > 1e-6) ? (avgProcessCpu / avgFrameCpu * 100) : 0,
                    processShare: 100.0,
                    coverage: processCoverage
                },
                {
                    name: 'MainThread',
                    avgMs: avgMainThreadCpu,
                    frameShare: (avgFrameCpu > 1e-6) ? (avgMainThreadCpu / avgFrameCpu * 100) : 0,
                    processShare: Number(data.mainThreadShareOfProcessPct || 0),
                    coverage: mainCoverage
                },
                {
                    name: 'Workers (Process-Main)',
                    avgMs: avgWorkerCpu,
                    frameShare: (avgFrameCpu > 1e-6) ? (avgWorkerCpu / avgFrameCpu * 100) : 0,
                    processShare: Number(data.workerThreadsShareOfProcessPct || 0),
                    coverage: workerCoverage
                }
            ];

            const maxAvg = Math.max(0.001, ...rows.map((r) => Number(r.avgMs || 0)));
            rows.forEach((r) => {
                const width = Math.max(0, Math.min(100, (Number(r.avgMs || 0) / maxAvg) * 100));
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td>${r.name}</td>
                    <td>
                        <div class="relation-share">
                            <div class="bar" style="width:${width}%"></div>
                            <div class="bar-label">${f3(r.avgMs)}</div>
                        </div>
                    </td>
                    <td>${f2(r.frameShare)}%</td>
                    <td>${f2(r.processShare)}%</td>
                    <td>${f2(r.coverage)}%</td>
                `;
                threadCpuTbody.appendChild(tr);
            });
        }

        // Charts
        const frameChartCanvas = document.getElementById('frameTimeChart');
        const rootChartCanvas = document.getElementById('rootCpuChart');
        if (window.Chart && frameChartCanvas) {
            const ctx = frameChartCanvas.getContext('2d');
            const gradient = ctx.createLinearGradient(0, 0, 0, 260);
            gradient.addColorStop(0, 'rgba(226, 185, 59, 0.30)');
            gradient.addColorStop(1, 'rgba(42, 157, 143, 0.05)');
            const labels = (data.frameTimes || []).map((_, i) => i);
            const budget60 = labels.map(() => 16.67);
            const budget30 = labels.map(() => 33.33);
            new Chart(ctx, {
                type: 'line',
                data: {
                    labels,
                    datasets: [
                        { label: 'CPU Frame (ms)', data: data.frameTimes || [], borderColor: '#e2b93b', backgroundColor: gradient, fill: true, tension: 0.3, pointRadius: 0 },
                        { label: 'GPU Frame (ms)', data: data.frameTimesGpu || [], borderColor: '#2a9d8f', backgroundColor: 'rgba(17,100,102,0.08)', fill: false, tension: 0.3, pointRadius: 0 },
                        { label: '60 FPS Budget', data: budget60, borderColor: 'rgba(42,157,143,0.45)', borderDash: [5, 4], pointRadius: 0, fill: false, tension: 0 },
                        { label: '30 FPS Budget', data: budget30, borderColor: 'rgba(214,40,40,0.55)', borderDash: [5, 4], pointRadius: 0, fill: false, tension: 0 }
                    ]
                },
                options: {
                    responsive: true,
                    plugins: { legend: { display: true } },
                    scales: {
                        x: { display: false },
                        y: { grid: { color: 'rgba(255,255,255,0.08)' }, ticks: { color: '#8a8a9a' } }
                    }
                }
            });
        }

        if (window.Chart && rootChartCanvas) {
            const semanticRoots = Array.isArray(data.semanticRootSections) ? [...data.semanticRootSections] : [];
            const rootsRaw = semanticRoots.length > 0
                ? semanticRoots
                : (Array.isArray(data.rootSections) ? [...data.rootSections] : []);
            const rootsActive = rootsRaw.filter((r) => !r.isIdleWait);
            const roots = rootsActive.length > 0 ? rootsActive : rootsRaw;
            roots.sort((a, b) => (b.avgCpuMs || 0) - (a.avgCpuMs || 0));
            const top = roots.slice(0, 8);
            const others = roots.slice(8);
            if (others.length > 0) {
                const otherAvg = others.reduce((s, r) => s + (r.avgCpuMs || 0), 0);
                top.push({ name: 'Others', avgCpuMs: otherAvg });
            }
            const labels = top.map(x => x.name);
            const values = top.map(x => x.avgCpuMs || 0);
            const colors = ['#e2b93b', '#2a9d8f', '#d62828', '#6a7fdb', '#55c070', '#f4a261', '#e28c3b', '#457b9d', '#8a8a9a'];
            new Chart(rootChartCanvas.getContext('2d'), {
                type: 'doughnut',
                data: { labels, datasets: [{ data: values, backgroundColor: colors.slice(0, values.length), borderWidth: 0 }] },
                options: { responsive: true, plugins: { legend: { position: 'bottom' } } }
            });
        }

        // Section Tree (Spark-style)
        const rawSections = Array.isArray(data.sections) ? data.sections : [];
        const semanticSections = Array.isArray(data.semanticSections) ? data.semanticSections : [];
        const treeContainer = document.getElementById('sectionTree');
        const collapsed = new Set();
        const treeState = {
            sortMode: 'self',
            filter: '',
            viewMode: (semanticSections.length > 0) ? 'semantic' : 'raw'
        };
        const sectionViewModeSelect = document.getElementById('sectionViewMode');
        if (sectionViewModeSelect) {
            if (semanticSections.length === 0) {
                sectionViewModeSelect.value = 'raw';
                const semanticOption = sectionViewModeSelect.querySelector('option[value="semantic"]');
                if (semanticOption) semanticOption.disabled = true;
            } else {
                sectionViewModeSelect.value = treeState.viewMode;
            }
        }

        const getSectionsByView = () => {
            if (treeState.viewMode === 'semantic' && semanticSections.length > 0) {
                return semanticSections;
            }
            return rawSections;
        };

        let nodes = new Map();
        let treeRoots = [];
        let maxSelfMs = 0.001;

        const rebuildTreeModel = () => {
            const sections = getSectionsByView();
            nodes = new Map();
            treeRoots = [];
            maxSelfMs = Math.max(0.001, ...sections.map((s) => Number(s.avgSelfCpuMs || 0)));

            sections.forEach((s) => {
                const path = s.path || s.name || '';
                if (!path) return;
                nodes.set(path, { ...s, path, children: [] });
            });

            nodes.forEach((node) => {
                const pp = node.parentPath || '';
                if (pp && nodes.has(pp)) nodes.get(pp).children.push(node);
                else treeRoots.push(node);
            });
        };
        const getTreeSortVal = (n) => {
            if (treeState.sortMode === 'inclusive') return n.avgCpuMs || 0;
            if (treeState.sortMode === 'calls') return n.callsPerFrame || 0;
            return n.avgSelfCpuMs || 0;
        };
        const sortTree = (arr) => {
            arr.sort((a, b) => getTreeSortVal(b) - getTreeSortVal(a));
            arr.forEach((n) => sortTree(n.children));
        };
        const getPctColor = (p) => {
            if (p < 1)  return '#55c070';
            if (p < 3)  return '#a3c23b';
            if (p < 10) return '#e2b93b';
            if (p < 30) return '#e28c3b';
            return '#d62828';
        };
        const renderTreeNode = (node, container) => {
            const hasKids = node.children.length > 0;
            const semanticMode = (treeState.viewMode === 'semantic' && semanticSections.length > 0);
            const framePct = (data.avgFrameTimeMs > 1e-6) ? ((node.avgCpuMs||0)/data.avgFrameTimeMs*100) : 0;
            const semanticPct = Number(node.semanticSharePct || 0);
            const pctForDisplay = semanticMode ? semanticPct : framePct;
            const selfPct  = ((node.avgSelfCpuMs||0)/maxSelfMs*100);
            const pctColor = getPctColor(pctForDisplay);

            const div = document.createElement('div');
            div.className = 'tree-node';
            div.dataset.path = node.path;

            const hdr = document.createElement('div');
            hdr.className = 'tree-node-header';

            // toggle / spacer
            let childrenDiv = null;
            if (hasKids) {
                const tog = document.createElement('span');
                tog.className = 'tree-toggle';
                tog.textContent = '\u25BE';
                tog.addEventListener('click', () => {
                    const p = node.path;
                    if (collapsed.has(p)) {
                        collapsed.delete(p);
                        tog.textContent = '\u25BE';
                        childrenDiv.style.display = '';
                    } else {
                        collapsed.add(p);
                        tog.textContent = '\u25B8';
                        childrenDiv.style.display = 'none';
                    }
                });
                hdr.appendChild(tog);
            } else {
                const sp = document.createElement('span');
                sp.className = 'tree-spacer';
                hdr.appendChild(sp);
            }

            // name
            const nm = document.createElement('span');
            nm.className = node.isIdleWait ? 'tree-name-idle' : 'tree-name';
            nm.textContent = node.name;
            nm.title = node.path;
            hdr.appendChild(nm);

            if (node.isIdleWait) {
                const it = document.createElement('span');
                it.className = 'tree-idle-tag';
                it.textContent = 'Idle';
                hdr.appendChild(it);
            }

            // pct
            const pc = document.createElement('span');
            pc.className = 'tree-pct';
            pc.style.color = pctColor;
            pc.textContent = f2(pctForDisplay) + '%';
            hdr.appendChild(pc);

            // ms
            const ms = document.createElement('span');
            ms.className = 'tree-ms';
            const msVal = semanticMode
                ? Number(node.avgCpuPerCycleMs || node.avgCpuMs || 0)
                : Number(node.avgCpuMs || 0);
            ms.textContent = semanticMode
                ? (f3(msVal) + 'ms/c')
                : (f3(msVal) + 'ms');
            hdr.appendChild(ms);

            // gpu
            if ((node.avgGpuMs||0) > 0.001) {
                const gp = document.createElement('span');
                gp.className = 'tree-gpu';
                gp.textContent = 'GPU:' + f3(node.avgGpuMs) + 'ms';
                hdr.appendChild(gp);
            }

            // calls
            if (!semanticMode && (node.callsPerFrame||0) > 1.5) {
                const cl = document.createElement('span');
                cl.className = 'tree-calls';
                cl.textContent = '\u00d7' + f2(node.callsPerFrame);
                hdr.appendChild(cl);
            }

            // self bar
            const sb = document.createElement('div');
            sb.className = 'tree-self-bar';
            const sf = document.createElement('div');
            sf.className = 'tree-self-bar-fill';
            sf.style.width = Math.min(100, selfPct) + '%';
            sb.appendChild(sf);
            hdr.appendChild(sb);

            div.appendChild(hdr);

            // children container
            childrenDiv = document.createElement('div');
            childrenDiv.className = 'tree-children';
            if (hasKids) node.children.forEach((c) => renderTreeNode(c, childrenDiv));
            div.appendChild(childrenDiv);

            container.appendChild(div);
        };

        const buildTreeFilter = () => {
            const q = treeState.filter.trim().toLowerCase();
            if (!q) return null;
            const vis = new Set();
            nodes.forEach((nd) => {
                const hay = (nd.name+' '+nd.path).toLowerCase();
                if (!hay.includes(q)) return;
                let cur = nd;
                while (cur) {
                    if (vis.has(cur.path)) break;
                    vis.add(cur.path);
                    cur = cur.parentPath && nodes.has(cur.parentPath) ? nodes.get(cur.parentPath) : null;
                }
            });
            return vis;
        };
        const applyTreeFilter = () => {
            const wl = buildTreeFilter();
            treeContainer.querySelectorAll('.tree-node').forEach((el) => {
                const p = el.dataset.path || '';
                el.style.display = (wl && !wl.has(p)) ? 'none' : '';
            });
        };
        const rerenderTree = () => {
            rebuildTreeModel();
            treeContainer.innerHTML = '';
            if (treeRoots.length === 0) {
                const empty = document.createElement('div');
                empty.className = 'hint';
                empty.textContent = '当前视图暂无可展示的节点数据。';
                treeContainer.appendChild(empty);
                return;
            }
            sortTree(treeRoots);
            treeRoots.forEach((r) => renderTreeNode(r, treeContainer));
            applyTreeFilter();
        };

        const buildRelationRowsFromSections = (sections) => {
            const map = new Map();
            sections.forEach((s) => {
                const path = s.path || s.name || '';
                if (!path) return;
                map.set(path, s);
            });
            const rows = [];
            map.forEach((child, childPath) => {
                const parentPath = child.parentPath || '';
                if (!parentPath || !map.has(parentPath)) return;
                const parent = map.get(parentPath);
                const childAvg = Number(child.avgCpuMs || 0);
                const parentAvg = Number(parent.avgCpuMs || 0);
                rows.push({
                    parentPath,
                    parentName: parent.name || parentPath,
                    childPath,
                    childName: child.name || childPath,
                    avgCpuMs: childAvg,
                    parentAvgCpuMs: parentAvg,
                    sharePct: (parentAvg > 1e-6) ? Math.max(0, Math.min(100, (childAvg / parentAvg) * 100)) : 0,
                    callsPerFrame: Number(child.callsPerFrame || 0)
                });
            });
            return rows;
        };

        const rawRelationRows = Array.isArray(data.sectionEdges) && data.sectionEdges.length > 0
            ? data.sectionEdges
            : buildRelationRowsFromSections(rawSections);
        const semanticRelationRows = buildRelationRowsFromSections(semanticSections);
        const getActiveRelationRows = () => {
            if (treeState.viewMode === 'semantic' && semanticRelationRows.length > 0) {
                return semanticRelationRows;
            }
            return rawRelationRows;
        };

        const relationState = { sortMode: 'avg', topN: 40, filter: '' };
        const relationTbody = document.querySelector('#relationTable tbody');
        const renderRelationTable = () => {
            if (!relationTbody) return;
            relationTbody.innerHTML = '';
            let rows = [...getActiveRelationRows()];
            const keyword = relationState.filter.trim().toLowerCase();
            if (keyword) {
                rows = rows.filter((r) => {
                    const hay = `${r.parentName || ''} ${r.parentPath || ''} ${r.childName || ''} ${r.childPath || ''}`.toLowerCase();
                    return hay.includes(keyword);
                });
            }
            if (relationState.sortMode === 'share') {
                rows.sort((a, b) => Number(b.sharePct || 0) - Number(a.sharePct || 0));
            } else if (relationState.sortMode === 'calls') {
                rows.sort((a, b) => Number(b.callsPerFrame || 0) - Number(a.callsPerFrame || 0));
            } else {
                rows.sort((a, b) => Number(b.avgCpuMs || 0) - Number(a.avgCpuMs || 0));
            }

            const top = Math.max(1, relationState.topN | 0);
            rows = rows.slice(0, top);
            if (rows.length === 0) {
                const tr = document.createElement('tr');
                tr.innerHTML = '<td colspan="5">当前条件下没有匹配的父子关系。</td>';
                relationTbody.appendChild(tr);
                return;
            }

            const maxAvg = Math.max(0.001, ...rows.map((r) => Number(r.avgCpuMs || 0)));
            rows.forEach((r) => {
                const avg = Number(r.avgCpuMs || 0);
                const share = Number(r.sharePct || 0);
                const calls = Number(r.callsPerFrame || 0);
                const width = Math.max(0, Math.min(100, (avg / maxAvg) * 100));
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td><span class="relation-parent" title="${r.parentPath || ''}">${r.parentName || ''}</span></td>
                    <td><span class="relation-child" title="${r.childPath || ''}">${r.childName || ''}</span></td>
                    <td>
                        <div class="relation-share">
                            <div class="bar" style="width:${width}%"></div>
                            <div class="bar-label">${f3(avg)}</div>
                        </div>
                    </td>
                    <td>${f2(calls)}</td>
                    <td>${f2(share)}%</td>
                `;
                relationTbody.appendChild(tr);
            });
        };

        document.getElementById('expandAll')?.addEventListener('click', () => {
            collapsed.clear();
            treeContainer.querySelectorAll('.tree-toggle').forEach(t => { t.textContent = '\u25BE'; });
            treeContainer.querySelectorAll('.tree-children').forEach(c => { c.style.display = ''; });
        });
        document.getElementById('collapseAll')?.addEventListener('click', () => {
            nodes.forEach((n, p) => { if (n.children.length > 0) collapsed.add(p); });
            treeContainer.querySelectorAll('.tree-toggle').forEach(t => { t.textContent = '\u25B8'; });
            treeContainer.querySelectorAll('.tree-children').forEach(c => {
                const nd = c.closest('.tree-node');
                if (nd && collapsed.has(nd.dataset.path)) c.style.display = 'none';
            });
        });
        document.getElementById('sortMode')?.addEventListener('change', function() {
            treeState.sortMode = this.value || 'self';
            rerenderTree();
        });
        document.getElementById('sectionFilter')?.addEventListener('input', function() {
            treeState.filter = this.value || '';
            applyTreeFilter();
        });
        document.getElementById('sectionViewMode')?.addEventListener('change', function() {
            const nextMode = (this.value === 'raw') ? 'raw' : 'semantic';
            treeState.viewMode = (nextMode === 'semantic' && semanticSections.length > 0) ? 'semantic' : 'raw';
            collapsed.clear();
            rerenderTree();
            renderRelationTable();
        });
        document.getElementById('relationSortMode')?.addEventListener('change', function() {
            relationState.sortMode = this.value || 'avg';
            renderRelationTable();
        });
        document.getElementById('relationTopN')?.addEventListener('change', function() {
            relationState.topN = Number(this.value || 40);
            renderRelationTable();
        });
        document.getElementById('relationFilter')?.addEventListener('input', function() {
            relationState.filter = this.value || '';
            renderRelationTable();
        });

        rerenderTree();
        renderRelationTable();
    </script>
</body>
</html>)";

  std::ofstream file(job.finalPath);
  if (file.is_open()) {
    file << html.str();
    file.close();
    war3dbg::Print("DXVK War3Perf: Report exported to %s\n",
                   job.finalPath.c_str());
  } else {
    war3dbg::Print("DXVK War3Perf: Failed to write report to %s\n",
                   job.finalPath.c_str());
  }
}

void War3PerfMonitor::cleanupTempFolder() {
  const std::string baseDir = getWarVkBaseDir();
  if (baseDir.empty())
    return;
  const std::string tempDir = baseDir + "Temp\\";

  WIN32_FIND_DATAA ffd = {};
  HANDLE hFind = FindFirstFileA((tempDir + "*").c_str(), &ffd);
  if (hFind == INVALID_HANDLE_VALUE)
    return;

  do {
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    const std::string filePath = tempDir + ffd.cFileName;
    DeleteFileA(filePath.c_str());
  } while (FindNextFileA(hFind, &ffd));
  FindClose(hFind);
}

} // namespace dxvk::war3
