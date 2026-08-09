#include "../war3_volumetric_shader_work_admission.h"

#include <cassert>
#include <cstdint>
#include <limits>

using namespace dxvk::war3::render;

namespace {

War3VolumetricShaderWorkRequest DefaultVolumeSunRequest(
    uint32_t fullWidth, uint32_t fullHeight) {
  War3VolumetricShaderWorkRequest request = {};
  request.rayWidth = (fullWidth + 3u) / 4u;
  request.rayHeight = (fullHeight + 3u) / 4u;
  request.sampleCount = 16u;
  request.directionalProbeCount = 8u;
  request.directionalCascadeSamples = 2u;
  request.directionalTapsPerCascade = 9u;
  request.shadowedPointLightCount = 2u;
  request.pointProbesPerLight = 2u;
  request.compositeWidth = fullWidth;
  request.compositeHeight = fullHeight;
  request.compositeTextureReadsPerPixel = 10u;
  return request;
}

} // namespace

int main() {
  {
    const auto estimate =
        EvaluateWar3VolumetricShaderWork(DefaultVolumeSunRequest(1920u, 1080u));
    assert(estimate.accepted);
    assert(estimate.rayPixels == 129'600u);
    assert(estimate.raySegments == 2'073'600u);
    assert(estimate.directionalProbeEvaluations == 16'588'800u);
    assert(estimate.directionalCascadeEvaluations == 33'177'600u);
    assert(estimate.directionalD32Reads == 298'598'400u);
    assert(estimate.pointCubeReads == 8'294'400u);
    assert(estimate.compositeTextureReads == 20'736'000u);
    assert(estimate.totalWork == 329'832'000u);
    assert(estimate.totalWork < estimate.limits.maxTotalWork);
  }

  {
    const auto estimate =
        EvaluateWar3VolumetricShaderWork(DefaultVolumeSunRequest(2560u, 1440u));
    assert(!estimate.accepted);
    assert(estimate.rejectReason ==
           War3VolumetricShaderWorkRejectReason::TotalWorkBudget);
    assert(estimate.directionalD32Reads == 530'841'600u);
  }

  {
    auto request = DefaultVolumeSunRequest(3840u, 2160u);
    // The existing 4M segment gate reduces this configuration to seven
    // samples. The real texture/read budget must still reject it.
    request.sampleCount = 7u;
    const auto estimate = EvaluateWar3VolumetricShaderWork(request);
    assert(!estimate.accepted);
    assert(estimate.rejectReason ==
           War3VolumetricShaderWorkRejectReason::TotalWorkBudget);
    assert(estimate.raySegments == 3'628'800u);
    assert(estimate.directionalD32Reads == 522'547'200u);
    assert(estimate.compositeTextureReads == 82'944'000u);
  }

  {
    auto request = DefaultVolumeSunRequest(1920u, 1080u);
    request.directionalProbeCount = 0u;
    request.directionalCascadeSamples = 0u;
    request.directionalTapsPerCascade = 0u;
    request.shadowedPointLightCount = 0u;
    request.pointProbesPerLight = 0u;
    const auto estimate = EvaluateWar3VolumetricShaderWork(request);
    assert(estimate.accepted);
    assert(estimate.directionalD32Reads == 0u);
    assert(estimate.pointCubeReads == 0u);
  }

  {
    auto request = DefaultVolumeSunRequest(1920u, 1080u);
    request.directionalTapsPerCascade = 4u;
    request.shadowedPointLightCount = 0u;
    request.pointProbesPerLight = 0u;
    const auto estimate = EvaluateWar3VolumetricShaderWork(request);
    assert(estimate.accepted);
    assert(estimate.directionalD32Reads == 132'710'400u);
  }

  {
    auto request = DefaultVolumeSunRequest(1920u, 1080u);
    request.directionalCascadeSamples = 1u;
    request.shadowedPointLightCount = 0u;
    request.pointProbesPerLight = 0u;
    const auto estimate = EvaluateWar3VolumetricShaderWork(request);
    assert(estimate.accepted);
    assert(estimate.directionalD32Reads == 149'299'200u);
  }

  {
    uint64_t value = 0u;
    assert(!War3VolumetricCheckedMultiply(
        std::numeric_limits<uint64_t>::max(), 2u, value));
    assert(!War3VolumetricCheckedAdd(
        std::numeric_limits<uint64_t>::max(), 1u, value));
  }

  {
    auto request = DefaultVolumeSunRequest(1920u, 1080u);
    request.directionalCascadeSamples = 0u;
    const auto estimate = EvaluateWar3VolumetricShaderWork(request);
    assert(!estimate.accepted);
    assert(estimate.rejectReason ==
           War3VolumetricShaderWorkRejectReason::InvalidRequest);
  }

  return 0;
}
