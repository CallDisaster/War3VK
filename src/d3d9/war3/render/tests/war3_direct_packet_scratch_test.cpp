#include "../war3_direct_packet_scratch.h"

#include <iostream>
#include <memory>

namespace {

using dxvk::war3::render::CurrentDrawAuthoritativeSample;
using dxvk::war3::render::AcquireScratchElement;
using dxvk::war3::render::RecycleScratchElements;
using dxvk::war3::render::ResetCurrentDrawAuthoritativeSamplePreserveScratch;
using dxvk::war3::render::ResetShadowDrawPacketPreserveScratch;
using dxvk::war3::shadow::ShadowDrawPacket;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_direct_packet_scratch_test: " << message << '\n';
  return condition;
}

bool testPacketReset() {
  ShadowDrawPacket packet = {};
  packet.renderable.renderablePart = reinterpret_cast<void*>(uintptr_t(0x10u));
  packet.resource.modelKey = 0x1234u;
  packet.resource.mapEpoch = 7u;
  packet.resource.dynamicIndexStream =
      reinterpret_cast<const uint16_t*>(uintptr_t(0x20u));
  packet.resource.dynamicIndexCount = 4u;
  packet.resource.dynamicIndexBackedByResourceKeepAlive = true;
  packet.resource.resourceKeepAlive = std::make_shared<uint32_t>(17u);
  packet.resource.ownedDynamicIndices =
      std::make_shared<const std::vector<uint16_t>>(
          std::vector<uint16_t>{1u, 2u, 3u});
  packet.resource.ownedPositions.reserve(64u);
  packet.resource.ownedPositions.push_back(1.0f);
  packet.resource.ownedVertexGroupIndices.reserve(96u);
  packet.resource.ownedVertexGroupIndices.push_back(2u);
  packet.resource.ownedVertexBlendWeights.reserve(48u);
  packet.resource.ownedVertexBlendWeights.push_back({1.0f, 0.0f, 0.0f});
  packet.resource.ownedVertexBlendIndices.reserve(48u);
  packet.resource.ownedVertexBlendIndices.push_back({0u, 1u, 2u, 3u});
  packet.resource.ownedIndices.reserve(80u);
  packet.resource.ownedIndices.push_back(1u);
  packet.resource.ownedMatrixGroupSizes.reserve(16u);
  packet.resource.ownedMatrixGroupSizes.push_back(1u);
  packet.resource.ownedMatrixIndices.reserve(32u);
  packet.resource.ownedMatrixIndices.push_back(1u);
  packet.pose.matrixPalette.reserve(12u);
  packet.pose.matrixPalette.emplace_back();
  packet.runtimeGroupPalette.reserve(24u);
  packet.runtimeGroupPalette.emplace_back();
  packet.resource.positions = &packet.resource.ownedPositions;
  packet.resource.vertexGroupIndices =
      &packet.resource.ownedVertexGroupIndices;

  const size_t positionCapacity = packet.resource.ownedPositions.capacity();
  const size_t groupCapacity =
      packet.resource.ownedVertexGroupIndices.capacity();
  const size_t blendWeightCapacity =
      packet.resource.ownedVertexBlendWeights.capacity();
  const size_t blendIndexCapacity =
      packet.resource.ownedVertexBlendIndices.capacity();
  const size_t indexCapacity = packet.resource.ownedIndices.capacity();
  const size_t groupSizeCapacity =
      packet.resource.ownedMatrixGroupSizes.capacity();
  const size_t matrixIndexCapacity =
      packet.resource.ownedMatrixIndices.capacity();
  const size_t poseCapacity = packet.pose.matrixPalette.capacity();
  const size_t runtimeCapacity = packet.runtimeGroupPalette.capacity();

  ResetShadowDrawPacketPreserveScratch(packet);

  return require(packet.renderable.renderablePart == nullptr,
                 "renderable identity survived reset") &&
      require(packet.resource.modelKey == 0u && packet.resource.mapEpoch == 0u,
              "resource generation survived reset") &&
      require(packet.resource.positions == nullptr &&
                  packet.resource.vertexGroupIndices == nullptr &&
                  packet.resource.dynamicIndexStream == nullptr,
              "raw pointer alias survived reset") &&
      require(packet.resource.dynamicIndexCount == 0u &&
                  !packet.resource.dynamicIndexBackedByResourceKeepAlive,
              "dynamic index provenance survived reset") &&
      require(!packet.resource.resourceKeepAlive &&
                  !packet.resource.ownedDynamicIndices,
              "shared owner survived reset") &&
      require(packet.resource.ownedPositions.empty() &&
                  packet.resource.ownedVertexGroupIndices.empty() &&
                  packet.resource.ownedVertexBlendWeights.empty() &&
                  packet.resource.ownedVertexBlendIndices.empty() &&
                  packet.resource.ownedIndices.empty() &&
                  packet.resource.ownedMatrixGroupSizes.empty() &&
                  packet.resource.ownedMatrixIndices.empty() &&
                  packet.pose.matrixPalette.empty() &&
                  packet.runtimeGroupPalette.empty(),
              "owned content survived reset") &&
      require(packet.resource.ownedPositions.capacity() >= positionCapacity &&
                  packet.resource.ownedVertexGroupIndices.capacity() >=
                      groupCapacity &&
                  packet.resource.ownedVertexBlendWeights.capacity() >=
                      blendWeightCapacity &&
                  packet.resource.ownedVertexBlendIndices.capacity() >=
                      blendIndexCapacity &&
                  packet.resource.ownedIndices.capacity() >= indexCapacity &&
                  packet.resource.ownedMatrixGroupSizes.capacity() >=
                      groupSizeCapacity &&
                  packet.resource.ownedMatrixIndices.capacity() >=
                      matrixIndexCapacity &&
                  packet.pose.matrixPalette.capacity() >= poseCapacity &&
                  packet.runtimeGroupPalette.capacity() >= runtimeCapacity,
              "vector capacity was not retained");
}

