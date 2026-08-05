#include "../war3_persistent_gpu_package_immutable.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace dxvk::war3;
using namespace dxvk::war3::gpu_skin;

model::ShadowGeosetResourceRecord MakeRecord() {
  model::ShadowGeosetResourceRecord record = {};
  record.geosetPtr = reinterpret_cast<void*>(0x1000u);
  record.geosetDataPtr = reinterpret_cast<void*>(0x2000u);
  record.vertexCount = 3u;
  record.positions = { -1.0f, 0.0f, 0.0f,
                        1.0f, 0.0f, 0.0f,
                        0.0f, 2.0f, 0.0f };
  record.normalCount = 3u;
  record.normals = { 0.0f, 0.0f, 1.0f,
                     0.0f, 0.0f, 1.0f,
                     0.0f, 0.0f, 1.0f };
  record.vertexGroupCount = 3u;
  record.vertexGroupIndices = { 0u, 1u, 0u };
  record.uvLayerCount = 1u;
  record.uvLayers = { { 3u, { 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f } } };
  record.primitiveCount = 1u;
  record.primitiveRecords = { { 4u, 3u } };
  record.indexCount = 3u;
  record.indices = { 0u, 1u, 2u };
  record.matrixGroupCount = 2u;
  record.matrixGroupSizes = { 1u, 2u };
  record.matrixIndexCount = 3u;
  record.matrixIndices = { 3u, 7u, 11u };
  record.contentHash = 0x123456789abcdef0ull;
  record.immutableModelGeneration = 17u;
  record.immutableCaptureStatus =
      model::ShadowGeosetImmutableCaptureStatus::Complete;
  return record;
}

bool Build(const model::ShadowGeosetResourceRecord& record,
           PersistentGpuPackageImmutableProof& proof,
           std::vector<PersistentGpuPackagePrimitiveProof>& primitives) {
  return BuildPersistentGpuPackageImmutableProof(record, proof, primitives);
}

std::vector<uint8_t> Pack(
    const model::ShadowGeosetResourceRecord& record,
    const PersistentGpuPackageImmutableProof& proof,
    uint64_t& indexOffset) {
  indexOffset = (uint64_t(proof.staticByteSize) + 1u) & ~uint64_t(1u);
  std::vector<uint8_t> bytes(
      size_t(indexOffset) + record.indices.size() * sizeof(uint16_t), 0u);
  std::memcpy(bytes.data() + proof.positionOffset, record.positions.data(),
      record.positions.size() * sizeof(float));
  std::memcpy(bytes.data() + proof.normalOffset, record.normals.data(),
      record.normals.size() * sizeof(float));
  std::memcpy(bytes.data() + proof.groupSlotOffset,
      record.vertexGroupIndices.data(), record.vertexGroupIndices.size());
  if (record.uvLayerCount >= 1u) {
    std::memcpy(bytes.data() + proof.texcoord0Offset,
        record.uvLayers[0].uvPairs.data(),
        record.uvLayers[0].uvPairs.size() * sizeof(float));
  }
  if (record.uvLayerCount >= 2u) {
    std::memcpy(bytes.data() + proof.texcoord1Offset,
        record.uvLayers[1].uvPairs.data(),
        record.uvLayers[1].uvPairs.size() * sizeof(float));
  }
  std::memcpy(bytes.data() + indexOffset, record.indices.data(),
      record.indices.size() * sizeof(uint16_t));
  return bytes;
}

void TestGenerationAuthorityAndBitExactPayload() {
  model::ImmutableModelGenerationIssuer issuer;
  assert(issuer.issue() == 1u);
  assert(issuer.issue() == 2u);
  // Issuer value contract only; cache merge ordering is source-contract
  // covered because this runnable deliberately links no Game/cache object.
  assert(issuer.issue() == 3u);
  model::ImmutableModelGenerationIssuer exhausted(
      std::numeric_limits<uint64_t>::max());
  assert(exhausted.issue() == 0u);
  assert(exhausted.exhausted());

  auto lhs = MakeRecord();
  auto rhs = lhs;
  rhs.lastSeenFrame = 99u;
  rhs.modelKey = 22u;
  rhs.contentHash ^= 1u;
  assert(model::SameShadowGeosetImmutableConsumerPayload(lhs, rhs));

  rhs = lhs;
  rhs.positions[1] = -0.0f;
  assert(!model::SameShadowGeosetImmutableConsumerPayload(lhs, rhs));

  uint32_t nanBits = 0x7fc01234u;
  float nanValue = 0.0f;
  std::memcpy(&nanValue, &nanBits, sizeof(nanValue));
  lhs.positions[0] = nanValue;
  rhs = lhs;
  assert(model::SameShadowGeosetImmutableConsumerPayload(lhs, rhs));
}

