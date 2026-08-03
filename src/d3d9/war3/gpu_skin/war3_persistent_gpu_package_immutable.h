#pragma once

#include "../model/war3_model_resource_cache.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace dxvk::war3::gpu_skin {

// This is the only packing schema the Store may mint.  Callers can request a
// schema for compatibility, but a value different from this constant is
// rejected before a key or queued miss is created.
inline constexpr uint32_t kPersistentGpuPackageStaticLayoutGeneration = 1u;
inline constexpr uint64_t kPersistentGpuPackageContentHashSeed =
    0xcbf29ce484222325ull;

// The immutable model cache and the exact D3D9 draw observer must hash bytes
// with one implementation.  Keeping this helper public prevents an Observe
// path from silently comparing two different "FNV-like" domains.
uint64_t ContinuePersistentGpuPackageContentHash(
    uint64_t seed, const void* data, size_t size) noexcept;
uint64_t HashPersistentGpuPackageContent(
    const void* data, size_t size) noexcept;
uint64_t HashPersistentGpuPackageStridedFloat3(
    const void* data, uint64_t byteCount, uint32_t vertexCount,
    uint32_t stride, uint32_t positionOffset) noexcept;

enum class PersistentGpuPackageCurrentDrawMatchDisposition : uint8_t {
  NotRequested = 0u,
  InvalidCurrentDraw,
  NotRigidStatic,
  MaterialRejected,
  SkinningRouteRejected,
  GeometryContractRejected,
  CpuSourceUnavailable,
  SourceGenerationMissing,
  PackageNotReady,
  PackageInvalid,
  SnapshotMismatch,
  MultiPrimitiveRejected,
  PackageLayoutMismatch,
  PositionContentMismatch,
  IndexContentMismatch,
  PrimitiveMismatch,
  ExactMatch,
};

// Frame-local, value-only proof sealed from the same CPU-readable allocation
// and source generations as the exact Arena capture.  It never contains a GPU
// slice and therefore cannot authorize binding or command recording.
struct PersistentGpuPackageCurrentDrawProof {
  uint64_t frameSerial = 0u;
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  uint64_t exactGeometryKeyHash = 0u;
  uintptr_t instanceIdentity = 0u;
  uintptr_t meshPayloadIdentity = 0u;
  uintptr_t renderablePartIdentity = 0u;
  uintptr_t positionOwnerIdentity = 0u;
  uint64_t positionIdentityGeneration = 0u;
  uint64_t positionAllocationGeneration = 0u;
  uint64_t positionContentGeneration = 0u;
  uintptr_t indexOwnerIdentity = 0u;
  uint64_t indexIdentityGeneration = 0u;
  uint64_t indexAllocationGeneration = 0u;
  uint64_t indexContentGeneration = 0u;
  uint64_t positionContentHash = 0u;
  uint64_t indexContentHash = 0u;
  uint32_t vertexCount = 0u;
  uint32_t indexCount = 0u;
  uint32_t sourceFirstIndex = 0u;
  uint32_t actualIndexMin = 0u;
  uint32_t actualIndexMax = 0u;
  uint32_t positionStride = 0u;
  uint32_t positionOffset = 0u;
  bool requested = false;
  bool sealed = false;
  bool rigidStatic = false;
  bool opaqueMaterial = false;
  bool gpuSkinBacked = false;
  bool vertexBlendEnabled = false;
  bool indexed = false;
  bool triangleList = false;
  bool uint16Indices = false;
  bool exactIndexDomainKnown = false;
  bool fullVertexDomainFallback = false;
  bool zeroBasedVertexRange = false;
  bool positionFloat3 = false;
  bool positionHostCached = false;
  bool indexHostCached = false;
};

// Store-owned frozen payload fields copied into a value view immediately
// before comparison.  The owner must validate the real frozen payload first;
// this POD cannot mint that authority by itself.
struct PersistentGpuPackageCurrentDrawPackageProof {
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  uint64_t packageGeneration = 0u;
  uintptr_t geosetDataIdentity = 0u;
  uint64_t positionContentHash = 0u;
  uint64_t indexContentHash = 0u;
  uint32_t vertexCount = 0u;
  uint32_t indexCount = 0u;
  uint32_t primitiveCount = 0u;
  uint32_t primitiveOrdinal = 0u;
  uint32_t primitiveFirstIndex = 0u;
  uint32_t primitiveIndexCount = 0u;
  uint32_t primitiveMinVertex = 0u;
  uint32_t primitiveMaxVertex = 0u;
  bool ready = false;
  bool frozenPayloadValid = false;
  bool snapshotIdentityExact = false;
};

