#pragma once

#include <cstdint>
#include <type_traits>

namespace dxvk::war3::render {

enum class War3TerrainBoundsCullMode : uint8_t {
  Off = 0u,
  Observe = 1u,
  Consume = 2u,
};

// A bound can be useful for diagnostics without being authoritative enough to
// remove a required caster. In particular, the generic scene-node sphere is
// never a culling proof.
enum class War3ShadowBoundsProvenance : uint8_t {
  Unknown = 0u,
  GenericDiagnostic,
  ExactLocalGeoset,
  ExactCurrentWorld,
  ConservativeAnimated,
};

enum class War3ShadowBoundsCullRejectReason : uint8_t {
  None = 0u,
  UnknownProvenance,
  DiagnosticOnly,
  AnimatedConservativeOnly,
  SourceGenerationUnknown,
  FrameGenerationUnknown,
  FrameGenerationStale,
  IdentityUnproven,
  DynamicOrSkinned,
  AnimatedAttachment,
  NonFiniteBounds,
  InvalidRadius,
  Count,
};

inline constexpr uint32_t kWar3ShadowBoundsCullRejectReasonCount =
    static_cast<uint32_t>(War3ShadowBoundsCullRejectReason::Count);

constexpr uint32_t War3ShadowBoundsCullRejectReasonIndex(
    War3ShadowBoundsCullRejectReason reason) {
  const uint32_t index = static_cast<uint32_t>(reason);
  return index < kWar3ShadowBoundsCullRejectReasonCount
      ? index
      : static_cast<uint32_t>(
            War3ShadowBoundsCullRejectReason::UnknownProvenance);
}

struct War3ShadowBoundsCullEvidence {
  War3ShadowBoundsProvenance provenance =
      War3ShadowBoundsProvenance::Unknown;
  uint64_t sourceGeneration = 0u;
  uint64_t boundsFrameSerial = 0u;
  uint64_t currentFrameSerial = 0u;
  bool identityProven = false;
  bool sourceWasSkinned = false;
  bool frameLocalDynamic = false;
  bool animatedAttachment = false;
  bool finiteBounds = false;
  bool positiveRadius = false;
};

struct War3ShadowBoundsCullDecision {
  War3ShadowBoundsCullRejectReason rejectReason =
      War3ShadowBoundsCullRejectReason::UnknownProvenance;
  bool mayCull = false;
};

constexpr bool War3ShadowBoundsProvenanceIsExact(
    War3ShadowBoundsProvenance provenance) {
  return provenance == War3ShadowBoundsProvenance::ExactLocalGeoset ||
      provenance == War3ShadowBoundsProvenance::ExactCurrentWorld;
}

constexpr War3ShadowBoundsCullDecision War3EvaluateBoundsCullEvidence(
    const War3ShadowBoundsCullEvidence& evidence) {
  if (evidence.provenance == War3ShadowBoundsProvenance::Unknown)
    return {War3ShadowBoundsCullRejectReason::UnknownProvenance, false};
  if (evidence.provenance ==
      War3ShadowBoundsProvenance::GenericDiagnostic)
    return {War3ShadowBoundsCullRejectReason::DiagnosticOnly, false};
  if (evidence.provenance ==
      War3ShadowBoundsProvenance::ConservativeAnimated)
    return {War3ShadowBoundsCullRejectReason::AnimatedConservativeOnly,
            false};
  if (!War3ShadowBoundsProvenanceIsExact(evidence.provenance))
    return {War3ShadowBoundsCullRejectReason::UnknownProvenance, false};
  if (evidence.sourceGeneration == 0u)
    return {War3ShadowBoundsCullRejectReason::SourceGenerationUnknown,
            false};
  if (evidence.boundsFrameSerial == 0u ||
      evidence.currentFrameSerial == 0u)
    return {War3ShadowBoundsCullRejectReason::FrameGenerationUnknown, false};
  if (evidence.boundsFrameSerial != evidence.currentFrameSerial)
    return {War3ShadowBoundsCullRejectReason::FrameGenerationStale, false};
  if (!evidence.identityProven)
    return {War3ShadowBoundsCullRejectReason::IdentityUnproven, false};
  if (evidence.sourceWasSkinned)
    return {War3ShadowBoundsCullRejectReason::DynamicOrSkinned, false};
  if (evidence.frameLocalDynamic &&
      evidence.provenance != War3ShadowBoundsProvenance::ExactCurrentWorld)
    return {War3ShadowBoundsCullRejectReason::DynamicOrSkinned, false};
  if (evidence.animatedAttachment)
    return {War3ShadowBoundsCullRejectReason::AnimatedAttachment, false};
  if (!evidence.finiteBounds)
    return {War3ShadowBoundsCullRejectReason::NonFiniteBounds, false};
  if (!evidence.positiveRadius)
    return {War3ShadowBoundsCullRejectReason::InvalidRadius, false};
  return {War3ShadowBoundsCullRejectReason::None, true};
}

static_assert(std::is_standard_layout_v<War3ShadowBoundsCullEvidence>);
static_assert(std::is_trivially_copyable_v<War3ShadowBoundsCullEvidence>);
static_assert(std::is_standard_layout_v<War3ShadowBoundsCullDecision>);
static_assert(std::is_trivially_copyable_v<War3ShadowBoundsCullDecision>);

} // namespace dxvk::war3::render
