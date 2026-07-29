#pragma once

#include "war3_render_state.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace dxvk::war3::render {

// Every producer which can publish data consumed by the shadow pipeline must
// identify itself here. This policy deliberately does not control the native
// main-colour draw: rejecting a producer only suppresses our shadow-side
// capture/publication work.
enum class ShadowProducerKind : uint8_t {
  CurrentDrawContract = 0,
  SemanticDirectGrouped,
  DrawTimeGeometry,
  DrawTimePose,
  ImmediateLegacyCapture,
  ImmediateLegacyAlphaPayload,
};

enum class ShadowProducerPolicyDecision : uint8_t {
  Allow = 0,
  RejectRangeIndicatorTarget,
  RejectStage10OwnedByImmediateLegacy,
  RejectStage13OwnedByImmediateLegacy,
  RejectIncompleteAlphaPayload,
};

constexpr size_t kShadowStageLifecycleStageCount = 32u;
constexpr size_t kShadowStageLifecycleBinCount =
    kShadowStageLifecycleStageCount + 1u;

struct ShadowProducerPolicyContext {
  int stage = -1;
  War3BatchTag batchTag = War3BatchTag::Unknown;
  War3BatchTag tlsBatchTag = War3BatchTag::Unknown;
  War3BatchTag semanticTag = War3BatchTag::Unknown;

  // Fail closed for the only producer kind which consumes this field
  // (ImmediateLegacyAlphaPayload). Callers must prove the complete UV,
  // texture and backing contract before publishing.
  bool alphaPayloadComplete = false;
};

// S10 doodads and S13 bridge/ramp draws each have one shadow owner. The
// default owner is the immediate same-draw legacy capture path; environment
// switches remain explicit diagnostic rollbacks.
bool IsStage10ImmediateLegacyShadowOwnerEnabled();
bool IsStage13ImmediateLegacyShadowOwnerEnabled();

// S12 is a main-colour overlay pass which redraws the selected target in
// green. It must never publish caster, pose or material state into the shadow
// pipeline. Keep this predicate centralized so every producer uses the same
// physical-stage/tag decision.
bool IsShadowVisualOverlay(const ShadowProducerPolicyContext& context);

struct ShadowStageLifecycleSnapshot {
  bool enabled = false;
  std::array<uint64_t, kShadowStageLifecycleBinCount> attempt = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> policyAccepted = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> metadataClassified = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> metadataCaptured = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> metadataApplied = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> canonicalPublished = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> replayPrepared = {};
  std::array<std::array<uint64_t, kShadowStageLifecycleBinCount>, 4u>
      cascadeDrawn = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> retiredHidden = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> retiredRemoved = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> retiredStageDisabled = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> retiredReplaced = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> rejectedOverlay = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> rejectedStage10Owner = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> rejectedStage13Owner = {};
  std::array<uint64_t, kShadowStageLifecycleBinCount> rejectedAlphaPayload = {};
};

bool IsShadowStageLifecycleTelemetryEnabled();
void NoteShadowStageMetadataClassified(int stage);
void NoteShadowStageMetadataCaptured(int stage);
void NoteShadowStageMetadataApplied(int stage);
void NoteShadowStageCanonicalPublished(int stage);
void NoteShadowStageReplayPrepared(int stage);
void NoteShadowStageCascadeDrawn(int stage, uint32_t cascade);
void NoteShadowStageRetired(int stage, uint32_t reason);
ShadowStageLifecycleSnapshot QueryShadowStageLifecycleSnapshot();

ShadowProducerPolicyDecision EvaluateShadowProducerPolicy(
    ShadowProducerKind producer,
    const ShadowProducerPolicyContext& context);

inline bool ShadowProducerPolicyAllows(
    ShadowProducerKind producer,
    const ShadowProducerPolicyContext& context) {
  return EvaluateShadowProducerPolicy(producer, context) ==
      ShadowProducerPolicyDecision::Allow;
}

} // namespace dxvk::war3::render
