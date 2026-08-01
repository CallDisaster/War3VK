#pragma once

#include <cstdint>
#include <type_traits>

namespace dxvk::war3::render {

// The runtime integration is deliberately separate from this pure policy
// layer. Observe may predict work which could be removed, but only an admitted
// Consume caller may turn a proven-outside bit into an effective rejection.
enum class War3UnionVisibilityMode : uint8_t {
  Off = 0u,
  Observe = 1u,
  Consume = 2u,
};

using War3UnionConsumerMask = uint32_t;

enum War3UnionConsumerBits : War3UnionConsumerMask {
  War3UnionConsumerMain = 1u << 0u,
  War3UnionConsumerCsm0 = 1u << 1u,
  War3UnionConsumerCsm1 = 1u << 2u,
  War3UnionConsumerCsm2 = 1u << 3u,
  War3UnionConsumerCsm3 = 1u << 4u,
  War3UnionConsumerPointShadow = 1u << 5u,
  War3UnionConsumerOutline = 1u << 6u,
};

constexpr War3UnionConsumerMask kWar3UnionConsumerCsmMask =
    War3UnionConsumerCsm0 | War3UnionConsumerCsm1 |
    War3UnionConsumerCsm2 | War3UnionConsumerCsm3;
constexpr War3UnionConsumerMask kWar3UnionConsumerAllMask =
    War3UnionConsumerMain | kWar3UnionConsumerCsmMask |
    War3UnionConsumerPointShadow | War3UnionConsumerOutline;

enum class War3UnionVisibilityRejectReason : uint8_t {
  None = 0u,
  ModeOff,
  UnsupportedMode,
  InvalidCascade,
  ConsumerNotRequested,
  NearCascadeConservative,
  DynamicOrSkinned,
  StaticRigidUnproven,
  UnknownIdentity,
  SourceNotExactCurrentFrame,
  CurrentFrameUnknown,
  CandidateGenerationStale,
  BoundsUnknown,
  BoundsGenerationStale,
  CameraUnknown,
  CameraGenerationStale,
  ConsumerStateUnknown,
  ConsumerStateGenerationStale,
  ResourceGenerationUnknown,
  ResourceGenerationMismatch,
  MatrixUnknown,
  NonFiniteBounds,
  InvalidBoundsRadius,
  NonFiniteMatrix,
  InvalidGuardBand,
  DegenerateClipW,
  NonFiniteProjection,
  ConsumeNotAdmitted,
};

enum War3UnionVisibilityProofBits : uint32_t {
  War3UnionProofModeEnabled = 1u << 0u,
  War3UnionProofFarCascade = 1u << 1u,
  War3UnionProofStaticRigid = 1u << 2u,
  War3UnionProofIdentityKnown = 1u << 3u,
  War3UnionProofExactCurrentSource = 1u << 4u,
  War3UnionProofCandidateCurrent = 1u << 5u,
  War3UnionProofBoundsCurrent = 1u << 6u,
  War3UnionProofCameraCurrent = 1u << 7u,
  War3UnionProofConsumerStateCurrent = 1u << 8u,
  War3UnionProofResourceCurrent = 1u << 9u,
  War3UnionProofFiniteBounds = 1u << 10u,
  War3UnionProofFiniteMatrix = 1u << 11u,
  War3UnionProofFiniteProjection = 1u << 12u,
  War3UnionProofOutside = 1u << 13u,
};

// Column-major, matching dxvk::Matrix4. Multiplication is M * column-vector.
// Keeping the policy type independent avoids pulling Vulkan/D3D state into
// runnable algorithm tests.
struct War3UnionMatrix4 {
  float columns[4][4] = {};
};

struct War3UnionBoundsSphere {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float radius = 0.0f;
};

// All frame-domain values must name the same exact generation. The resource
// generation is separate because CSM images may remain stable across frames
// while their per-frame matrices do not.
struct War3UnionGenerationProof {
  uint64_t currentFrameGeneration = 0u;
  uint64_t candidateFrameGeneration = 0u;
  uint64_t boundsFrameGeneration = 0u;
  uint64_t cameraFrameGeneration = 0u;
  uint64_t consumerStateFrameGeneration = 0u;
  uint64_t resourceGeneration = 0u;
  uint64_t expectedResourceGeneration = 0u;
};

struct War3UnionCsmSphereQuery {
  War3UnionVisibilityMode mode = War3UnionVisibilityMode::Off;
  War3UnionConsumerMask requestedMask = kWar3UnionConsumerAllMask;
  uint32_t cascadeIndex = 0u;

  War3UnionBoundsSphere bounds = {};
  War3UnionMatrix4 lightViewProjection = {};
  War3UnionGenerationProof generations = {};

  bool identityKnown = false;
  bool exactCurrentFrameSource = false;
  bool boundsKnown = false;
  bool cameraKnown = false;
  bool consumerStateKnown = false;
  bool matrixKnown = false;
  bool staticRigidProven = false;
  bool dynamic = false;
  bool skinned = false;

  float radiusScale = 1.0f;
  float guardBandNdc = 0.0f;
  float depthGuardBandNdc = 0.0f;

  // This remains false until the long Observe correctness/performance gate is
  // passed. Mode=Consume alone is intentionally insufficient.
  bool consumeAdmissionGranted = false;
};

struct War3UnionVisibilityDecision {
  War3UnionConsumerMask requestedMask = 0u;
  War3UnionConsumerMask predictedVisibleMask = 0u;
  War3UnionConsumerMask effectiveVisibleMask = 0u;
  War3UnionConsumerMask provenInvisibleMask = 0u;
  uint32_t proofBits = 0u;
  War3UnionVisibilityRejectReason rejectReason =
      War3UnionVisibilityRejectReason::None;
  bool failVisible = true;
  bool consumeAllowed = false;
};

static_assert(std::is_standard_layout_v<War3UnionMatrix4>);
static_assert(std::is_trivially_copyable_v<War3UnionMatrix4>);
static_assert(std::is_standard_layout_v<War3UnionBoundsSphere>);
static_assert(std::is_trivially_copyable_v<War3UnionBoundsSphere>);
static_assert(std::is_standard_layout_v<War3UnionGenerationProof>);
static_assert(std::is_trivially_copyable_v<War3UnionGenerationProof>);
static_assert(std::is_standard_layout_v<War3UnionVisibilityDecision>);
static_assert(std::is_trivially_copyable_v<War3UnionVisibilityDecision>);

constexpr War3UnionConsumerMask War3UnionCsmConsumerBit(
    uint32_t cascadeIndex) {
  return cascadeIndex < 4u
      ? War3UnionConsumerMask(War3UnionConsumerCsm0 << cascadeIndex)
      : 0u;
}

bool War3UnionIsFiniteBounds(const War3UnionBoundsSphere& bounds);
bool War3UnionIsFiniteMatrix(const War3UnionMatrix4& matrix);

War3UnionVisibilityDecision War3UnionMakeFailVisibleDecision(
    War3UnionConsumerMask requestedMask,
    War3UnionVisibilityRejectReason reason,
    uint32_t proofBits = 0u);

// Conservative CSM sphere test. It can prove only C2/C3 static/rigid entries
// outside. C0/C1, dynamic/skinned, unknown, stale or non-finite inputs always
// remain visible and carry a diagnostic reject reason.
War3UnionVisibilityDecision War3EvaluateConservativeCsmSphere(
    const War3UnionCsmSphereQuery& query);

} // namespace dxvk::war3::render