void TestEveryImmutableClassChangesProof() {
  const auto base = MakeRecord();
  PersistentGpuPackageImmutableProof expected = {};
  std::vector<PersistentGpuPackagePrimitiveProof> expectedPrimitives;
  assert(Build(base, expected, expectedPrimitives));

  const auto changed = [&](model::ShadowGeosetResourceRecord record) {
    PersistentGpuPackageImmutableProof proof = {};
    std::vector<PersistentGpuPackagePrimitiveProof> primitives;
    assert(Build(record, proof, primitives));
    assert(!SamePersistentGpuPackageImmutableProof(expected, proof) ||
        primitives.size() != expectedPrimitives.size() ||
        !SamePersistentGpuPackagePrimitiveProof(
            expectedPrimitives[0], primitives[0]));
  };

  auto record = base; record.positions[0] += 0.25f; changed(record);
  record = base; record.normals[0] += 0.25f; changed(record);
  record = base; record.vertexGroupIndices[0] = 1u; changed(record);
  record = base; record.uvLayers[0].uvPairs[0] += 0.25f; changed(record);
  record = base; record.indices = { 0u, 2u, 1u }; changed(record);
  record = base;
  record.primitiveRecords[0].primitiveTypeOrMaterialSlot += 1u;
  changed(record);
  record = base; record.matrixGroupSizes = { 2u, 1u }; changed(record);
  record = base; record.matrixIndices[0] += 1u; changed(record);
  record = base; record.immutableModelGeneration += 1u; changed(record);
}

void TestFailureIsClosedAndClearsOutputs() {
  const auto base = MakeRecord();
  const auto rejected = [&](model::ShadowGeosetResourceRecord record) {
    PersistentGpuPackageImmutableProof proof = {};
    proof.contentHash = 1u;
    std::vector<PersistentGpuPackagePrimitiveProof> primitives(1u);
    assert(!Build(record, proof, primitives));
    assert(proof.contentHash == 0u);
    assert(primitives.empty());
  };

  auto record = base;
  record.immutableCaptureStatus =
      model::ShadowGeosetImmutableCaptureStatus::AttemptedFailed;
  rejected(record);
  record = base; record.uvLayers[0].uvCount = 2u; rejected(record);
  record = base; record.matrixGroupSizes = { 1u, 1u }; rejected(record);
  record = base; record.indices[2] = 9u; rejected(record);
  record = base;
  record.positions[0] = std::numeric_limits<float>::infinity();
  rejected(record);
  record = base;
  record.positions[0] = std::numeric_limits<float>::quiet_NaN();
  rejected(record);
  record = base;
  record.normals[0] = std::numeric_limits<float>::infinity();
  rejected(record);
  record = base;
  record.normals[0] = std::numeric_limits<float>::quiet_NaN();
  rejected(record);
  record = base;
  record.uvLayers[0].uvPairs[0] = std::numeric_limits<float>::infinity();
  rejected(record);
  record = base;
  record.uvLayers[0].uvPairs[0] =
      std::numeric_limits<float>::quiet_NaN();
  rejected(record);
  record = base; record.matrixGroupSizes[0] = 0u; rejected(record);
  record = base; record.vertexGroupIndices[0] = 2u; rejected(record);
  record = base; record.matrixGroupCount = 3u; rejected(record);
  record = base; record.matrixIndexCount = 4u; rejected(record);
  record = base; record.primitiveRecords[0].indexCount = 2u;
  rejected(record);
}

