#pragma once

#include "../../d3d9_war3_scene.h"
#include "war3_render_objects.h"

#include <cstdint>

namespace dxvk::war3::render {

struct ShadowRuntimeBridgeSummary {
  uint64_t modelRegistryCount = 0;
  uint64_t instanceRegistryCount = 0;
  uint64_t runtimeBoundCount = 0;
  uint64_t completeIdentityCount = 0;
  uint64_t poseReadyCount = 0;
  uint64_t spriteFramePoseCount = 0;
  uint64_t matrixPaletteCount = 0;
  uint64_t shadowRuntimeBoundCount = 0;
  uint64_t shadowIdentityCount = 0;
  uint64_t shadowPoseReadyCount = 0;
  uint64_t poseFrame = 0;
  bool runtimePoseHooksActive = false;
  bool runtimeChainWarm = false;
  bool runtimeChainNeedsRepair = false;
};

struct ShadowRuntimeBridgeTrackingDecision {
  bool wantsObjectIdentity = false;
  bool wantsFallbackBridge = false;
};

void NoteShadowRuntimeRenderObject(const RenderObjectInfo& info);
void NoteShadowRuntimeIdentity(void* worldObjectEntry, void* sceneNode,
                               void* unitPtr, void* spritePtr,
                               uint32_t jHandle, uint32_t rawcode,
                               ObjectKind kind);
void NoteShadowRuntimeModelBinding(void* spritePtr, void* runtimeModelPtr,
                                   void* modelResourcePtr,
                                   const std::string& modelPath,
                                   uint32_t modelType, uint32_t modelFlags,
                                   uint64_t modelKey);
void NoteShadowRuntimePose(void* runtimeModelPtr, void* sceneNode, void* unitPtr,
                           uint32_t sequenceId, float sequenceTime, float scale,
                           float yaw, float pitch, float roll, float height,
                           bool hasWorldTransform = false,
                           const Matrix4* worldTransform = nullptr,
                           uint32_t matrixCount = 0,
                           uint64_t matrixHash = 0);
void NoteShadowRuntimeSpriteFramePose(void* runtimeModelPtr, void* spritePtr,
                                      void* sceneNode, void* unitPtr, float dt,
                                      uint32_t sequenceId, float sequenceTime,
                                      float scale, float yaw, float pitch,
                                      float roll, float height,
                                      bool hasWorldTransform = false,
                                      const Matrix4* worldTransform = nullptr,
                                      uint32_t matrixCount = 0,
                                      uint64_t matrixHash = 0);

ShadowRuntimeBridgeSummary QueryShadowRuntimeBridgeSummary();
ShadowRuntimeBridgeTrackingDecision ComputeShadowRuntimeBridgeTracking();
void ResetShadowRuntimeBridgeState();

bool AugmentShadowSemanticContext(dxvk::War3ShadowSemanticContext& semantic,
                                  const RenderObjectInfo* currentObj);

} // namespace dxvk::war3::render
