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
  draw.sourceVertexFirst = 102u;
  draw.sourceFirstIndex = 9u;
  draw.actualIndexMin = 2u;
  draw.actualIndexMax = 5u;
  draw.positionStride = 32u;
  draw.baseVertexIndex = 100;
  draw.requested = true;
  draw.sealed = true;
  draw.rigidStatic = true;
  draw.opaqueMaterial = true;
  draw.indexed = true;
  draw.triangleList = true;
  draw.uint16Indices = true;
  draw.exactIndexDomainKnown = true;
  draw.exactContiguousVertexRange = true;
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
  package.primitiveCount = 3u;
  package.primitiveOrdinal = 1u;
  package.primitiveFirstIndex = 4u;
  package.primitiveIndexCount = 6u;
  package.primitiveMinVertex = 2u;
  package.primitiveMaxVertex = 5u;
  package.ready = true;
  package.frozenPayloadValid = true;
  package.snapshotIdentityExact = true;
  package.primitiveSelected = true;
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
  const uint64_t packedSubrangeHash = HashPersistentGpuPackageContent(
      packed.data() + 3u, 6u * sizeof(float));
  const uint64_t stridedSubrangeHash =
      HashPersistentGpuPackageStridedFloat3(
          interleaved.data() + 1u, 2u * sizeof(InterleavedVertex), 2u,
          sizeof(InterleavedVertex), 0u);
  assert(packedSubrangeHash != 0u &&
         packedSubrangeHash == stridedSubrangeHash);
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
  draw.positionHostCached = false;
  draw.positionBoundedObserveReadable = true;
  draw.boundedUncachedPositionCopy = true;
  draw.boundedPositionCopyBytes = 128u;
  draw.boundedPositionCopyTicks = 2u;
  draw.indexHostCached = false;
  draw.indexBoundedObserveReadable = true;
  draw.boundedUncachedIndexScan = true;
  draw.boundedIndexScanBytes = 12u;
  draw.boundedIndexScanTicks = 3u;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::ExactMatch);
  draw = MakeDraw();
  package.primitiveSelected = false;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::PrimitiveSelectionRejected);
  package = MakePackage();
  package.vertexCount = 3u;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::PackageLayoutMismatch);
  package = MakePackage();
  package.positionContentHash ^= 1u;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::PositionContentMismatch);
  package = MakePackage();
  package.primitiveMaxVertex = 2u;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::PrimitiveMismatch);
  draw = MakeDraw();
  draw.exactContiguousVertexRange = false;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(
             draw, MakePackage()) == Disposition::GeometryContractRejected);
  draw.requested = false;
  assert(EvaluatePersistentGpuPackageCurrentDrawEquivalence(draw, package) ==
         Disposition::NotRequested);
  return 0;
}