PersistentGpuPackageCurrentDrawMatchDisposition
EvaluatePersistentGpuPackageCurrentDrawEquivalence(
    const PersistentGpuPackageCurrentDrawProof& draw,
    const PersistentGpuPackageCurrentDrawPackageProof& package) noexcept;

static_assert(std::is_standard_layout_v<
    PersistentGpuPackageCurrentDrawProof>);
static_assert(std::is_trivially_copyable_v<
    PersistentGpuPackageCurrentDrawProof>);
static_assert(std::is_standard_layout_v<
    PersistentGpuPackageCurrentDrawPackageProof>);
static_assert(std::is_trivially_copyable_v<
    PersistentGpuPackageCurrentDrawPackageProof>);

struct PersistentGpuPackagePrimitiveProof {
  uint64_t indexContentHash = 0u;
  uint32_t ordinal = 0u;
  uint32_t primitiveTypeOrMaterialSlot = 0u;
  uint32_t firstIndex = 0u;
  uint32_t indexCount = 0u;
  uint32_t minVertex = 0u;
  uint32_t maxVertex = 0u;
};

// CPU-only proof derived from a cache-owned immutable snapshot.  It contains
// every model stream, including palette-group topology that is not copied into
// the static atlas but still changes the meaning of per-vertex group slots.
struct PersistentGpuPackageImmutableProof {
  uint64_t immutableModelGeneration = 0u;
  uint64_t contentHash = 0u;
  uint64_t positionContentHash = 0u;
  uint64_t normalContentHash = 0u;
  uint64_t vertexGroupContentHash = 0u;
  uint64_t uv0ContentHash = 0u;
  uint64_t uv1ContentHash = 0u;
  uint64_t indexContentHash = 0u;
  uint64_t matrixGroupContentHash = 0u;
  uint64_t matrixIndexContentHash = 0u;
  uint64_t primitiveProofHash = 0u;
  uint64_t localBoundsHash = 0u;
  uint32_t layoutGeneration = 0u;
  uint32_t vertexCount = 0u;
  uint32_t indexCount = 0u;
  uint32_t uvLayerCount = 0u;
  uint32_t primitiveProofCount = 0u;
  uint32_t matrixGroupCount = 0u;
  uint32_t matrixIndexCount = 0u;
  uint32_t maxVertexGroupSlot = 0u;
  uint32_t positionOffset = 0u;
  uint32_t normalOffset = 0u;
  uint32_t groupSlotOffset = 0u;
  uint32_t texcoord0Offset = 0u;
  uint32_t texcoord1Offset = 0u;
  uint32_t staticByteSize = 0u;
  float localMinX = 0.0f;
  float localMinY = 0.0f;
  float localMinZ = 0.0f;
  float localMaxX = 0.0f;
  float localMaxY = 0.0f;
  float localMaxZ = 0.0f;
};

bool BuildPersistentGpuPackageImmutableProof(
    const model::ShadowGeosetResourceRecord& record,
    PersistentGpuPackageImmutableProof& proof,
    std::vector<PersistentGpuPackagePrimitiveProof>& primitiveProofs) noexcept;

bool SamePersistentGpuPackageImmutableProof(
    const PersistentGpuPackageImmutableProof& lhs,
    const PersistentGpuPackageImmutableProof& rhs) noexcept;

bool SamePersistentGpuPackagePrimitiveProof(
    const PersistentGpuPackagePrimitiveProof& lhs,
    const PersistentGpuPackagePrimitiveProof& rhs) noexcept;

bool ValidatePersistentGpuPackagePackedBytes(
    const void* packageBytes,
    uint64_t packageByteCount,
    uint64_t indexRelativeOffset,
    const PersistentGpuPackageImmutableProof& proof,
    const std::vector<PersistentGpuPackagePrimitiveProof>&
        primitiveProofs) noexcept;

}  // namespace dxvk::war3::gpu_skin
