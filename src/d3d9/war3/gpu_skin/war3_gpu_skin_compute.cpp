#include "war3_gpu_skin_compute.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <utility>

#include "../../../dxvk/dxvk_context.h"
#include "../../../dxvk/dxvk_shader_spirv.h"

#if __has_include(<war3_gpu_skin_comp.h>)
#include <war3_gpu_skin_comp.h>
#elif __has_include(<war3_gpu_skin.h>)
#include <war3_gpu_skin.h>
#define war3_gpu_skin_comp war3_gpu_skin
#else
#error "The generated war3_gpu_skin compute shader header is missing"
#endif

namespace dxvk::war3::gpu_skin {
namespace {

constexpr uint32_t kMaxDispatchDimension = 65535u;

DxvkBindingInfo MakeStorageBinding(uint32_t slot, VkAccessFlags access) {
  DxvkBindingInfo binding = { };
  binding.set = 0u;
  binding.binding = slot;
  binding.resourceIndex = slot;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.access = access;
  return binding;
}

Rc<DxvkShader> CreateGpuSkinShader() {
  const std::array<DxvkBindingInfo, 4> bindings = {
      MakeStorageBinding(kGpuSkinStaticSourceSlot,
                         VK_ACCESS_SHADER_READ_BIT),
      MakeStorageBinding(kGpuSkinPaletteSlot,
                         VK_ACCESS_SHADER_READ_BIT),
      MakeStorageBinding(kGpuSkinJobsSlot,
                         VK_ACCESS_SHADER_READ_BIT),
      MakeStorageBinding(kGpuSkinOutputSlot,
                         VK_ACCESS_SHADER_WRITE_BIT),
  };

  DxvkSpirvShaderCreateInfo info = { };
  info.bindingCount = bindings.size();
  info.bindings = bindings.data();
  info.debugName = "war3_gpu_skin.comp";

  SpirvCodeBuffer code(
      sizeof(war3_gpu_skin_comp) / sizeof(war3_gpu_skin_comp[0]),
      war3_gpu_skin_comp);
  return new DxvkSpirvShader(info, std::move(code));
}

bool HasSliceContract(const DxvkBufferSlice& slice,
                      VkBufferUsageFlags requiredUsage,
                      VkPipelineStageFlags requiredStages,
                      VkAccessFlags requiredAccess) {
  if (!slice.defined() || !slice.length())
    return false;

  const auto& info = slice.buffer()->info();
  if ((info.usage & requiredUsage) != requiredUsage ||
      (info.stages & requiredStages) != requiredStages ||
      (info.access & requiredAccess) != requiredAccess)
    return false;

  constexpr VkDeviceSize kStorageAlignmentMask = sizeof(uint32_t) - 1u;
  return ((slice.offset() | slice.length()) & kStorageAlignmentMask) == 0u &&
         slice.offset() <= info.size &&
         slice.length() <= info.size - slice.offset();
}

Rc<DxvkBufferView> CreateStorageView(const DxvkBufferSlice& slice) {
  DxvkBufferViewKey viewInfo = { };
  viewInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  viewInfo.offset = slice.offset();
  viewInfo.size = slice.length();
  return slice.buffer()->createView(viewInfo);
}

bool SliceRangeFits(const DxvkBufferSlice& slice,
                    VkDeviceSize offset,
                    VkDeviceSize byteCount) {
  return offset <= slice.length() &&
      byteCount <= slice.length() - offset;
}

}  // namespace

War3GpuSkinCompute::War3GpuSkinCompute(
    VkDeviceSize storageBufferOffsetAlignment)
    : m_shader(CreateGpuSkinShader()),
      m_storageBufferOffsetAlignment(std::max<VkDeviceSize>(
          sizeof(uint32_t), storageBufferOffsetAlignment)) {
}

War3GpuSkinCompute::~War3GpuSkinCompute() = default;

bool War3GpuSkinCompute::canRecordBatch(
    const GpuSkinComputeBatch& batch) const {
  if (!m_shader || !batch.jobCount || !batch.maxVertexCount)
    return false;

  const uint32_t groupCountX =
      GetGpuSkinDispatchGroupCount(batch.maxVertexCount);
  if (groupCountX > kGpuSkinMaxDispatchGroupCountX ||
      groupCountX > kMaxDispatchDimension ||
      batch.jobCount > kMaxDispatchDimension ||
      batch.vertexBucket >= kGpuSkinVertexBucketCount)
    return false;

  const auto storageOffsetAligned = [this](const DxvkBufferSlice& slice) {
    return m_storageBufferOffsetAlignment != 0u &&
        (slice.offset() % m_storageBufferOffsetAlignment) == 0u;
  };

  if (!HasSliceContract(batch.staticSource,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT) ||
      !HasSliceContract(batch.palette,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT) ||
      !HasSliceContract(batch.jobs,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT) ||
      !HasSliceContract(batch.output,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT) ||
      !storageOffsetAligned(batch.staticSource) ||
      !storageOffsetAligned(batch.palette) ||
      !storageOffsetAligned(batch.jobs) ||
      !storageOffsetAligned(batch.output))
    return false;

  const VkDeviceSize requiredJobBytes =
      VkDeviceSize(batch.jobCount) * sizeof(GpuSkinJob);
  const void* jobData = batch.jobs.buffer()->mapPtr(batch.jobs.offset());
  if (batch.jobs.length() != requiredJobBytes || jobData == nullptr)
    return false;

  uint32_t observedMaxVertexCount = 0u;
  uint64_t observedActualVertexCount = 0u;
  uint64_t observedRoundedInvocationCount = 0u;
  for (uint32_t i = 0u; i < batch.jobCount; ++i) {
    GpuSkinJob job;
    std::memcpy(&job,
                static_cast<const uint8_t*>(jobData) +
                    VkDeviceSize(i) * sizeof(job),
                sizeof(job));
    const uint32_t outputStride = GetGpuSkinFvfStride(job.outputFormat);
    const uint32_t requiredUvLayers =
        GetGpuSkinFvfUvLayerCount(job.outputFormat);
    const GpuSkinStaticSourceLayout sourceLayout =
        GetGpuSkinStaticSourceLayout(job.vertexCount, job.reserved1);
    const VkDeviceSize paletteBytes =
        VkDeviceSize(job.reserved0) * 12u * sizeof(float);
    const VkDeviceSize outputBytes =
        VkDeviceSize(job.vertexCount) * outputStride;
    constexpr uint32_t kOffsetAlignmentMask = sizeof(uint32_t) - 1u;

    if (job.frameTag == 0u || job.token == 0u || job.vertexCount == 0u ||
        job.vertexCount > batch.maxVertexCount || outputStride == 0u ||
        job.outputStride != outputStride || job.layoutGeneration == 0u ||
        job.flags != 1u ||
        job.reserved0 == 0u || job.reserved0 > 256u ||
        job.reserved1 > 2u || job.reserved1 < requiredUvLayers ||
        job.reserved2 != 0u || sourceLayout.byteSize == 0u ||
        ((job.inputVertexOffset | job.paletteOffset | job.outputOffset) &
         kOffsetAlignmentMask) != 0u ||
        !SliceRangeFits(batch.staticSource, job.inputVertexOffset,
                        sourceLayout.byteSize) ||
        !SliceRangeFits(batch.palette, job.paletteOffset, paletteBytes) ||
        !SliceRangeFits(batch.output, job.outputOffset, outputBytes)) {
      return false;
    }
    if (job.vertexCount > observedMaxVertexCount)
      observedMaxVertexCount = job.vertexCount;
    const uint32_t jobGroupCount =
        GetGpuSkinDispatchGroupCount(job.vertexCount);
    if (GetGpuSkinDispatchVertexBucket(job.vertexCount) !=
        batch.vertexBucket) {
      return false;
    }
    observedActualVertexCount += job.vertexCount;
    observedRoundedInvocationCount +=
        uint64_t(jobGroupCount) * kGpuSkinLocalSizeX;
  }

  const uint64_t observedLaunchedInvocationCount =
      uint64_t(groupCountX) * kGpuSkinLocalSizeX * batch.jobCount;
  if (observedMaxVertexCount != batch.maxVertexCount ||
      observedActualVertexCount != batch.actualVertexCount ||
      observedRoundedInvocationCount != batch.roundedInvocationCount ||
      observedLaunchedInvocationCount != batch.launchedInvocationCount ||
      observedActualVertexCount > observedRoundedInvocationCount ||
      observedRoundedInvocationCount > observedLaunchedInvocationCount)
    return false;

  return true;
}

void War3GpuSkinCompute::recordBatch(
    DxvkContext& context,
    const GpuSkinComputeBatch& batch) const {
  assert(canRecordBatch(batch));

  const uint32_t groupCountX =
      GetGpuSkinDispatchGroupCount(batch.maxVertexCount);

  Rc<DxvkBufferView> staticSourceView =
      CreateStorageView(batch.staticSource);
  Rc<DxvkBufferView> paletteView = CreateStorageView(batch.palette);
  Rc<DxvkBufferView> jobsView = CreateStorageView(batch.jobs);
  Rc<DxvkBufferView> outputView = CreateStorageView(batch.output);

  context.bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(
      Rc<DxvkShader>(m_shader));
  context.bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT,
      kGpuSkinStaticSourceSlot, std::move(staticSourceView));
  context.bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT,
      kGpuSkinPaletteSlot, std::move(paletteView));
  context.bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT,
      kGpuSkinJobsSlot, std::move(jobsView));
  context.bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT,
      kGpuSkinOutputSlot, std::move(outputView));

  context.dispatch(groupCountX, batch.jobCount, 1u);

  context.bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT,
      kGpuSkinStaticSourceSlot, nullptr);
  context.bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT,
      kGpuSkinPaletteSlot, nullptr);
  context.bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT,
      kGpuSkinJobsSlot, nullptr);
  context.bindResourceBufferView(VK_SHADER_STAGE_COMPUTE_BIT,
      kGpuSkinOutputSlot, nullptr);
  context.bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(nullptr);
}

}  // namespace dxvk::war3::gpu_skin
