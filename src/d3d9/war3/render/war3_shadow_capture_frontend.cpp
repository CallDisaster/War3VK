#include "war3_shadow_capture_frontend.h"

#include "../core/war3_runtime_profile.h"
#include "../reimpl/war3_render_types.h"
#include "../../util/util_env.h"

#include <algorithm>
#include <atomic>
namespace dxvk::war3::render {

namespace {

std::atomic<uint32_t> g_shadowArenaDispatchClaims{0u};
std::atomic<uint32_t> g_shadowArenaDispatchClaimMisses{0u};

uint64_t SumShadowCaptureBytes(const ShadowCaptureBudgetPolicy& policy) {
  return policy.posBytes + policy.blendBytes + policy.uvBytes +
         policy.indexBytes;
}

float ResolveBudgetSoftLimitScale(ShadowCapturePriority priority,
                                  const ShadowCaptureBudgetPolicy& policy) {
  if (policy.aggressiveExperimental)
    return 0.70f;
  if (priority == ShadowCapturePriority::Low)
    return 0.72f;
  if (priority == ShadowCapturePriority::Medium)
    return 0.82f;
  return 0.90f;
}

bool IsShadowArenaDispatchEligible(War3BatchTag tag, int stage, int kind) {
  if (kind == 0)
    return false;

  switch (tag) {
  case War3BatchTag::WorldObjects:
  case War3BatchTag::Decorations:
    return true;
  default:
    break;
  }

  return stage == 10 || stage == 11;
}


} // namespace

ShadowCapturePriority ComputeShadowCapturePriority(
    const ShadowCaptureCandidateInfo& candidate) {
  if (candidate.objectKind == ObjectKind::Building)
    return ShadowCapturePriority::Critical;

  if (candidate.terrainS1Caster || candidate.terrainTileCaster)
    return ShadowCapturePriority::High;

  if (candidate.objectKind == ObjectKind::Unit ||
      candidate.objectKind == ObjectKind::Destructible ||
      candidate.objectKind == ObjectKind::Item)
    return ShadowCapturePriority::High;

  if (candidate.category == War3RenderState::StageCategory::WorldObject &&
      !candidate.terrainDecorationLike)
    return ShadowCapturePriority::High;

  if (candidate.objectKind == ObjectKind::Effect ||
      candidate.terrainDoodadCaster || candidate.terrainDecorationLike)
    return ShadowCapturePriority::Low;

  if (candidate.alphaBlend && !candidate.depthWriteEnabled &&
      !candidate.alphaTestEnabled)
    return ShadowCapturePriority::Low;

  return ShadowCapturePriority::Medium;
}

const char* ShadowCapturePriorityName(ShadowCapturePriority priority) {
  switch (priority) {
  case ShadowCapturePriority::Critical:
    return "critical";
  case ShadowCapturePriority::High:
    return "high";
  case ShadowCapturePriority::Medium:
    return "medium";
  case ShadowCapturePriority::Low:
  default:
    return "low";
  }
}

ShadowCaptureBudgetDecision DecideShadowCaptureBudget(
    const ShadowCaptureCandidateInfo& candidate,
    const ShadowCaptureBudgetPolicy& policy) {
  ShadowCaptureBudgetDecision decision = {};
  decision.priority = ComputeShadowCapturePriority(candidate);

  if (!policy.freezeDynamicEnabled || policy.hardBudgetBytes == 0)
    return decision;

  const uint64_t totalBytes = SumShadowCaptureBytes(policy);
  if (totalBytes == 0)
    return decision;

  const uint64_t usedBytes = policy.usedBudgetBytes;
  const uint64_t hardBudgetBytes = policy.hardBudgetBytes;

  // Required directional casters form one atomic publication. Applying an
  // order-dependent soft threshold here makes a scene that fits the Arena
  // alternate between complete and incomplete as Warcraft changes draw order.
  // Preserve the hard cap, but never trade one required caster for a partial
  // CSM candidate.
  if (policy.requiredDirectionalCaster) {
    if (RequiredShadowCasterFitsHardBudget(policy))
      return decision;
    decision.skipCaster = true;
    decision.reason = "required_caster_hard_budget";
    return decision;
  }

  const uint64_t predictedBytes = usedBytes + totalBytes;
  const double budgetPressure =
      hardBudgetBytes != 0
          ? (static_cast<double>(usedBytes) / static_cast<double>(hardBudgetBytes))
          : 0.0;

  const float softScale = ResolveBudgetSoftLimitScale(decision.priority, policy);
  const uint64_t softBudgetBytes =
      static_cast<uint64_t>(static_cast<double>(hardBudgetBytes) * softScale);

  const bool nearFarEdge =
      candidate.shadowMaxDistance > 0.0f &&
      candidate.viewDepth > (candidate.shadowMaxDistance * 0.82f);
  const bool farDecoration =
      candidate.shadowMaxDistance > 0.0f &&
      candidate.viewDepth > (candidate.shadowMaxDistance * 0.68f) &&
      (candidate.terrainDecorationLike ||
       candidate.objectKind == ObjectKind::Effect ||
       candidate.terrainDoodadCaster);
  const bool veryLargeCaster =
      candidate.indexCount >= 12000u || candidate.vertexCount >= 8000u;
  const bool hugeBudgetShare = totalBytes > (hardBudgetBytes / 12u);
  const bool defaultBudgetStabilityMode =
      !policy.aggressiveExperimental && budgetPressure >= 0.78;

  if (predictedBytes <= softBudgetBytes)
    return decision;

  if (decision.priority == ShadowCapturePriority::Critical)
    return decision;

  if (decision.priority == ShadowCapturePriority::High &&
      predictedBytes <= hardBudgetBytes)
    return decision;

  if (policy.uvBytes != 0 && decision.priority != ShadowCapturePriority::Critical &&
      (candidate.alphaBlend || candidate.alphaTestEnabled)) {
    const uint64_t withoutAlphaBytes = predictedBytes - policy.uvBytes;
    if (withoutAlphaBytes <= hardBudgetBytes &&
        (budgetPressure >= 0.72 || nearFarEdge || farDecoration ||
         policy.aggressiveExperimental)) {
      decision.disableAlphaCapture = true;
      decision.reason = "drop_alpha_budget";
      return decision;
    }
  }

  const bool lowPrioritySkip =
      decision.priority == ShadowCapturePriority::Low &&
      (nearFarEdge || farDecoration || veryLargeCaster || hugeBudgetShare ||
       budgetPressure >= 0.68 || policy.aggressiveExperimental);
  const bool mediumPrioritySkip =
      decision.priority == ShadowCapturePriority::Medium &&
      (policy.aggressiveExperimental || defaultBudgetStabilityMode) &&
      (nearFarEdge || farDecoration || veryLargeCaster || hugeBudgetShare ||
       budgetPressure >= (policy.aggressiveExperimental ? 0.80 : 0.88));

  if (predictedBytes > hardBudgetBytes ||
      (predictedBytes > softBudgetBytes &&
       (lowPrioritySkip || mediumPrioritySkip))) {
    decision.skipCaster = true;
    decision.reason = mediumPrioritySkip ? "skip_medium_priority_budget"
                    : farDecoration      ? "skip_far_decoration_budget"
                                         : "skip_low_priority_budget";
    return decision;
  }

  return decision;
}

bool IsShadowArenaCaptureEnabled() {
  // Phase 3：Arena 捕获现为默认路径，彻底替代 vkCmdCopyBuffer Freeze。
  // 如需回退到旧 Freeze 路径（调试用），显式设置 DXVK_WAR3_SHADOW_ARENA_CAPTURE=0。
  // 默认 ON：Arena GPU copy 消除 LegacyFreeze 预算问题，完全避免 CPU 读 BAR/WC 内存。
  static const bool s_enabled =
      dxvk::env::getEnvVar("DXVK_WAR3_SHADOW_ARENA_CAPTURE") != "0";
  return s_enabled;
}

void BeginShadowArenaCaptureFrame() {
  if (!IsShadowArenaCaptureEnabled())
    return;

  g_shadowArenaDispatchClaims.store(0u, std::memory_order_relaxed);
  g_shadowArenaDispatchClaimMisses.store(0u, std::memory_order_relaxed);
}

void RegisterShadowArenaBatchRange(const void* batchArrayBase,
                                   uint32_t before,
                                   uint32_t after) {
  (void)batchArrayBase;
  (void)before;
  (void)after;
}

bool TryClaimShadowArenaDispatch(void* renderablePart, War3BatchTag tag,
                                 int stage, int kind) {
  if (!IsShadowArenaCaptureEnabled() || renderablePart == nullptr ||
      !IsShadowArenaDispatchEligible(tag, stage, kind)) {
    g_shadowArenaDispatchClaimMisses.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }

  g_shadowArenaDispatchClaims.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

ShadowArenaDispatchTelemetry GetShadowArenaDispatchTelemetry() {
  ShadowArenaDispatchTelemetry telemetry = {};
  telemetry.dispatchClaims =
      g_shadowArenaDispatchClaims.load(std::memory_order_relaxed);
  telemetry.dispatchClaimMisses =
      g_shadowArenaDispatchClaimMisses.load(std::memory_order_relaxed);
  return telemetry;
}

} // namespace dxvk::war3::render
