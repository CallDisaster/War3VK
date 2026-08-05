#pragma once

#include <cstddef>
#include <cstdint>

#include "../../../dxvk/dxvk_buffer.h"
#include "../../../dxvk/dxvk_shader.h"
#include "war3_gpu_skin_types.h"

namespace dxvk {

class DxvkContext;

namespace war3::gpu_skin {

constexpr uint32_t kGpuSkinStaticSourceSlot = 64u;
constexpr uint32_t kGpuSkinPaletteSlot = 65u;
constexpr uint32_t kGpuSkinJobsSlot = 66u;
constexpr uint32_t kGpuSkinOutputSlot = 67u;
constexpr uint32_t kGpuSkinLocalSizeX = 64u;
constexpr uint32_t kGpuSkinMaxBatchUploadBytes = 1u << 20;
constexpr uint32_t kGpuSkinVertexBucketCount = 9u;
constexpr uint32_t kGpuSkinMaxDispatchGroupCountX = 256u;
static_assert(kGpuSkinMaxDispatchGroupCountX ==
              (1u << (kGpuSkinVertexBucketCount - 1u)));

constexpr uint32_t GetGpuSkinDispatchGroupCount(uint32_t vertexCount) {
  return vertexCount == 0u
      ? 0u
      : (vertexCount - 1u) / kGpuSkinLocalSizeX + 1u;
}

// floor(log2(ceil(vertices / 64))). Native ordinary geosets are capped at
// 16384 vertices, hence groupCount is 1..256 and exactly nine buckets suffice.
constexpr uint32_t GetGpuSkinDispatchVertexBucket(uint32_t vertexCount) {
  uint32_t groupCount = GetGpuSkinDispatchGroupCount(vertexCount);
  if (groupCount == 0u || groupCount > kGpuSkinMaxDispatchGroupCountX)
    return kGpuSkinVertexBucketCount;
  uint32_t bucket = 0u;
  while (groupCount > 1u) {
    groupCount >>= 1u;
    ++bucket;
  }
  return bucket;
}

// Static source data is packed once per geoset in this order. Every blob starts
// at a four-byte-aligned inputVertexOffset. Group slots remain tightly packed
// bytes; float streams remain naturally aligned.
struct GpuSkinStaticSourceLayout {
  uint32_t positionOffset = 0u;
  uint32_t normalOffset = 0u;
  uint32_t groupSlotOffset = 0u;
  uint32_t texcoord0Offset = 0u;
  uint32_t texcoord1Offset = 0u;
  uint32_t byteSize = 0u;
};

constexpr uint32_t GpuSkinAlign4(uint32_t value) {
  return (value + 3u) & ~3u;
}

constexpr GpuSkinStaticSourceLayout GetGpuSkinStaticSourceLayout(
    uint32_t vertexCount,
    uint32_t sourceUvLayerCount) {
  GpuSkinStaticSourceLayout result;
  if (!vertexCount || sourceUvLayerCount > 2u)
    return result;

  result.normalOffset = vertexCount * 12u;
  result.groupSlotOffset = result.normalOffset + vertexCount * 12u;
  result.texcoord0Offset = GpuSkinAlign4(
      result.groupSlotOffset + vertexCount);
  result.texcoord1Offset = result.texcoord0Offset + vertexCount * 8u;
  result.byteSize = result.texcoord0Offset +
      vertexCount * 8u * sourceUvLayerCount;
  return result;
}

constexpr uint32_t GetGpuSkinFvfStride(uint32_t outputFormat) {
  constexpr uint32_t strides[] = {24u, 28u, 32u, 36u, 40u, 44u};
  return outputFormat < 6u ? strides[outputFormat] : 0u;
}

constexpr uint32_t GetGpuSkinFvfUvLayerCount(uint32_t outputFormat) {
  return outputFormat < 6u ? outputFormat / 2u : 0u;
}

// Compute-owned use of GpuSkinJob's reserved tail. The other fields retain the
// public meanings declared in war3_gpu_skin_types.h. All byte offsets are
// relative to the corresponding bound buffer slice.
inline bool ConfigureGpuSkinComputeJob(
    GpuSkinJob& job,
    uint32_t paletteMatrixCount,
    uint32_t sourceUvLayerCount) {
  const uint32_t outputStride = GetGpuSkinFvfStride(job.outputFormat);
  const uint32_t requiredUvLayers =
      GetGpuSkinFvfUvLayerCount(job.outputFormat);

  // The compute kernel can materialize all six learned FVF layouts. Odd
  // formats synthesize the native ordinary-geoset white diffuse word. The
  // manager still limits those formats to Dual parity until byte identity is
  // proven; takeover/bypass modes keep their stricter whitelist.
  if (!job.vertexCount || !outputStride || !paletteMatrixCount ||
      paletteMatrixCount > 256u ||
      sourceUvLayerCount > 2u || sourceUvLayerCount < requiredUvLayers)
    return false;

  constexpr uint32_t requiredAlignment = alignof(uint32_t) - 1u;
  if (((job.inputVertexOffset | job.paletteOffset | job.outputOffset) &
       requiredAlignment) != 0u)
    return false;

  job.outputStride = outputStride;
  job.reserved0 = paletteMatrixCount;
  job.reserved1 = sourceUvLayerCount;
  job.reserved2 = 0u;
  return true;
}

static_assert(sizeof(GpuSkinJob) == 64u);
static_assert(alignof(GpuSkinJob) <= alignof(uint64_t));
static_assert(offsetof(GpuSkinJob, inputVertexOffset) == 16u);
static_assert(offsetof(GpuSkinJob, paletteOffset) == 20u);
static_assert(offsetof(GpuSkinJob, outputOffset) == 24u);
static_assert(offsetof(GpuSkinJob, vertexCount) == 28u);
static_assert(offsetof(GpuSkinJob, outputStride) == 32u);
static_assert(offsetof(GpuSkinJob, outputFormat) == 36u);
static_assert(offsetof(GpuSkinJob, reserved0) == 52u);
static_assert(offsetof(GpuSkinJob, reserved1) == 56u);
static_assert(offsetof(GpuSkinJob, reserved2) == 60u);

struct GpuSkinComputeBatch {
  // Jobs use byte offsets relative to these four bound slices. Individual
  // output leases remain exact sub-slices of output.
  DxvkBufferSlice staticSource;
  DxvkBufferSlice palette;
  DxvkBufferSlice jobs;
  DxvkBufferSlice output;
  uint32_t jobCount = 0u;
  uint32_t maxVertexCount = 0u;
  uint32_t vertexBucket = 0u;
  // Exact dispatch-efficiency contract, recomputed by canRecordBatch from the
  // immutable mapped jobs before host acceptance.
  uint64_t actualVertexCount = 0u;
  uint64_t roundedInvocationCount = 0u;
  uint64_t launchedInvocationCount = 0u;
};

class War3GpuSkinCompute {
public:
  explicit War3GpuSkinCompute(VkDeviceSize storageBufferOffsetAlignment);
  ~War3GpuSkinCompute();

  War3GpuSkinCompute(const War3GpuSkinCompute&) = delete;
  War3GpuSkinCompute& operator=(const War3GpuSkinCompute&) = delete;

  bool isAvailable() const {
    return m_shader != nullptr;
  }

  // Synchronous recording contract. A batch accepted by the host must pass
  // this before it is captured by EmitCs.
  bool canRecordBatch(const GpuSkinComputeBatch& batch) const;

  // Records one flush-level dispatch. No draw-level dispatch entry point is
  // intentionally exposed by this module. The caller must have validated the
  // immutable captured batch with canRecordBatch.
  void recordBatch(DxvkContext& context,
                   const GpuSkinComputeBatch& batch) const;

private:
  Rc<DxvkShader> m_shader;
  VkDeviceSize m_storageBufferOffsetAlignment = sizeof(uint32_t);
};

}  // namespace war3::gpu_skin
}  // namespace dxvk
