#include "war3_shadow_runtime_bridge.h"

#include "../model/war3_model_hook.h"
#include "../model/war3_model_registry.h"
#include "../core/war3_internal_test_config.h"
#include "war3_shadow_object_registry.h"
#include "../state/war3_render_state.h"

#include <atomic>

namespace dxvk::war3::render {

namespace {

std::atomic<uint64_t> g_shadowBridgeRepairUntilFrame{0u};
std::atomic<uint64_t> g_shadowBridgeRepairCooldownUntilFrame{0u};

constexpr uint64_t kShadowBridgeRepairBurstFrames = 24u;
constexpr uint64_t kShadowBridgeRepairCooldownFrames = 120u;

uint64_t CurrentRuntimeBridgeFrame() {
  if (model::IsPoseHookEnabled())
    return model::PoseRegistry::instance().frameNumber();
  return model::ModelInstanceRegistry::instance().frameNumber();
}

void RequestShadowBridgeRepairBurst(uint64_t nowFrame) {
  if (nowFrame == 0u)
    return;

  const uint64_t activeUntil =
      g_shadowBridgeRepairUntilFrame.load(std::memory_order_relaxed);
  if (nowFrame <= activeUntil)
    return;

  const uint64_t cooldownUntil =
      g_shadowBridgeRepairCooldownUntilFrame.load(std::memory_order_relaxed);
  if (nowFrame < cooldownUntil)
    return;

  g_shadowBridgeRepairCooldownUntilFrame.store(
      nowFrame + kShadowBridgeRepairCooldownFrames,
      std::memory_order_relaxed);
  g_shadowBridgeRepairUntilFrame.store(nowFrame + kShadowBridgeRepairBurstFrames,
                                       std::memory_order_relaxed);
}

bool IsRegistryFrameFresh(uint64_t sampleFrame, uint64_t registryFrame) {
  return sampleFrame != 0 && sampleFrame + 1 >= registryFrame;
}

void MaybeApplyPoseSnapshot(dxvk::War3ShadowSemanticContext& semantic,
                            bool hasTransform, const Matrix4& transform,
                            float scale, float height, bool fromSpriteFrame,
                            uint64_t sampleFrame, uint64_t& bestFrame) {
  if (!hasTransform || sampleFrame == 0 || sampleFrame < bestFrame)
    return;
  if (sampleFrame == bestFrame && !fromSpriteFrame &&
      semantic.hasPoseTransform && semantic.poseFromSpriteFrame) {
    // 对飞行单位/挂点模型，sprite-frame pose 通常比 root pose 更接近
    // 本帧最终渲染姿态。相同 frame 的情况下不允许 root pose 把它覆盖掉。
    return;
  }

  semantic.hasPoseTransform = true;
  semantic.poseFromSpriteFrame = fromSpriteFrame;
  semantic.poseTransform = transform;
  semantic.poseScale = scale;
  semantic.poseHeight = height;
  bestFrame = sampleFrame;
}

void MaybeApplyPoseMatrices(dxvk::War3ShadowSemanticContext& semantic,
                            uint32_t matrixCount, uint64_t matrixHash,
                            uint64_t sampleFrame, uint64_t& bestFrame) {
  if (matrixCount == 0 || sampleFrame == 0 || sampleFrame < bestFrame)
    return;

  semantic.poseMatrixCount = matrixCount;
  semantic.poseMatrixHash = matrixHash;
  bestFrame = sampleFrame;
}

void MergeRenderObject(dxvk::War3ShadowSemanticContext& semantic,
                       const RenderObjectInfo* object) {
  if (object == nullptr)
    return;

  if (semantic.object == nullptr)
    semantic.object = object;
  if (semantic.sceneNode == nullptr)
    semantic.sceneNode = object->sceneNode;
  if (semantic.worldObjectEntry == nullptr)
    semantic.worldObjectEntry = object->worldObjectEntry;
  if (semantic.jHandle == 0u)
    semantic.jHandle = object->jHandle;
  if (semantic.rawcode == 0u)
    semantic.rawcode = object->rawcode;
  if (static_cast<uint32_t>(semantic.objectKind) == 0u)
    semantic.objectKind = object->kind;
}

} // namespace

void NoteShadowRuntimeRenderObject(const RenderObjectInfo& info) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::ModelInstanceRegistry::instance().noteRenderObject(info);
  ShadowObjectRegistry::instance().noteRenderObject(info);
}

