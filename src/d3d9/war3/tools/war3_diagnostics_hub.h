#pragma once

#include <cstdint>
#include <string>

namespace dxvk::war3::tools {

struct War3RuntimeStatusModuleSnapshot {
  uint32_t registered = 0;
  uint32_t loaded = 0;
  uint64_t dispatchCalls = 0;
  uint64_t handlers = 0;
  uint64_t callbackErrors = 0;
  std::string state;
};

struct War3RuntimeStatusPerfSnapshot {
  bool enabled = false;
  bool recording = false;
};

struct War3RuntimeStatusProfileSnapshot {
  std::string name;
  std::string disabledModules;
  std::string enabledModules;
};

struct War3RuntimeStatusRuntimeSnapshot {
  bool runtimeReady = false;
  bool jassReady = false;
  bool gameStarted = false;
};

struct War3RuntimeStatusRenderSnapshot {
  bool inGameRenderReady = false;
  bool isInGame = false;
  bool isLoading = false;
  uint64_t worldPtr = 0;
};

struct War3RuntimeStatusFrameSnapshot {
  uint64_t frameNumber = 0;
  uint64_t publishRevision = 0;
  uint64_t visibleCount = 0;
  uint64_t mainQueueCount = 0;
  uint64_t transparentCount = 0;
  uint64_t recordsWithStableIdentity = 0;
  uint64_t recordsWithResolvedGeoset = 0;
  uint64_t recordsWithRuntimeModel = 0;
  uint64_t recordsWithModelResource = 0;
  uint64_t unitCount = 0;
  uint64_t buildingCount = 0;
  uint64_t destructibleCount = 0;
  uint64_t unitWithResolvedGeoset = 0;
  uint64_t buildingWithResolvedGeoset = 0;
  uint64_t destructibleWithResolvedGeoset = 0;
  uint64_t unitWithMeshData = 0;
  uint64_t buildingWithMeshData = 0;
  uint64_t destructibleWithMeshData = 0;
  uint64_t unitWithModelResource = 0;
  uint64_t buildingWithModelResource = 0;
  uint64_t destructibleWithModelResource = 0;
  uint64_t sampleUnitSceneNode = 0;
  uint64_t sampleUnitWorldObjectEntry = 0;
  uint64_t sampleUnitUnitPtr = 0;
  uint64_t sampleUnitMeshData = 0;
  uint64_t sampleUnitRuntimeModel = 0;
  uint64_t sampleUnitModelResource = 0;
  uint64_t sampleUnitPoseCtx = 0;
  uint64_t sampleUnitPoseCtxRuntimeCandidate = 0;
  uint64_t sampleUnitSceneNodeRuntimeCandidate = 0;
  uint64_t sampleUnitWorldObjectEntryRuntimeCandidate = 0;
  uint32_t sampleUnitJHandle = 0;
  uint32_t sampleUnitRawcode = 0;
  uint32_t sampleUnitMeshIndex = 0xFFFFFFFFu;
  uint32_t sampleUnitGeosetIndex = 0xFFFFFFFFu;
  uint32_t sampleUnitPoseCtxRuntimeOffset = 0xFFFFFFFFu;
  uint32_t sampleUnitSceneNodeRuntimeOffset = 0xFFFFFFFFu;
  uint32_t sampleUnitWorldObjectEntryRuntimeOffset = 0xFFFFFFFFu;
  uint32_t sampleUnitGeosetVertexCount = 0;
  uint32_t sampleUnitGeosetPrimitiveCount = 0;
  uint32_t sampleUnitGeosetMatrixGroupCount = 0;
  uint32_t sampleUnitGeosetMatrixIndexCount = 0;
  bool sampleUnitMeshIndexReadable = false;
  bool sampleUnitMeshDataLooksLikeGeosetData = false;
  uint64_t itemCount = 0;
  uint64_t effectCount = 0;
  uint64_t unknownCount = 0;
};

struct War3RuntimeStatusShadowSnapshot {
  uint64_t matrixPaletteCount = 0;
  uint64_t shadowReadyGeosetCount = 0;
  uint64_t shadowModelResourceCount = 0;
  uint64_t shadowRuntimeModelCount = 0;
  uint64_t upperLayerResolveAuthoritativeRigid = 0;
  uint64_t upperLayerResolveAuthoritativeSkinned = 0;
  uint64_t upperLayerResolvedAuthoritativeItems = 0;
  uint64_t upperLayerEmitted = 0;
  uint64_t semanticCoreFrameSerial = 0;
  uint64_t semanticCoreResolved = 0;
  uint64_t semanticCoreSkinnedResolved = 0;
  uint64_t semanticCoreExplicitResourceOwnerRigidResolved = 0;
  uint64_t semanticCoreExplicitResourceOwnerRigidWorldTransformResolved = 0;
  uint64_t semanticCoreExplicitResourceOwnerRigidNoMatrixPalette = 0;
  uint64_t semanticCoreSubmittedDrawCount = 0;
  uint64_t semanticCoreSkippedNoRuntimeGroupPalette = 0;
  uint64_t fallbackDrawCount = 0;
  uint64_t fallbackDrawCountTerrain = 0;
  uint64_t fallbackDrawCountWorldObject = 0;
  uint64_t fallbackDrawCountUnitObject = 0;
  uint64_t objectFallbackDrawCount = 0;
  uint64_t semanticSceneSubmitted = 0;
  uint64_t semanticSceneSubmittedUnit = 0;
  uint64_t semanticSceneSubmittedSkinned = 0;
  uint64_t semanticSceneSubmittedFrameLocal = 0;
  uint64_t semanticSceneSubmittedPersistent = 0;
  uint64_t semanticSceneStatsPublishCount = 0;
  uint64_t semanticSceneLastFrameSerial = 0;
  uint64_t semanticSceneLastSelectedFrameSerial = 0;
  uint64_t semanticSceneLastReusableFrameSerial = 0;
  uint64_t semanticSceneLastSourcePublishRevision = 0;
  uint64_t semanticSceneLastTargetPublishRevision = 0;
  uint64_t semanticSceneLastInputDrawCount = 0;
  uint64_t semanticSceneLastSubmittedDrawCount = 0;
  uint64_t semanticSceneSelectedFrameEligibleZeroCount = 0;
  uint64_t semanticSceneReusableFrameForcedCount = 0;
  uint64_t semanticSceneReusableFrameUnavailableCount = 0;
  uint64_t semanticSceneReusableFrameRejectedNativeValidationCount = 0;
  uint64_t semanticScenePublishRevisionLag = 0;
  uint64_t semanticFallbackPruned = 0;
  bool semanticCoreFrameFresh = false;
  bool semanticCoreBuildInProgress = false;
  bool semanticCoreBuildRequestPending = false;
  uint64_t semanticCoreBuildCurrentRecordIndex = 0;
  uint64_t semanticCoreBuildRecordCount = 0;
  uint64_t semanticCoreBuildChunkCount = 0;
  uint64_t semanticStaticCandidateCount = 0;
  uint64_t semanticStaticCandidateBuildingCount = 0;
  uint64_t semanticStaticCandidateDestructibleCount = 0;
  uint64_t semanticStaticCandidateMaybeDoodadOrEffectCount = 0;
  uint64_t semanticStaticCandidateWithStableIdentity = 0;
  uint64_t semanticStaticCandidateWithMeshData = 0;
  uint64_t semanticStaticCandidateWithRuntimeModel = 0;
  uint64_t semanticStaticCandidateWithModelResource = 0;
  uint64_t semanticStaticCandidateWithResolvedGeoset = 0;
  uint64_t semanticStaticCandidateRejectedUnitsOnlyFilter = 0;
  uint64_t semanticStaticCandidateRejectedNoIdentity = 0;
  uint64_t semanticStaticCandidateRejectedNoMeshData = 0;
  uint64_t semanticStaticCandidateRejectedNoResource = 0;
  uint64_t semanticStaticCandidateRejectedNoGeoset = 0;
  uint64_t semanticStaticCandidateRejectedNonCanonicalKind = 0;
};

struct War3RuntimeStatusSnapshot {
  uint64_t timestampMs = 0;
  std::string source;
  uint64_t frameIndex = 0;
  War3RuntimeStatusModuleSnapshot module = {};
  War3RuntimeStatusPerfSnapshot perf = {};
  War3RuntimeStatusProfileSnapshot profile = {};
  War3RuntimeStatusRuntimeSnapshot runtime = {};
  War3RuntimeStatusRenderSnapshot render = {};
  War3RuntimeStatusFrameSnapshot frame = {};
  War3RuntimeStatusShadowSnapshot shadow = {};
};

/**
 * @brief 记录运行时初始化总览（仅首次打印）。
 *
 * 输出内容包含：
 * - 模块系统统计；
 * - PerfMonitor 开关状态。
 *
 * @param source 触发来源（例如 `ActivateWar3Runtime`）。
 */
void LogRuntimeSummaryOnce(const char* source);

/**
 * @brief 低频记录运行时健康状态。
 *
 * @param frameIndex 当前帧号。
 * @param interval 每隔多少帧输出一次。
 */
void LogRuntimeHealthPeriodic(uint64_t frameIndex, uint32_t interval = 1200);

/**
 * @brief 立即写出 runtime 状态快照到 `WarVK/Temp/runtime_status.json`。
 *
 * 用于外部自动化（MCP）按文件轮询“运行时就绪/进图状态”。
 *
 * @param source 来源标签（例如 `War3Events/OnGameStart`）。
 * @param frameIndex 当前帧号（未知时传 0）。
 */
void ExportRuntimeStatusSnapshot(const char* source, uint64_t frameIndex = 0);

/**
 * @brief 查询当前运行时状态快照。
 *
 * 该接口是 control plane 与自动化的统一状态来源。
 */
War3RuntimeStatusSnapshot QueryRuntimeStatusSnapshot(
    const char* source = nullptr, uint64_t frameIndex = 0);

/**
 * @brief 标记“已看到正式进游戏渲染信号”。
 *
 * 用于修正仅靠 NetEvent runtimeReady 不足以覆盖的场景。
 */
void MarkInGameRenderReady(const char* source, uint64_t frameIndex = 0);

/**
 * @brief 查询“已看到正式进游戏渲染信号”标记。
 */
bool IsInGameRenderReady();

/**
 * @brief 重置运行态就绪补充信号。
 */
void ResetRuntimeReadySignals();

} // namespace dxvk::war3::tools
