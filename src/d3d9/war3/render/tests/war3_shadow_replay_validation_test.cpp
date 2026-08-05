#include "../war3_shadow_replay_validation.h"

#include <cassert>
#include <limits>

using namespace dxvk::war3::render;

namespace {

War3ShadowReplayValidationInput ValidIndexed() {
  War3ShadowReplayValidationInput input = {};
  input.expectedMapEpoch = 7u;
  input.expectedDeviceEpoch = 3u;
  input.drawMapEpoch = 7u;
  input.drawDeviceEpoch = 3u;
  input.worldMatrixFinite = true;
  input.position = {true, 1200u, 12u, 0u, 12u};
  input.indexed = true;
  input.indexBufferPresent = true;
  input.indexBufferSize = 12u;
  input.indexTypeBytes = 2u;
  input.indexCount = 6u;
  input.numVertices = 4u;
  input.actualIndexDomainKnown = true;
  input.actualIndexMin = 0u;
  input.actualIndexMax = 3u;
  return input;
}

} // namespace

int main() {
  auto input = ValidIndexed();
  assert(ValidateWar3ShadowReplayDraw(input));

  input.drawMapEpoch = 6u;
  assert(ValidateWar3ShadowReplayDraw(input).reason ==
         War3ShadowReplayRejectReason::StaleMapEpoch);
  input = ValidIndexed();
  input.indexTypeBytes = 1u;
  assert(ValidateWar3ShadowReplayDraw(input).reason ==
         War3ShadowReplayRejectReason::InvalidIndexType);
  input = ValidIndexed();
  input.firstIndex = std::numeric_limits<uint32_t>::max();
  assert(ValidateWar3ShadowReplayDraw(input).reason ==
         War3ShadowReplayRejectReason::IndexRangeOutOfBounds);
  input = ValidIndexed();
  input.vertexOffset = -1;
  assert(ValidateWar3ShadowReplayDraw(input).reason ==
         War3ShadowReplayRejectReason::NegativeVertexDomain);
  input = ValidIndexed();
  input.position.size = 36u;
  assert(ValidateWar3ShadowReplayDraw(input).reason ==
         War3ShadowReplayRejectReason::PositionRangeOutOfBounds);
  input = ValidIndexed();
  input.actualIndexDomainKnown = false;
  input.fullVertexDomainFallback = true;
  input.vertexOffset = 4096;
  input.numVertices = 16u;
  input.position.size = 16u * input.position.stride;
  assert(ValidateWar3ShadowReplayDraw(input));
  input = ValidIndexed();
  input.blendRequired = true;
  assert(ValidateWar3ShadowReplayDraw(input).reason ==
         War3ShadowReplayRejectReason::MissingBlendBuffer);
  input = ValidIndexed();
  input.uvRequired = true;
  input.uv = {true, 16u, 8u, 0u, 8u};
  assert(ValidateWar3ShadowReplayDraw(input).reason ==
         War3ShadowReplayRejectReason::UvRangeOutOfBounds);
  input = ValidIndexed();
  input.gpuSkinRequired = true;
  input.gpuSkinLeaseValid = true;
  input.gpuSkinMapEpoch = input.expectedMapEpoch;
  input.gpuSkinDeviceEpoch = input.expectedDeviceEpoch;
  input.gpuSkinSourceSize = 64u;
  input.gpuSkinSourceOffset = 48u;
  input.gpuSkinSourceLength = 32u;
  input.gpuSkinPaletteSize = 64u;
  input.gpuSkinPaletteLength = 64u;
  assert(ValidateWar3ShadowReplayDraw(input).reason ==
         War3ShadowReplayRejectReason::GpuSkinSourceRangeOutOfBounds);

  auto nonIndexed = ValidIndexed();
  nonIndexed.indexed = false;
  nonIndexed.vertexCount = 4u;
  nonIndexed.firstVertex = 2u;
  nonIndexed.position.size = 72u;
  assert(ValidateWar3ShadowReplayDraw(nonIndexed));
  return 0;
}