void NoteShadowRuntimeIdentity(void* worldObjectEntry, void* sceneNode,
                               void* unitPtr, void* spritePtr,
                               uint32_t jHandle, uint32_t rawcode,
                               ObjectKind kind) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::ModelInstanceRegistry::instance().noteInstanceIdentity(
      worldObjectEntry, sceneNode, unitPtr, spritePtr, jHandle, rawcode);
  ShadowObjectRegistry::instance().noteInstanceIdentity(
      worldObjectEntry, sceneNode, unitPtr, spritePtr, jHandle, rawcode, kind);
}

void NoteShadowRuntimeModelBinding(void* spritePtr, void* runtimeModelPtr,
                                   void* modelResourcePtr,
                                   const std::string& modelPath,
                                   uint32_t modelType, uint32_t modelFlags,
                                   uint64_t modelKey) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::ModelInstanceRegistry::instance().bindRuntimeModelToSprite(
      spritePtr, runtimeModelPtr, modelKey, modelResourcePtr);
  ShadowObjectRegistry::instance().noteModelBinding(
      spritePtr, runtimeModelPtr, modelResourcePtr, modelPath, modelType,
      modelFlags, modelKey);
}

void NoteShadowRuntimePose(void* runtimeModelPtr, void* sceneNode, void* unitPtr,
                           uint32_t sequenceId, float sequenceTime, float scale,
                           float yaw, float pitch, float roll, float height,
                           bool hasWorldTransform,
                           const Matrix4* worldTransform, uint32_t matrixCount,
                           uint64_t matrixHash) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::PoseRegistry::instance().recordPose(
      runtimeModelPtr, sceneNode, unitPtr, sequenceId, sequenceTime, scale, yaw,
      pitch, roll, height, hasWorldTransform, worldTransform);
  ShadowObjectRegistry::instance().notePose(
      runtimeModelPtr, sceneNode, unitPtr, sequenceId, sequenceTime, scale, yaw,
      pitch, roll, height, hasWorldTransform, worldTransform, matrixCount,
      matrixHash);
}

void NoteShadowRuntimeSpriteFramePose(void* runtimeModelPtr, void* spritePtr,
                                      void* sceneNode, void* unitPtr, float dt,
                                      uint32_t sequenceId, float sequenceTime,
                                      float scale, float yaw, float pitch,
                                      float roll, float height,
                                      bool hasWorldTransform,
                                      const Matrix4* worldTransform,
                                      uint32_t matrixCount,
                                      uint64_t matrixHash) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::PoseRegistry::instance().recordSpriteFramePose(
      runtimeModelPtr, spritePtr, sceneNode, unitPtr, dt, sequenceId,
      sequenceTime, scale, yaw, pitch, roll, height, hasWorldTransform,
      worldTransform);
  ShadowObjectRegistry::instance().noteSpriteFramePose(
      runtimeModelPtr, spritePtr, sceneNode, unitPtr, dt, sequenceId,
      sequenceTime, scale, yaw, pitch, roll, height, hasWorldTransform,
      worldTransform, matrixCount, matrixHash);
}

ShadowRuntimeBridgeSummary QueryShadowRuntimeBridgeSummary() {
  ShadowRuntimeBridgeSummary summary = {};
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return summary;
  summary.runtimePoseHooksActive = model::IsPoseHookEnabled();
  summary.modelRegistryCount = model::ModelRegistry::instance().recordCount();
  summary.instanceRegistryCount =
      model::ModelInstanceRegistry::instance().recordCount();
  summary.runtimeBoundCount =
      model::ModelInstanceRegistry::instance().runtimeBoundCount();
  summary.completeIdentityCount =
      model::ModelInstanceRegistry::instance().completeIdentityCount();
  summary.poseReadyCount = model::PoseRegistry::instance().readyPoseCount();
  summary.spriteFramePoseCount =
      model::PoseRegistry::instance().spriteFramePoseCount();
  summary.matrixPaletteCount =
      model::PoseRegistry::instance().matrixPaletteCount();
  summary.shadowRuntimeBoundCount =
      ShadowObjectRegistry::instance().runtimeBoundCount();
  summary.shadowIdentityCount =
      ShadowObjectRegistry::instance().completeIdentityCount();
  summary.shadowPoseReadyCount =
      ShadowObjectRegistry::instance().poseReadyCount();
  summary.poseFrame = summary.runtimePoseHooksActive
                          ? model::PoseRegistry::instance().frameNumber()
                          : model::ModelInstanceRegistry::instance().frameNumber();

  summary.runtimeChainWarm =
      summary.modelRegistryCount >= 16 &&
      summary.runtimeBoundCount >= 16 &&
      summary.completeIdentityCount + 4 >= summary.runtimeBoundCount &&
      (!summary.runtimePoseHooksActive ||
       (summary.poseReadyCount >= 16 &&
        (summary.spriteFramePoseCount + summary.matrixPaletteCount) >= 8));

  const uint64_t identityGap =
      summary.runtimeBoundCount > summary.completeIdentityCount
          ? (summary.runtimeBoundCount - summary.completeIdentityCount)
          : 0u;
  const uint64_t runtimeBindGap =
      summary.modelRegistryCount > summary.runtimeBoundCount
          ? (summary.modelRegistryCount - summary.runtimeBoundCount)
          : 0u;
  const uint64_t poseGap =
      summary.runtimeBoundCount > summary.poseReadyCount
          ? (summary.runtimeBoundCount - summary.poseReadyCount)
          : 0u;

  const bool identityNeedsRepair =
      summary.runtimeBoundCount >= 24u && identityGap > 12u &&
      identityGap * 4u > summary.runtimeBoundCount;
  const bool runtimeBindNeedsRepair =
      summary.modelRegistryCount >= 32u && runtimeBindGap > 8u &&
      runtimeBindGap * 2u > summary.modelRegistryCount;
  const bool poseNeedsRepair =
      summary.runtimePoseHooksActive && summary.runtimeBoundCount >= 24u &&
      ((poseGap > 12u && poseGap * 4u > summary.runtimeBoundCount) ||
       (summary.spriteFramePoseCount + summary.matrixPaletteCount) == 0u);

  summary.runtimeChainNeedsRepair =
      identityNeedsRepair || runtimeBindNeedsRepair || poseNeedsRepair;
  return summary;
}

