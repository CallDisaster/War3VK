#include "war3_volumetric_shader_work_admission.h"

#include <limits>

namespace dxvk::war3::render {

bool War3VolumetricCheckedAdd(uint64_t lhs, uint64_t rhs,
                             uint64_t& out) noexcept {
  if (lhs > std::numeric_limits<uint64_t>::max() - rhs)
    return false;
  out = lhs + rhs;
  return true;
}

bool War3VolumetricCheckedMultiply(uint64_t lhs, uint64_t rhs,
                                  uint64_t& out) noexcept {
  if (lhs != 0u && rhs > std::numeric_limits<uint64_t>::max() / lhs)
    return false;
  out = lhs * rhs;
  return true;
}

War3VolumetricShaderWorkEstimate EvaluateWar3VolumetricShaderWork(
    const War3VolumetricShaderWorkRequest& request,
    const War3VolumetricShaderWorkLimits& limits) noexcept {
  War3VolumetricShaderWorkEstimate result = {};
  result.request = request;
  result.limits = limits;

  const bool directionalTupleValid =
      request.directionalProbeCount == 0u
          ? request.directionalCascadeSamples == 0u &&
                request.directionalTapsPerCascade == 0u
          : request.directionalCascadeSamples != 0u &&
                request.directionalTapsPerCascade != 0u;
  const bool pointTupleValid =
      request.shadowedPointLightCount == 0u
          ? request.pointProbesPerLight == 0u
          : request.pointProbesPerLight != 0u;
  if (!limits.valid() || request.rayWidth == 0u ||
      request.rayHeight == 0u || request.sampleCount == 0u ||
      request.compositeWidth == 0u || request.compositeHeight == 0u ||
      request.compositeTextureReadsPerPixel == 0u ||
      !directionalTupleValid || !pointTupleValid) {
    result.rejectReason =
        War3VolumetricShaderWorkRejectReason::InvalidRequest;
    return result;
  }

  uint64_t pointReadsPerSegment = 0u;
  if (!War3VolumetricCheckedMultiply(
          request.rayWidth, request.rayHeight, result.rayPixels) ||
      !War3VolumetricCheckedMultiply(
          result.rayPixels, request.sampleCount, result.raySegments) ||
      !War3VolumetricCheckedMultiply(
          result.raySegments, request.directionalProbeCount,
          result.directionalProbeEvaluations) ||
      !War3VolumetricCheckedMultiply(
          result.directionalProbeEvaluations,
          request.directionalCascadeSamples,
          result.directionalCascadeEvaluations) ||
      !War3VolumetricCheckedMultiply(
          result.directionalCascadeEvaluations,
          request.directionalTapsPerCascade,
          result.directionalD32Reads) ||
      !War3VolumetricCheckedMultiply(
          request.shadowedPointLightCount,
          request.pointProbesPerLight, pointReadsPerSegment) ||
      !War3VolumetricCheckedMultiply(
          result.raySegments, pointReadsPerSegment,
          result.pointCubeReads) ||
      !War3VolumetricCheckedMultiply(
          request.compositeWidth, request.compositeHeight,
          result.compositePixels) ||
      !War3VolumetricCheckedMultiply(
          result.compositePixels,
          request.compositeTextureReadsPerPixel,
          result.compositeTextureReads)) {
    result.rejectReason =
        War3VolumetricShaderWorkRejectReason::ArithmeticOverflow;
    return result;
  }

  // The ray shader performs one full-resolution scene-depth fetch per effect
  // fragment before entering the bounded march.
  result.rayDepthReads = result.rayPixels;
  uint64_t shadowReads = 0u;
  if (!War3VolumetricCheckedAdd(
          result.rayDepthReads, result.directionalD32Reads, shadowReads) ||
      !War3VolumetricCheckedAdd(
          shadowReads, result.pointCubeReads, shadowReads) ||
      !War3VolumetricCheckedAdd(
          shadowReads, result.compositeTextureReads,
          result.totalTextureReads) ||
      !War3VolumetricCheckedAdd(
          result.raySegments, result.totalTextureReads,
          result.totalWork)) {
    result.rejectReason =
        War3VolumetricShaderWorkRejectReason::ArithmeticOverflow;
    return result;
  }

  if (result.raySegments > limits.maxRaySegments) {
    result.rejectReason =
        War3VolumetricShaderWorkRejectReason::RaySegmentBudget;
    return result;
  }
  if (result.totalWork > limits.maxTotalWork) {
    result.rejectReason =
        War3VolumetricShaderWorkRejectReason::TotalWorkBudget;
    return result;
  }

  result.accepted = true;
  return result;
}

} // namespace dxvk::war3::render
