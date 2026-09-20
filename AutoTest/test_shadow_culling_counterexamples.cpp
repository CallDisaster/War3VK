// Counterexamples for the shared shadow-culling policies.
//
// Validation-lane build example (no production source is modified here):
//   cl /nologo /std:c++17 /I. /EHsc /W4 ^
//     AutoTest\test_shadow_culling_counterexamples.cpp ^
//     src\d3d9\war3\render\war3_union_consumer_visibility.cpp ^
//     /Fe:build32\test_shadow_culling_counterexamples.exe
// or with g++:
//   g++ -std=c++17 -I. AutoTest/test_shadow_culling_counterexamples.cpp \
//       src/d3d9/war3/render/war3_union_consumer_visibility.cpp \
//       -o build32/test_shadow_culling_counterexamples
//
// This test intentionally exercises the production shared policies:
// - war3_shadow_bounds_policy.h
// - war3_union_consumer_visibility.h/.cpp
// It does not include any test-only surrogate policy.

#include "../src/d3d9/war3/render/war3_shadow_bounds_policy.h"
#include "../src/d3d9/war3/render/war3_union_consumer_visibility.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using namespace dxvk::war3::render;

int g_failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,  \
                   #condition);                                                \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

War3UnionMatrix4 IdentityMatrix() {
  War3UnionMatrix4 matrix = {};
  matrix.columns[0][0] = 1.0f;
  matrix.columns[1][1] = 1.0f;
  matrix.columns[2][2] = 1.0f;
  matrix.columns[3][3] = 1.0f;
  return matrix;
}

War3UnionMatrix4 TranslationMatrix(float x, float y, float z) {
  War3UnionMatrix4 matrix = IdentityMatrix();
  matrix.columns[3][0] = x;
  matrix.columns[3][1] = y;
  matrix.columns[3][2] = z;
  return matrix;
}

War3UnionMatrix4 ScaleMatrix(float sx, float sy, float sz) {
  War3UnionMatrix4 matrix = {};
  matrix.columns[0][0] = sx;
  matrix.columns[1][1] = sy;
  matrix.columns[2][2] = sz;
  matrix.columns[3][3] = 1.0f;
  return matrix;
}

// Column-major rotation around Z.  This keeps the test in the same matrix
// convention as the policy and production call site.
War3UnionMatrix4 RotationZMatrix(float angleRadians) {
  War3UnionMatrix4 matrix = IdentityMatrix();
  const float c = std::cos(angleRadians);
  const float s = std::sin(angleRadians);
  matrix.columns[0][0] = c;
  matrix.columns[0][1] = s;
  matrix.columns[1][0] = -s;
  matrix.columns[1][1] = c;
  return matrix;
}

War3UnionMatrix4 Multiply(const War3UnionMatrix4& a,
                          const War3UnionMatrix4& b) {
  War3UnionMatrix4 out = {};
  for (uint32_t column = 0u; column < 4u; ++column) {
    for (uint32_t row = 0u; row < 4u; ++row) {
      float value = 0.0f;
      for (uint32_t k = 0u; k < 4u; ++k)
        value += a.columns[k][row] * b.columns[column][k];
      out.columns[column][row] = value;
    }
  }
  return out;
}

War3UnionCsmSphereQuery ValidQuery(
    uint32_t cascadeIndex,
    War3UnionVisibilityMode mode = War3UnionVisibilityMode::Observe) {
  War3UnionCsmSphereQuery query = {};
  query.mode = mode;
  query.requestedMask = kWar3UnionConsumerAllMask;
  query.cascadeIndex = cascadeIndex;
  query.bounds = {0.0f, 0.0f, 0.5f, 0.1f};
  query.lightViewProjection = IdentityMatrix();
  query.generations.currentFrameGeneration = 100u;
  query.generations.candidateFrameGeneration = 100u;
  query.generations.boundsFrameGeneration = 100u;
  query.generations.cameraFrameGeneration = 100u;
  query.generations.consumerStateFrameGeneration = 100u;
  query.generations.currentMapGeneration = 41u;
  query.generations.candidateMapGeneration = 41u;
  query.generations.consumerMapGeneration = 41u;
  query.generations.currentDeviceGeneration = 43u;
  query.generations.candidateDeviceGeneration = 43u;
  query.generations.consumerDeviceGeneration = 43u;
  query.generations.resourceGeneration = 7u;
  query.generations.expectedResourceGeneration = 7u;
  query.identityKnown = true;
  query.exactCurrentFrameSource = true;
  query.boundsKnown = true;
  query.cameraKnown = true;
  query.consumerStateKnown = true;
  query.matrixKnown = true;
  query.staticRigidProven = true;
  return query;
}

