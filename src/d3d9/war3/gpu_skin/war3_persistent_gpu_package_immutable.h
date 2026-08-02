#pragma once

#include "../model/war3_model_resource_cache.h"

#include <cstdint>
#include <vector>

namespace dxvk::war3::gpu_skin {

// This is the only packing schema the Store may mint.  Callers can request a
// schema for compatibility, but a value different from this constant is
// rejected before a key or queued miss is created.
inline constexpr uint32_t kPersistentGpuPackageStaticLayoutGeneration = 1u;

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