ShadowRuntimeBridgeTrackingDecision ComputeShadowRuntimeBridgeTracking() {
  ShadowRuntimeBridgeTrackingDecision decision = {};
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return decision;
  const auto summary = QueryShadowRuntimeBridgeSummary();
  const uint64_t refreshPeriod = summary.runtimePoseHooksActive ? 240u : 300u;
  const uint64_t warmupFrames = summary.runtimePoseHooksActive ? 60u : 24u;
  const bool repairBurstActive =
      summary.poseFrame != 0u &&
      summary.poseFrame <=
          g_shadowBridgeRepairUntilFrame.load(std::memory_order_relaxed);
  const bool shouldRefreshIdentity =
      summary.poseFrame < warmupFrames ||
      ((summary.poseFrame % refreshPeriod) == 0u &&
       summary.poseFrame >= warmupFrames) ||
      repairBurstActive ||
      summary.runtimeChainNeedsRepair;

  decision.wantsObjectIdentity =
      shouldRefreshIdentity || !summary.runtimeChainWarm;
  decision.wantsFallbackBridge =
      !summary.runtimeChainWarm || summary.poseFrame < warmupFrames ||
      repairBurstActive ||
      summary.runtimeChainNeedsRepair;
  return decision;
}

void ResetShadowRuntimeBridgeState() {
  g_shadowBridgeRepairUntilFrame.store(0u, std::memory_order_relaxed);
  g_shadowBridgeRepairCooldownUntilFrame.store(0u,
                                               std::memory_order_relaxed);
}

