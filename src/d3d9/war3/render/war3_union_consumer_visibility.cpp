#include "war3_union_consumer_visibility.h"

#include <cmath>

namespace dxvk::war3::render {

namespace {

constexpr float kMinimumClipW = 1.0e-6f;

bool IsSupportedMode(War3UnionVisibilityMode mode) {
  return mode == War3UnionVisibilityMode::Off ||
      mode == War3UnionVisibilityMode::Observe ||
      mode == War3UnionVisibilityMode::Consume;
}

float RowLength3(const War3UnionMatrix4& matrix, uint32_t row) {
  const float x = matrix.columns[0][row];
  const float y = matrix.columns[1][row];
  const float z = matrix.columns[2][row];
  return std::sqrt(x * x + y * y + z * z);
}

void TransformPoint(const War3UnionMatrix4& matrix,
                    const War3UnionBoundsSphere& bounds,
                    float (&clip)[4]) {
  const float point[4] = {bounds.x, bounds.y, bounds.z, 1.0f};
  for (uint32_t row = 0u; row < 4u; ++row) {
    clip[row] = 0.0f;
    for (uint32_t column = 0u; column < 4u; ++column)
      clip[row] += matrix.columns[column][row] * point[column];
  }
}

} // namespace

bool War3UnionIsFiniteBounds(const War3UnionBoundsSphere& bounds) {
  return std::isfinite(bounds.x) && std::isfinite(bounds.y) &&
      std::isfinite(bounds.z) && std::isfinite(bounds.radius);
}

bool War3UnionIsFiniteMatrix(const War3UnionMatrix4& matrix) {
  for (uint32_t column = 0u; column < 4u; ++column) {
    for (uint32_t row = 0u; row < 4u; ++row) {
      if (!std::isfinite(matrix.columns[column][row]))
        return false;
    }
  }
  return true;
}

War3UnionVisibilityDecision War3UnionMakeFailVisibleDecision(
    War3UnionConsumerMask requestedMask,
    War3UnionVisibilityRejectReason reason,
    uint32_t proofBits) {
  War3UnionVisibilityDecision result = {};
  result.requestedMask = requestedMask;
  result.predictedVisibleMask = requestedMask;
  result.effectiveVisibleMask = requestedMask;
  result.proofBits = proofBits;
  result.rejectReason = reason;
  result.failVisible = true;
  return result;
}

War3UnionVisibilityDecision War3EvaluateConservativeCsmSphere(
    const War3UnionCsmSphereQuery& query) {
  War3UnionVisibilityDecision result = War3UnionMakeFailVisibleDecision(
      query.requestedMask, War3UnionVisibilityRejectReason::None);

  if (!IsSupportedMode(query.mode)) {
    result.rejectReason = War3UnionVisibilityRejectReason::UnsupportedMode;
    return result;
  }
  if (query.mode == War3UnionVisibilityMode::Off) {
    result.rejectReason = War3UnionVisibilityRejectReason::ModeOff;
    return result;
  }
  result.proofBits |= War3UnionProofModeEnabled;

  const War3UnionConsumerMask consumerBit =
      War3UnionCsmConsumerBit(query.cascadeIndex);
  if (consumerBit == 0u) {
    result.rejectReason = War3UnionVisibilityRejectReason::InvalidCascade;
    return result;
  }
  if ((query.requestedMask & consumerBit) == 0u) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::ConsumerNotRequested;
    return result;
  }