bool testSampleReset() {
  CurrentDrawAuthoritativeSample sample = {};
  sample.contract.known = true;
  sample.contract.renderablePart = reinterpret_cast<void*>(uintptr_t(0x30u));
  sample.palette.reserve(20u);
  sample.palette.emplace_back();
  sample.groupSlots.reserve(128u);
  sample.groupSlots.push_back(3u);
  sample.paletteCount = 1u;
  sample.paletteHash = 0x55u;
  sample.groupHash = 0x66u;
  sample.maxGroupSlot = 3u;

  const size_t paletteCapacity = sample.palette.capacity();
  const size_t groupCapacity = sample.groupSlots.capacity();
  ResetCurrentDrawAuthoritativeSamplePreserveScratch(sample);

  return require(!sample.contract.known &&
                     sample.contract.renderablePart == nullptr,
                 "sample identity survived reset") &&
      require(sample.palette.empty() && sample.groupSlots.empty(),
              "sample content survived reset") &&
      require(sample.paletteCount == 0u && sample.paletteHash == 0u &&
                  sample.groupHash == 0u && sample.maxGroupSlot == 0u,
              "sample scalar state survived reset") &&
      require(sample.palette.capacity() >= paletteCapacity &&
                  sample.groupSlots.capacity() >= groupCapacity,
              "sample capacity was not retained");
}

bool testElementRecycler() {
  std::vector<ShadowDrawPacket> live;
  std::vector<ShadowDrawPacket> recycled;
  live.reserve(4u);
  live.emplace_back();
  live.back().runtimeGroupPalette.reserve(18u);
  live.back().runtimeGroupPalette.emplace_back();
  live.back().renderable.sceneNode =
      reinterpret_cast<void*>(uintptr_t(0x40u));
  const size_t outerCapacity = live.capacity();
  const size_t paletteCapacity = live.back().runtimeGroupPalette.capacity();

  RecycleScratchElements(live, recycled);
  if (!require(live.empty(), "live vector was not cleared by swap") ||
      !require(recycled.size() == 1u,
               "live element was not transferred to recycler")) {
    return false;
  }

  ShadowDrawPacket packet = AcquireScratchElement(recycled);
  if (!require(recycled.empty(), "acquired element remained in recycler"))
    return false;
  ResetShadowDrawPacketPreserveScratch(packet);
  live.push_back(std::move(packet));
  RecycleScratchElements(live, recycled);
  const bool outerWarm = live.capacity() >= outerCapacity;
  packet = AcquireScratchElement(recycled);
  ResetShadowDrawPacketPreserveScratch(packet);
  return require(outerWarm,
                 "double-buffered outer allocation did not warm up") &&
      require(packet.renderable.sceneNode == nullptr,
                 "recycled identity survived explicit reset") &&
      require(packet.runtimeGroupPalette.empty() &&
                  packet.runtimeGroupPalette.capacity() >= paletteCapacity,
              "recycled palette scratch was not retained");
}

} // namespace

int main() {
  return testPacketReset() && testSampleReset() && testElementRecycler() ? 0
                                                                         : 1;
}