bool AugmentShadowSemanticContext(dxvk::War3ShadowSemanticContext& semantic,
                                  const RenderObjectInfo* currentObj) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return false;
  const void* currentUnitPtr = currentObj != nullptr ? currentObj->unitPtr : nullptr;
  uint64_t bestPoseFrame = 0;
  uint64_t bestMatrixFrame = 0;

  const bool runtimePoseHooksActive = model::IsPoseHookEnabled();
  const bool needsRuntimePoseAugment =
      runtimePoseHooksActive &&
      (currentUnitPtr != nullptr ||
       semantic.objectKind == ObjectKind::Unit ||
       (currentObj != nullptr && currentObj->kind == ObjectKind::Unit) ||
       semantic.runtimeModelPtr != nullptr);
  const bool needsStableIdentityAugment =
      semantic.object == nullptr || semantic.sceneNode == nullptr ||
      semantic.worldObjectEntry == nullptr || semantic.jHandle == 0u ||
      semantic.rawcode == 0u || semantic.objectKind == ObjectKind::Unknown;
  const bool needsRuntimeRecovery =
      needsStableIdentityAugment &&
      (semantic.sceneNode != nullptr || semantic.worldObjectEntry != nullptr ||
       semantic.jHandle != 0u || currentUnitPtr != nullptr);
  const bool allowRuntimeRegistryLookup =
      needsRuntimePoseAugment || needsStableIdentityAugment ||
      needsRuntimeRecovery;

  if (allowRuntimeRegistryLookup) {
    model::ModelInstanceRecord instanceRecord = {};
    bool instanceRecordHit = false;
    auto& instanceRegistry = model::ModelInstanceRegistry::instance();
    if (semantic.sceneNode != nullptr)
      instanceRecordHit =
          instanceRegistry.findBySceneNode(semantic.sceneNode, instanceRecord);
    if (!instanceRecordHit && semantic.object != nullptr &&
        semantic.object->unitPtr != nullptr) {
      instanceRecordHit =
          instanceRegistry.findByUnitPtr(semantic.object->unitPtr, instanceRecord);
    }
    if (!instanceRecordHit && currentUnitPtr != nullptr)
      instanceRecordHit = instanceRegistry.findByUnitPtr(
          const_cast<void*>(currentUnitPtr), instanceRecord);
    if (!instanceRecordHit && semantic.jHandle != 0u)
      instanceRecordHit =
          instanceRegistry.findByHandle(semantic.jHandle, instanceRecord);

    if (instanceRecordHit) {
      if (semantic.sceneNode == nullptr)
        semantic.sceneNode = instanceRecord.sceneNode;
      if (semantic.worldObjectEntry == nullptr)
        semantic.worldObjectEntry = instanceRecord.worldObjectEntry;
      if (semantic.runtimeModelPtr == nullptr)
        semantic.runtimeModelPtr = instanceRecord.runtimeModelPtr;
      if (semantic.modelResourcePtr == nullptr)
        semantic.modelResourcePtr = instanceRecord.modelResourcePtr;
      if (semantic.modelKey == 0u)
        semantic.modelKey = instanceRecord.modelKey;
      if (semantic.jHandle == 0u)
        semantic.jHandle = instanceRecord.jHandle;
      if (semantic.rawcode == 0u)
        semantic.rawcode = instanceRecord.rawcode;
    }

    const bool needsShadowRegistryRecovery =
        needsRuntimePoseAugment || semantic.sceneNode == nullptr ||
        semantic.worldObjectEntry == nullptr || semantic.jHandle == 0u ||
        semantic.rawcode == 0u || semantic.objectKind == ObjectKind::Unknown;
    if (needsShadowRegistryRecovery) {
      ShadowObjectRecord shadowRecord = {};
      bool shadowRecordHit = false;
      auto& shadowRegistry = ShadowObjectRegistry::instance();
      const uint64_t shadowRegistryFrame = shadowRegistry.frameNumber();
      if (semantic.sceneNode != nullptr)
        shadowRecordHit =
            shadowRegistry.findBySceneNode(semantic.sceneNode, shadowRecord);
      if (!shadowRecordHit && semantic.object != nullptr &&
          semantic.object->unitPtr != nullptr) {
        shadowRecordHit =
            shadowRegistry.findByUnitPtr(semantic.object->unitPtr, shadowRecord);
      }
      if (!shadowRecordHit && currentObj != nullptr && currentObj->unitPtr != nullptr)
        shadowRecordHit =
            shadowRegistry.findByUnitPtr(currentObj->unitPtr, shadowRecord);
      if (!shadowRecordHit && semantic.jHandle != 0u)
        shadowRecordHit =
            shadowRegistry.findByHandle(semantic.jHandle, shadowRecord);
      if (!shadowRecordHit && semantic.runtimeModelPtr != nullptr)
        shadowRecordHit =
            shadowRegistry.findByRuntimeModel(semantic.runtimeModelPtr, shadowRecord);

      if (shadowRecordHit) {
        if (semantic.sceneNode == nullptr)
          semantic.sceneNode = shadowRecord.sceneNode;
        if (semantic.worldObjectEntry == nullptr)
          semantic.worldObjectEntry = shadowRecord.worldObjectEntry;
        if (semantic.runtimeModelPtr == nullptr)
          semantic.runtimeModelPtr = shadowRecord.runtimeModelPtr;
        if (semantic.modelResourcePtr == nullptr)
          semantic.modelResourcePtr = shadowRecord.modelResourcePtr;
        if (semantic.jHandle == 0u)
          semantic.jHandle = shadowRecord.jHandle;
        if (semantic.rawcode == 0u)
          semantic.rawcode = shadowRecord.rawcode;
        if (semantic.modelKey == 0u)
          semantic.modelKey = shadowRecord.modelKey;
        if (semantic.objectKind == ObjectKind::Unknown)
          semantic.objectKind = shadowRecord.kind;
        if (IsRegistryFrameFresh(shadowRecord.lastSpriteFramePoseFrame,
                                 shadowRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, shadowRecord.hasSpriteFrameTransform,
                                 shadowRecord.spriteFrameTransform,
                                 shadowRecord.scale, shadowRecord.height, true,
                                 shadowRecord.lastSpriteFramePoseFrame,
                                 bestPoseFrame);
        if (IsRegistryFrameFresh(shadowRecord.lastRootPoseFrame,
                                 shadowRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, shadowRecord.hasWorldTransform,
                                 shadowRecord.worldTransform,
                                 shadowRecord.scale, shadowRecord.height, false,
                                 shadowRecord.lastRootPoseFrame, bestPoseFrame);
        if (IsRegistryFrameFresh(shadowRecord.lastMatrixPaletteFrame,
                                 shadowRegistryFrame))
          MaybeApplyPoseMatrices(semantic, shadowRecord.matrixCount,
                                 shadowRecord.matrixHash,
                                 shadowRecord.lastMatrixPaletteFrame,
                                 bestMatrixFrame);
      }
    }

    if (needsRuntimePoseAugment) {
      model::PoseRecord poseRecord = {};
      bool poseRecordHit = false;
      auto& poseRegistry = model::PoseRegistry::instance();
      const uint64_t poseRegistryFrame = poseRegistry.frameNumber();
      if (semantic.runtimeModelPtr != nullptr)
        poseRecordHit =
            poseRegistry.findByRuntimeModel(semantic.runtimeModelPtr, poseRecord);
      if (!poseRecordHit && semantic.sceneNode != nullptr)
        poseRecordHit = poseRegistry.findBySceneNode(semantic.sceneNode, poseRecord);
      if (!poseRecordHit && semantic.object != nullptr &&
          semantic.object->unitPtr != nullptr) {
        poseRecordHit =
            poseRegistry.findByUnitPtr(semantic.object->unitPtr, poseRecord);
      }
      if (!poseRecordHit && currentUnitPtr != nullptr)
        poseRecordHit = poseRegistry.findByUnitPtr(
            const_cast<void*>(currentUnitPtr), poseRecord);

      if (poseRecordHit) {
        if (IsRegistryFrameFresh(poseRecord.lastSpriteFramePoseFrame,
                                 poseRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, poseRecord.hasSpriteFrameTransform,
                                 poseRecord.spriteFrameTransform,
                                 poseRecord.scale, poseRecord.height, true,
                                 poseRecord.lastSpriteFramePoseFrame,
                                 bestPoseFrame);
        if (IsRegistryFrameFresh(poseRecord.lastRootPoseFrame, poseRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, poseRecord.hasWorldTransform,
                                 poseRecord.worldTransform, poseRecord.scale,
                                 poseRecord.height, false,
                                 poseRecord.lastRootPoseFrame, bestPoseFrame);
        if (IsRegistryFrameFresh(poseRecord.lastMatrixPaletteFrame,
                                 poseRegistryFrame))
          MaybeApplyPoseMatrices(semantic, poseRecord.matrixCount,
                                 poseRecord.matrixHash,
                                 poseRecord.lastMatrixPaletteFrame,
                                 bestMatrixFrame);
      }
    }
  }

  if (semantic.object == nullptr) {
    auto& registry = RenderObjectRegistry::instance();
    const RenderObjectInfo* object = nullptr;
    if (semantic.sceneNode != nullptr)
      object = registry.findBySceneNode(semantic.sceneNode);
    if (object == nullptr && semantic.worldObjectEntry != nullptr)
      object = registry.findByEntry(semantic.worldObjectEntry);
    if (object == nullptr && semantic.jHandle != 0u)
      object = registry.findByHandle(semantic.jHandle);
    MergeRenderObject(semantic, object);
  }

  const bool hasMeaningfulContext =
      semantic.sceneNode != nullptr || semantic.worldObjectEntry != nullptr ||
      semantic.jHandle != 0u || currentUnitPtr != nullptr ||
      semantic.runtimeModelPtr != nullptr || semantic.object != nullptr;
  const bool bridgeReady = semantic.HasStableIdentity() || semantic.hasPoseTransform ||
                           semantic.poseMatrixCount != 0u;

  // 某个对象第一次进入视野时，如果这帧仍拿不到稳定身份/姿态，
  // 不要等到全局周期刷新再补票。这里对“当前缺失对象”
  // 触发一个很短的 repair burst，但带冷却，避免在超大地图上
  // 长时间把全量对象身份桥常驻打开。
  if (!bridgeReady && hasMeaningfulContext) {
    const uint64_t nowFrame = CurrentRuntimeBridgeFrame();
    RequestShadowBridgeRepairBurst(nowFrame);
  }

  return bridgeReady;
}

} // namespace dxvk::war3::render
