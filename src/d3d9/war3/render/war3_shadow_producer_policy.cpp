#include "war3_shadow_producer_policy.h"

#include "../../util/util_env.h"

#include <atomic>

namespace dxvk::war3::render {

namespace {

using StageCounterArray =
    std::array<std::atomic<uint64_t>, kShadowStageLifecycleBinCount>;

StageCounterArray g_attempt = {};
StageCounterArray g_policyAccepted = {};
StageCounterArray g_metadataClassified = {};
StageCounterArray g_metadataCaptured = {};
StageCounterArray g_metadataApplied = {};
StageCounterArray g_canonicalPublished = {};
StageCounterArray g_replayPrepared = {};
std::array<StageCounterArray, 4u> g_cascadeDrawn = {};
std::array<StageCounterArray, 4u> g_retiredByReason = {};
StageCounterArray g_rejectedOverlay = {};
StageCounterArray g_rejectedLightning = {};
StageCounterArray g_rejectedStage10Owner = {};
StageCounterArray g_rejectedStage13Owner = {};
StageCounterArray g_rejectedAlphaPayload = {};

size_t StageBin(int stage) {
  return stage >= 0 && size_t(stage) < kShadowStageLifecycleStageCount
      ? size_t(stage)
      : kShadowStageLifecycleStageCount;
}

void IncrementStageCounter(StageCounterArray& counters, int stage) {
  counters[StageBin(stage)].fetch_add(1u, std::memory_order_relaxed);
}

std::array<uint64_t, kShadowStageLifecycleBinCount>
SnapshotStageCounters(const StageCounterArray& counters) {
  std::array<uint64_t, kShadowStageLifecycleBinCount> result = {};
  for (size_t i = 0u; i < result.size(); ++i)
    result[i] = counters[i].load(std::memory_order_relaxed);
  return result;
}

bool IsImmediateLegacyOwnedProducer(ShadowProducerKind producer) {
  return producer == ShadowProducerKind::ImmediateLegacyCapture ||
      producer == ShadowProducerKind::ImmediateLegacyAlphaPayload;
}

} // namespace

bool IsStage10ImmediateLegacyShadowOwnerEnabled() {
  static const bool enabled =
      env::getEnvVar("DXVK_WAR3_KEEP_STAGE10_TERRAIN_DOODAD_LEGACY_CAPTURE") !=
      "0";
  return enabled;
}

bool IsStage13ImmediateLegacyShadowOwnerEnabled() {
  static const bool enabled =
      env::getEnvVar("DXVK_WAR3_KEEP_STAGE13_WORLDOBJECT_LEGACY_CAPTURE") !=
      "0";
  return enabled;
}

bool IsShadowStageLifecycleTelemetryEnabled() {
  static const bool enabled =
      env::getEnvVar("DXVK_WAR3_SHADOW_STAGE_LIFECYCLE") == "1";
  return enabled;
}

bool IsShadowVisualOverlay(const ShadowProducerPolicyContext& context) {
  // S12 is the green range-indicator target redraw, not another physical
  // world-object submission. Keep the tag checks as a second line of defence
  // for builds where the transient stage number is unavailable or remapped.
  return context.stage == 12 ||
      context.batchTag == War3BatchTag::RangeIndicatorTarget ||
      context.tlsBatchTag == War3BatchTag::RangeIndicatorTarget ||
      context.semanticTag == War3BatchTag::RangeIndicatorTarget;
}

bool IsShadowNativeLightning(const ShadowProducerPolicyContext& context) {
  // Prefer the physical S20 gate and retain all tag channels as fail-closed
  // defence when a caller has already left the transient stage scope.
  return context.stage == 20 ||
      context.batchTag == War3BatchTag::Lightning ||
      context.tlsBatchTag == War3BatchTag::Lightning ||
      context.semanticTag == War3BatchTag::Lightning;
}

void NoteShadowStageMetadataClassified(int stage) {
  if (IsShadowStageLifecycleTelemetryEnabled())
    IncrementStageCounter(g_metadataClassified, stage);
}

void NoteShadowStageMetadataCaptured(int stage) {
  if (IsShadowStageLifecycleTelemetryEnabled())
    IncrementStageCounter(g_metadataCaptured, stage);
}

void NoteShadowStageMetadataApplied(int stage) {
  if (IsShadowStageLifecycleTelemetryEnabled())
    IncrementStageCounter(g_metadataApplied, stage);
}

void NoteShadowStageCanonicalPublished(int stage) {
  if (IsShadowStageLifecycleTelemetryEnabled())
    IncrementStageCounter(g_canonicalPublished, stage);
}

void NoteShadowStageReplayPrepared(int stage) {
  if (IsShadowStageLifecycleTelemetryEnabled())
    IncrementStageCounter(g_replayPrepared, stage);
}

void NoteShadowStageCascadeDrawn(int stage, uint32_t cascade) {
  if (IsShadowStageLifecycleTelemetryEnabled() &&
      cascade < g_cascadeDrawn.size()) {
    IncrementStageCounter(g_cascadeDrawn[cascade], stage);
  }
}

void NoteShadowStageRetired(int stage, uint32_t reason) {
  if (IsShadowStageLifecycleTelemetryEnabled() &&
      reason < g_retiredByReason.size()) {
    IncrementStageCounter(g_retiredByReason[reason], stage);
  }
}

ShadowStageLifecycleSnapshot QueryShadowStageLifecycleSnapshot() {
  ShadowStageLifecycleSnapshot result = {};
  result.enabled = IsShadowStageLifecycleTelemetryEnabled();
  result.attempt = SnapshotStageCounters(g_attempt);
  result.policyAccepted = SnapshotStageCounters(g_policyAccepted);
  result.metadataClassified =
      SnapshotStageCounters(g_metadataClassified);
  result.metadataCaptured = SnapshotStageCounters(g_metadataCaptured);
  result.metadataApplied = SnapshotStageCounters(g_metadataApplied);
  result.canonicalPublished = SnapshotStageCounters(g_canonicalPublished);
  result.replayPrepared = SnapshotStageCounters(g_replayPrepared);
  for (size_t cascade = 0u; cascade < result.cascadeDrawn.size(); ++cascade) {
    result.cascadeDrawn[cascade] =
        SnapshotStageCounters(g_cascadeDrawn[cascade]);
  }
  result.retiredHidden = SnapshotStageCounters(g_retiredByReason[0u]);
  result.retiredRemoved = SnapshotStageCounters(g_retiredByReason[1u]);
  result.retiredStageDisabled =
      SnapshotStageCounters(g_retiredByReason[2u]);
  result.retiredReplaced = SnapshotStageCounters(g_retiredByReason[3u]);
  result.rejectedOverlay = SnapshotStageCounters(g_rejectedOverlay);
  result.rejectedLightning = SnapshotStageCounters(g_rejectedLightning);
  result.rejectedStage10Owner =
      SnapshotStageCounters(g_rejectedStage10Owner);
  result.rejectedStage13Owner =
      SnapshotStageCounters(g_rejectedStage13Owner);
  result.rejectedAlphaPayload =
      SnapshotStageCounters(g_rejectedAlphaPayload);
  return result;
}

ShadowProducerPolicyDecision EvaluateShadowProducerPolicy(
    ShadowProducerKind producer,
    const ShadowProducerPolicyContext& context) {
  const bool traceLifecycle = IsShadowStageLifecycleTelemetryEnabled();
  if (traceLifecycle)
    IncrementStageCounter(g_attempt, context.stage);

  if (IsShadowNativeLightning(context)) {
    if (traceLifecycle)
      IncrementStageCounter(g_rejectedLightning, context.stage);
    return ShadowProducerPolicyDecision::RejectNativeLightningStage;
  }

  if (IsShadowVisualOverlay(context)) {
    if (traceLifecycle)
      IncrementStageCounter(g_rejectedOverlay, context.stage);
    return ShadowProducerPolicyDecision::RejectRangeIndicatorTarget;
  }

  if (producer == ShadowProducerKind::ImmediateLegacyAlphaPayload &&
      !context.alphaPayloadComplete) {
    if (traceLifecycle)
      IncrementStageCounter(g_rejectedAlphaPayload, context.stage);
    return ShadowProducerPolicyDecision::RejectIncompleteAlphaPayload;
  }

  if (context.stage == 10 &&
      IsStage10ImmediateLegacyShadowOwnerEnabled() &&
      !IsImmediateLegacyOwnedProducer(producer)) {
    if (traceLifecycle)
      IncrementStageCounter(g_rejectedStage10Owner, context.stage);
    return ShadowProducerPolicyDecision::
        RejectStage10OwnedByImmediateLegacy;
  }

  if (context.stage == 13 &&
      IsStage13ImmediateLegacyShadowOwnerEnabled() &&
      !IsImmediateLegacyOwnedProducer(producer)) {
    if (traceLifecycle)
      IncrementStageCounter(g_rejectedStage13Owner, context.stage);
    return ShadowProducerPolicyDecision::
        RejectStage13OwnedByImmediateLegacy;
  }

  if (traceLifecycle)
    IncrementStageCounter(g_policyAccepted, context.stage);
  return ShadowProducerPolicyDecision::Allow;
}

} // namespace dxvk::war3::render
