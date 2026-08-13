#pragma once

#include "war3_render_objects.h"
#include "war3_render_state.h"

#include <cstdint>
#include <limits>

namespace dxvk::war3::render {

enum class ShadowCapturePriority : uint8_t {
  Critical = 0,
  High = 1,
  Medium = 2,
  Low = 3,
};

struct ShadowCaptureCandidateInfo {
  War3RenderState::StageCategory category = War3RenderState::StageCategory::Unknown;
  ObjectKind objectKind = ObjectKind::Unknown;
  bool terrainTileCaster = false;
  bool terrainDoodadCaster = false;
  bool terrainS1Caster = false;
  bool terrainDecorationLike = false;
  bool alphaBlend = false;
  bool alphaTestEnabled = false;
  bool depthWriteEnabled = false;
  bool vertexBlendEnabled = false;
  bool indexed = false;
  uint32_t stage = 0;
  uint32_t vertexCount = 0;
  uint32_t indexCount = 0;
  float viewDepth = 0.0f;
  float shadowMaxDistance = 0.0f;
  float boundsRadius = 0.0f;
};

struct ShadowCaptureBudgetPolicy {
  uint64_t hardBudgetBytes = 0;
  uint64_t usedBudgetBytes = 0;
  uint64_t posBytes = 0;
  uint64_t blendBytes = 0;
  uint64_t uvBytes = 0;
  uint64_t indexBytes = 0;
  bool freezeDynamicEnabled = true;
  bool aggressiveExperimental = false;
  // A required directional caster may only be rejected by the real hard
  // capacity. Soft priority thresholds are useful for optional work, but
  // omitting one required caster invalidates the entire atomic CSM candidate.
  bool requiredDirectionalCaster = false;
};

inline bool RequiredShadowCasterFitsHardBudget(
    const ShadowCaptureBudgetPolicy& policy) noexcept {
  uint64_t totalBytes = 0u;
  const uint64_t parts[] = {
      policy.posBytes, policy.blendBytes, policy.uvBytes, policy.indexBytes};
  for (const uint64_t bytes : parts) {
    if (bytes > std::numeric_limits<uint64_t>::max() - totalBytes)
      return false;
    totalBytes += bytes;
  }
  return policy.usedBudgetBytes <= policy.hardBudgetBytes &&
      totalBytes <= policy.hardBudgetBytes - policy.usedBudgetBytes;
}

struct ShadowCaptureBudgetDecision {
  ShadowCapturePriority priority = ShadowCapturePriority::Medium;
  bool skipCaster = false;
  bool disableAlphaCapture = false;
  const char* reason = "allow";
};

struct ShadowArenaDispatchTelemetry {
  uint32_t dispatchClaims = 0;
  uint32_t dispatchClaimMisses = 0;
};

ShadowCapturePriority ComputeShadowCapturePriority(
    const ShadowCaptureCandidateInfo& candidate);

const char* ShadowCapturePriorityName(ShadowCapturePriority priority);

ShadowCaptureBudgetDecision DecideShadowCaptureBudget(
    const ShadowCaptureCandidateInfo& candidate,
    const ShadowCaptureBudgetPolicy& policy);

bool IsShadowArenaCaptureEnabled();
void BeginShadowArenaCaptureFrame();
void RegisterShadowArenaBatchRange(const void* batchArrayBase,
                                   uint32_t before,
                                   uint32_t after);
bool TryClaimShadowArenaDispatch(void* renderablePart, War3BatchTag tag,
                                 int stage,
                                 int kind);
ShadowArenaDispatchTelemetry GetShadowArenaDispatchTelemetry();

} // namespace dxvk::war3::render
