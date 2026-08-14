#pragma once

#include <cstdint>

namespace dxvk::war3::render {

enum class War3VolumetricShaderWorkRejectReason : uint32_t {
  None = 0u,
  InvalidRequest = 1u,
  ArithmeticOverflow = 2u,
  RaySegmentBudget = 3u,
  TotalWorkBudget = 4u,
};

struct War3VolumetricShaderWorkLimits {
  // Preserve the existing ray-march loop bound independently from texture
  // work. The shader still performs substantial arithmetic in every segment.
  uint64_t maxRaySegments = 4'000'000ull;
  // One work unit is either one ray segment or one shader texture access from
  // the bounded model below. 350 Mi keeps the 1080p release default eligible
  // after the directional-guide reconstruction raised the composite bound,
  // while rejecting the known 1440p/4K volume-sun worst cases.
  uint64_t maxTotalWork = 350ull * 1024ull * 1024ull;

  bool valid() const noexcept {
    return maxRaySegments != 0u && maxTotalWork != 0u;
  }
};

struct War3VolumetricShaderWorkRequest {
  uint32_t rayWidth = 0u;
  uint32_t rayHeight = 0u;
  uint32_t sampleCount = 0u;

  // Per ray segment, directional shadowing performs up to probeCount probes.
  // Each probe can sample cascadeSamples cascades and tapsPerCascade D32
  // texels. Zero probes means the directional shadow branch is disabled.
  uint32_t directionalProbeCount = 0u;
  uint32_t directionalCascadeSamples = 0u;
  uint32_t directionalTapsPerCascade = 0u;

  // Each shadowed point light performs pointProbesPerLight cube lookups per
  // ray segment. Zero lights means the point-shadow branch is disabled.
  uint32_t shadowedPointLightCount = 0u;
  uint32_t pointProbesPerLight = 0u;

  uint32_t compositeWidth = 0u;
  uint32_t compositeHeight = 0u;
  // Source-proven conservative bound for the composite shader. The normal
  // guide-aware low-resolution path is 24 accesses per full-resolution
  // fragment in the conservative branch model.
  uint32_t compositeTextureReadsPerPixel = 0u;
};

struct War3VolumetricShaderWorkEstimate {
  War3VolumetricShaderWorkRequest request = {};
  War3VolumetricShaderWorkLimits limits = {};
  War3VolumetricShaderWorkRejectReason rejectReason =
      War3VolumetricShaderWorkRejectReason::None;

  uint64_t rayPixels = 0u;
  uint64_t raySegments = 0u;
  uint64_t rayDepthReads = 0u;
  uint64_t directionalProbeEvaluations = 0u;
  uint64_t directionalCascadeEvaluations = 0u;
  uint64_t directionalD32Reads = 0u;
  uint64_t pointCubeReads = 0u;
  uint64_t compositePixels = 0u;
  uint64_t compositeTextureReads = 0u;
  uint64_t totalTextureReads = 0u;
  uint64_t totalWork = 0u;
  bool accepted = false;
};

bool War3VolumetricCheckedAdd(uint64_t lhs, uint64_t rhs,
                             uint64_t& out) noexcept;
bool War3VolumetricCheckedMultiply(uint64_t lhs, uint64_t rhs,
                                  uint64_t& out) noexcept;

War3VolumetricShaderWorkEstimate EvaluateWar3VolumetricShaderWork(
    const War3VolumetricShaderWorkRequest& request,
    const War3VolumetricShaderWorkLimits& limits = {}) noexcept;

} // namespace dxvk::war3::render
