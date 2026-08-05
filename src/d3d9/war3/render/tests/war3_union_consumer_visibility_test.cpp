#include "../war3_union_consumer_visibility.h"

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

War3UnionCsmSphereQuery ValidQuery(uint32_t cascadeIndex,
                                   War3UnionVisibilityMode mode) {
  War3UnionCsmSphereQuery query = {};
  query.mode = mode;
  query.requestedMask = kWar3UnionConsumerAllMask;
  query.cascadeIndex = cascadeIndex;
  query.bounds = {0.0f, 0.0f, 0.5f, 0.1f};
  query.lightViewProjection = IdentityMatrix();
  query.generations.currentFrameGeneration = 17u;
  query.generations.candidateFrameGeneration = 17u;
  query.generations.boundsFrameGeneration = 17u;
  query.generations.cameraFrameGeneration = 17u;
  query.generations.consumerStateFrameGeneration = 17u;
  query.generations.resourceGeneration = 4u;
  query.generations.expectedResourceGeneration = 4u;
  query.identityKnown = true;
  query.exactCurrentFrameSource = true;
  query.boundsKnown = true;
  query.cameraKnown = true;
  query.consumerStateKnown = true;
  query.matrixKnown = true;
  query.staticRigidProven = true;
  return query;
}

void TestObserveDoesNotCullEffectiveMask() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 3.0f;
  const auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::None);
  CHECK(!result.failVisible);
  CHECK(!result.consumeAllowed);
  CHECK((result.predictedVisibleMask & War3UnionConsumerCsm2) == 0u);
  CHECK((result.provenInvisibleMask & War3UnionConsumerCsm2) != 0u);
  CHECK(result.effectiveVisibleMask == query.requestedMask);
  CHECK((result.predictedVisibleMask & War3UnionConsumerMain) != 0u);
  CHECK((result.predictedVisibleMask & War3UnionConsumerPointShadow) != 0u);
  CHECK((result.proofBits & War3UnionProofOutside) != 0u);
}

void TestConsumeRequiresSeparateAdmission() {
  auto query = ValidQuery(3u, War3UnionVisibilityMode::Consume);
  query.bounds.y = -3.0f;
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::ConsumeNotAdmitted);
  CHECK(result.failVisible);
  CHECK(!result.consumeAllowed);
  CHECK(result.effectiveVisibleMask == query.requestedMask);

  query.consumeAdmissionGranted = true;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::None);
  CHECK(!result.failVisible);
  CHECK(result.consumeAllowed);
  CHECK((result.effectiveVisibleMask & War3UnionConsumerCsm3) == 0u);
}

void TestInsideAndGuardBandStayVisible() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::None);
  CHECK(!result.failVisible);
  CHECK(result.predictedVisibleMask == query.requestedMask);

  query.bounds.x = 1.15f;
  query.bounds.radius = 0.01f;
  query.guardBandNdc = 0.20f;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.predictedVisibleMask == query.requestedMask);
}

void TestNearAndDynamicAlwaysFailVisible() {
  for (uint32_t cascade = 0u; cascade < 2u; ++cascade) {
    auto query = ValidQuery(cascade, War3UnionVisibilityMode::Observe);
    query.bounds.x = 100.0f;
    const auto result = War3EvaluateConservativeCsmSphere(query);
    CHECK(result.rejectReason ==
          War3UnionVisibilityRejectReason::NearCascadeConservative);
    CHECK(result.failVisible);
    CHECK(result.predictedVisibleMask == query.requestedMask);
  }

  auto dynamicQuery = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  dynamicQuery.bounds.x = 100.0f;
  dynamicQuery.dynamic = true;
  auto result = War3EvaluateConservativeCsmSphere(dynamicQuery);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::DynamicOrSkinned);
  CHECK(result.predictedVisibleMask == dynamicQuery.requestedMask);

  dynamicQuery.dynamic = false;
  dynamicQuery.skinned = true;
  result = War3EvaluateConservativeCsmSphere(dynamicQuery);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::DynamicOrSkinned);
  CHECK(result.predictedVisibleMask == dynamicQuery.requestedMask);
}

void TestUnknownAndStaleInputsFailVisible() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = 100.0f;

  query.identityKnown = false;
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::UnknownIdentity);
  CHECK(result.failVisible);
  query.identityKnown = true;

  query.generations.candidateFrameGeneration = 16u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::CandidateGenerationStale);
  query.generations.candidateFrameGeneration = 17u;

  query.generations.boundsFrameGeneration = 16u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::BoundsGenerationStale);
  query.generations.boundsFrameGeneration = 17u;

  query.generations.cameraFrameGeneration = 16u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::CameraGenerationStale);
  query.generations.cameraFrameGeneration = 17u;

  query.generations.consumerStateFrameGeneration = 16u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::ConsumerStateGenerationStale);
  query.generations.consumerStateFrameGeneration = 17u;

  query.generations.resourceGeneration = 3u;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::ResourceGenerationMismatch);
  CHECK(result.predictedVisibleMask == query.requestedMask);
}

void TestNonFiniteAndDegenerateInputsFailVisible() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.x = std::numeric_limits<float>::quiet_NaN();
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::NonFiniteBounds);
  CHECK(result.predictedVisibleMask == query.requestedMask);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.lightViewProjection.columns[1][2] =
      std::numeric_limits<float>::infinity();
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::NonFiniteMatrix);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.bounds.radius = 0.0f;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::InvalidBoundsRadius);

  query = ValidQuery(2u, War3UnionVisibilityMode::Observe);
  query.lightViewProjection.columns[3][3] = 0.0f;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::DegenerateClipW);
}

void TestOffAndUnprovenClassificationFailVisible() {
  auto query = ValidQuery(2u, War3UnionVisibilityMode::Off);
  query.bounds.x = 100.0f;
  auto result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason == War3UnionVisibilityRejectReason::ModeOff);
  CHECK(result.effectiveVisibleMask == query.requestedMask);

  query.mode = War3UnionVisibilityMode::Observe;
  query.staticRigidProven = false;
  result = War3EvaluateConservativeCsmSphere(query);
  CHECK(result.rejectReason ==
        War3UnionVisibilityRejectReason::StaticRigidUnproven);
  CHECK(result.effectiveVisibleMask == query.requestedMask);
}

} // namespace

int main() {
  TestObserveDoesNotCullEffectiveMask();
  TestConsumeRequiresSeparateAdmission();
  TestInsideAndGuardBandStayVisible();
  TestNearAndDynamicAlwaysFailVisible();
  TestUnknownAndStaleInputsFailVisible();
  TestNonFiniteAndDegenerateInputsFailVisible();
  TestOffAndUnprovenClassificationFailVisible();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d union visibility checks failed\n", g_failures);
    return 1;
  }
  std::puts("war3_union_consumer_visibility_test: PASS");
  return 0;
}
