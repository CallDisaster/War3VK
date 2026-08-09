#include "../war3_shadow_replay_validation.h"
#include "../war3_outline_mask_layout.h"

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
  // Indexed D3DVBF_0WEIGHTS has no explicit weight attribute, but the one
  // matrix index consumed by the vertex shader still needs a bounded stream.
  input = ValidIndexed();
  input.blendRequired = true;
  input.blend = {true, 16u, 4u, 0u, 4u};
  assert(ValidateWar3ShadowReplayDraw(input));
  input.blend.size = 12u;
  assert(ValidateWar3ShadowReplayDraw(input).reason ==
         War3ShadowReplayRejectReason::BlendRangeOutOfBounds);
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

  // Outline and every other replay consumer validate an immutable batch
  // before command recording. A malformed second draw rejects the entire
  // batch rather than granting permission to submit the valid prefix.
  War3ShadowReplayValidationInput batchInputs[2] = {
      ValidIndexed(), ValidIndexed()};
  auto batch = ValidateWar3ShadowReplayBatch(batchInputs, 2u);
  assert(batch);
  assert(batch.validatedCount == 2u);
  batchInputs[1].position.size = 1u;
  batch = ValidateWar3ShadowReplayBatch(batchInputs, 2u);
  assert(!batch);
  assert(batch.failureIndex == 1u);
  assert(batch.validatedCount == 1u);
  assert(batch.failure.reason ==
         War3ShadowReplayRejectReason::PositionRangeOutOfBounds);
  assert(!ValidateWar3ShadowReplayBatch(nullptr, 1u));

  // First use discards UNDEFINED contents. Every later frame performs a real
  // ShaderReadOnly -> ColorAttachment -> ShaderReadOnly round trip.
  auto begin = PlanWar3OutlineMaskBegin(
      War3OutlineMaskLayoutState::Undefined);
  assert(begin);
  assert(begin.discardContents);
  assert(begin.newState == War3OutlineMaskLayoutState::ColorAttachment);
  auto end = PlanWar3OutlineMaskEnd(begin.newState);
  assert(end);
  assert(end.newState == War3OutlineMaskLayoutState::ShaderReadOnly);
  begin = PlanWar3OutlineMaskBegin(end.newState);
  assert(begin);
  assert(!begin.discardContents);
  assert(!PlanWar3OutlineMaskBegin(begin.newState));
  assert(!PlanWar3OutlineMaskEnd(
      War3OutlineMaskLayoutState::Undefined));
  return 0;
}