  // The first admission phase never culls the two near cascades.
  if (query.cascadeIndex < 2u) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::NearCascadeConservative;
    return result;
  }
  result.proofBits |= War3UnionProofFarCascade;

  if (query.dynamic || query.skinned) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::DynamicOrSkinned;
    return result;
  }
  if (!query.staticRigidProven) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::StaticRigidUnproven;
    return result;
  }
  result.proofBits |= War3UnionProofStaticRigid;

  if (!query.identityKnown) {
    result.rejectReason = War3UnionVisibilityRejectReason::UnknownIdentity;
    return result;
  }
  result.proofBits |= War3UnionProofIdentityKnown;

  if (!query.exactCurrentFrameSource) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::SourceNotExactCurrentFrame;
    return result;
  }
  result.proofBits |= War3UnionProofExactCurrentSource;

  const auto& generation = query.generations;
  if (generation.currentFrameGeneration == 0u) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::CurrentFrameUnknown;
    return result;
  }
  if (generation.candidateFrameGeneration !=
      generation.currentFrameGeneration) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::CandidateGenerationStale;
    return result;
  }
  result.proofBits |= War3UnionProofCandidateCurrent;

  if (!query.boundsKnown) {
    result.rejectReason = War3UnionVisibilityRejectReason::BoundsUnknown;
    return result;
  }
  if (generation.boundsFrameGeneration !=
      generation.currentFrameGeneration) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::BoundsGenerationStale;
    return result;
  }
  result.proofBits |= War3UnionProofBoundsCurrent;

  if (!query.cameraKnown) {
    result.rejectReason = War3UnionVisibilityRejectReason::CameraUnknown;
    return result;
  }
  if (generation.cameraFrameGeneration !=
      generation.currentFrameGeneration) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::CameraGenerationStale;
    return result;
  }
  result.proofBits |= War3UnionProofCameraCurrent;

  if (!query.consumerStateKnown) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::ConsumerStateUnknown;
    return result;
  }
  if (generation.consumerStateFrameGeneration !=
      generation.currentFrameGeneration) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::ConsumerStateGenerationStale;
    return result;
  }
  result.proofBits |= War3UnionProofConsumerStateCurrent;

  if (generation.resourceGeneration == 0u ||
      generation.expectedResourceGeneration == 0u) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::ResourceGenerationUnknown;
    return result;
  }
  if (generation.resourceGeneration !=
      generation.expectedResourceGeneration) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::ResourceGenerationMismatch;
    return result;
  }
  result.proofBits |= War3UnionProofResourceCurrent;

  if (!query.matrixKnown) {
    result.rejectReason = War3UnionVisibilityRejectReason::MatrixUnknown;
    return result;
  }
  if (!War3UnionIsFiniteBounds(query.bounds)) {
    result.rejectReason = War3UnionVisibilityRejectReason::NonFiniteBounds;
    return result;
  }
  if (!(query.bounds.radius > 0.0f)) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::InvalidBoundsRadius;
    return result;
  }
  result.proofBits |= War3UnionProofFiniteBounds;

  if (!War3UnionIsFiniteMatrix(query.lightViewProjection)) {
    result.rejectReason = War3UnionVisibilityRejectReason::NonFiniteMatrix;
    return result;
  }
  result.proofBits |= War3UnionProofFiniteMatrix;

  if (!std::isfinite(query.radiusScale) || query.radiusScale < 1.0f ||
      !std::isfinite(query.guardBandNdc) || query.guardBandNdc < 0.0f ||
      !std::isfinite(query.depthGuardBandNdc) ||
      query.depthGuardBandNdc < 0.0f) {
    result.rejectReason = War3UnionVisibilityRejectReason::InvalidGuardBand;
    return result;
  }

  float clip[4] = {};
  TransformPoint(query.lightViewProjection, query.bounds, clip);
  const float absW = std::abs(clip[3]);
  if (!std::isfinite(absW) || !(absW > kMinimumClipW)) {
    result.rejectReason = War3UnionVisibilityRejectReason::DegenerateClipW;
    return result;
  }

  const float row0Length = RowLength3(query.lightViewProjection, 0u);
  const float row1Length = RowLength3(query.lightViewProjection, 1u);
  const float row2Length = RowLength3(query.lightViewProjection, 2u);
  const float scaledRadius = query.bounds.radius * query.radiusScale;
  const float invW = 1.0f / absW;
  const float ndcX = clip[0] * invW;
  const float ndcY = clip[1] * invW;
  const float ndcZ = clip[2] * invW;
  const float radiusX = scaledRadius * row0Length * invW;
  const float radiusY = scaledRadius * row1Length * invW;
  const float radiusZ = scaledRadius * row2Length * invW;
  if (!std::isfinite(row0Length) || !std::isfinite(row1Length) ||
      !std::isfinite(row2Length) || !std::isfinite(scaledRadius) ||
      !std::isfinite(invW) || !std::isfinite(ndcX) ||
      !std::isfinite(ndcY) || !std::isfinite(ndcZ) ||
      !std::isfinite(radiusX) || !std::isfinite(radiusY) ||
      !std::isfinite(radiusZ)) {
    result.rejectReason =
        War3UnionVisibilityRejectReason::NonFiniteProjection;
    return result;
  }
  result.proofBits |= War3UnionProofFiniteProjection;

  const float guard = query.guardBandNdc;
  const float depthGuard = query.depthGuardBandNdc;
  const bool outside =
      ndcX + radiusX < -1.0f - guard ||
      ndcX - radiusX > 1.0f + guard ||
      ndcY + radiusY < -1.0f - guard ||
      ndcY - radiusY > 1.0f + guard ||
      ndcZ + radiusZ < -depthGuard ||
      ndcZ - radiusZ > 1.0f + depthGuard;
  if (!outside) {
    result.failVisible = false;
    return result;
  }

  result.proofBits |= War3UnionProofOutside;
  result.predictedVisibleMask &= ~consumerBit;
  result.provenInvisibleMask |= consumerBit;
  result.failVisible = false;

  if (query.mode == War3UnionVisibilityMode::Consume) {
    if (!query.consumeAdmissionGranted) {
      result.rejectReason =
          War3UnionVisibilityRejectReason::ConsumeNotAdmitted;
      result.failVisible = true;
      return result;
    }
    result.consumeAllowed = true;
    result.effectiveVisibleMask = result.predictedVisibleMask;
  }

  return result;
}

} // namespace dxvk::war3::render
