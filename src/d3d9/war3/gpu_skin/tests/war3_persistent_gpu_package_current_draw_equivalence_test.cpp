#include "../war3_persistent_gpu_package_immutable.h"

#include <array>
#include <cassert>
#include <cstdint>

using namespace dxvk::war3::gpu_skin;

namespace {

PersistentGpuPackageCurrentDrawProof MakeDraw() {
  PersistentGpuPackageCurrentDrawProof draw = {};
  draw.frameSerial = 7u;
  draw.mapEpoch = 11u;
  draw.deviceEpoch = 13u;
  draw.exactGeometryKeyHash = 17u;
  draw.instanceIdentity = 19u;
  draw.meshPayloadIdentity = 23u;
  draw.renderablePartIdentity = 29u;
  draw.positionOwnerIdentity = 31u;
  draw.positionIdentityGeneration = 37u;
  draw.positionAllocationGeneration = 41u;
  draw.positionContentGeneration = 43u;
  draw.indexOwnerIdentity = 47u;
  draw.indexIdentityGeneration = 53u;
  draw.indexAllocationGeneration = 59u;
  draw.indexContentGeneration = 61u;
  draw.positionContentHash = 67u;
  draw.indexContentHash = 71u;
  draw.vertexCount = 4u;
  draw.indexCount = 6u;
  draw.actualIndexMax = 3u;
  draw.positionStride = 32u;
  draw.requested = true;
  draw.sealed = true;
  draw.rigidStatic = true;
  draw.opaqueMaterial = true;
  draw.indexed = true;
  draw.triangleList = true;
  draw.uint16Indices = true;
  draw.exactIndexDomainKnown = true;
  draw.zeroBasedVertexRange = true;
  draw.positionFloat3 = true;
  draw.positionHostCached = true;
  draw.indexHostCached = true;
  return draw;
}

PersistentGpuPackageCurrentDrawPackageProof MakePackage() {
  PersistentGpuPackageCurrentDrawPackageProof package = {};
  package.mapEpoch = 11u;
  package.deviceEpoch = 13u;
  package.packageGeneration = 73u;
  package.geosetDataIdentity = 23u;
  package.positionContentHash = 67u;
  package.indexContentHash = 71u;
  package.vertexCount = 4u;
  package.indexCount = 6u;
  package.primitiveCount = 1u;
  package.primitiveIndexCount = 6u;
  package.primitiveMaxVertex = 3u;
  package.ready = true;
  package.frozenPayloadValid = true;
  package.snapshotIdentityExact = true;
  return package;
}

}  // namespace

int main() {
  const std::array<float, 12> packed = {
      0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f,
      6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f};
  struct InterleavedVertex {
    float xyz[3];
    float ignored[2];
  };
  std::array<InterleavedVertex, 4> interleaved = {};
  for (size_t i = 0u; i < interleaved.size(); ++i) {
    for (size_t component = 0u; component < 3u; ++component)
      interleaved[i].xyz[component] = packed[i * 3u + component];
    interleaved[i].ignored[0] = 100.0f + float(i);
    interleaved[i].ignored[1] = 200.0f + float(i);
  }
  const uint64_t packedHash = HashPersistentGpuPackageContent(
      packed.data(), packed.size() * sizeof(float));
  const uint64_t stridedHash = HashPersistentGpuPackageStridedFloat3(
      interleaved.data(), sizeof(interleaved), uint32_t(interleaved.size()),
      sizeof(InterleavedVertex), 0u);
  assert(packedHash != 0u && packedHash == stridedHash);
  assert(HashPersistentGpuPackageStridedFloat3(
             interleaved.data(), sizeof(interleaved) - 9u,
             uint32_t(interleaved.size()), sizeof(InterleavedVertex), 0u) ==
         0u);

  using Disposition = PersistentGpuPackageCurrentDrawMatchDisposition;
  auto draw = MakeDraw();
  auto package = MakePackage();
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::ExactMatch);
  draw.positionHostCached = false;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::CpuSourceUnavailable);
  draw = MakeDraw();
  package.primitiveCount = 2u;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::MultiPrimitiveRejected);
  package = MakePackage();
  package.positionContentHash ^= 1u;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::PositionContentMismatch);
  package = MakePackage();
  package.primitiveMaxVertex = 2u;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::PrimitiveMismatch);
  draw.requested = false;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::NotRequested);
  return 0;
}