void TestUv2AndMultiPrimitiveProofs() {
  auto uv2 = MakeRecord();
  uv2.uvLayerCount = 2u;
  uv2.uvLayers.push_back(
      { 3u, { 0.25f, 0.25f, 0.75f, 0.25f, 0.5f, 0.75f } });
  PersistentGpuPackageImmutableProof uv2Proof = {};
  std::vector<PersistentGpuPackagePrimitiveProof> uv2Primitives;
  assert(Build(uv2, uv2Proof, uv2Primitives));
  assert(uv2Proof.uv1ContentHash != 0u);
  uint64_t uv2IndexOffset = 0u;
  auto uv2Bytes = Pack(uv2, uv2Proof, uv2IndexOffset);
  assert(ValidatePersistentGpuPackagePackedBytes(
      uv2Bytes.data(), uv2Bytes.size(), uv2IndexOffset,
      uv2Proof, uv2Primitives));
  uv2Bytes[uv2Proof.texcoord1Offset] ^= 1u;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      uv2Bytes.data(), uv2Bytes.size(), uv2IndexOffset,
      uv2Proof, uv2Primitives));

  auto multi = MakeRecord();
  multi.primitiveCount = 2u;
  multi.primitiveRecords = { { 4u, 3u }, { 9u, 3u } };
  multi.indexCount = 6u;
  multi.indices = { 0u, 1u, 2u, 2u, 1u, 0u };
  PersistentGpuPackageImmutableProof multiProof = {};
  std::vector<PersistentGpuPackagePrimitiveProof> multiPrimitives;
  assert(Build(multi, multiProof, multiPrimitives));
  assert(multiPrimitives.size() == 2u);
  assert(multiPrimitives[0].firstIndex == 0u);
  assert(multiPrimitives[1].firstIndex == 3u);
  uint64_t multiIndexOffset = 0u;
  const auto multiBytes = Pack(multi, multiProof, multiIndexOffset);
  assert(ValidatePersistentGpuPackagePackedBytes(
      multiBytes.data(), multiBytes.size(), multiIndexOffset,
      multiProof, multiPrimitives));
  auto badPrimitives = multiPrimitives;
  badPrimitives[1].firstIndex = 2u;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      multiBytes.data(), multiBytes.size(), multiIndexOffset,
      multiProof, badPrimitives));
  badPrimitives = multiPrimitives;
  badPrimitives[1].primitiveTypeOrMaterialSlot ^= 1u;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      multiBytes.data(), multiBytes.size(), multiIndexOffset,
      multiProof, badPrimitives));
  badPrimitives = multiPrimitives;
  badPrimitives[1].ordinal = 7u;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      multiBytes.data(), multiBytes.size(), multiIndexOffset,
      multiProof, badPrimitives));
  badPrimitives = multiPrimitives;
  badPrimitives[1].minVertex = 1u;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      multiBytes.data(), multiBytes.size(), multiIndexOffset,
      multiProof, badPrimitives));
}

void TestPackedByteRehash() {
  const auto record = MakeRecord();
  PersistentGpuPackageImmutableProof proof = {};
  std::vector<PersistentGpuPackagePrimitiveProof> primitives;
  assert(Build(record, proof, primitives));
  uint64_t indexOffset = 0u;
  auto bytes = Pack(record, proof, indexOffset);
  assert(ValidatePersistentGpuPackagePackedBytes(
      bytes.data(), bytes.size(), indexOffset, proof, primitives));

  for (uint64_t offset : {
         uint64_t(proof.positionOffset), uint64_t(proof.normalOffset),
         uint64_t(proof.groupSlotOffset), uint64_t(proof.texcoord0Offset),
         indexOffset }) {
    auto corrupted = bytes;
    corrupted[size_t(offset)] ^= 1u;
    assert(!ValidatePersistentGpuPackagePackedBytes(
        corrupted.data(), corrupted.size(), indexOffset, proof, primitives));
  }
  auto truncated = bytes;
  truncated.pop_back();
  assert(!ValidatePersistentGpuPackagePackedBytes(
      truncated.data(), truncated.size(), indexOffset, proof, primitives));
  assert(!ValidatePersistentGpuPackagePackedBytes(
      bytes.data(), bytes.size(), indexOffset + 1u, proof, primitives));
  auto trailing = bytes;
  trailing.push_back(0u);
  assert(!ValidatePersistentGpuPackagePackedBytes(
      trailing.data(), trailing.size(), indexOffset, proof, primitives));
  assert(!ValidatePersistentGpuPackagePackedBytes(
      nullptr, bytes.size(), indexOffset, proof, primitives));
  assert(!ValidatePersistentGpuPackagePackedBytes(
      bytes.data(), 0u, indexOffset, proof, primitives));

  const uint64_t groupEnd =
      uint64_t(proof.groupSlotOffset) + proof.vertexCount;
  assert(groupEnd < proof.texcoord0Offset);
  auto badPadding = bytes;
  badPadding[size_t(groupEnd)] = 1u;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      badPadding.data(), badPadding.size(), indexOffset, proof, primitives));

  auto badProof = proof;
  badProof.localMaxX += 1.0f;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      bytes.data(), bytes.size(), indexOffset, badProof, primitives));
  badProof = proof;
  badProof.positionContentHash ^= 1u;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      bytes.data(), bytes.size(), indexOffset, badProof, primitives));
  badProof = proof;
  badProof.primitiveProofHash ^= 1u;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      bytes.data(), bytes.size(), indexOffset, badProof, primitives));
  auto badPrimitives = primitives;
  badPrimitives[0].indexCount = 2u;
  assert(!ValidatePersistentGpuPackagePackedBytes(
      bytes.data(), bytes.size(), indexOffset, proof, badPrimitives));
}

}  // namespace

int main() {
  TestGenerationAuthorityAndBitExactPayload();
  TestEveryImmutableClassChangesProof();
  TestFailureIsClosedAndClearsOutputs();
  TestUv2AndMultiPrimitiveProofs();
  TestPackedByteRehash();
  return 0;
}