War3ShadowBoundsCullEvidence ValidBoundsEvidence() {
  War3ShadowBoundsCullEvidence evidence = {};
  evidence.provenance = War3ShadowBoundsProvenance::ExactCurrentWorld;
  evidence.sourceGeneration = 7u;
  evidence.boundsFrameSerial = 100u;
  evidence.currentFrameSerial = 100u;
  evidence.identityProven = true;
  evidence.finiteBounds = true;
  evidence.positiveRadius = true;
  return evidence;
}

void TestBoundsEvidenceFailVisibleCounterexamples() {
  auto evidence = ValidBoundsEvidence();
  auto result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(result.mayCull);
  CHECK(result.rejectReason == War3ShadowBoundsCullRejectReason::None);

  evidence.provenance = War3ShadowBoundsProvenance::Unknown;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::UnknownProvenance);

  evidence = ValidBoundsEvidence();
  evidence.provenance = War3ShadowBoundsProvenance::GenericDiagnostic;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::DiagnosticOnly);

  evidence = ValidBoundsEvidence();
  evidence.provenance = War3ShadowBoundsProvenance::ConservativeAnimated;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::AnimatedConservativeOnly);

  evidence = ValidBoundsEvidence();
  evidence.sourceGeneration = 0u;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::SourceGenerationUnknown);

  evidence = ValidBoundsEvidence();
  evidence.boundsFrameSerial = 99u;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::FrameGenerationStale);

  evidence = ValidBoundsEvidence();
  evidence.boundsFrameSerial = 0u;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::FrameGenerationUnknown);

  evidence = ValidBoundsEvidence();
  evidence.identityProven = false;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::IdentityUnproven);

  evidence = ValidBoundsEvidence();
  evidence.sourceWasSkinned = true;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::DynamicOrSkinned);

  evidence = ValidBoundsEvidence();
  evidence.animatedAttachment = true;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::AnimatedAttachment);

  evidence = ValidBoundsEvidence();
  evidence.finiteBounds = false;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::NonFiniteBounds);

  evidence = ValidBoundsEvidence();
  evidence.positiveRadius = false;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::InvalidRadius);

  // Exact local geoset is a static-resource proof; frame-local dynamic
  // geometry must not inherit it.  Exact current world is the explicit
  // current-frame exception and may still prove a dynamic slice.
  evidence = ValidBoundsEvidence();
  evidence.provenance = War3ShadowBoundsProvenance::ExactLocalGeoset;
  evidence.frameLocalDynamic = true;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(!result.mayCull);
  CHECK(result.rejectReason ==
        War3ShadowBoundsCullRejectReason::DynamicOrSkinned);

  evidence = ValidBoundsEvidence();
  evidence.frameLocalDynamic = true;
  result = War3EvaluateBoundsCullEvidence(evidence);
  CHECK(result.mayCull);
  CHECK(result.rejectReason == War3ShadowBoundsCullRejectReason::None);
}

void TestObserveAndNearCascadesNeverConsume() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 3.0f;
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(!result.failVisible);
  CHECK(!result.consumeAllowed);
  CHECK((result.predictedVisibleMask & War3UnionConsumerCsm2) == 0u);
  CHECK((result.provenInvisibleMask & War3UnionConsumerCsm2) != 0u);
  CHECK(result.effectiveVisibleMask == query.requestedMask);
  CHECK((result.proofBits & War3UnionProofOutside) != 0u);

  for (uint32_t cascade = 0u; cascade < 2u; ++cascade) {
    query = ValidQuery(cascade, War3UnionVisibilityMode::Observe);
    query.bounds.x = 100.0f;
    result = War3EvaluateConservativeCsmSphere(query);
    CHECK(result.failVisible);
    CHECK(result.predictedVisibleMask == query.requestedMask);
    CHECK(result.rejectReason ==
          War3UnionVisibilityRejectReason::NearCascadeConservative);
  }
}

void TestDynamicAndUnknownCounterexamples() {
  auto query = ValidQuery(3u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 100.0f;

  query.dynamic = true;
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::DynamicOrSkinned);

  query.dynamic = false;
  query.skinned = true;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::DynamicOrSkinned);

  query.skinned = false;
  query.staticRigidProven = false;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::StaticRigidUnproven);

  query.staticRigidProven = true;
  query.identityKnown = false;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::UnknownIdentity);

  query.identityKnown = true;
  query.exactCurrentFrameSource = false;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::SourceNotExactCurrentFrame);

  query.exactCurrentFrameSource = true;
  query.boundsKnown = false;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::BoundsUnknown);

  query.boundsKnown = true;
  query.generations.candidateFrameGeneration = 99u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::CandidateGenerationStale);

  query.generations.candidateFrameGeneration = 100u;
  query.generations.boundsFrameGeneration = 99u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::BoundsGenerationStale);

  query.generations.boundsFrameGeneration = 100u;
  query.generations.cameraFrameGeneration = 99u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::CameraGenerationStale);

  query.generations.cameraFrameGeneration = 100u;
  query.generations.consumerStateFrameGeneration = 99u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::ConsumerStateGenerationStale);

  query.generations.consumerStateFrameGeneration = 100u;
  query.generations.resourceGeneration = 6u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::ResourceGenerationMismatch);
}

void TestNonFiniteAndDegenerateInputs() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = std::numeric_limits<float>::quiet_NaN();
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::NonFiniteBounds);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.lightViewProjection.columns[1][2] =
      std::numeric_limits<float>::infinity();
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::NonFiniteMatrix);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.radius = 0.0f;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::InvalidBoundsRadius);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.lightViewProjection.columns[3][3] = 0.0f;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::NonOrthographicProjection);
}

void TestConsumeRequiresAdmission() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Consume);
  query.bounds.x = 3.0f;
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(!result.consumeAllowed);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::ConsumeNotAdmitted);
  CHECK(result.effectiveVisibleMask == query.requestedMask);

  query.consumeAdmissionGranted = true;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(!result.failVisible);
  CHECK(result.consumeAllowed);
  CHECK((result.effectiveVisibleMask & War3UnionConsumerCsm2) == 0u);
}

void TestBoundaryContactAndRotationScale() {
  // Strict comparison: a sphere whose conservative NDC extent exactly touches
  // the guard boundary remains visible; one epsilon beyond is proven outside.
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 1.10f;
  query.bounds.radius = 0.10f;
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(!result.failVisible);
  CHECK((result.predictedVisibleMask & War3UnionConsumerCsm2) != 0u);

  query.bounds.x = 1.11f;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(!result.failVisible);
  CHECK((result.predictedVisibleMask & War3UnionConsumerCsm2) == 0u);
  CHECK((result.proofBits & War3UnionProofOutside) != 0u);

  // Rotation + non-uniform scale + negative X scale.  The conservative
  // row-length bound must stay symmetric under the negative scale and still
  // prove a world point pushed well outside the light volume.
  query = ValidQuery(3u, War3UnionVisibilityMode::Observe);
  query.bounds = {0.0f, 0.0f, 0.5f, 0.1f};
  query.lightViewProjection = Multiply(
      TranslationMatrix(2.5f, 0.0f, 0.5f),
      Multiply(RotationZMatrix(0.61f), ScaleMatrix(-1.5f, 2.0f, 3.0f)));
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(!result.failVisible);
  CHECK((result.predictedVisibleMask & War3UnionConsumerCsm3) == 0u);
  CHECK((result.proofBits & War3UnionProofFiniteProjection) != 0u);
  CHECK((result.proofBits & War3UnionProofOutside) != 0u);
}

void TestObserverProofContractCounterexamples() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 3.0f;
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(!result.failVisible);
  CHECK((result.proofBits & War3UnionProofOutside) != 0u);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.lightViewProjection.columns[0][3] = 1.0e-30f;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::NonOrthographicProjection);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.lightViewProjection.columns[3][3] = -1.0f;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::NonOrthographicProjection);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.lightViewProjection.columns[0][3] =
      std::numeric_limits<float>::quiet_NaN();
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::NonFiniteMatrix);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.generations.candidateMapGeneration = 0u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::MapGenerationUnknown);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.generations.currentMapGeneration = 0u;
  query.generations.candidateMapGeneration = 0u;
  query.generations.consumerMapGeneration = 0u;
  query.generations.currentDeviceGeneration = 0u;
  query.generations.candidateDeviceGeneration = 0u;
  query.generations.consumerDeviceGeneration = 0u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::MapGenerationUnknown);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.generations.consumerDeviceGeneration = 0u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::DeviceGenerationUnknown);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.generations.candidateMapGeneration = 40u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::MapGenerationMismatch);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.generations.consumerDeviceGeneration = 44u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::DeviceGenerationMismatch);
}


void TestSingleConsumerMaskRepresentation() {
  for (uint32_t cascade = 0u; cascade < 4u; ++cascade) {
    const War3UnionConsumerMask bit = War3UnionCsmConsumerBit(cascade);
    auto query = ValidQuery(cascade, War3UnionVisibilityMode::Observe);
    query.requestedMask = bit;
    query.bounds.x = 3.0f;
    const auto result = War3EvaluateConservativeCsmSphere(query);
    CHECK(result.effectiveVisibleMask == bit);
    if (cascade < 2u) {
      CHECK(result.failVisible);
      CHECK(result.rejectReason ==
            War3UnionVisibilityRejectReason::NearCascadeConservative);
      CHECK(result.predictedVisibleMask == bit);
    } else {
      CHECK(!result.failVisible);
      CHECK((result.predictedVisibleMask & bit) == 0u);
      CHECK((result.provenInvisibleMask & bit) != 0u);
    }
  }

  const War3UnionConsumerMask nonCsmConsumers[] = {
      War3UnionConsumerMain, War3UnionConsumerPointShadow,
      War3UnionConsumerOutline};
  for (War3UnionConsumerMask consumer : nonCsmConsumers) {
    auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
    query.requestedMask = consumer;
    query.bounds.x = 3.0f;
    const auto result = War3EvaluateConservativeCsmSphere(query);
    CHECK(result.failVisible);
    CHECK(result.rejectReason ==
          War3UnionVisibilityRejectReason::ConsumerNotRequested);
    CHECK(result.predictedVisibleMask == consumer);
    CHECK(result.effectiveVisibleMask == consumer);
  }

  // War3UnionConsumerBits intentionally has no volume-sun bit. The current
  // pure policy cannot represent or prove that consumer.
}

void TestOffCameraOtherConsumerCounterexample() {
  // Synthetic pure-policy counterpart only. The policy has no camera frustum
  // input, so a light-space outside sphere plus a second requested consumer
  // models an off-camera caster that could still contribute elsewhere. This is
  // not pass integration and does not prove any GPU consumer.
  const War3UnionConsumerMask otherConsumers[] = {
      War3UnionConsumerMain, War3UnionConsumerCsm0,
      War3UnionConsumerCsm1, War3UnionConsumerCsm3,
      War3UnionConsumerPointShadow, War3UnionConsumerOutline};
  for (War3UnionConsumerMask other : otherConsumers) {
    auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
    query.requestedMask = War3UnionConsumerCsm2 | other;
    query.bounds.x = 3.0f;
    const auto result = War3EvaluateConservativeCsmSphere(query);
    CHECK(!result.failVisible);
    CHECK((result.predictedVisibleMask & War3UnionConsumerCsm2) == 0u);
    CHECK((result.predictedVisibleMask & other) != 0u);
    CHECK((result.provenInvisibleMask & War3UnionConsumerCsm2) != 0u);
    CHECK((result.provenInvisibleMask & other) == 0u);
    CHECK(result.effectiveVisibleMask == query.requestedMask);
  }
}

void TestProofDomainGapsStayVisible() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 3.0f;
  query.generations.currentFrameGeneration = 100u;
  query.generations.candidateFrameGeneration = 100u;
  query.generations.cameraFrameGeneration = 99u;
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::CameraGenerationStale);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 3.0f;
  query.generations.currentFrameGeneration = 100u;
  query.generations.candidateFrameGeneration = 99u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::CandidateGenerationStale);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 3.0f;
  query.matrixKnown = false;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::MatrixUnknown);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 3.0f;
  query.generations.resourceGeneration = 7u;
  query.generations.expectedResourceGeneration = 8u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.failVisible);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::ResourceGenerationMismatch);
}

void TestAbsentConsumerClosureIsNotProof() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.requestedMask = War3UnionConsumerCsm2;
  query.bounds.x = 3.0f;
  const auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK((result.predictedVisibleMask & War3UnionConsumerCsm2) == 0u);
  CHECK((result.provenInvisibleMask & War3UnionConsumerCsm2) != 0u);
  // Main was not requested. Both masks read zero, but the policy has no
  // unmodeled/unknown-consumer closure state to distinguish proven-outside
  // from never-evaluated. Absence is not omission authority.
  CHECK((result.predictedVisibleMask & War3UnionConsumerMain) == 0u);
  CHECK((result.provenInvisibleMask & War3UnionConsumerMain) == 0u);
  CHECK(result.effectiveVisibleMask == query.requestedMask);
}

} // namespace

int main() {
  CHECK(kWar3ShadowBoundsCullRejectReasonCount == 12u);
  TestBoundsEvidenceFailVisibleCounterexamples();
  TestObserveAndNearCascadesNeverConsume();
  TestDynamicAndUnknownCounterexamples();
  TestNonFiniteAndDegenerateInputs();
  TestConsumeRequiresAdmission();
  TestBoundaryContactAndRotationScale();
  TestObserverProofContractCounterexamples();
  TestSingleConsumerMaskRepresentation();
  TestOffCameraOtherConsumerCounterexample();
  TestProofDomainGapsStayVisible();
  TestAbsentConsumerClosureIsNotProof();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d shadow-culling counterexample checks failed\n",
                 g_failures);
    return 1;
  }
  std::puts("test_shadow_culling_counterexamples: PASS");
  return 0;
}
