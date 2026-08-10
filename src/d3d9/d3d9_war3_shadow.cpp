#include "d3d9_war3_shadow.h"
#include "d3d9_shader.h"
#include "d3d9_war3_debug.h"
#include "war3/core/war3_internal_test_config.h"
#include "war3/core/war3_runtime_profile.h"
#include "war3/gpu_skin/war3_gpu_skin_compute.h"
#include "war3/hooks/war3_hook_widget_identity.h"
#include "war3/render/war3_render_objects.h"
#include "war3/render/war3_hybrid_ray_tracing.h"
#include "war3/render/war3_shadow_lifecycle.h"
#include "war3/render/war3_shadow_alpha_cascade_contract.h"
#include "war3/render/war3_shadow_observer_build_policy.h"
#include "war3/render/war3_shadow_producer_policy.h"
#include "war3/render/war3_shadow_replay_validation.h"
#include "war3/render/war3_shadow_runtime_bridge.h"
#include "war3/render/war3_union_consumer_visibility.h"
#include "war3/shader/war3_shader_manager.h"
#include "war3/tools/war3_diagnostics_hub.h"
#include "war3/tools/war3_perf_monitor.h"
#include "war3_shader_api.h"

#include <sstream>

#include "../dxvk/dxvk_access.h"
#include "../dxvk/dxvk_buffer.h"
#include "../dxvk/dxvk_context.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_util.h"
#include "d3d9_war3_light.h"

#include <war3_fullscreen_vert.h>
#include <war3_motion_vector.h>
#include <war3_outline_edge.h>
#include <war3_outline_expand_vert.h>
#include <war3_outline_mask.h>
#include <war3_shadow_caster_frag.h>
#include <war3_shadow_caster_mask.h>
#include <war3_shadow_caster_point_frag.h>
#include <war3_shadow_caster_vert.h>
#include <war3_shadow_receiver.h>
#include <war3_shadow_visibility.h>
#include <war3_unit_outline.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#include "d3d9_device.h"
#include "d3d9_texture.h"
#include "d3d9_war3_hook.h"
#include "../util/util_env.h"
#include "../util/util_bit.h"
#include "../util/util_matrix.h"
#include "../util/util_time.h"

namespace dxvk {

namespace {
std::mutex g_shadowDiagnosticsMutex;
ShadowTaaDiagnostics g_shadowTaaDiagnostics = {};
CsmResolutionDiagnostics g_csmResolutionDiagnostics = {};
PointShadowPersistentDiagnostics g_pointShadowPersistentDiagnostics = {};
war3::render::War3GpuWorkloadGovernorDiagnostics
    g_gpuWorkloadGovernorDiagnostics = {};
struct ShadowReplayDiagnosticsAtomic {
  std::atomic<uint64_t> mapEpoch{0u};
  std::atomic<uint64_t> deviceEpoch{0u};
  std::atomic<uint64_t> candidateFrameSerial{0u};
  std::atomic<uint64_t> firstCompleteLatencyFrames{0u};
  std::atomic<uint64_t> staleEpochConsumerRejectCount{0u};
  std::atomic<uint64_t> validationRejectCount{0u};
  std::atomic<uint64_t> partialPreventedCount{0u};
  std::atomic<uint64_t> pointWorkerCancelCount{0u};
  std::atomic<uint64_t> pointLateResultRejectCount{0u};
  std::atomic<uint32_t> plannedCasterCount{0u};
  std::atomic<uint32_t> replayCasterCount{0u};
  std::atomic<uint32_t> validatedCasterCount{0u};
  std::atomic<uint32_t> drawnCasterCount{0u};
  std::atomic<uint32_t> lastRejectReason{0u};
  std::atomic<uint64_t> lastOffenderMapEpoch{0u};
  std::atomic<uint64_t> lastRequiredEnd{0u};
  std::atomic<uint64_t> lastAvailableSize{0u};
  std::atomic<int64_t> lastMinimumVertex{0};
  std::atomic<int64_t> lastMaximumVertex{0};
  std::atomic<int32_t> lastVertexOffset{0};
  std::atomic<int32_t> lastStage{-1};
  std::atomic<uint32_t> lastCategory{0u};
  std::atomic<uint32_t> lastBatchTag{0u};
  std::atomic<uint32_t> lastObjectKind{0u};
  std::atomic<uint32_t> lastRawcode{0u};
  std::atomic<uint32_t> lastJHandle{0u};
  std::atomic<uint32_t> lastIndexCount{0u};
  std::atomic<uint32_t> lastFirstIndex{0u};
  std::atomic<uint32_t> lastMinVertexIndex{0u};
  std::atomic<uint32_t> lastNumVertices{0u};
  std::atomic<uint32_t> lastActualIndexMin{0u};
  std::atomic<uint32_t> lastActualIndexMax{0u};
  std::atomic<uint32_t> lastActualIndexDomainKnown{0u};
  std::atomic<uint32_t> lastFullVertexDomainFallback{0u};
  std::atomic<uint64_t> lastPositionSize{0u};
};
ShadowReplayDiagnosticsAtomic g_shadowReplayDiagnostics = {};
std::atomic<uint64_t> g_pointShadowPersistentRendererEpoch{1u};

uint64_t MintPointShadowPersistentRendererEpoch() noexcept {
  uint64_t epoch =
      g_pointShadowPersistentRendererEpoch.load(std::memory_order_relaxed);
  for (;;) {
    // Saturate permanently at exhaustion. Zero is an invalid owner token and
    // no prior epoch may ever be recycled after integer wrap.
    if (epoch == 0u || epoch == std::numeric_limits<uint64_t>::max())
      return 0u;
    if (g_pointShadowPersistentRendererEpoch.compare_exchange_weak(
            epoch, epoch + 1u, std::memory_order_relaxed,
            std::memory_order_relaxed))
      return epoch;
  }
}

template <typename Fn>
class War3ScopeExit final {
public:
  explicit War3ScopeExit(Fn fn) : m_fn(std::move(fn)) {}
  War3ScopeExit(const War3ScopeExit &) = delete;
  War3ScopeExit &operator=(const War3ScopeExit &) = delete;
  War3ScopeExit(War3ScopeExit &&other) noexcept(
      std::is_nothrow_move_constructible_v<Fn>)
      : m_fn(std::move(other.m_fn)), m_active(other.m_active) {
    other.m_active = false;
  }
  ~War3ScopeExit() noexcept {
    if (!m_active)
      return;
    try {
      m_fn();
    } catch (...) {
      // Scope-exit cleanup must never mask the original renderer exception.
    }
  }

private:
  Fn m_fn;
  bool m_active = true;
};

template <typename Fn>
auto MakeWar3ScopeExit(Fn &&fn) {
  return War3ScopeExit<std::decay_t<Fn>>(std::forward<Fn>(fn));
}

struct ReceiverPushConstants {
  uint32_t colorSampler;
  uint32_t rawShadowSampler;
  uint32_t compareShadowSampler;
  // 0=nearest comparison, 1=hardware comparison-linear,
  // 2=manual compare-first 2x2 fallback.
  uint32_t shadowCompareMode;
};

enum War3ShadowTaaHistoryInvalidationBits : uint32_t {
  kShadowTaaInvalidateLifecycle = 1u << 0,
  kShadowTaaInvalidateCameraCut = 1u << 1,
  kShadowTaaInvalidateProjection = 1u << 2,
  kShadowTaaInvalidateViewport = 1u << 3,
  kShadowTaaInvalidateSun = 1u << 4,
  kShadowTaaInvalidateCsm = 1u << 5,
  kShadowTaaInvalidateCasterContent = 1u << 6,
  kShadowTaaInvalidateDynamicPose = 1u << 7,
  kShadowTaaInvalidateShadowMapResource = 1u << 8,
  kShadowTaaInvalidateTaaResource = 1u << 9,
  kShadowTaaInvalidateCsmFallback = 1u << 10,
  kShadowTaaInvalidateModeSwitch = 1u << 11,
};

// ===== 阴影投射器推送常量 =====
// 必须与 shaders/war3_shadow_caster_vert.vert 和 frag.frag 中的 push_block
// 保持一致
struct ShadowCasterPushConstants {
  Matrix4 mvp;            // 模型视图投影矩阵
  uint32_t paletteOffset; // 骨骼调色板偏移
  uint32_t blendCount;    // 混合权重数量 (0-3)
  uint32_t flags;         // bit0=useBlend, bit1=indexed, bit2=alphaTest
  float alphaRef;         // Alpha测试阈值 (0.0-1.0)
  uint32_t samplerIndex;  // [NEW] Bindless Sampler Index
  float terrainDepthBias; // stage 1 terrain caster-only NDC depth bias
  uint32_t padding[2];    // Padding to 96 bytes (16-byte alignment)
  Vector4 pointLightPosRange; // xyz=point light position, w=range for linear cube depth
};

constexpr uint32_t kShadowCasterFlagUseBlend = 0x1u;
constexpr uint32_t kShadowCasterFlagIndexedBlend = 0x2u;
constexpr uint32_t kShadowCasterFlagAlphaTest = 0x4u;
constexpr uint32_t kShadowCasterFlagHashAlpha = 0x8u;
constexpr uint32_t kShadowCasterFlagStage1Terrain = 0x10u;
constexpr uint32_t kShadowCasterFlagPointShadowLinearDepth = 0x20u;
constexpr uint32_t kShadowCasterFlagGpuSkinDirectInput = 0x40u;
constexpr uint32_t kShadowCasterFlagGpuSkinNoFallback = 0x80u;
constexpr uint32_t kShadowCasterGpuSkinOutputFormatShift = 8u;
constexpr uint32_t kShadowCasterGpuSkinLayoutGenerationShift = 12u;
constexpr uint32_t kShadowCasterGpuSkinUvLayerCountShift = 16u;
constexpr uint32_t kShadowCasterGpuSkinMetadataMask = 0x000fff00u;
constexpr uint8_t kPointShadowCompleteFaceMask = 0x3fu;

static_assert((kShadowCasterFlagGpuSkinDirectInput & 0x3fu) == 0u);
static_assert((kShadowCasterFlagGpuSkinNoFallback & 0x7fu) == 0u);
static_assert((kShadowCasterGpuSkinMetadataMask & 0xffu) == 0u);

constexpr uint32_t PackShadowCasterGpuSkinMetadata(
    uint32_t outputFormat,
    uint32_t layoutGeneration,
    uint32_t sourceUvLayerCount) {
  return (outputFormat << kShadowCasterGpuSkinOutputFormatShift) |
         (layoutGeneration <<
          kShadowCasterGpuSkinLayoutGenerationShift) |
         (sourceUvLayerCount <<
          kShadowCasterGpuSkinUvLayerCountShift);
}

constexpr uint32_t PackShadowCasterGpuSkinMetadata(
    const war3::gpu_skin::GpuSkinInputLeaseDesc& desc) {
  return PackShadowCasterGpuSkinMetadata(
      desc.outputFormat, desc.layoutGeneration, desc.sourceUvLayerCount);
}

static_assert(PackShadowCasterGpuSkinMetadata(2u, 1u, 1u) == 0x00011200u);

struct ShadowGpuSkinDirectDecision {
  bool requested = false;
  bool inputExact = false;
  bool stateExact = false;

  explicit operator bool() const {
    return requested && inputExact && stateExact;
  }
};

ShadowGpuSkinDirectDecision EvaluateShadowGpuSkinDirectInput(
    const War3ShadowCasterDraw& draw) {
  using war3::gpu_skin::GetGpuSkinStaticSourceLayout;
  using war3::gpu_skin::GpuSkinConsumerBits;

  ShadowGpuSkinDirectDecision result = {};
  result.requested = draw.gpuSkinInput.valid;
  if (!result.requested)
    return result;

  const War3GpuSkinDrawInput& input = draw.gpuSkinInput;
  const auto& desc = input.desc;
  const auto sourceLayout = GetGpuSkinStaticSourceLayout(
      desc.vertexCount, desc.sourceUvLayerCount);
  const uint32_t requiredConsumers =
      static_cast<uint32_t>(GpuSkinConsumerBits::Main) |
      static_cast<uint32_t>(GpuSkinConsumerBits::Shadow);
  constexpr uint32_t kMaxNativeVertexCount = 0x4000u;

  result.inputExact = static_cast<bool>(input) &&
      desc.mapEpoch != 0u && desc.deviceEpoch != 0u &&
      desc.frameTag != 0u && desc.token != 0u &&
      desc.dispatchEpoch != 0u && desc.uploadEpoch != 0u &&
      desc.staticByteOffset == input.staticSource.offset() &&
      desc.staticByteLength == input.staticSource.length() &&
      desc.paletteByteOffset == input.palette.offset() &&
      desc.paletteByteLength == input.palette.length() &&
      desc.vertexCount != 0u &&
      desc.vertexCount <= kMaxNativeVertexCount &&
      desc.paletteMatrixCount != 0u &&
      desc.paletteMatrixCount <= 256u &&
      uint64_t(desc.paletteMatrixCount) * 48u ==
          desc.paletteByteLength &&
      desc.sourceUvLayerCount == 1u &&
      desc.outputFormat == 2u && desc.layoutGeneration == 1u &&
      desc.consumerBits == requiredConsumers &&
      input.storageLeaseId != 0u &&
      input.storagePageGeneration != 0u && input.storagePageId != 0u &&
      sourceLayout.byteSize != 0u &&
      sourceLayout.positionOffset == 0u &&
      sourceLayout.byteSize == desc.staticByteLength &&
      input.staticSource.buffer() != nullptr &&
      (input.staticSource.buffer()->info().usage &
       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0u &&
      (input.staticSource.buffer()->info().stages &
       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT) != 0u &&
      (input.staticSource.buffer()->info().access &
       VK_ACCESS_SHADER_READ_BIT) != 0u &&
      input.palette.buffer() != nullptr &&
      (input.palette.buffer()->info().usage &
       (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT)) ==
          (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
           VK_BUFFER_USAGE_TRANSFER_DST_BIT) &&
      (input.palette.buffer()->info().stages &
       (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT)) ==
          (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
           VK_PIPELINE_STAGE_TRANSFER_BIT) &&
      (input.palette.buffer()->info().access &
       (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)) ==
          (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
  if (!result.inputExact)
    return result;

  const uint64_t fallbackBytes = uint64_t(desc.vertexCount) * 32u;
  const uint64_t directPositionBytes = uint64_t(desc.vertexCount) * 12u;
  const bool drawRangeExact = draw.indexed
      ? draw.indexCount != 0u && draw.vertexOffset == 0 &&
            draw.numVertices == desc.vertexCount
      : draw.vertexCount == desc.vertexCount && draw.firstVertex == 0u &&
            draw.numVertices == desc.vertexCount;
  // VS-A/B0 保留 format-2 compute/CPU 输出作为着色器兜底。VS-B1 已跳过
  // CPU kernel，只允许 position binding 精确别名到 static atlas 的位置段；
  // shader 私有门失败时必须裁掉 primitive，不能读取原生动态 VB。
  const bool fallbackStateExact = !input.irreversible &&
      draw.positionStorage != nullptr &&
      draw.positionInfo.buffer != VK_NULL_HANDLE &&
      uint64_t(draw.positionInfo.size) == fallbackBytes &&
      draw.positionStride == 32u && draw.positionOffset == 0u;
  const auto staticInfo = input.staticSource.getSliceInfo(
      0u, directPositionBytes);
  const bool directOnlyStateExact = input.irreversible &&
      draw.positionStorage == input.staticSource.buffer() &&
      draw.positionInfo.buffer != VK_NULL_HANDLE &&
      draw.positionInfo.buffer == staticInfo.buffer &&
      draw.positionInfo.offset == staticInfo.offset &&
      draw.positionInfo.size == staticInfo.size &&
      uint64_t(draw.positionInfo.size) == directPositionBytes &&
      draw.positionStride == 12u && draw.positionOffset == 0u;
  result.stateExact = (fallbackStateExact || directOnlyStateExact) &&
      draw.positionFormat == VK_FORMAT_R32G32B32_SFLOAT &&
      draw.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST &&
      !draw.vertexBlendEnabled && !draw.vertexBlendIndexed &&
      draw.vertexBlendCount == 0u && draw.blendBinding == 0u &&
      drawRangeExact;
  return result;
}

uint32_t War3ShadowFormatByteSize(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_USCALED:
    case VK_FORMAT_R8G8B8A8_SSCALED:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SSCALED:
    case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
      return 4u;
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SSCALED:
      return 8u;
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_R32G32B32_UINT:
    case VK_FORMAT_R32G32B32_SINT:
      return 12u;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
      return 16u;
    default:
      return 0u;
  }
}

uint32_t War3ShadowAttributeEnd(uint32_t offset, VkFormat format) {
  const uint32_t bytes = War3ShadowFormatByteSize(format);
  return bytes != 0u && offset <= std::numeric_limits<uint32_t>::max() - bytes
      ? offset + bytes
      : 0u;
}

bool War3ShadowMatrixFinite(const Matrix4& matrix) {
  for (uint32_t column = 0u; column < 4u; ++column) {
    if (!std::isfinite(matrix[column].x) ||
        !std::isfinite(matrix[column].y) ||
        !std::isfinite(matrix[column].z) ||
        !std::isfinite(matrix[column].w))
      return false;
  }
  return true;
}

war3::render::War3ShadowReplayValidationInput
MakeWar3ShadowReplayValidationInput(
    const War3ShadowCasterDraw& draw, const War3PipelineInput& input) {
  using war3::render::War3ShadowReplayBufferAccess;
  war3::render::War3ShadowReplayValidationInput validation = {};
  validation.expectedMapEpoch = input.mapEpoch;
  validation.expectedDeviceEpoch = input.deviceEpoch;
  validation.drawMapEpoch = draw.mapEpoch;
  validation.drawDeviceEpoch = draw.deviceEpoch;
  validation.worldMatrixFinite = War3ShadowMatrixFinite(draw.worldMatrix);
  validation.position = {
      draw.positionStorage != nullptr &&
          draw.positionInfo.buffer != VK_NULL_HANDLE,
      uint64_t(draw.positionInfo.size), draw.positionStride,
      draw.positionOffset, War3ShadowFormatByteSize(draw.positionFormat)};
  validation.indexed = draw.indexed;
  validation.indexBufferPresent =
      draw.indexStorage != nullptr && draw.indexInfo.buffer != VK_NULL_HANDLE;
  validation.indexBufferSize = uint64_t(draw.indexInfo.size);
  validation.indexTypeBytes = draw.indexType == VK_INDEX_TYPE_UINT16
      ? 2u
      : draw.indexType == VK_INDEX_TYPE_UINT32 ? 4u : 0u;
  validation.firstIndex = draw.firstIndex;
  validation.indexCount = draw.indexCount;
  validation.vertexOffset = draw.vertexOffset;
  validation.minVertexIndex = draw.minVertexIndex;
  validation.numVertices = draw.numVertices;
  validation.firstVertex = draw.firstVertex;
  validation.vertexCount = draw.vertexCount;
  validation.actualIndexDomainKnown = draw.shadowActualIndexDomainKnown;
  validation.fullVertexDomainFallback = draw.shadowFullVertexDomainFallback;
  validation.actualIndexMin = draw.shadowActualIndexMin;
  validation.actualIndexMax = draw.shadowActualIndexMax;

  // D3DVBF_0WEIGHTS still consumes one indexed matrix when
  // INDEXEDVERTEXBLENDENABLE is set.  It has no explicit weight attribute, but
  // its blend-index attribute and (possibly separate) backing are mandatory.
  validation.blendRequired = draw.vertexBlendEnabled &&
      (draw.vertexBlendCount != 0u || draw.vertexBlendIndexed);
  if (validation.blendRequired) {
    const uint32_t weightEnd = draw.vertexBlendCount != 0u
        ? War3ShadowAttributeEnd(draw.blendWeightOffset,
                                draw.blendWeightFormat)
        : 0u;
    const uint32_t indexEnd = draw.vertexBlendIndexed
        ? War3ShadowAttributeEnd(draw.blendIndexOffset,
                                draw.blendIndexFormat)
        : 0u;
    const bool sharesPosition = draw.blendBinding == 0u;
    validation.blend = {
        sharesPosition
            ? validation.position.present
            : draw.blendStorage != nullptr &&
                  draw.blendInfo.buffer != VK_NULL_HANDLE,
        sharesPosition ? uint64_t(draw.positionInfo.size)
                       : uint64_t(draw.blendInfo.size),
        sharesPosition ? draw.positionStride : draw.blendStride,
        0u, std::max(weightEnd, indexEnd)};
  }

  validation.uvRequired = draw.alphaTestEnabled;
  if (validation.uvRequired) {
    const bool sharesPosition = draw.uvBinding == 0u;
    validation.uv = {
        sharesPosition
            ? validation.position.present
            : draw.uvStorage != nullptr && draw.uvInfo.buffer != VK_NULL_HANDLE,
        sharesPosition ? uint64_t(draw.positionInfo.size)
                       : uint64_t(draw.uvInfo.size),
        sharesPosition ? draw.positionStride : draw.uvStride,
        draw.uvOffset, War3ShadowFormatByteSize(draw.uvFormat)};
  }

  validation.paletteRequired = draw.vertexBlendEnabled;
  validation.paletteIndex = draw.paletteIndex;
  validation.paletteCount =
      static_cast<uint32_t>((std::min)(
          input.scene.shadowPalettes.size(),
          size_t(std::numeric_limits<uint32_t>::max())));
  validation.paletteMatricesPerEntry = 256u;

  validation.gpuSkinRequired = draw.gpuSkinInput.valid;
  if (validation.gpuSkinRequired) {
    const auto& skin = draw.gpuSkinInput;
    validation.gpuSkinLeaseValid = static_cast<bool>(skin);
    validation.gpuSkinMapEpoch = skin.desc.mapEpoch;
    validation.gpuSkinDeviceEpoch = skin.desc.deviceEpoch;
    validation.gpuSkinSourceSize = skin.staticSource.buffer() != nullptr
        ? uint64_t(skin.staticSource.buffer()->info().size)
        : 0u;
    validation.gpuSkinSourceOffset = skin.desc.staticByteOffset;
    validation.gpuSkinSourceLength = skin.desc.staticByteLength;
    validation.gpuSkinPaletteSize = skin.palette.buffer() != nullptr
        ? uint64_t(skin.palette.buffer()->info().size)
        : 0u;
    validation.gpuSkinPaletteOffset = skin.desc.paletteByteOffset;
    validation.gpuSkinPaletteLength = skin.desc.paletteByteLength;
  }
  return validation;
}

void SetShadowGpuSkinStorageDescriptors(
    std::array<DxvkDescriptorWrite, 5>& descriptors,
    const DxvkResourceBufferInfo& matrixFallback,
    const War3ShadowCasterDraw& draw,
    bool direct) {
  descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptors[3].buffer = direct
      ? draw.gpuSkinInput.staticSource.getSliceInfo()
      : matrixFallback;
  descriptors[4].buffer = direct
      ? draw.gpuSkinInput.palette.getSliceInfo()
      : matrixFallback;
}

bool ShadowS1TerrainCasterMaskRuntimeEnabled() {
  static int cached = -1;
  if (cached >= 0)
    return cached != 0;

  cached = war3::internal::kShadowS1TerrainCasterMaskEnabled ? 1 : 0;
  const char* env = std::getenv("DXVK_WAR3_S1_TERRAIN_CASTER_MASK");
  if (env && env[0] != '\0') {
    const char c = env[0];
    cached = (c == '0' || c == 'f' || c == 'F' || c == 'n' || c == 'N')
                 ? 0
                 : 1;
  }

  return cached != 0;
}

enum class ReceiverInputRejectReason : uint32_t {
  None = 0,
  MissingColor = 1,
  MissingDepth = 2,
  MissingCamera = 3,
  EmptyExtent = 4,
  ExtentMismatch = 5,
  BadViewport = 6,
  SmallViewport = 7,
  BadDepthRange = 8,
  BadMatrix = 9,
  StaleCamera = 10,
};

const char* ReceiverInputRejectReasonName(ReceiverInputRejectReason reason) {
  switch (reason) {
  case ReceiverInputRejectReason::None:
    return "none";
  case ReceiverInputRejectReason::MissingColor:
    return "missing-color";
  case ReceiverInputRejectReason::MissingDepth:
    return "missing-depth";
  case ReceiverInputRejectReason::MissingCamera:
    return "missing-camera";
  case ReceiverInputRejectReason::EmptyExtent:
    return "empty-extent";
  case ReceiverInputRejectReason::ExtentMismatch:
    return "extent-mismatch";
  case ReceiverInputRejectReason::BadViewport:
    return "bad-viewport";
  case ReceiverInputRejectReason::SmallViewport:
    return "small-viewport";
  case ReceiverInputRejectReason::BadDepthRange:
    return "bad-depth-range";
  case ReceiverInputRejectReason::BadMatrix:
    return "bad-matrix";
  case ReceiverInputRejectReason::StaleCamera:
    return "stale-camera";
  }
  return "unknown";
}

bool IsFiniteMatrix(const Matrix4& matrix) {
  for (uint32_t r = 0; r < 4; r++) {
    for (uint32_t c = 0; c < 4; c++) {
      if (!std::isfinite(matrix[r][c]))
        return false;
    }
  }
  return true;
}

bool EnvFlagDefault(const char* name, bool fallback) {
  const std::string value = env::getEnvVar(name);
  if (value.empty())
    return fallback;
  if (value == "0" || value == "false" || value == "FALSE" ||
      value == "off" || value == "OFF")
    return false;
  if (value == "1" || value == "true" || value == "TRUE" ||
      value == "on" || value == "ON")
    return true;
  return fallback;
}

template <typename T>
uint64_t War3RcObjectId(const Rc<T>& object) {
  return uint64_t(reinterpret_cast<uintptr_t>(object.ptr()));
}

float EnvFloatDefault(const char* name, float fallback) {
  const std::string value = env::getEnvVar(name);
  if (value.empty())
    return fallback;
  char* end = nullptr;
  const float parsed = std::strtof(value.c_str(), &end);
  return end != value.c_str() && std::isfinite(parsed) ? parsed : fallback;
}

uint32_t EnvU32Default(const char* name, uint32_t fallback) {
  const std::string value = env::getEnvVar(name);
  if (value.empty())
    return fallback;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (end == value.c_str())
    return fallback;
  return static_cast<uint32_t>(
      std::min<unsigned long>(parsed, std::numeric_limits<uint32_t>::max()));
}

int EnvIntOverride(const char* name, int minValue, int maxValue) {
  const std::string value = env::getEnvVar(name);
  if (value.empty())
    return minValue - 1;
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str())
    return minValue - 1;
  return std::clamp<int>(static_cast<int>(parsed), minValue, maxValue);
}

war3::render::War3UnionVisibilityMode War3UnionCullModeRuntime() {
  if constexpr (!war3::render::kDevelopmentShadowObserversEnabled)
    return war3::render::War3UnionVisibilityMode::Off;
  static const auto mode = war3::render::ParseShadowObserverBuildMode(
      EnvU32Default("DXVK_WAR3_UNION_CONSUMER_CULL_MODE", 0u));
  return mode == war3::render::War3ShadowObserverBuildMode::Observe
      ? war3::render::War3UnionVisibilityMode::Observe
      : war3::render::War3UnionVisibilityMode::Off;
}

war3::render::War3TerrainBoundsCullMode
War3TerrainBoundsCullModeRuntime() {
  if constexpr (!war3::render::kDevelopmentShadowObserversEnabled)
    return war3::render::War3TerrainBoundsCullMode::Off;
  static const auto mode = war3::render::ParseShadowObserverBuildMode(
      EnvU32Default("DXVK_WAR3_CSM_TERRAIN_BOUNDS_MODE", 0u));
  return mode == war3::render::War3ShadowObserverBuildMode::Observe
      ? war3::render::War3TerrainBoundsCullMode::Observe
      : war3::render::War3TerrainBoundsCullMode::Off;
}

War3ShadowTaaMode ResolveShadowTaaRequestedMode(
    const War3ShadowSettings* settings) {
  const War3ShadowTaaMode configured =
      settings != nullptr
          ? settings->shadowTaaMode
          : War3ShadowTaaMode::DirectInline;
  if (configured != War3ShadowTaaMode::DirectInline)
    return configured;
  return settings != nullptr && settings->shadowTaaEnabled
      ? War3ShadowTaaMode::Temporal
      : War3ShadowTaaMode::DirectInline;
}

bool ShadowTaaDisableForSemanticDynamicEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC",
      war3::internal::kShadowDisableTaaForSemanticDynamicCasters);
  return s_enabled;
}

bool ShadowTaaDisableOnSunMotionEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SHADOW_TAA_DISABLE_ON_SUN_MOTION",
      war3::internal::kShadowSunMotionAwareTaaDisable);
  return s_enabled;
}

float ShadowTaaSunDirectionDelta(
    const Vector4& a, const Vector4& b) {
  return std::max(
      std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)),
      std::abs(a.z - b.z));
}

bool War3ShadowPhaseBreakdownEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_PERF_SHADOW_PHASE_BREAKDOWN", false);
  return s_enabled;
}

uint32_t War3ShadowPhaseBreakdownSamplePeriod() {
  static const uint32_t s_period = std::clamp<uint32_t>(
      EnvU32Default("DXVK_WAR3_PERF_SHADOW_PHASE_SAMPLE_PERIOD", 16u),
      1u, 4096u);
  return s_period;
}

uint32_t War3ShadowPhaseSampleWeight(uint64_t frameSerial, uint64_t salt) {
  if (!War3ShadowPhaseBreakdownEnabled() ||
      !war3::War3PerfMonitor::instance().isRecording()) {
    return 0u;
  }

  const uint32_t period = War3ShadowPhaseBreakdownSamplePeriod();
  if (period <= 1u)
    return 1u;

  // SplitMix64 makes the selected frame phase independent of CSM reuse/update
  // cadence. Main and directional-map callers use different salts so one
  // observer's report flush normally does not fall inside the other's sample.
  static thread_local uint64_t s_zeroSerialOrdinal = 0u;
  uint64_t x = frameSerial != 0u ? frameSerial : ++s_zeroSerialOrdinal;
  x += salt + 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30u)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27u)) * 0x94d049bb133111ebull;
  x ^= x >> 31u;
  return (x % period) == 0u ? period : 0u;
}

bool War3CsmDescriptorReuseEnabled() {
  static const bool s_enabled =
      EnvFlagDefault("DXVK_WAR3_CSM_DESCRIPTOR_REUSE", true);
  return s_enabled;
}

bool War3CsmDescriptorReuseVerifierEnabled() {
  static const bool s_enabled =
      EnvFlagDefault("DXVK_WAR3_CSM_DESCRIPTOR_REUSE_VERIFY", false);
  return s_enabled;
}

bool War3CsmDescriptorReuseVerifierAssertEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_CSM_DESCRIPTOR_REUSE_VERIFY_ASSERT", false);
  return s_enabled;
}

struct War3CsmBufferDescriptorSignature {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0u;
  VkDeviceSize size = 0u;
  VkDeviceAddress gpuAddress = 0u;
};

struct War3CsmAlphaDescriptorSignature {
  VkImageView imageView = VK_NULL_HANDLE;
  VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct War3CsmDescriptorSignature {
  const DxvkPipelineLayout* layout = nullptr;
  std::array<War3CsmBufferDescriptorSignature, 3> buffers = {};
  War3CsmAlphaDescriptorSignature alpha = {};
};

War3CsmBufferDescriptorSignature War3CsmBufferDescriptorKey(
    const DxvkResourceBufferInfo& info) {
  return {info.buffer, info.offset, info.size, info.gpuAddress};
}

bool War3CsmBufferDescriptorKeyEquals(
    const War3CsmBufferDescriptorSignature& a,
    const War3CsmBufferDescriptorSignature& b) {
  return a.buffer == b.buffer && a.offset == b.offset && a.size == b.size &&
      a.gpuAddress == b.gpuAddress;
}

War3CsmDescriptorSignature War3CsmDescriptorKey(
    const DxvkPipelineLayout* layout,
    const std::array<DxvkDescriptorWrite, 5>& descriptors) {
  War3CsmDescriptorSignature result = {};
  result.layout = layout;
  result.buffers[0] = War3CsmBufferDescriptorKey(descriptors[0].buffer);
  result.buffers[1] = War3CsmBufferDescriptorKey(descriptors[3].buffer);
  result.buffers[2] = War3CsmBufferDescriptorKey(descriptors[4].buffer);
  if (descriptors[1].descriptor != nullptr) {
    result.alpha.imageView =
        descriptors[1].descriptor->legacy.image.imageView;
    result.alpha.imageLayout =
        descriptors[1].descriptor->legacy.image.imageLayout;
  }
  return result;
}

bool War3CsmDescriptorBufferKeysEqual(
    const War3CsmDescriptorSignature& a,
    const War3CsmDescriptorSignature& b) {
  for (size_t i = 0u; i < a.buffers.size(); ++i) {
    if (!War3CsmBufferDescriptorKeyEquals(a.buffers[i], b.buffers[i]))
      return false;
  }
  return true;
}

bool War3CsmDescriptorAlphaKeysEqual(
    const War3CsmDescriptorSignature& a,
    const War3CsmDescriptorSignature& b) {
  return a.alpha.imageView == b.alpha.imageView &&
      a.alpha.imageLayout == b.alpha.imageLayout;
}

bool War3CsmDescriptorWriteEquivalent(
    const DxvkDescriptorWrite& a, const DxvkDescriptorWrite& b) {
  if (a.descriptorType != b.descriptorType)
    return false;
  switch (a.descriptorType) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      return a.buffer.buffer == b.buffer.buffer &&
          a.buffer.offset == b.buffer.offset &&
          a.buffer.size == b.buffer.size &&
          a.buffer.gpuAddress == b.buffer.gpuAddress;

    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      if (a.descriptor == b.descriptor)
        return true;
      if (a.descriptor == nullptr || b.descriptor == nullptr)
        return false;
      return a.descriptor->legacy.image.sampler ==
              b.descriptor->legacy.image.sampler &&
          a.descriptor->legacy.image.imageView ==
              b.descriptor->legacy.image.imageView &&
          a.descriptor->legacy.image.imageLayout ==
              b.descriptor->legacy.image.imageLayout &&
          a.descriptor->descriptor == b.descriptor->descriptor;

    default:
      return false;
  }
}

bool War3CsmDescriptorWritesEquivalent(
    const std::array<DxvkDescriptorWrite, 5>& a,
    const std::array<DxvkDescriptorWrite, 5>& b) {
  for (size_t i = 0u; i < a.size(); ++i) {
    if (!War3CsmDescriptorWriteEquivalent(a[i], b[i]))
      return false;
  }
  return true;
}

template <size_t PhaseCount>
class War3SampledPhaseRawTiming final {
public:
  War3SampledPhaseRawTiming(
      uint32_t sampleWeight, const char* parentName,
      const char* parentPath,
      const std::array<const char*, PhaseCount>& phaseNames)
      : m_sampleWeight(sampleWeight),
        m_parentName(parentName),
        m_parentPath(parentPath),
        m_phaseNames(phaseNames) {
    // Keep the default-off path free of array clearing stores. These buckets
    // are never read unless the opt-in sample is active.
    if (m_sampleWeight != 0u) {
      m_ticks.fill(0u);
      m_calls.fill(0u);
    }
  }

  ~War3SampledPhaseRawTiming() noexcept {
    try {
      finish();
    } catch (...) {
      // Diagnostics must never turn a renderer exception or allocation
      // failure into process termination.
    }
  }

  War3SampledPhaseRawTiming(const War3SampledPhaseRawTiming&) = delete;
  War3SampledPhaseRawTiming& operator=(
      const War3SampledPhaseRawTiming&) = delete;

  void enter(size_t phase) {
    if (m_sampleWeight == 0u || phase >= PhaseCount)
      return;
    const int64_t now = dxvk::high_resolution_clock::get_counter();
    closeCurrent(now);
    m_phase = phase;
    m_begin = now;
    m_calls[phase] += m_sampleWeight;
  }

private:
  void closeCurrent(int64_t now) {
    if (m_begin == 0 || m_phase >= PhaseCount)
      return;
    if (now > m_begin)
      m_ticks[m_phase] += uint64_t(now - m_begin) * m_sampleWeight;
  }

  void finish() {
    if (m_sampleWeight == 0u)
      return;

    closeCurrent(dxvk::high_resolution_clock::get_counter());
    m_begin = 0;
    m_phase = PhaseCount;

    uint64_t totalTicks = 0u;
    for (const uint64_t ticks : m_ticks)
      totalTicks += ticks;

    // This is an orthogonal, non-additive FramePipeline overlay. The synthetic
    // parent is defined as the exact sum of the contiguous raw-QPC children,
    // so its closure error is zero by construction. Boundary QPC cost remains
    // assigned to the preceding phase instead of being hidden by correction.
    static const double s_ticksToMs =
        1000.0 / double(dxvk::high_resolution_clock::get_frequency());
    auto& perf = war3::War3PerfMonitor::instance();
    perf.addCpuSample(m_parentName, double(totalTicks) * s_ticksToMs,
                      "FramePipeline", m_sampleWeight);
    for (size_t i = 0u; i < PhaseCount; ++i) {
      if (m_calls[i] == 0u)
        continue;
      perf.addCpuSample(m_phaseNames[i], double(m_ticks[i]) * s_ticksToMs,
                        m_parentPath, m_calls[i]);
    }

    m_sampleWeight = 0u;
  }

  uint32_t m_sampleWeight = 0u;
  const char* m_parentName = nullptr;
  const char* m_parentPath = nullptr;
  const std::array<const char*, PhaseCount>& m_phaseNames;
  std::array<uint64_t, PhaseCount> m_ticks;
  std::array<uint32_t, PhaseCount> m_calls;
  size_t m_phase = PhaseCount;
  int64_t m_begin = 0;
};

enum class War3DirectionalShadowMapRawPhase : size_t {
  EntryAndReplay = 0u,
  MatrixUpload,
  BeginTransitions,
  PreparePipelines,
  CullAndSortSetup,
  CascadeCull,
  CascadeRecord,
  TerrainMask,
  FinalizeAndReadTransition,
  Count,
};

constexpr size_t kWar3DirectionalShadowMapRawPhaseCount =
    static_cast<size_t>(War3DirectionalShadowMapRawPhase::Count);
constexpr std::array<const char*,
                     kWar3DirectionalShadowMapRawPhaseCount>
    kWar3DirectionalShadowMapRawPhaseNames = {
        "EntryAndReplay", "MatrixUpload", "BeginTransitions",
        "PreparePipelines", "CullAndSortSetup", "CascadeCull",
        "CascadeRecord", "TerrainMask", "FinalizeAndReadTransition"};

enum class War3ShadowMainRawPhase : size_t {
  EntryAndValidation = 0u,
  LightingAndPolicy,
  ReplayAndCsmPrepare,
  ShadowMapAndVolume,
  PointAndCopies,
  ReceiverPrepare,
  ReceiverPasses,
  OutlineAndPublish,
  Count,
};

constexpr size_t kWar3ShadowMainRawPhaseCount =
    static_cast<size_t>(War3ShadowMainRawPhase::Count);
constexpr std::array<const char*, kWar3ShadowMainRawPhaseCount>
    kWar3ShadowMainRawPhaseNames = {
        "EntryAndValidation", "LightingAndPolicy", "ReplayAndCsmPrepare",
        "ShadowMapAndVolume", "PointAndCopies", "ReceiverPrepare",
        "ReceiverPasses", "OutlineAndPublish"};

bool SemanticReceiverStabilityModeEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_MODE", false);
  return s_enabled;
}

bool SemanticReceiverFreezeLastGoodLightingEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_FREEZE_LAST_GOOD_LIGHTING", true);
  return s_enabled;
}

bool ShadowAdaptiveMapUpdateRuntimeEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SHADOW_ADAPTIVE_MAP_UPDATE",
      war3::internal::kShadowAdaptiveMapUpdateEnabled);
  return s_enabled;
}

enum class ReceiverRunEarlyReturnReason : uint32_t {
  None = 0u,
  InvalidInput = 1u,
  ShadowsAndOutlineDisabled = 2u,
  NoWorkNeeded = 3u,
  CsmComputeFailed = 4u,
};

enum ReceiverRunEntryFlag : uint32_t {
  ReceiverRunEntryInputValid = 1u << 0,
  ReceiverRunEntryShadowsEnabled = 1u << 1,
  ReceiverRunEntryOutlineEnabled = 1u << 2,
  ReceiverRunEntryHasSunShadow = 1u << 3,
  ReceiverRunEntryHasPointShadow = 1u << 4,
  ReceiverRunEntryHasPointLights = 1u << 5,
  ReceiverRunEntryNeedOutlinePass = 1u << 6,
  ReceiverRunEntryNeedsShadowMap = 1u << 7,
  ReceiverRunEntryNeedsReceiverPass = 1u << 8,
  ReceiverRunEntryHasReplayDraws = 1u << 9,
  ReceiverRunEntryShadowMapExecuted = 1u << 10,
  ReceiverRunEntryDebugShadow = 1u << 11,
};

float SemanticReceiverStableStrengthClamp() {
  static const float s_clamp = EnvFloatDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_STRENGTH_CLAMP", 0.55f);
  return s_clamp;
}

float SemanticReceiverStablePcfRadiusOverride() {
  static const float s_radius = EnvFloatDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_PCF_RADIUS", -1.0f);
  return s_radius;
}

bool SemanticReceiverHoldUntilStableIdentityEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_HOLD_SHADOWMAP_UNTIL_STABLE_IDENTITY", true);
  return s_enabled;
}

uint32_t SemanticReceiverStableIdentityFramesBeforeRedraw() {
  static const uint32_t s_frames = EnvU32Default(
      "DXVK_WAR3_SEMANTIC_IDENTITY_STABLE_FRAMES_BEFORE_REDRAW", 2u);
  return std::max<uint32_t>(1u, s_frames);
}

bool SemanticReceiverHoldOnCoverageDropEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_HOLD_SHADOWMAP_ON_COVERAGE_DROP", true);
  return s_enabled;
}

uint32_t SemanticReceiverCoverageDropTolerance() {
  static const uint32_t s_tolerance = EnvU32Default(
      "DXVK_WAR3_SEMANTIC_COVERAGE_DROP_TOLERANCE", 0u);
  return s_tolerance;
}

uint32_t SemanticReceiverCoverageDropMaxHoldFrames() {
  static const uint32_t s_frames = EnvU32Default(
      "DXVK_WAR3_SEMANTIC_COVERAGE_DROP_MAX_HOLD_FRAMES", 30u);
  return s_frames;
}

bool SemanticReceiverDisablePointLightsEnabled() {
  static const bool s_enabled = EnvFlagDefault(
      "DXVK_WAR3_SEMANTIC_RECEIVER_DISABLE_POINT_LIGHTS", false);
  return s_enabled;
}

ReceiverInputRejectReason ValidateMainWorldReceiverInput(
    const War3PipelineInput& input, VkExtent3D* colorExtentOut = nullptr,
    VkExtent3D* depthExtentOut = nullptr) {
  if (!input.colorView)
    return ReceiverInputRejectReason::MissingColor;
  if (!input.depthView)
    return ReceiverInputRejectReason::MissingDepth;
  if (!input.scene.worldCamera.valid)
    return ReceiverInputRejectReason::MissingCamera;

  const VkExtent3D colorExtent = input.colorView->mipLevelExtent(0u);
  const VkExtent3D depthExtent = input.depthView->mipLevelExtent(0u);
  if (colorExtentOut)
    *colorExtentOut = colorExtent;
  if (depthExtentOut)
    *depthExtentOut = depthExtent;

  if (colorExtent.width == 0u || colorExtent.height == 0u ||
      depthExtent.width == 0u || depthExtent.height == 0u)
    return ReceiverInputRejectReason::EmptyExtent;

  if (colorExtent.width != depthExtent.width ||
      colorExtent.height != depthExtent.height)
    return ReceiverInputRejectReason::ExtentMismatch;

  const auto& cam = input.scene.worldCamera;
  // The current capture is exact; one immediately preceding last-good camera
  // is an intentional fail-soft for a transient orthographic/overlay frame.
  // Older matrices and future identities are stale. Ring slot zero is valid
  // and can never serve as an unknown-frame sentinel.
  if (!War3WorldCameraIsFreshForFrame(cam, input.frameSerial))
    return ReceiverInputRejectReason::StaleCamera;

  const auto& vp = cam.viewport;
  if (vp.Width == 0u || vp.Height == 0u || vp.X >= colorExtent.width ||
      vp.Y >= colorExtent.height)
    return ReceiverInputRejectReason::BadViewport;

  const uint64_t vpRight = uint64_t(vp.X) + uint64_t(vp.Width);
  const uint64_t vpBottom = uint64_t(vp.Y) + uint64_t(vp.Height);
  if (vpRight > colorExtent.width || vpBottom > colorExtent.height)
    return ReceiverInputRejectReason::BadViewport;

  const uint64_t vpArea = uint64_t(vp.Width) * uint64_t(vp.Height);
  const uint64_t colorArea =
      uint64_t(colorExtent.width) * uint64_t(colorExtent.height);
  if (colorArea == 0u || vpArea * 4u < colorArea)
    return ReceiverInputRejectReason::SmallViewport;

  if (!std::isfinite(vp.MinZ) || !std::isfinite(vp.MaxZ) ||
      vp.MinZ < 0.0f || vp.MaxZ > 1.0f || vp.MaxZ <= vp.MinZ)
    return ReceiverInputRejectReason::BadDepthRange;

  if (!IsFiniteMatrix(cam.view) || !IsFiniteMatrix(cam.proj) ||
      !IsFiniteMatrix(cam.viewProj) || !IsFiniteMatrix(cam.invViewProj))
    return ReceiverInputRejectReason::BadMatrix;

  return ReceiverInputRejectReason::None;
}

// 同一帧内 BuildShadowReplayDraws 会被多个调用点重复调（executePassImpl 入口、
// renderPointShadow 等）。每次调用都做 heap allocation + N 次 push_back + 可能
// 还会做 stable_sort，是干净的合批/缓存机会。
//
// 缓存采用 thread_local + monotonic frameSerial + scene 三元组（地址 + size）
// 作为命中 key：
//   - scene 的 shadowCasters / shadowInstances / shadowFallbacks 是
//     `War3FrameScene` 的成员 vector，scene 在每帧初被 reset({})，向量地址保持
//     稳定但内容可能改变；
//   - 同一帧内 vector 不会再被 push（这点由 BeforeUi 的 publish 流程保证），
//     所以 frameSerial + (address, size) 三元组对"同帧同 scene"是稳定的标识。
//   - 不同帧 scene 重新填充时，即使 vector 地址与 size 偶然不变，也必须重跑
//     path blocker / alpha caster 等最终过滤逻辑。
//   - 跨线程调用走各自 thread_local 副本，无锁竞争。
struct ReplayDrawsCacheKey {
  uint64_t frameSerial = 0u;
  const void* castersAddr = nullptr;
  const void* instancesAddr = nullptr;
  const void* fallbacksAddr = nullptr;
  size_t castersSize = 0u;
  size_t instancesSize = 0u;
  size_t fallbacksSize = 0u;

  bool operator==(const ReplayDrawsCacheKey& rhs) const {
    return frameSerial == rhs.frameSerial &&
           castersAddr == rhs.castersAddr &&
           instancesAddr == rhs.instancesAddr &&
           fallbacksAddr == rhs.fallbacksAddr &&
           castersSize == rhs.castersSize &&
           instancesSize == rhs.instancesSize &&
           fallbacksSize == rhs.fallbacksSize;
  }
};

struct ReplayDrawsCache {
  ReplayDrawsCacheKey key{};
  bool valid = false;
  std::vector<const War3ShadowCasterDraw*> draws;
};

// =====================================================================
// 2026-05-31: Path blocker 最终防线（draw-time chokepoint）
// =====================================================================
// BuildShadowReplayDraws 是所有 shadow caster（来自 6+ 个 append 站点）汇聚到
// GPU shadow map 渲染前的**唯一收集点**，每个 War3ShadowCasterDraw 都带
// rawcode / jHandle / batchHandle。在这里做最终 path blocker 过滤，覆盖任何
// 上游漏判（包括 rawcode 在上游为 0、到这里才被 widget cache 解析出来的情况）。
//
// 用户实测：上游 EntryGate/Producer 日志命中 YTfb/YTpb/YTab，但视觉仍可见。
// 说明存在某条路径，caster 在上游 rawcode=0 未被识别就 append 了。这里用
// rawcode + jHandle 双通道兜底，是 D3D9 CSM 侧最后一道闸。
//
// 诊断：env DXVK_WAR3_SHADOW_DRAW_SURVEY=1 时，前 40 个 unique rawcode 各写
// 1 行到 war3_d3d9.log，直接告诉我们 shadow map 实际画了哪些对象。

std::atomic<uint64_t> g_shadowReplayPathBlockerRejectCount{0};

inline uint32_t War3ReplayNormalizeFourCc(uint32_t rawcode) {
  if (rawcode == 0u)
    return 0u;
  // 第二字符大小写归一化（兼容 YTlc/Ytlc 编辑器输出差异）。
  auto normChar2 = [](uint32_t code) -> uint32_t {
    const uint8_t c1 = uint8_t((code >> 16) & 0xFFu);
    if (c1 >= 'a' && c1 <= 'z')
      return (code & 0xFF00FFFFu) | (uint32_t(c1 - 'a' + 'A') << 16);
    return code;
  };
  return normChar2(rawcode);
}

inline bool War3ReplayIsPathBlockerRawcode(uint32_t rawcode) {
  if (rawcode == 0u)
    return false;
  const uint32_t norm = War3ReplayNormalizeFourCc(rawcode);
  // 同时尝试字节翻转（内存序 vs 编辑器序）。
  const uint32_t swapped = War3ReplayNormalizeFourCc(
      ((rawcode & 0xFFu) << 24) | ((rawcode & 0xFF00u) << 8) |
      ((rawcode >> 8) & 0xFF00u) | ((rawcode >> 24) & 0xFFu));
  for (uint32_t i = 0u; i < dxvk::war3::internal::kPathBlockerFourCCsCount;
       ++i) {
    const uint32_t b = dxvk::war3::internal::kPathBlockerFourCCs[i];
    if (norm == b || swapped == b || rawcode == b)
      return true;
  }
  return false;
}

inline bool War3ReplayDrawIsAnonymousStage11Marker(
    const War3ShadowCasterDraw& draw) {
  if (!dxvk::war3::internal::
          kPathBlockerStage11AnonymousRigidMarkerGateEnabled)
    return false;
  if (!dxvk::war3::internal::kPathBlockerAnonymousRigidMarkerGateEnabled)
    return false;
  if (draw.stage != 11 ||
      draw.category != War3RenderState::StageCategory::WorldObject)
    return false;
  if (draw.rawcode != 0u || draw.jHandle != 0u || draw.batchHandle != 0u)
    return false;
  const bool exactCurrentDrawContractBacked =
      static_cast<war3::render::ObjectKind>(draw.objectKind) ==
          war3::render::ObjectKind::Unit &&
      draw.shadowUnitIdentityProven &&
      draw.shadowRenderablePart != nullptr &&
      draw.shadowPartLifecycleState ==
          War3ShadowPartLifecycleState::RequiredCurrent;
  if (exactCurrentDrawContractBacked)
    return false;
  if (draw.vertexBlendEnabled || draw.vertexBlendIndexed ||
      draw.alphaTestEnabled || draw.alphaBlendEnabled)
    return false;

  const auto objectKind = static_cast<war3::render::ObjectKind>(draw.objectKind);
  if (objectKind != war3::render::ObjectKind::Unknown &&
      objectKind != war3::render::ObjectKind::Unit)
    return false;

  if (draw.numVertices == 0u ||
      draw.numVertices >
          dxvk::war3::internal::
              kPathBlockerAnonymousRigidMarkerMaxVertices)
    return false;
  return draw.indexCount <=
         dxvk::war3::internal::kPathBlockerAnonymousRigidMarkerMaxIndices;
}

inline bool War3ReplayDrawIsAnonymousSmallMarker(
    const War3ShadowCasterDraw& draw) {
  if (!dxvk::war3::internal::kPathBlockerAnonymousSmallFlatMarkerGateEnabled)
    return false;
  if (draw.rawcode != 0u || draw.jHandle != 0u || draw.batchHandle != 0u)
    return false;
  const bool exactCurrentDrawContractBacked =
      static_cast<war3::render::ObjectKind>(draw.objectKind) ==
          war3::render::ObjectKind::Unit &&
      draw.shadowUnitIdentityProven &&
      draw.shadowRenderablePart != nullptr &&
      draw.shadowPartLifecycleState ==
          War3ShadowPartLifecycleState::RequiredCurrent;
  if (exactCurrentDrawContractBacked)
    return false;
  if (draw.vertexBlendEnabled || draw.vertexBlendIndexed ||
      draw.alphaTestEnabled || draw.alphaBlendEnabled)
    return false;
  if (draw.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
    return false;

  const uint32_t vertexCount =
      War3ShadowReferencedVertexUpperBound(draw);
  if (vertexCount < 3u ||
      vertexCount >
          dxvk::war3::internal::kPathBlockerBelowGroundFlatMarkerMaxVertices) {
    return false;
  }
  if (draw.indexCount < 3u ||
      draw.indexCount >
          dxvk::war3::internal::kPathBlockerBelowGroundFlatMarkerMaxIndices) {
    return false;
  }

  const bool worldLane =
      draw.category == War3RenderState::StageCategory::WorldObject ||
      draw.batchTag == War3BatchTag::WorldObjects ||
      draw.batchTag == War3BatchTag::Decorations ||
      draw.batchTag == War3BatchTag::RangeIndicatorTarget ||
      draw.stage == 11;
  if (!worldLane)
    return false;

  const auto objectKind = static_cast<war3::render::ObjectKind>(draw.objectKind);
  return objectKind == war3::render::ObjectKind::Unknown ||
         objectKind == war3::render::ObjectKind::Unit ||
         objectKind == war3::render::ObjectKind::Destructible;
}

inline void War3ReplayNoteAnonymousStage11Reject(
    const War3ShadowCasterDraw& draw) {
  static std::atomic<uint32_t> s_logCount{0};
  const uint32_t idx = s_logCount.fetch_add(1u, std::memory_order_relaxed);
  if (idx >= 24u)
    return;

  const float wx = draw.worldMatrix[3].x;
  const float wy = draw.worldMatrix[3].y;
  const float wz = draw.worldMatrix[3].z;
  char buf[280];
  snprintf(buf, sizeof(buf),
           "DXVK War3Hook[Shadow]: PATH BLOCKER REJECT #%u rawcode=0x00000000 "
           "(anon-stage11) kind=%u vtx=%u idx=%u pos=(%.0f,%.0f,%.0f)",
           idx + 1u, unsigned(draw.objectKind), unsigned(draw.numVertices),
           unsigned(draw.indexCount), double(wx), double(wy), double(wz));
  ::dxvk::Logger::info(buf);
}

inline bool War3ReplayDrawIsPathBlocker(const War3ShadowCasterDraw& draw) {
  if (!dxvk::war3::internal::kPathBlockerHideEnabled)
    return false;
  if (draw.pathBlocker)
    return true;
  if (draw.pathBlockerGeometryMarker)
    return true;
  // rawcode 是稳定对象身份，不会触发历史上 batchHandle sweep 导致的逐帧抖动。
  // 只要 append 站点已经填上 rawcode，最终 replay gate 必须无条件兜底拦截。
  if (War3ReplayIsPathBlockerRawcode(draw.rawcode))
    return true;
  if (War3ReplayDrawIsAnonymousSmallMarker(draw)) {
    War3ReplayNoteAnonymousStage11Reject(draw);
    return true;
  }
  if (War3ReplayDrawIsAnonymousStage11Marker(draw)) {
    War3ReplayNoteAnonymousStage11Reject(draw);
    return true;
  }
  // rawcode=0 的 handle 兜底历史上误判过真实单位；仅在显式开启时作为诊断/保险。
  if (!dxvk::war3::internal::kPathBlockerDrawTimeSweepEnabled)
    return false;
  if (!dxvk::war3::internal::kPathBlockerDrawTimeSweepHandleFallbackEnabled)
    return false;
  // 2026-05-31：caster rawcode 为 0 时，才用 jHandle/batchHandle → widget cache 兜底。
  const uint32_t handle = draw.jHandle != 0u ? draw.jHandle : draw.batchHandle;
  if (handle != 0u) {
    const uint32_t cached =
        dxvk::war3::hooks::QueryWidgetRawcodeByHandle(handle);
    if (cached != 0u && War3ReplayIsPathBlockerRawcode(cached))
      return true;
  }
  return false;
}

inline void War3ReplayDrawSurvey(const War3ShadowCasterDraw& draw) {
  // Shadow draw survey 属于取证输出，默认关闭；需要复查 caster 身份时可用
  // DXVK_WAR3_SHADOW_DRAW_SURVEY=1 临时打开。
  static const bool enabled = []() {
    const char* env = std::getenv("DXVK_WAR3_SHADOW_DRAW_SURVEY");
    if (env != nullptr && env[0] != '\0')
      return env[0] != '0';
    return dxvk::war3::internal::kShadowReplayDrawSurveyLogging;
  }();
  if (!enabled)
    return;
  // 优先用 caster 自带 rawcode；为 0 时用 batchHandle 查 widget cache。
  uint32_t rawcode = draw.rawcode;
  const uint32_t handle = draw.jHandle != 0u ? draw.jHandle : draw.batchHandle;
  if (rawcode == 0u && handle != 0u)
    rawcode = dxvk::war3::hooks::QueryWidgetRawcodeByHandle(handle);
  static std::atomic<uint32_t> s_logCount{0};
  static std::array<std::atomic<uint32_t>, 40> s_logged{};
  if (s_logCount.load(std::memory_order_relaxed) >= 40u)
    return;
  // 2026-05-31：rawcode/handle 都为 0 的"匿名 caster"才是 path blocker 漏网
  // 的嫌疑对象（上游没识别到身份）。用 (category, vertexCount) 合成一个 dedup
  // key，确保这类匿名几何也能被记录一次（否则旧逻辑 logKey=0 直接 return，
  // 恰好把最需要看的对象漏掉）。
  uint32_t logKey;
  if (rawcode != 0u)
    logKey = rawcode;
  else if (handle != 0u)
    logKey = 0x80000000u | handle;
  else
    logKey = 0x40000000u | ((uint32_t(draw.category) & 0xFFu) << 20) |
             (draw.numVertices & 0xFFFFFu);
  if (logKey == 0u)
    return;
  uint32_t freeSlot = 40u;
  for (uint32_t i = 0u; i < 40u; ++i) {
    const uint32_t cur = s_logged[i].load(std::memory_order_relaxed);
    if (cur == logKey)
      return;
    if (cur == 0u && freeSlot == 40u)
      freeSlot = i;
  }
  if (freeSlot >= 40u)
    return;
  uint32_t expected = 0u;
  if (!s_logged[freeSlot].compare_exchange_strong(expected, logKey,
                                                  std::memory_order_relaxed))
    return;
  const uint32_t idx = s_logCount.fetch_add(1, std::memory_order_relaxed);
  char fc[5] = {char((rawcode >> 24) & 0xFF), char((rawcode >> 16) & 0xFF),
                char((rawcode >> 8) & 0xFF), char(rawcode & 0xFF), 0};
  // 2026-05-31：补充几何签名 + 世界坐标。path blocker 漏网时 rawcode/handle
  // 多半为 0，靠"顶点数 + 世界位置（在悬崖上）+ objectKind"才能确认它就是
  // 那个看不见的 marker。worldMatrix 第 4 行是平移（DXVK 列存：m[3] = 平移）。
  const float wx = draw.worldMatrix[3].x;
  const float wy = draw.worldMatrix[3].y;
  const float wz = draw.worldMatrix[3].z;
  char buf[320];
  snprintf(buf, sizeof(buf),
           "DXVK War3Shadow: SHADOW DRAW SURVEY #%u rawcode=0x%08X (%s) "
           "batchHandle=0x%X kind=%u cat=%d vtx=%u idx=%u blend=%d "
           "pos=(%.0f,%.0f,%.0f)",
           idx + 1, rawcode, (rawcode != 0u ? fc : "----"), unsigned(handle),
           unsigned(draw.objectKind), int(draw.category),
           unsigned(draw.numVertices), unsigned(draw.indexCount),
           int(draw.vertexBlendEnabled), double(wx), double(wy), double(wz));
  ::dxvk::Logger::info(buf);
}

// 2026-07-21 优化：返回 thread_local 缓存的 const 引用，避免 cache 命中时
// 每次拷贝 N 个指针（本函数每帧被调 2-3 次）。生命周期安全：worker 在
// 下一次 cache 重建前必然已被 scope-exit guard / 显式 wait 排空。
const std::vector<const War3ShadowCasterDraw*>& BuildShadowReplayDraws(
    const War3FrameScene& scene, uint64_t frameSerial) {
  // thread_local 缓存：同一帧内重复调用直接复用上次的 draws 向量。
  thread_local ReplayDrawsCache cache;

  ReplayDrawsCacheKey key;
  key.frameSerial = frameSerial;
  key.castersAddr = scene.shadowCasters.data();
  key.instancesAddr = scene.shadowInstances.data();
  key.fallbacksAddr = scene.shadowFallbacks.data();
  key.castersSize = scene.shadowCasters.size();
  key.instancesSize = scene.shadowInstances.size();
  key.fallbacksSize = scene.shadowFallbacks.size();

  if (cache.valid && cache.key == key) {
    return cache.draws;
  }

  std::vector<const War3ShadowCasterDraw*>& draws = cache.draws;
  draws.clear();
  draws.reserve(scene.shadowInstances.size() + scene.shadowFallbacks.size());

  for (const auto& instance : scene.shadowInstances) {
    if (instance.replayDrawIndex >= scene.shadowCasters.size())
      continue;
    const auto& caster = scene.shadowCasters[instance.replayDrawIndex];
    // 2026-05-31：draw-time 最终 path blocker 防线（覆盖所有上游漏判）。
    if (War3ReplayDrawIsPathBlocker(caster)) {
      g_shadowReplayPathBlockerRejectCount.fetch_add(
          1, std::memory_order_relaxed);
      continue;
    }
    War3ReplayDrawSurvey(caster);
    draws.push_back(&caster);
  }

  for (const auto& fallback : scene.shadowFallbacks) {
    if (War3ReplayDrawIsPathBlocker(fallback.snapshot)) {
      g_shadowReplayPathBlockerRejectCount.fetch_add(
          1, std::memory_order_relaxed);
      continue;
    }
    War3ReplayDrawSurvey(fallback.snapshot);
    draws.push_back(&fallback.snapshot);
  }

  if (!war3::internal::kShadowReplayCasterCapEnabled) {
    cache.key = key;
    cache.valid = true;
    war3::render::NoteFinalShadowCasterFrame(
        scene, draws, frameSerial);
    return draws;
  }

  const size_t cap =
      std::max<size_t>(war3::internal::kShadowReplayCasterCap, 1u);
  if (draws.size() <= cap) {
    cache.key = key;
    cache.valid = true;
    war3::render::NoteFinalShadowCasterFrame(
        scene, draws, frameSerial);
    return draws;
  }

  auto computeTier = [](const War3ShadowCasterDraw& draw) {
    if (draw.category == War3RenderState::StageCategory::Terrain)
      return 0u;

    switch (static_cast<war3::render::ObjectKind>(draw.objectKind)) {
    case war3::render::ObjectKind::Building:
      return 1u;
    case war3::render::ObjectKind::Unit:
      return 2u;
    case war3::render::ObjectKind::Destructible:
    case war3::render::ObjectKind::Item:
      return 3u;
    case war3::render::ObjectKind::Effect:
      return 5u;
    default:
      break;
    }

    if (draw.alphaBlendEnabled && !draw.alphaTestEnabled)
      return 5u;

    return 4u;
  };

  auto computeDepth = [&](const War3ShadowCasterDraw& draw) {
    if (!scene.worldCamera.valid || !(draw.boundsRadius > 0.0f))
      return 0.0f;
    const Vector4 viewPos = scene.worldCamera.view * draw.boundsCenter;
    return std::abs(viewPos.z);
  };

  struct RankedDraw {
    const War3ShadowCasterDraw* draw = nullptr;
    uint32_t tier = 0;
    float depth = 0.0f;
    uint32_t order = 0;
  };

  std::vector<RankedDraw> ranked;
  ranked.reserve(draws.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(draws.size()); i++) {
    ranked.push_back(
        RankedDraw{draws[i], computeTier(*draws[i]), computeDepth(*draws[i]), i});
  }

  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const RankedDraw& a, const RankedDraw& b) {
                     if (a.tier != b.tier)
                       return a.tier < b.tier;
                     if (a.depth != b.depth)
                       return a.depth < b.depth;
                     return a.order < b.order;
                   });

  std::vector<const War3ShadowCasterDraw*> limited;
  limited.reserve(cap);
  for (size_t i = 0; i < cap; i++)
    limited.push_back(ranked[i].draw);

  static uint32_t s_capLogCounter = 0;
  if (war3dbg::RenderLogEnabled() &&
      (s_capLogCounter++ % 300u) == 0u) {
    WAR3_RENDER_LOG(
        "DXVK War3Shadow: replay caster cap active total=%u kept=%u dropped=%u\n",
        static_cast<unsigned>(draws.size()), static_cast<unsigned>(limited.size()),
        static_cast<unsigned>(draws.size() - limited.size()));
  }

  // 把裁剪后的 limited 写回 cache.draws（覆盖 unfiltered draws），后续同 scene
  // 调用直接命中。
  draws = std::move(limited);
  cache.key = key;
  cache.valid = true;
  war3::render::NoteFinalShadowCasterFrame(scene, draws, frameSerial);
  return draws;
}

uint32_t ComputeAdaptiveShadowMapPeriod(size_t replayCasterCount) {
  uint32_t period =
      std::max<uint32_t>(war3::internal::kShadowAdaptiveMapUpdatePeriod, 1u);

  if (replayCasterCount >=
      war3::internal::kShadowAdaptiveMapUpdateHugeCasterThreshold) {
    period = std::max<uint32_t>(
        period, war3::internal::kShadowAdaptiveMapUpdateHugeCasterPeriod);
  } else if (replayCasterCount >=
             war3::internal::kShadowAdaptiveMapUpdateHighCasterThreshold) {
    period = std::max<uint32_t>(
        period, war3::internal::kShadowAdaptiveMapUpdateHighCasterPeriod);
  }

  return period;
}

uint64_t EstimateShadowReplayGeometryWork(
    const std::vector<const War3ShadowCasterDraw*>& replayDraws) {
  uint64_t work = 0u;
  for (const auto* draw : replayDraws) {
    if (draw == nullptr)
      continue;

    uint32_t primitiveWork = draw->indexed ? draw->indexCount
                                           : draw->vertexCount;
    if (primitiveWork == 0u)
      primitiveWork = draw->numVertices;
    work += primitiveWork;
  }
  return work;
}

float MaxMatrixAbsDelta(const Matrix4& a, const Matrix4& b) {
  float delta = 0.0f;
  for (uint32_t i = 0; i < 4; i++) {
    delta = std::max(delta, std::abs(a[i].x - b[i].x));
    delta = std::max(delta, std::abs(a[i].y - b[i].y));
    delta = std::max(delta, std::abs(a[i].z - b[i].z));
    delta = std::max(delta, std::abs(a[i].w - b[i].w));
  }
  return delta;
}

float MaxCsmAbsDelta(const War3CsmData& a, const War3CsmData& b) {
  if (a.cascadeCount != b.cascadeCount)
    return std::numeric_limits<float>::infinity();

  float delta = 0.0f;
  for (uint32_t i = 0; i < a.cascadeCount; i++) {
    delta = std::max(delta, MaxMatrixAbsDelta(a.cascades[i].lightViewProj,
                                              b.cascades[i].lightViewProj));
    delta = std::max(delta,
                     std::abs(a.cascades[i].splitFar - b.cascades[i].splitFar));
  }
  return delta;
}

bool ShadowTaaIsCameraCut(const Matrix4& currentView,
                          const Matrix4& historyView,
                          float shadowFarDistance) {
  const Matrix4 currentInvView = inverse(currentView);
  const Matrix4 historyInvView = inverse(historyView);
  if (!IsFiniteMatrix(currentInvView) || !IsFiniteMatrix(historyInvView))
    return true;

  const Vector4 currentEye = currentInvView[3];
  const Vector4 historyEye = historyInvView[3];
  const float dx = currentEye.x - historyEye.x;
  const float dy = currentEye.y - historyEye.y;
  const float dz = currentEye.z - historyEye.z;
  const float positionDeltaSq = dx * dx + dy * dy + dz * dz;

  // Raw view-projection elements contain world-scale translation, so a normal
  // one-tick War3 camera pan easily exceeds an arbitrary matrix epsilon. Use
  // camera pose instead: only a jump spanning a meaningful fraction of the
  // active shadow range is a cut. Ordinary motion remains reprojectable.
  const float positionCutDistance =
      std::clamp(shadowFarDistance * 0.10f, 384.0f, 2048.0f);
  if (!std::isfinite(positionDeltaSq) ||
      positionDeltaSq > positionCutDistance * positionCutDistance) {
    return true;
  }

  const Vector4 currentForward = currentInvView[2];
  const Vector4 historyForward = historyInvView[2];
  const float currentForwardLenSq =
      currentForward.x * currentForward.x +
      currentForward.y * currentForward.y +
      currentForward.z * currentForward.z;
  const float historyForwardLenSq =
      historyForward.x * historyForward.x +
      historyForward.y * historyForward.y +
      historyForward.z * historyForward.z;
  if (!std::isfinite(currentForwardLenSq) ||
      !std::isfinite(historyForwardLenSq) ||
      currentForwardLenSq <= 1.0e-8f || historyForwardLenSq <= 1.0e-8f) {
    return true;
  }

  const float forwardCosine =
      (currentForward.x * historyForward.x +
       currentForward.y * historyForward.y +
       currentForward.z * historyForward.z) /
      std::sqrt(currentForwardLenSq * historyForwardLenSq);
  // A >50 degree one-frame rotation is a camera cut. Smaller rotations are
  // handled by reverse reprojection and per-pixel depth validation.
  return !std::isfinite(forwardCosine) || forwardCosine < 0.64278764f;
}

float MaxCsmSnappedCenterDeltaTexels(
    const War3CsmData& a, const War3CsmData& b) {
  if (a.cascadeCount != b.cascadeCount)
    return std::numeric_limits<float>::infinity();

  float deltaTexels = 0.0f;
  for (uint32_t i = 0u; i < a.cascadeCount; ++i) {
    const float texelSize =
        std::max(a.cascades[i].texelSize, b.cascades[i].texelSize);
    if (!std::isfinite(texelSize) || texelSize <= 1.0e-6f)
      return std::numeric_limits<float>::infinity();
    const Vector4& ac = a.cascades[i].snappedCenterLightSpace;
    const Vector4& bc = b.cascades[i].snappedCenterLightSpace;
    const float cascadeDelta =
        std::max(std::abs(ac.x - bc.x), std::abs(ac.y - bc.y)) /
        texelSize;
    deltaTexels = std::max(deltaTexels, cascadeDelta);
  }
  return deltaTexels;
}

float MaxCsmTexelSizeDelta(const War3CsmData& a, const War3CsmData& b) {
  if (a.cascadeCount != b.cascadeCount)
    return std::numeric_limits<float>::infinity();

  float delta = 0.0f;
  for (uint32_t i = 0u; i < a.cascadeCount; ++i) {
    delta = std::max(
        delta,
        std::abs(a.cascades[i].texelSize - b.cascades[i].texelSize));
  }
  return delta;
}

bool War3CsmContinuityTraceEnabled() {
  static const bool enabled =
      env::getEnvVar("DXVK_WAR3_CSM_CONTINUITY_TRACE") == "1";
  return enabled;
}

uint64_t War3ContinuityHashMatrix(uint64_t hash, const Matrix4& matrix) {
  for (uint32_t row = 0u; row < 4u; ++row) {
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].x));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].y));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].z));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[row].w));
  }
  return hash;
}

uint64_t War3ContinuityHashVector(uint64_t hash, const Vector4& vector) {
  hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(vector.x));
  hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(vector.y));
  hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(vector.z));
  return bit::fnv1a_iter(hash, bit::cast<uint32_t>(vector.w));
}

uint64_t War3ContinuityHashCamera(const War3WorldCameraState& camera) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, uint32_t(camera.valid));
  hash = War3ContinuityHashMatrix(hash, camera.view);
  hash = War3ContinuityHashMatrix(hash, camera.proj);
  hash = War3ContinuityHashMatrix(hash, camera.viewProj);
  hash = bit::fnv1a_iter(hash, camera.viewport.X);
  hash = bit::fnv1a_iter(hash, camera.viewport.Y);
  hash = bit::fnv1a_iter(hash, camera.viewport.Width);
  hash = bit::fnv1a_iter(hash, camera.viewport.Height);
  hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(camera.viewport.MinZ));
  return bit::fnv1a_iter(hash,
                         bit::cast<uint32_t>(camera.viewport.MaxZ));
}

uint64_t War3ContinuityHashCsm(const War3CsmData& csm) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, csm.cascadeCount);
  for (uint32_t i = 0u; i < csm.cascadeCount; ++i) {
    hash = War3ContinuityHashMatrix(hash, csm.cascades[i].lightViewProj);
    hash =
        bit::fnv1a_iter(hash, bit::cast<uint32_t>(csm.cascades[i].splitNear));
    hash =
        bit::fnv1a_iter(hash, bit::cast<uint32_t>(csm.cascades[i].splitFar));
    hash = War3ContinuityHashVector(
        hash, csm.cascades[i].snappedCenterLightSpace);
    hash = bit::fnv1a_iter(
        hash, bit::cast<uint32_t>(csm.cascades[i].texelSize));
  }
  hash = War3ContinuityHashVector(hash, csm.lightDirection);
  return War3ContinuityHashVector(hash, csm.worldUp);
}

uint64_t War3ContinuityDeltaNano(float delta) {
  if (!std::isfinite(delta))
    return std::numeric_limits<uint64_t>::max();
  const double scaled =
      std::max(0.0, static_cast<double>(delta)) * 1000000000.0;
  if (scaled >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
    return std::numeric_limits<uint64_t>::max();
  return static_cast<uint64_t>(std::llround(scaled));
}

struct War3ReplayContinuityHashes {
  uint64_t contentHash = 0u;
  uint64_t backingHash = 0u;
  uint64_t stage13ContentHash = 0u;
  uint64_t stage13BackingHash = 0u;
  uint32_t stage13DrawCount = 0u;
};

uint64_t War3ContinuityHashDrawContent(
    uint64_t hash, const War3ShadowCasterDraw& draw) {
  hash = bit::fnv1a_iter(hash, uint32_t(draw.indexed));
  hash = bit::fnv1a_iter(hash, uint32_t(draw.topology));
  hash = bit::fnv1a_iter(hash, draw.positionStride);
  hash = bit::fnv1a_iter(hash, draw.positionOffset);
  hash = bit::fnv1a_iter(hash, uint32_t(draw.positionFormat));
  hash = bit::fnv1a_iter(hash, uint32_t(draw.indexType));
  hash = bit::fnv1a_iter(hash, draw.indexCount);
  hash = bit::fnv1a_iter(hash, draw.firstIndex);
  hash = bit::fnv1a_iter(hash, uint32_t(draw.vertexOffset));
  hash = bit::fnv1a_iter(hash, draw.vertexCount);
  hash = bit::fnv1a_iter(hash, draw.firstVertex);
  hash = bit::fnv1a_iter(hash, draw.minVertexIndex);
  hash = bit::fnv1a_iter(hash, draw.numVertices);
  hash = bit::fnv1a_iter(hash, uint32_t(draw.alphaTestEnabled));
  hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(draw.alphaRef));
  hash = bit::fnv1a_iter(hash, draw.uvStride);
  hash = bit::fnv1a_iter(hash, draw.uvOffset);
  hash = bit::fnv1a_iter(hash, uint32_t(draw.uvFormat));
  hash = bit::fnv1a_iter(hash, draw.uvBinding);
  hash = bit::fnv1a_iter(hash, uint32_t(draw.category));
  hash = bit::fnv1a_iter(hash, uint32_t(draw.batchTag));
  hash = bit::fnv1a_iter(hash, uint32_t(int32_t(draw.stage)));
  hash = bit::fnv1a_iter(hash, draw.rawcode);
  hash = bit::fnv1a_iter(hash, draw.jHandle);
  hash = War3ContinuityHashMatrix(hash, draw.worldMatrix);
  hash = War3ContinuityHashVector(hash, draw.boundsCenter);
  return bit::fnv1a_iter(hash, bit::cast<uint32_t>(draw.boundsRadius));
}

uint64_t War3ContinuityHashDrawBacking(
    uint64_t hash, const War3ShadowCasterDraw& draw) {
  hash = bit::fnv1a_iter(hash, War3RcObjectId(draw.positionStorage));
  hash = bit::fnv1a_iter(hash, uint64_t(draw.positionInfo.offset));
  hash = bit::fnv1a_iter(hash, uint64_t(draw.positionInfo.size));
  hash = bit::fnv1a_iter(hash, War3RcObjectId(draw.indexStorage));
  hash = bit::fnv1a_iter(hash, uint64_t(draw.indexInfo.offset));
  hash = bit::fnv1a_iter(hash, uint64_t(draw.indexInfo.size));
  hash = bit::fnv1a_iter(hash, War3RcObjectId(draw.blendStorage));
  hash = bit::fnv1a_iter(hash, uint64_t(draw.blendInfo.offset));
  hash = bit::fnv1a_iter(hash, uint64_t(draw.blendInfo.size));
  hash = bit::fnv1a_iter(hash, War3RcObjectId(draw.uvStorage));
  hash = bit::fnv1a_iter(hash, uint64_t(draw.uvInfo.offset));
  hash = bit::fnv1a_iter(hash, uint64_t(draw.uvInfo.size));
  return bit::fnv1a_iter(hash, War3RcObjectId(draw.diffuseTexture));
}

War3ReplayContinuityHashes War3BuildReplayContinuityHashes(
    const std::vector<const War3ShadowCasterDraw*>& replayDraws) {
  War3ReplayContinuityHashes result = {};
  result.contentHash = bit::fnv1a_init();
  result.backingHash = bit::fnv1a_init();
  result.stage13ContentHash = bit::fnv1a_init();
  result.stage13BackingHash = bit::fnv1a_init();
  result.contentHash =
      bit::fnv1a_iter(result.contentHash, uint64_t(replayDraws.size()));
  result.backingHash =
      bit::fnv1a_iter(result.backingHash, uint64_t(replayDraws.size()));
  for (const War3ShadowCasterDraw* draw : replayDraws) {
    if (draw == nullptr)
      continue;
    result.contentHash =
        War3ContinuityHashDrawContent(result.contentHash, *draw);
    result.backingHash =
        War3ContinuityHashDrawBacking(result.backingHash, *draw);
    if (draw->stage != 13)
      continue;
    ++result.stage13DrawCount;
    result.stage13ContentHash =
        War3ContinuityHashDrawContent(result.stage13ContentHash, *draw);
    result.stage13BackingHash =
        War3ContinuityHashDrawBacking(result.stage13BackingHash, *draw);
  }
  result.stage13ContentHash =
      bit::fnv1a_iter(result.stage13ContentHash, result.stage13DrawCount);
  result.stage13BackingHash =
      bit::fnv1a_iter(result.stage13BackingHash, result.stage13DrawCount);
  return result;
}

struct OutlineEdgePushConstants {
  uint32_t visibleSampler;
  uint32_t allSampler;
  float invWidth;
  float invHeight;
  float widthPx;
  uint32_t showVisible;  // 0/1
  uint32_t showOccluded; // 0/1
  float color[4];        // RGBA
};

// Must match `shaders/war3_shadow_receiver.frag` UBO layout (scalar +
// row_major). Shader order:
//   mat4 u_view; mat4 u_invViewProj; mat4 u_lightViewProj[4];
//   vec4 u_splitFar; vec4 u_params; vec4 u_params2; vec4 u_params3; vec4
//   u_params4; vec4 u_sunDir; vec4 u_params5; vec4 u_params6; vec4 u_viewport;
//   vec4 u_viewportZ; mat4 u_prevViewProj; vec4 u_taaParams; mat4 u_proj;
//   vec4 u_pointRayParams; vec4 u_pointRayParams2; vec4 u_depthContract;
struct ShadowReceiverUniform {
  Matrix4 view;
  Matrix4 invViewProj;
  Matrix4 lightViewProj[4];
  Vector4 splitFar;
  Vector4
      params; // x=shadowStrength, y=pcfRadius, z=invShadowRes, w=cascadeCount
  Vector4 params2; // x=receiverBias, y=cascadeBlendRange, z=debugMode,
                   // w=pointLightsEnabled
  Vector4 params3; // x=invViewportW, y=invViewportH, z=pcssEnable,
                   // w=pcssSearchRadius(texel)
  Vector4 params4; // x=pcssMinRadius(texel), y=pcssMaxRadius(texel),
                   // z=pcssDepthScale, w=cascadeBiasScale
  Vector4 sunDir;  // xyz=direction, w=unused
  Vector4
      params5; // x=normalBiasScale, y=rimIntensity, z=rimPower, w=receiverMode
  Vector4 params6;   // x=pcfKernel, y=pcfRotateMode, z=pcssSearchKernel,
                     // w=pcfCascadeRadiusScale
  Vector4 viewport;  // x=vpX, y=vpY, z=vpW, w=vpH
  Vector4 viewportZ; // x=minZ, y=maxZ, z=S1 terrain mask enabled,
                     // w=S1 terrain mask depth epsilon
  Matrix4 prevViewProj;
  Vector4
      taaParams; // x=taaEnabled, y=blendFactor, z=neighborClamp, w=hasHistory
  Matrix4 proj;
  Vector4 pointRayParams; // x=enabled, y=strength, z=maxDistance,
                          // w=surfaceThickness (view/world units)
  Vector4 pointRayParams2; // x=steps, y=startOffset, z=maxLights, w=A1 layers
  Vector4 depthContract; // x=far clear raw, y=known, z=raw quantum, w=reserved
};
static_assert(sizeof(ShadowReceiverUniform) == 736u,
              "ShadowReceiverUniform must match GLSL scalar layout");
static_assert(offsetof(ShadowReceiverUniform, proj) == 624u,
              "ShadowReceiver projection ABI drift");
static_assert(offsetof(ShadowReceiverUniform, pointRayParams) == 688u,
              "ShadowReceiver point-ray ABI drift");
static_assert(offsetof(ShadowReceiverUniform, depthContract) == 720u,
              "ShadowReceiver depth-contract ABI drift");

Vector4 Cross3(const Vector4 &a, const Vector4 &b) {
  return Vector4(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x, 0.0f);
}

Vector4 Normalize3(Vector4 v) {
  const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
  if (len2 <= std::numeric_limits<float>::min())
    return v;
  const float inv = 1.0f / std::sqrt(len2);
  v.x *= inv;
  v.y *= inv;
  v.z *= inv;
  v.w = 0.0f;
  return v;
}

Matrix4 MakeLookAtLH(const Vector4 &eye, const Vector4 &target,
                     const Vector4 &up) {
  // Row-vector convention: p' = p * M
  const Vector4 f = Normalize3(
      Vector4(target.x - eye.x, target.y - eye.y, target.z - eye.z, 0.0f));
  Vector4 r = Normalize3(Cross3(up, f));
  const Vector4 u = Cross3(f, r);

  Matrix4 m;
  m[0] = Vector4(r.x, u.x, f.x, 0.0f);
  m[1] = Vector4(r.y, u.y, f.y, 0.0f);
  m[2] = Vector4(r.z, u.z, f.z, 0.0f);
  m[3] = Vector4(-(eye.x * r.x + eye.y * r.y + eye.z * r.z),
                 -(eye.x * u.x + eye.y * u.y + eye.z * u.z),
                 -(eye.x * f.x + eye.y * f.y + eye.z * f.z), 1.0f);
  return m;
}

uint64_t War3ShadowWorkloadVertexCount(
    const War3ShadowCasterDraw& draw) noexcept {
  if (!draw.indexed)
    return draw.vertexCount;
  if (draw.numVertices != 0u)
    return draw.numVertices;
  if (draw.vertexCount != 0u)
    return draw.vertexCount;
  // A missing indexed vertex hint must not make the workload look free.
  return draw.indexCount;
}

bool AddWar3ShadowWorkloadDraw(
    war3::render::War3GpuWorkloadCost& cost,
    const War3ShadowCasterDraw& draw, uint64_t repeatCount) noexcept {
  return war3::render::War3GpuWorkloadGovernor::addRepeatedDraw(
      cost, repeatCount, War3ShadowWorkloadVertexCount(draw),
      draw.indexed ? uint64_t(draw.indexCount) : 0u);
}

const char* War3GpuWorkloadConsumerName(
    war3::render::War3GpuWorkloadConsumer consumer) noexcept {
  switch (consumer) {
  case war3::render::War3GpuWorkloadConsumer::DirectionalCsm:
    return "directional-csm";
  case war3::render::War3GpuWorkloadConsumer::VolumeSun:
    return "volume-sun";
  case war3::render::War3GpuWorkloadConsumer::PointShadow:
    return "point-shadow";
  default:
    return "invalid";
  }
}

class ScopedWar3GpuWorkloadReservation final {
public:
  explicit ScopedWar3GpuWorkloadReservation(
      war3::render::War3GpuWorkloadGovernor& governor) noexcept
      : m_governor(governor) {}

  ~ScopedWar3GpuWorkloadReservation() {
    if (m_active && !m_committed) {
      m_governor.cancelReservation(m_consumer, m_itemCount, m_cost);
    }
  }

  bool reserve(war3::render::War3GpuWorkloadConsumer consumer,
               uint64_t itemCount,
               const war3::render::War3GpuWorkloadCost& cost) noexcept {
    if (m_active || !m_governor.tryReserve(consumer, itemCount, cost))
      return false;
    m_consumer = consumer;
    m_itemCount = itemCount;
    m_cost = cost;
    m_active = true;
    return true;
  }

  void commit() noexcept {
    if (m_active)
      m_committed = true;
  }

private:
  war3::render::War3GpuWorkloadGovernor& m_governor;
  war3::render::War3GpuWorkloadConsumer m_consumer =
      war3::render::War3GpuWorkloadConsumer::Count;
  uint64_t m_itemCount = 0u;
  war3::render::War3GpuWorkloadCost m_cost = {};
  bool m_active = false;
  bool m_committed = false;
};
} // namespace

ShadowTaaDiagnostics QueryShadowTaaDiagnostics() {
  std::lock_guard<std::mutex> lock(g_shadowDiagnosticsMutex);
  return g_shadowTaaDiagnostics;
}

CsmResolutionDiagnostics QueryCsmResolutionDiagnostics() {
  std::lock_guard<std::mutex> lock(g_shadowDiagnosticsMutex);
  return g_csmResolutionDiagnostics;
}

PointShadowPersistentDiagnostics
QueryPointShadowPersistentDiagnostics() {
  std::lock_guard<std::mutex> lock(g_shadowDiagnosticsMutex);
  return g_pointShadowPersistentDiagnostics;
}

war3::render::War3GpuWorkloadGovernorDiagnostics
QueryWar3GpuWorkloadGovernorDiagnostics() {
  std::lock_guard<std::mutex> lock(g_shadowDiagnosticsMutex);
  return g_gpuWorkloadGovernorDiagnostics;
}

ShadowReplayDiagnostics QueryShadowReplayDiagnostics() {
  ShadowReplayDiagnostics result = {};
#define WAR3_LOAD_REPLAY_DIAG(name) \
  result.name = g_shadowReplayDiagnostics.name.load(std::memory_order_acquire)
  WAR3_LOAD_REPLAY_DIAG(mapEpoch);
  WAR3_LOAD_REPLAY_DIAG(deviceEpoch);
  WAR3_LOAD_REPLAY_DIAG(candidateFrameSerial);
  WAR3_LOAD_REPLAY_DIAG(firstCompleteLatencyFrames);
  WAR3_LOAD_REPLAY_DIAG(staleEpochConsumerRejectCount);
  WAR3_LOAD_REPLAY_DIAG(validationRejectCount);
  WAR3_LOAD_REPLAY_DIAG(partialPreventedCount);
  WAR3_LOAD_REPLAY_DIAG(pointWorkerCancelCount);
  WAR3_LOAD_REPLAY_DIAG(pointLateResultRejectCount);
  WAR3_LOAD_REPLAY_DIAG(plannedCasterCount);
  WAR3_LOAD_REPLAY_DIAG(replayCasterCount);
  WAR3_LOAD_REPLAY_DIAG(validatedCasterCount);
  WAR3_LOAD_REPLAY_DIAG(drawnCasterCount);
  WAR3_LOAD_REPLAY_DIAG(lastRejectReason);
  WAR3_LOAD_REPLAY_DIAG(lastOffenderMapEpoch);
  WAR3_LOAD_REPLAY_DIAG(lastRequiredEnd);
  WAR3_LOAD_REPLAY_DIAG(lastAvailableSize);
  WAR3_LOAD_REPLAY_DIAG(lastMinimumVertex);
  WAR3_LOAD_REPLAY_DIAG(lastMaximumVertex);
  WAR3_LOAD_REPLAY_DIAG(lastVertexOffset);
  WAR3_LOAD_REPLAY_DIAG(lastStage);
  WAR3_LOAD_REPLAY_DIAG(lastCategory);
  WAR3_LOAD_REPLAY_DIAG(lastBatchTag);
  WAR3_LOAD_REPLAY_DIAG(lastObjectKind);
  WAR3_LOAD_REPLAY_DIAG(lastRawcode);
  WAR3_LOAD_REPLAY_DIAG(lastJHandle);
  WAR3_LOAD_REPLAY_DIAG(lastIndexCount);
  WAR3_LOAD_REPLAY_DIAG(lastFirstIndex);
  WAR3_LOAD_REPLAY_DIAG(lastMinVertexIndex);
  WAR3_LOAD_REPLAY_DIAG(lastNumVertices);
  WAR3_LOAD_REPLAY_DIAG(lastActualIndexMin);
  WAR3_LOAD_REPLAY_DIAG(lastActualIndexMax);
  WAR3_LOAD_REPLAY_DIAG(lastActualIndexDomainKnown);
  WAR3_LOAD_REPLAY_DIAG(lastFullVertexDomainFallback);
  WAR3_LOAD_REPLAY_DIAG(lastPositionSize);
#undef WAR3_LOAD_REPLAY_DIAG
  return result;
}

void PublishShadowTaaDiagnostics(
    const ShadowTaaDiagnostics& diagnostics) {
  std::lock_guard<std::mutex> lock(g_shadowDiagnosticsMutex);
  g_shadowTaaDiagnostics = diagnostics;
}

void PublishCsmResolutionDiagnostics(
    const CsmResolutionDiagnostics& diagnostics) {
  std::lock_guard<std::mutex> lock(g_shadowDiagnosticsMutex);
  g_csmResolutionDiagnostics = diagnostics;
}

void PublishPointShadowPersistentDiagnostics(
    const PointShadowPersistentDiagnostics& diagnostics) {
  std::lock_guard<std::mutex> lock(g_shadowDiagnosticsMutex);
  g_pointShadowPersistentDiagnostics = diagnostics;
}

War3ShadowReceiverPass::War3ShadowReceiverPass(D3D9DeviceEx *device)
    : m_parent(device), m_device(device->GetDXVKDevice()),
      m_layout(createPipelineLayout()),
      m_shadowCasterLayout(createShadowCasterPipelineLayout()) {
  m_pointShadowPersistentRendererEpoch =
      MintPointShadowPersistentRendererEpoch();
  // Cannot init MRT layout here until we know we need it?
  // Better to defer init.

  DxvkSamplerKey samplerInfo = {};
  samplerInfo.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                        VK_SAMPLER_MIPMAP_MODE_NEAREST);
  samplerInfo.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  samplerInfo.setUsePixelCoordinates(false);

  m_samplerLinear = m_device->createSampler(samplerInfo);

  DxvkSamplerKey shadowSampler = {};
  shadowSampler.setFilter(VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                          VK_SAMPLER_MIPMAP_MODE_NEAREST);
  shadowSampler.setAddressModes(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  shadowSampler.setUsePixelCoordinates(false);
  m_shadowSampler = m_device->createSampler(shadowSampler);

  DxvkSamplerKey shadowCompareSampler = shadowSampler;
  shadowCompareSampler.setDepthCompare(true, VK_COMPARE_OP_LESS_OR_EQUAL);
  m_shadowCompareSampler = m_device->createSampler(shadowCompareSampler);

  DxvkSamplerKey shadowCompareSamplerLinear = {};
  shadowCompareSamplerLinear.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                       VK_SAMPLER_MIPMAP_MODE_NEAREST);
  shadowCompareSamplerLinear.setAddressModes(
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  shadowCompareSamplerLinear.setUsePixelCoordinates(false);
  shadowCompareSamplerLinear.setDepthCompare(
      true, VK_COMPARE_OP_LESS_OR_EQUAL);
  m_shadowCompareSamplerLinear =
      m_device->createSampler(shadowCompareSamplerLinear);
  m_shadowCompareSamplerActive = m_shadowCompareSampler;

  const VkFormatFeatureFlags2 d32Features =
      m_device->getFormatFeatures(VK_FORMAT_D32_SFLOAT).optimal;
  m_shadowCompareLinearSupported =
      (d32Features &
       VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0u;
  WAR3_RENDER_LOG(
      "DXVK War3Shadow: D32 comparison-linear PCF %s; unsupported devices "
      "use manual compare-first 2x2.\n",
      m_shadowCompareLinearSupported ? "supported" : "unsupported");

  // 初始化性能监控设备句柄（用于 GPU 时间戳）
  war3::War3PerfMonitor::instance().setDevice(m_device.ptr());

  // 初始化 AlphaTest 采样器（稳定树影轮廓，避免 mip 闪烁）
  DxvkSamplerKey fallbackKey = {};
  fallbackKey.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                        VK_SAMPLER_MIPMAP_MODE_NEAREST);
  fallbackKey.setAddressModes(VK_SAMPLER_ADDRESS_MODE_REPEAT,
                              VK_SAMPLER_ADDRESS_MODE_REPEAT,
                              VK_SAMPLER_ADDRESS_MODE_REPEAT);
  fallbackKey.setLodRange(0.0f, 0.0f, 0.0f);
  m_fallbackSampler = m_device->createSampler(fallbackKey);

  // Alpha 阴影允许 Mip 的采样器（远景更稳定）
  DxvkSamplerKey fallbackMipKey = {};
  fallbackMipKey.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                           VK_SAMPLER_MIPMAP_MODE_LINEAR);
  fallbackMipKey.setAddressModes(VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                 VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                 VK_SAMPLER_ADDRESS_MODE_REPEAT);
  fallbackMipKey.setLodRange(0.0f, 12.0f, 0.0f);
  m_fallbackSamplerMip = m_device->createSampler(fallbackMipKey);

  DxvkBufferCreateInfo uboInfo = {};
  uboInfo.size = sizeof(ShadowReceiverUniform);
  uboInfo.usage =
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  uboInfo.stages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  uboInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
  m_shadowUniformBuffer =
      m_device->createBuffer(uboInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  // Create Light Buffer
  DxvkBufferCreateInfo lightInfo = {};
  lightInfo.size = sizeof(LightUniform);
  lightInfo.usage =
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  lightInfo.stages =
      VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  lightInfo.access = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
  m_lightBuffer =
      m_device->createBuffer(lightInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

War3ShadowReceiverPass::~War3ShadowReceiverPass() {
  if (m_pointShadowPersistentWorker)
    m_pointShadowPersistentWorker->shutdown();
  // Point-shadow CPU preparation captures this pass and writes its plan/state.
  // Drain it before tearing down perf state, pipelines, or member resources;
  // relying on std::future's member destructor is too late because that runs
  // only after this destructor body has already destroyed Vulkan objects.
  waitPointShadowCpuPrepare();
  war3::War3PerfMonitor::instance().shutdown();
  auto vk = m_device->vkd();
  for (auto &kv : m_pipelines) {
    if (kv.second.pipeline != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second.pipeline, nullptr);
  }
  // Shadow-caster pipelines are command-list-owned.  Releasing the cache
  // here cannot destroy a pipeline still referenced by submitted GPU work.
  m_shadowCasterPipelines.clear();
  if (m_motionVectorPipeline != VK_NULL_HANDLE)
    vk->vkDestroyPipeline(vk->device(), m_motionVectorPipeline, nullptr);
  if (m_shadowVisibilityPipeline != VK_NULL_HANDLE)
    vk->vkDestroyPipeline(vk->device(), m_shadowVisibilityPipeline, nullptr);
  // m_outlineMaskLayouts loop removed (not tracked in list)
  for (auto &kv : m_outlineMaskVisiblePipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
  for (auto &kv : m_outlineMaskAllPipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
  for (auto &kv : m_outlineEdgePipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
  for (auto &kv : m_outlineMaskMRTPipelines) {
    if (kv.second != VK_NULL_HANDLE)
      vk->vkDestroyPipeline(vk->device(), kv.second, nullptr);
  }
}

Rc<DxvkSampler> War3ShadowReceiverPass::getFallbackSampler(bool useMip,
                                                           float mipLodBias) {
  if (!useMip)
    return m_fallbackSampler;

  // 量化 LOD bias，避免每次微调都创建新的 VkSampler
  // 经验值：0.25 步进足以满足调参，同时不会引入明显“卡档”感。
  const float finiteMipLodBias =
      std::isfinite(mipLodBias) ? mipLodBias : 0.0f;
  const float clampedBias = std::clamp(finiteMipLodBias, -4.0f, 4.0f);
  constexpr float kStep = 0.25f;
  const int32_t q = int32_t(std::lround(clampedBias / kStep));
  if (q == 0)
    return m_fallbackSamplerMip;

  auto it = m_fallbackSamplerMipBias.find(q);
  if (it != m_fallbackSamplerMipBias.end())
    return it->second;

  const float biasQ = float(q) * kStep;

  DxvkSamplerKey key = {};
  key.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                VK_SAMPLER_MIPMAP_MODE_LINEAR);
  key.setAddressModes(VK_SAMPLER_ADDRESS_MODE_REPEAT,
                      VK_SAMPLER_ADDRESS_MODE_REPEAT,
                      VK_SAMPLER_ADDRESS_MODE_REPEAT);
  key.setLodRange(0.0f, 12.0f, biasQ);

  Rc<DxvkSampler> sampler = m_device->createSampler(key);
  m_fallbackSamplerMipBias.emplace(q, sampler);
  return sampler;
}

const DxvkPipelineLayout *War3ShadowReceiverPass::createPipelineLayout() const {
  std::array<DxvkDescriptorSetLayoutBinding, 14> bindings = {
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT), // 0: color
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT), // 1: depth
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 2: shadow map (CSM)
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 3: CSM uniforms
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT), // 4: Lights
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 5: [NEW] Point Shadow Cube
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 6: [NEW] PointShadow UBO
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 7: [ShadowTAA] ShadowCurrent
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 8: [ShadowTAA] MotionVector
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 9: [ShadowTAA] ShadowHistory(Read)
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 10: [ShadowTAA] ShadowHistory(Write)
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 11: S1 terrain caster mask
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 12: A1 contact visibility/confidence
      DxvkDescriptorSetLayoutBinding(
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
          VK_SHADER_STAGE_FRAGMENT_BIT), // 13: A1 Hi-Z min/max pyramid
  };
  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap, VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(ReceiverPushConstants), bindings.size(), bindings.data());
}

const DxvkPipelineLayout *
War3ShadowReceiverPass::createOutlineMaskPipelineLayout() const {
  // sampler heap 仍由 set0 隐式提供；下列数组按顺序声明 set1 的
  // binding0..4，并与 regular shadow caster 使用同一资源槽语义。

  std::array<DxvkDescriptorSetLayoutBinding, 5> bindings = {
      // binding=0：骨骼/世界矩阵 SSBO
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT),
      // binding=1：Alpha 测试纹理
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      // binding=2：描边手动比较使用的场景深度
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      // binding=3/4：VS-S1 静态输入与调色板；非 direct 使用矩阵 SSBO 兜底。
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT),
  };

  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap,
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(ShadowCasterPushConstants), bindings.size(), bindings.data());
}

War3ShadowReceiverPass::Pipeline
War3ShadowReceiverPass::getPipeline(VkFormat format,
                                    VkSampleCountFlagBits samples) {
  PipelineKey key;
  key.format = format;
  key.samples = samples;

  auto it = m_pipelines.find(key);
  if (it != m_pipelines.end())
    return it->second;

  Pipeline pipeline = createPipeline(key);
  m_pipelines.insert({key, pipeline});
  return pipeline;
}

War3ShadowReceiverPass::Pipeline
War3ShadowReceiverPass::createPipeline(const PipelineKey &key) const {
  util::DxvkBuiltInGraphicsState state = {};

  state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);

  // Check for override
  auto *mat =
      war3::ShaderManager::get().getMaterial(war3shader::RenderStageId::Shadow);
  IDirect3DPixelShader9 *overridePs =
      (mat && mat->isCompiled()) ? mat->getPixelShader() : nullptr;

  std::vector<uint32_t> customSpirv;

  if (overridePs) {
    auto *d3dPs = static_cast<D3D9PixelShader *>(overridePs);

    std::stringstream ss;
    d3dPs->GetCommonShader()->GetShader()->dump(ss);
    std::string sData = ss.str();

    customSpirv.resize(sData.size() / 4);
    std::memcpy(customSpirv.data(), sData.data(), sData.size());

    state.fs.code = customSpirv.data();
    state.fs.size = customSpirv.size() * sizeof(uint32_t);
    state.fs.spec = nullptr;
  } else {
    state.fs = util::DxvkBuiltInShaderStage(war3_shadow_receiver, nullptr);
  }

  state.colorFormat = key.format;
  state.sampleCount = key.samples;

  Pipeline p;
  p.layout = m_layout;
  p.pipeline = m_device->createBuiltInGraphicsPipeline(m_layout, state);
  return p;
}

const DxvkPipelineLayout *
War3ShadowReceiverPass::createShadowCasterPipelineLayout() const {
  // binding 0..4 与描边布局保持一致；regular 路径的 binding2 是未使用的
  // sampled-image 槽，描边路径继续在同一位置绑定 scene depth。
  std::array<DxvkDescriptorSetLayoutBinding, 5> bindings = {
      // binding=0：骨骼/世界矩阵 SSBO
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT),
      // binding=1：Alpha 测试纹理
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT),
      DxvkDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT),
  };

  return m_device->createBuiltInPipelineLayout(
      DxvkPipelineLayoutFlag::UsesSamplerHeap, // [Fix] 启用采样器堆
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      sizeof(ShadowCasterPushConstants), bindings.size(), bindings.data());
}

War3ShadowReceiverPass::ShadowCasterPipeline
War3ShadowReceiverPass::getShadowCasterPipeline(
    const ShadowCasterPipelineKey &key) {
  auto it = m_shadowCasterPipelines.find(key);
  if (it != m_shadowCasterPipelines.end())
    return it->second;

  ShadowCasterPipeline pipeline = createShadowCasterPipeline(key);
  m_shadowCasterPipelines.insert({key, pipeline});
  return pipeline;
}

War3ShadowReceiverPass::ShadowCasterPipeline
War3ShadowReceiverPass::createShadowCasterPipeline(
    const ShadowCasterPipelineKey &key) const {
  // Caster masks are directional-CSM color outputs, whereas point shadows
  // write radial distance into cube depth. A pipeline can never be both.
  if (key.casterMaskEnabled && key.pointShadowRadialDepth)
    return {};

  if (key.alphaTestEnabled &&
      (key.uvBinding > 2u ||
       (key.uvBinding == 0u && key.uvStride != key.positionStride) ||
       (key.uvBinding == 1u && key.blendBinding == 1u &&
        key.uvStride != key.blendStride))) {
    return {};
  }

  VkVertexInputBindingDescription bindings[3] = {};
  uint32_t bindingCount = 1;

  bindings[0].binding = 0;
  bindings[0].stride = key.positionStride;
  bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  if (key.blendBinding == 1) {
    bindings[bindingCount].binding = 1;
    bindings[bindingCount].stride = key.blendStride;
    bindings[bindingCount].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    ++bindingCount;
  }

  if (key.alphaTestEnabled && key.uvBinding != 0u &&
      key.uvBinding != key.blendBinding) {
    bindings[bindingCount].binding = key.uvBinding;
    bindings[bindingCount].stride = key.uvStride;
    bindings[bindingCount].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    ++bindingCount;
  }

  // ===== 顶点属性：位置、混合权重、混合索引、UV =====
  std::array<VkVertexInputAttributeDescription, 4> attributes = {};
  uint32_t attributeCount = 0;

  // 位置 (location = 0)
  attributes[attributeCount].location = 0;
  attributes[attributeCount].binding = 0;
  attributes[attributeCount].format = key.positionFormat;
  attributes[attributeCount].offset = key.positionOffset;
  attributeCount++;

  // 混合权重 (location = 1)
  if (key.blendWeightFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 1;
    attributes[attributeCount].binding = key.blendBinding;
    attributes[attributeCount].format = key.blendWeightFormat;
    attributes[attributeCount].offset = key.blendWeightOffset;
    attributeCount++;
  }

  // 混合索引 (location = 2)
  if (key.blendIndexFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 2;
    attributes[attributeCount].binding = key.blendBinding;
    attributes[attributeCount].format = key.blendIndexFormat;
    attributes[attributeCount].offset = key.blendIndexOffset;
    attributeCount++;
  }

  // UV纹理坐标 (location = 3) - Alpha测试用
  if (key.alphaTestEnabled && key.uvFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 3;
    attributes[attributeCount].binding = key.uvBinding;
    attributes[attributeCount].format = key.uvFormat;
    attributes[attributeCount].offset = key.uvOffset;
    attributeCount++;
  }

  VkPipelineVertexInputStateCreateInfo viState = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  viState.vertexBindingDescriptionCount = bindingCount;
  viState.pVertexBindingDescriptions = bindings;
  viState.vertexAttributeDescriptionCount = attributeCount;
  viState.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo iaState = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  iaState.topology = key.topology;
  iaState.primitiveRestartEnable = VK_FALSE;

  VkPipelineRasterizationStateCreateInfo rsState = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rsState.depthClampEnable = VK_TRUE;
  rsState.rasterizerDiscardEnable = VK_FALSE;
  rsState.polygonMode = VK_POLYGON_MODE_FILL;
  rsState.cullMode = VK_CULL_MODE_NONE;
  rsState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  const bool depthBiasEnabled =
      (m_shadowCasterBiasConstant != 0.0f) || (m_shadowCasterBiasSlope != 0.0f);
  rsState.depthBiasEnable = depthBiasEnabled ? VK_TRUE : VK_FALSE;
  rsState.depthBiasConstantFactor = m_shadowCasterBiasConstant;
  rsState.depthBiasSlopeFactor = m_shadowCasterBiasSlope;
  rsState.depthBiasClamp = m_shadowCasterBiasClamp;
  rsState.lineWidth = 1.0f;

  VkPipelineDepthStencilStateCreateInfo dsState = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  dsState.depthTestEnable = VK_TRUE;
  dsState.depthWriteEnable =
      key.casterMaskEnabled ? VK_FALSE : VK_TRUE;
  dsState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  dsState.depthBoundsTestEnable = VK_FALSE;
  dsState.stencilTestEnable = VK_FALSE;

  util::DxvkBuiltInGraphicsState state = {};
  state.vs = util::DxvkBuiltInShaderStage(war3_shadow_caster_vert, nullptr);
  if (key.casterMaskEnabled) {
    state.fs = util::DxvkBuiltInShaderStage(war3_shadow_caster_mask, nullptr);
  } else if (key.pointShadowRadialDepth) {
    state.fs =
        util::DxvkBuiltInShaderStage(war3_shadow_caster_point_frag, nullptr);
  } else {
    state.fs = util::DxvkBuiltInShaderStage(war3_shadow_caster_frag, nullptr);
  }
  if (key.casterMaskEnabled)
    state.colorFormat = VK_FORMAT_R8_UNORM;
  state.depthFormat = VK_FORMAT_D32_SFLOAT;
  state.viState = &viState;
  state.iaState = &iaState;
  state.rsState = &rsState;
  state.dsState = &dsState;

  ShadowCasterPipeline p;
  p.layout = m_shadowCasterLayout;
  p.pipeline =
      m_device->createBuiltInGraphicsPipeline(m_shadowCasterLayout, state);
  p.lifetime = war3::render::AdoptWar3TrackedVkPipeline(
      m_device, p.pipeline);
  if (!p.lifetime)
    p.pipeline = VK_NULL_HANDLE;
  return p;
}

// [NEW] Helper to create MRT pipeline
War3ShadowReceiverPass::ShadowCasterPipeline
War3ShadowReceiverPass::createOutlineMaskPipeline(
    const ShadowCasterPipelineKey &key) const {
  if (key.alphaTestEnabled &&
      (key.uvBinding > 2u ||
       (key.uvBinding == 0u && key.uvStride != key.positionStride) ||
       (key.uvBinding == 1u && key.blendBinding == 1u &&
        key.uvStride != key.blendStride))) {
    return {};
  }

  VkVertexInputBindingDescription bindings[3] = {};
  uint32_t bindingCount = 1;

  bindings[0].binding = 0;
  bindings[0].stride = key.positionStride;
  bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  if (key.blendBinding == 1) {
    bindings[bindingCount].binding = 1;
    bindings[bindingCount].stride = key.blendStride;
    bindings[bindingCount].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    ++bindingCount;
  }

  if (key.alphaTestEnabled && key.uvBinding != 0u &&
      key.uvBinding != key.blendBinding) {
    bindings[bindingCount].binding = key.uvBinding;
    bindings[bindingCount].stride = key.uvStride;
    bindings[bindingCount].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    ++bindingCount;
  }

  std::array<VkVertexInputAttributeDescription, 4> attributes = {};
  uint32_t attributeCount = 0;

  attributes[attributeCount].location = 0;
  attributes[attributeCount].binding = 0;
  attributes[attributeCount].format = key.positionFormat;
  attributes[attributeCount].offset = key.positionOffset;
  attributeCount++;

  if (key.blendWeightFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 1;
    attributes[attributeCount].binding = key.blendBinding;
    attributes[attributeCount].format = key.blendWeightFormat;
    attributes[attributeCount].offset = key.blendWeightOffset;
    attributeCount++;
  }

  if (key.blendIndexFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 2;
    attributes[attributeCount].binding = key.blendBinding;
    attributes[attributeCount].format = key.blendIndexFormat;
    attributes[attributeCount].offset = key.blendIndexOffset;
    attributeCount++;
  }

  if (key.alphaTestEnabled && key.uvFormat != VK_FORMAT_UNDEFINED) {
    attributes[attributeCount].location = 3;
    attributes[attributeCount].binding = key.uvBinding;
    attributes[attributeCount].format = key.uvFormat;
    attributes[attributeCount].offset = key.uvOffset;
    attributeCount++;
  }

  VkPipelineVertexInputStateCreateInfo viState = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  viState.vertexBindingDescriptionCount = bindingCount;
  viState.pVertexBindingDescriptions = bindings;
  viState.vertexAttributeDescriptionCount = attributeCount;
  viState.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo iaState = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  iaState.topology = key.topology;
  iaState.primitiveRestartEnable = VK_FALSE;

  VkPipelineRasterizationStateCreateInfo rsState = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rsState.depthClampEnable = VK_FALSE;
  rsState.rasterizerDiscardEnable = VK_FALSE;
  rsState.polygonMode = VK_POLYGON_MODE_FILL;
  rsState.cullMode =
      VK_CULL_MODE_BACK_BIT; // [Fix] Cull Back faces (Draw Front only)
  rsState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // [Fix] Disable Hardware Bias (Rely on Shader Manual Bias)
  rsState.depthBiasEnable = VK_FALSE;
  rsState.depthBiasConstantFactor = 0.0f;
  rsState.depthBiasSlopeFactor = 0.0f;
  rsState.lineWidth = 1.0f;

  // Manual Depth Compare: Disable Hardware Depth Test
  VkPipelineDepthStencilStateCreateInfo dsState = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  dsState.depthTestEnable = VK_FALSE;
  dsState.depthWriteEnable = VK_FALSE;
  dsState.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  dsState.stencilTestEnable = VK_FALSE;

  // MRT: 2 Color Attachments
  VkPipelineColorBlendAttachmentState blendAtt[2];
  for (int i = 0; i < 2; ++i) {
    blendAtt[i] = {};
    blendAtt[i].blendEnable = VK_FALSE;
    blendAtt[i].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT; // Only R channel needed
  }

  VkPipelineColorBlendStateCreateInfo cbInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbInfo.attachmentCount = 2;
  cbInfo.pAttachments = blendAtt;
  cbInfo.logicOpEnable = VK_FALSE;

  // Dynamic State
  std::vector<VkDynamicState> dynamicStates;
  dynamicStates.push_back(VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
  dynamicStates.push_back(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);

  VkPipelineDynamicStateCreateInfo dyInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyInfo.dynamicStateCount = uint32_t(dynamicStates.size());
  dyInfo.pDynamicStates = dynamicStates.data();

  // Shader Stages
  auto vkd = m_device->vkd();
  VkShaderModuleCreateInfo vsModuleInfo = {
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  vsModuleInfo.codeSize = sizeof(war3_shadow_caster_vert);
  vsModuleInfo.pCode = war3_shadow_caster_vert;
  VkShaderModule vsModule;
  if (vkd->vkCreateShaderModule(vkd->device(), &vsModuleInfo, nullptr,
                                &vsModule) != VK_SUCCESS)
    return {};

  VkShaderModuleCreateInfo fsModuleInfo = {
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  fsModuleInfo.codeSize = sizeof(war3_outline_mask);
  fsModuleInfo.pCode = war3_outline_mask;
  VkShaderModule fsModule;
  if (vkd->vkCreateShaderModule(vkd->device(), &fsModuleInfo, nullptr,
                                &fsModule) != VK_SUCCESS) {
    vkd->vkDestroyShaderModule(vkd->device(), vsModule, nullptr);
    return {};
  }

  VkPipelineShaderStageCreateInfo stages[2];
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vsModule;
  stages[0].pName = "main";
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fsModule;
  stages[1].pName = "main";

  // Graphics Pipeline
  VkGraphicsPipelineCreateInfo pipeInfo = {
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeInfo.stageCount = 2;
  pipeInfo.pStages = stages;
  pipeInfo.pVertexInputState = &viState;
  pipeInfo.pInputAssemblyState = &iaState;
  pipeInfo.pViewportState =
      nullptr; // Ignored if dynamic, but usually needs non-null ptr with
               // counts? Spec says pViewportState must be valid if
               // rasterization is not disabled. But if dynamic, the counts in
               // pViewportState matter.
  VkPipelineViewportStateCreateInfo vpInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vpInfo.viewportCount = 1;
  vpInfo.scissorCount = 1;
  pipeInfo.pViewportState = &vpInfo;

  pipeInfo.pRasterizationState = &rsState;
  pipeInfo.pMultisampleState = nullptr; // Must be valid!
  VkPipelineMultisampleStateCreateInfo msInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  msInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  msInfo.minSampleShading = 1.0f;
  pipeInfo.pMultisampleState = &msInfo;

  pipeInfo.pDepthStencilState = &dsState;
  pipeInfo.pColorBlendState = &cbInfo;
  pipeInfo.pDynamicState = &dyInfo;
  pipeInfo.layout = m_outlineMaskLayout->getPipelineLayout();
  pipeInfo.renderPass = VK_NULL_HANDLE;
  pipeInfo.subpass = 0;

  // Dynamic Rendering
  // If SDK is old, we might need extension struct.
  // Assuming user has VK_KHR_dynamic_rendering or 1.3
  // DXVK D3D9 uses it logic internally.
  // If we pass VK_NULL_HANDLE, we MUST chain VkPipelineRenderingCreateInfo.
  VkPipelineRenderingCreateInfo renderingInfo = {
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkFormat colorFormats[2] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM};
  renderingInfo.colorAttachmentCount = 2;
  renderingInfo.pColorAttachmentFormats = colorFormats;
  pipeInfo.pNext = &renderingInfo;

  VkPipeline pipeline;
  if (vkd->vkCreateGraphicsPipelines(vkd->device(), VK_NULL_HANDLE, 1,
                                     &pipeInfo, nullptr,
                                     &pipeline) != VK_SUCCESS) {
    vkd->vkDestroyShaderModule(vkd->device(), vsModule, nullptr);
    vkd->vkDestroyShaderModule(vkd->device(), fsModule, nullptr);
    return {};
  }

  vkd->vkDestroyShaderModule(vkd->device(), vsModule, nullptr);
  vkd->vkDestroyShaderModule(vkd->device(), fsModule, nullptr);

  ShadowCasterPipeline p;
  p.layout = m_outlineMaskLayout;
  p.pipeline = pipeline;
  return p;
}

void War3ShadowReceiverPass::renderMotionVectors(
    const Rc<DxvkCommandList> &ctx, const War3PipelineInput &input) {
  if (!m_motionVectorView || !m_depthCopyView || !m_shadowUniformBuffer)
    return;

  if (m_motionVectorPipeline == VK_NULL_HANDLE) {
    util::DxvkBuiltInGraphicsState state = {};
    state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
    state.fs = util::DxvkBuiltInShaderStage(war3_motion_vector, nullptr);
    state.colorFormat = VK_FORMAT_R16G16_SFLOAT;
    state.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    m_motionVectorPipeline =
        m_device->createBuiltInGraphicsPipeline(m_layout, state);
  }

  if (m_motionVectorPipeline == VK_NULL_HANDLE)
    return;

  VkExtent3D extent = m_motionVectorView->mipLevelExtent(0u);

  VkClearValue clear = {};
  clear.color.float32[0] = 0.0f;
  clear.color.float32[1] = 0.0f;
  clear.color.float32[2] = 0.0f;
  clear.color.float32[3] = 0.0f;

  VkRenderingAttachmentInfo attachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = m_motionVectorView->handle();
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.clearValue = clear;

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea.offset = {0u, 0u};
  renderInfo.renderArea.extent = {extent.width, extent.height};
  renderInfo.layerCount = 1u;
  renderInfo.colorAttachmentCount = 1u;
  renderInfo.pColorAttachments = &attachment;

  // Transition to COLOR_ATTACHMENT_OPTIMAL for rendering
  {
    const auto transition = m_motionVectorLayout.plan(
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    const auto subresources = m_motionVectorView->imageSubresources();
    VkImageMemoryBarrier2 barrier =
        war3::render::MakeWar3OwnedImageBarrier(
            transition, m_motionVectorImage->handle(), subresources);

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
    war3::render::CommitWar3OwnedImageLayout(
        m_motionVectorLayout, transition, *m_motionVectorImage, subresources);
  }

  ctx->cmdBeginRendering(&renderInfo);

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = float(extent.width);
  viewport.height = float(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor = {};
  scissor.offset = {0, 0};
  scissor.extent = {extent.width, extent.height};

  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);

  std::array<DxvkDescriptorWrite, 14> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor =
      m_colorCopyView ? m_colorCopyView->getDescriptor() : nullptr;

  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = m_depthCopyView->getDescriptor();

  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[2].descriptor =
      m_shadowMapSampleView ? m_shadowMapSampleView->getDescriptor() : nullptr;

  descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[3].descriptor = nullptr;
  descriptors[3].buffer =
      m_shadowUniformBuffer->getSliceInfo(0, sizeof(ShadowReceiverUniform));

  descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[4].descriptor = nullptr;
  descriptors[4].buffer =
      m_lightBuffer ? m_lightBuffer->getSliceInfo(0, sizeof(LightUniform))
                    : DxvkResourceBufferInfo{};

  descriptors[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[5].descriptor = nullptr;

  descriptors[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[6].descriptor = nullptr;

  descriptors[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[7].descriptor = nullptr;

  descriptors[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[8].descriptor = nullptr;

  descriptors[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  const uint32_t readIndex = m_shadowHistoryIndex;
  if (m_shadowHistoryView[readIndex])
    descriptors[9].descriptor = m_shadowHistoryView[readIndex]->getDescriptor();

  descriptors[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptors[10].descriptor = nullptr;

  descriptors[11].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[11].descriptor =
      m_shadowCasterMaskSampleView
          ? m_shadowCasterMaskSampleView->getDescriptor()
          : m_shadowMapSampleView->getDescriptor();

  descriptors[12].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[12].descriptor =
      m_pointRayHiZVisibilityView
          ? m_pointRayHiZVisibilityView->getDescriptor()
          : m_depthCopyView->getDescriptor();
  descriptors[13].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[13].descriptor =
      m_pointRayHiZView
          ? m_pointRayHiZView->getDescriptor()
          : (m_depthCopyView2D ? m_depthCopyView2D->getDescriptor() : nullptr);

  ReceiverPushConstants pc = {};
  pc.colorSampler = m_samplerLinear->getDescriptor().samplerIndex;
  pc.rawShadowSampler = m_shadowSampler->getDescriptor().samplerIndex;
  pc.compareShadowSampler =
      m_shadowCompareSamplerActive->getDescriptor().samplerIndex;
  pc.shadowCompareMode = m_shadowCompareMode;

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                       VK_PIPELINE_BIND_POINT_GRAPHICS, m_motionVectorPipeline);
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_layout, descriptors.size(),
                     descriptors.data(), sizeof(pc), &pc);
  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  // Transition to read-only for sampling
  const auto transition = m_motionVectorLayout.plan(
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_SHADER_READ_BIT);
  const auto subresources = m_motionVectorView->imageSubresources();
  VkImageMemoryBarrier2 barrier =
      war3::render::MakeWar3OwnedImageBarrier(
          transition, m_motionVectorImage->handle(), subresources);

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &barrier;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  war3::render::CommitWar3OwnedImageLayout(
      m_motionVectorLayout, transition, *m_motionVectorImage, subresources);

  ctx->track(m_motionVectorView->image(), DxvkAccess::Write);
  ctx->track(m_depthCopyView->image(), DxvkAccess::Read);
  ctx->track(m_shadowUniformBuffer, DxvkAccess::Read);
  if (m_shadowHistory[readIndex])
  ctx->track(m_shadowHistory[readIndex], DxvkAccess::Read);
  ctx->track(m_samplerLinear);
  ctx->track(m_shadowSampler);
  ctx->track(m_shadowCompareSamplerActive);
  reconciliation.shadowMotionVectorExecutedThisFrame = 1u;
}

void War3ShadowReceiverPass::InvalidateMapEpoch(uint64_t mapEpoch,
                                                uint64_t deviceEpoch) {
  if (mapEpoch == 0u || deviceEpoch == 0u ||
      (mapEpoch == m_shadowMapEpoch && deviceEpoch == m_shadowDeviceEpoch))
    return;

  // The std::async path writes pass-owned plan state, so it must be drained
  // at the render-thread transition. The persistent worker owns a sealed
  // value mailbox; shutting it down cancels admission and joins any job before
  // the old renderer epoch can be observed again.
  if (m_pointShadowPrepareFuture.valid())
    ++m_pointShadowWorkerCancelCount;
  waitPointShadowCpuPrepare();
  if (m_pointShadowPersistentWorker) {
    if (m_pointShadowPersistentPending)
      ++m_pointShadowWorkerCancelCount;
    m_pointShadowPersistentWorker->shutdown();
    m_pointShadowPersistentWorker.reset();
  }
  m_pointShadowPersistentPending = false;
  m_pointShadowPersistentPendingGeneration = {};
  m_pointShadowPersistentPendingSeal = {};
  m_pointShadowPersistentExpectedSettings = {};
  m_pointShadowPersistentExpectedHistory = {};
  m_pointShadowPersistentExpectedLights = {};
  m_pointShadowPersistentExpectedLightCount = 0u;
  m_pointShadowPersistentExpectedDynamicPoseSignature = 0u;
  m_pointShadowPersistentExpectedDynamicPoseCount = 0u;
  m_pointShadowPersistentExpectedDynamicSkinnedOutputCount = 0u;
  m_pointShadowPersistentRendererEpoch =
      MintPointShadowPersistentRendererEpoch();
  resetPointShadowCpuPlanPreservingCapacity();
  invalidatePointShadowPublishedState();

  m_shadowMapEpoch = mapEpoch;
  m_shadowDeviceEpoch = deviceEpoch;
  m_shadowLifecycleTombstoneSerialSeen =
      war3::render::CurrentShadowCasterTombstoneSerial();
  m_shadowStagePolicyRevisionSeen =
      war3::render::CurrentShadowStagePolicyRevision();
  m_epochFirstCandidateFrameSerial = 0u;
  m_epochFirstCompleteLatencyFrames = 0u;
  g_shadowReplayDiagnostics.mapEpoch.store(mapEpoch,
                                           std::memory_order_release);
  g_shadowReplayDiagnostics.deviceEpoch.store(deviceEpoch,
                                               std::memory_order_release);
  g_shadowReplayDiagnostics.candidateFrameSerial.store(
      0u, std::memory_order_release);
  g_shadowReplayDiagnostics.firstCompleteLatencyFrames.store(
      0u, std::memory_order_release);
  g_shadowReplayDiagnostics.plannedCasterCount.store(
      0u, std::memory_order_release);
  g_shadowReplayDiagnostics.replayCasterCount.store(
      0u, std::memory_order_release);
  g_shadowReplayDiagnostics.validatedCasterCount.store(
      0u, std::memory_order_release);
  g_shadowReplayDiagnostics.drawnCasterCount.store(
      0u, std::memory_order_release);
  g_shadowReplayDiagnostics.pointWorkerCancelCount.store(
      m_pointShadowWorkerCancelCount, std::memory_order_release);
  g_shadowReplayDiagnostics.pointLateResultRejectCount.store(
      m_pointShadowLateResultRejectCount, std::memory_order_release);
  m_hasCompleteShadowMap = false;
  m_replayValidationFailedThisFrame = false;
  m_replayValidationHoldFramesRemaining = 0u;
  m_shadowPublicationSettledFrameSerial = 0u;
  m_shadowHistoryValid = false;
  m_shadowTaaWasActiveLastFrame = false;
  m_shadowTaaHistoryContractValid = false;
  m_shadowTaaHistoryLifecycleSerial = 0u;
  m_shadowTaaHistoryStagePolicyRevision = 0u;
  m_shadowTaaHistoryMapResourceGeneration = 0u;
  m_shadowTaaHistoryResourceGeneration = 0u;
  m_lastShadowMapCasterCount = 0u;
  m_lastDynamicPoseSignature = 0u;
  m_lastShadowMapReplayContentHash = 0u;
  m_lastShadowMapReplayBackingHash = 0u;
  m_lastShadowMapStagePolicyRevision = 0u;
  m_lastShadowMapCsmHash = 0u;
  m_lastShadowMapResourceGeneration = 0u;
  m_shadowAdaptiveFrameIndex = 0u;
  m_transientEmptyReplayHoldFramesRemaining = 0u;
  m_recentSemanticDynamicHoldFramesRemaining = 0u;
  m_semanticIdentityChurnHoldFramesRemaining = 0u;
  m_semanticCoverageDropHoldStreak = 0u;
  m_lastShadowMapSemanticIdentityHash = 0u;
  m_pendingShadowMapSemanticIdentityHash = 0u;
  m_pendingShadowMapSemanticIdentityStableFrames = 0u;
  m_hasLastShadowMapLighting = false;
  m_hasLastGoodReceiverCamera = false;

  m_pointRayHiZVisibilityView = nullptr;
  m_pointRayHiZView = nullptr;
  m_pointRayHiZLightCount = 0u;
  m_pointRayHiZFrameSerial = 0u;
  m_pointRayHiZResourceGeneration = 0u;
  m_pointRayHiZLightGeneration = 0u;
  invalidateVolumeSunShadowPublication();
}

void War3ShadowReceiverPass::renderShadowVisibility(
    const Rc<DxvkCommandList> &ctx, const War3PipelineInput &input) {
  if (!m_shadowCurrentView || !m_depthCopyView || !m_shadowMapSampleView ||
      !m_shadowUniformBuffer)
    return;

  if (m_shadowVisibilityPipeline == VK_NULL_HANDLE) {
    util::DxvkBuiltInGraphicsState state = {};
    state.vs = util::DxvkBuiltInShaderStage(war3_fullscreen_vert, nullptr);
    state.fs = util::DxvkBuiltInShaderStage(war3_shadow_visibility, nullptr);
    state.colorFormat = VK_FORMAT_R8_UNORM;
    state.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    m_shadowVisibilityPipeline =
        m_device->createBuiltInGraphicsPipeline(m_layout, state);
  }

  if (m_shadowVisibilityPipeline == VK_NULL_HANDLE)
    return;

  VkExtent3D extent = m_shadowCurrentView->mipLevelExtent(0u);

  VkClearValue clear = {};
  clear.color.float32[0] = 1.0f; // 默认无阴影
  clear.color.float32[1] = 1.0f;
  clear.color.float32[2] = 1.0f;
  clear.color.float32[3] = 1.0f;

  VkRenderingAttachmentInfo attachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = m_shadowCurrentView->handle();
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.clearValue = clear;

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea.offset = {0u, 0u};
  renderInfo.renderArea.extent = {extent.width, extent.height};
  renderInfo.layerCount = 1u;
  renderInfo.colorAttachmentCount = 1u;
  renderInfo.pColorAttachments = &attachment;

  // Transition to COLOR_ATTACHMENT_OPTIMAL for rendering
  {
    const auto transition = m_shadowCurrentLayout.plan(
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    const auto subresources = m_shadowCurrentView->imageSubresources();
    VkImageMemoryBarrier2 barrier =
        war3::render::MakeWar3OwnedImageBarrier(
            transition, m_shadowCurrent->handle(), subresources);

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
    war3::render::CommitWar3OwnedImageLayout(
        m_shadowCurrentLayout, transition, *m_shadowCurrent, subresources);
  }

  ctx->cmdBeginRendering(&renderInfo);

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = float(extent.width);
  viewport.height = float(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor = {};
  scissor.offset = {0, 0};
  scissor.extent = {extent.width, extent.height};

  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);

  std::array<DxvkDescriptorWrite, 14> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor =
      m_colorCopyView ? m_colorCopyView->getDescriptor() : nullptr;

  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = m_depthCopyView->getDescriptor();

  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[2].descriptor = m_shadowMapSampleView->getDescriptor();

  descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[3].descriptor = nullptr;
  descriptors[3].buffer =
      m_shadowUniformBuffer->getSliceInfo(0, sizeof(ShadowReceiverUniform));

  descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[4].descriptor = nullptr;
  descriptors[4].buffer =
      m_lightBuffer ? m_lightBuffer->getSliceInfo(0, sizeof(LightUniform))
                    : DxvkResourceBufferInfo{};

  descriptors[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[5].descriptor = nullptr;
  descriptors[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[6].descriptor = nullptr;
  descriptors[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[7].descriptor = nullptr;
  descriptors[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[8].descriptor = nullptr;
  descriptors[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[9].descriptor = nullptr;
  descriptors[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptors[10].descriptor = nullptr;

  descriptors[11].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[11].descriptor =
      m_shadowCasterMaskSampleView
          ? m_shadowCasterMaskSampleView->getDescriptor()
          : m_shadowMapSampleView->getDescriptor();

  descriptors[12].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[12].descriptor =
      m_pointRayHiZVisibilityView
          ? m_pointRayHiZVisibilityView->getDescriptor()
          : m_depthCopyView->getDescriptor();
  descriptors[13].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[13].descriptor =
      m_pointRayHiZView
          ? m_pointRayHiZView->getDescriptor()
          : (m_depthCopyView2D ? m_depthCopyView2D->getDescriptor() : nullptr);

  ReceiverPushConstants pc = {};
  pc.colorSampler = m_samplerLinear->getDescriptor().samplerIndex;
  pc.rawShadowSampler = m_shadowSampler->getDescriptor().samplerIndex;
  pc.compareShadowSampler =
      m_shadowCompareSamplerActive->getDescriptor().samplerIndex;
  pc.shadowCompareMode = m_shadowCompareMode;

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                       VK_PIPELINE_BIND_POINT_GRAPHICS,
                       m_shadowVisibilityPipeline);
  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, m_layout, descriptors.size(),
                     descriptors.data(), sizeof(pc), &pc);
  ctx->cmdDraw(3, 1, 0, 0);
  ctx->cmdEndRendering();

  // Transition to read-only for sampling
  const auto transition = m_shadowCurrentLayout.plan(
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_SHADER_READ_BIT);
  const auto subresources = m_shadowCurrentView->imageSubresources();
  VkImageMemoryBarrier2 barrier =
      war3::render::MakeWar3OwnedImageBarrier(
          transition, m_shadowCurrent->handle(), subresources);

  VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &barrier;
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  war3::render::CommitWar3OwnedImageLayout(
      m_shadowCurrentLayout, transition, *m_shadowCurrent, subresources);

  ctx->track(m_shadowCurrentView->image(), DxvkAccess::Write);
  ctx->track(m_depthCopyView->image(), DxvkAccess::Read);
  ctx->track(m_shadowMapSampleView->image(), DxvkAccess::Read);
  if (m_shadowCasterMask)
    ctx->track(m_shadowCasterMask, DxvkAccess::Read);
  ctx->track(m_shadowUniformBuffer, DxvkAccess::Read);
  ctx->track(m_samplerLinear);
  ctx->track(m_shadowSampler);
  ctx->track(m_shadowCompareSamplerActive);
  reconciliation.shadowVisibilityExecutedThisFrame = 1u;
}

bool War3ShadowReceiverPass::renderVolumeSunShadow(
    const Rc<DxvkCommandList>& ctx, const War3PipelineInput& input,
    const std::vector<const War3ShadowCasterDraw*>* replayDraws) {
  if (!ctx || !input.settings || !replayDraws || replayDraws->empty())
    return false;
  if (!input.scene.worldCamera.valid || m_csmData.cascadeCount == 0u)
    return false;

  const auto& vol = input.settings->postFx.volumetricLight;
  if (!vol.enabled || !vol.volumeSunShadowEnabled)
    return false;

  const uint32_t resolution = std::max<uint32_t>(
      std::min<uint32_t>(vol.volumeSunResolution, 2048u), 256u);
  ensureVolumeSunShadowResources(resolution);
  if (!m_volumeSunShadowMap || !m_volumeSunShadowSampleView ||
      !m_volumeSunShadowLayerViews[0])
    return false;

  const Matrix4 invView = inverse(input.scene.worldCamera.view);
  const Vector4 cameraPos(invView[3].x, invView[3].y, invView[3].z, 1.0f);
  if (!std::isfinite(cameraPos.x) || !std::isfinite(cameraPos.y) ||
      !std::isfinite(cameraPos.z))
    return false;

  const float radiusFar = std::clamp(
      std::isfinite(vol.volumeSunOrthoRadius) ? vol.volumeSunOrthoRadius
                                              : 3400.0f,
      256.0f, 8000.0f);
  const float nearScale = std::clamp(
      std::isfinite(vol.volumeSunNearRadiusScale) ? vol.volumeSunNearRadiusScale
                                                  : 0.42f,
      0.20f, 0.85f);
  const float radiusNear = std::max(radiusFar * nearScale, 128.0f);
  const float depthExt = std::clamp(
      std::isfinite(vol.volumeSunDepthExtension) ? vol.volumeSunDepthExtension
                                                 : 640.0f,
      0.0f, 2000.0f);
  const float depthMargin = std::clamp(
      std::isfinite(vol.volumeSunDepthMargin) ? vol.volumeSunDepthMargin
                                              : 96.0f,
      0.0f, 500.0f);
  const bool dual = vol.volumeSunDualCascade && m_volumeSunShadowLayerViews[1];

  War3VolumeSunOrtho orthoFar = ComputeVolumeSunOrtho(
      m_csmData.lightDirection, m_csmData.worldUp, cameraPos, radiusFar,
      depthMargin, depthExt, resolution, true);
  if (!orthoFar.valid)
    return false;

  War3VolumeSunOrtho orthoNear = orthoFar;
  if (dual) {
    orthoNear = ComputeVolumeSunOrtho(
        m_csmData.lightDirection, m_csmData.worldUp, cameraPos, radiusNear,
        depthMargin, depthExt, resolution, true);
    if (!orthoNear.valid)
      orthoNear = orthoFar;
  }

  m_volumeSunSoftRadius =
      std::clamp(std::isfinite(vol.volumeSunSoftRadius) ? vol.volumeSunSoftRadius
                                                        : 1.85f,
                 0.5f, 4.0f);
  m_volumeSunReceiverBias =
      std::clamp(std::isfinite(vol.volumeSunReceiverBias)
                     ? vol.volumeSunReceiverBias
                     : 0.0075f,
                 0.0001f, 0.05f);

  // 临时把 renderShadowMap 目标换成体积太阳 1/2 层 ortho。
  // 表面 CSM 成员在函数返回前必须完整恢复。
  const Rc<DxvkImage> savedMap = m_shadowMap;
  const Rc<DxvkImageView> savedSample = m_shadowMapSampleView;
  const auto savedLayers = m_shadowMapLayerViews;
  const uint32_t savedRes = m_shadowMapResolution;
  const uint32_t savedLayerCount = m_shadowMapLayers;
  const War3CsmData savedCsm = m_csmData;
  const bool savedComplete = m_hasCompleteShadowMap;
  // 2026-07-21 修复（诊断正确性）：renderShadowMap 入口会重置并全程写入
  // reconciliation 计数，且在结尾 ++m_shadowMapRenderSerial。体积太阳路径复用
  // 同一函数，若不隔离，对外发布的"每帧"主 CSM 统计（drawnCasters/级联
  // drawn/culled/prepared 等）会被体积 pass（1-2 层）的值覆盖，render serial
  // 也会每帧双增，依赖这些计数的 crash-gate/报表会读错。与上面的成员交换
  // 同模式保存/恢复：发布后看到的永远是主 CSM pass 的计数与连续 serial。
  const ShadowReconciliationCounters savedReconciliation = reconciliation;
  const uint64_t savedShadowMapRenderSerial = m_shadowMapRenderSerial;

  const uint32_t layerCount = dual && orthoNear.valid ? 2u : 1u;
  m_shadowMap = m_volumeSunShadowMap;
  m_shadowMapSampleView = m_volumeSunShadowSampleView;
  m_shadowMapLayerViews = {};
  m_shadowMapLayerViews[0] = m_volumeSunShadowLayerViews[0];
  if (layerCount > 1u)
    m_shadowMapLayerViews[1] = m_volumeSunShadowLayerViews[1];
  m_shadowMapResolution = resolution;
  m_shadowMapLayers = layerCount;
  m_csmData.cascadeCount = layerCount;
  // 层序：0=近（更锐），1=远（更广）。shader volume-sun 路径按层优先近级。
  if (layerCount > 1u) {
    m_csmData.cascades[0].lightViewProj = orthoNear.lightViewProj;
    m_csmData.cascades[0].splitNear = 0.0f;
    m_csmData.cascades[0].splitFar = 1.0e9f;
    m_csmData.cascades[1].lightViewProj = orthoFar.lightViewProj;
    m_csmData.cascades[1].splitNear = 0.0f;
    m_csmData.cascades[1].splitFar = 1.0e9f;
  } else {
    m_csmData.cascades[0].lightViewProj = orthoFar.lightViewProj;
    m_csmData.cascades[0].splitNear = 0.0f;
    m_csmData.cascades[0].splitFar = 1.0e9f;
  }
  m_csmData.lightDirection = orthoFar.lightDirection;
  m_csmData.worldUp = orthoFar.worldUp;
  m_volumeSunRenderPathActive = true;

  bool rendered = false;
  try {
    rendered = renderShadowMap(ctx, input, replayDraws);
  } catch (...) {
    m_volumeSunRenderPathActive = false;
    m_shadowMap = savedMap;
    m_shadowMapSampleView = savedSample;
    m_shadowMapLayerViews = savedLayers;
    m_shadowMapResolution = savedRes;
    m_shadowMapLayers = savedLayerCount;
    m_csmData = savedCsm;
    m_hasCompleteShadowMap = savedComplete;
    reconciliation = savedReconciliation;
    m_shadowMapRenderSerial = savedShadowMapRenderSerial;
    throw;
  }

  m_volumeSunRenderPathActive = false;
  m_shadowMap = savedMap;
  m_shadowMapSampleView = savedSample;
  m_shadowMapLayerViews = savedLayers;
  m_shadowMapResolution = savedRes;
  m_shadowMapLayers = savedLayerCount;
  m_csmData = savedCsm;
  m_hasCompleteShadowMap = savedComplete;
  reconciliation = savedReconciliation;
  m_shadowMapRenderSerial = savedShadowMapRenderSerial;

  if (!rendered)
    return false;

  if (layerCount > 1u) {
    m_volumeSunOrthoNear = orthoNear;
    m_volumeSunOrthoFar = orthoFar;
    m_volumeSunLightViewProj[0] = orthoNear.lightViewProj;
    m_volumeSunLightViewProj[1] = orthoFar.lightViewProj;
  } else {
    m_volumeSunOrthoNear = {};
    m_volumeSunOrthoFar = orthoFar;
    m_volumeSunLightViewProj[0] = orthoFar.lightViewProj;
    m_volumeSunLightViewProj[1] = orthoFar.lightViewProj;
  }
  m_volumeSunPublishedFrameSerial = input.frameSerial;
  m_volumeSunShadowReady = true;
  return true;
}

bool War3ShadowReceiverPass::validateShadowReplayDraws(
    const War3PipelineInput& input,
    const std::vector<const War3ShadowCasterDraw*>& replayDraws,
    const char* consumer) {
  // Validate the complete immutable batch before any consumer begins Vulkan
  // rendering. A valid prefix is never permission to submit partial output.
  thread_local std::vector<
      war3::render::War3ShadowReplayValidationInput> validationInputs;
  validationInputs.clear();
  validationInputs.reserve(replayDraws.size());
  for (const War3ShadowCasterDraw* draw : replayDraws) {
    if (draw != nullptr) {
      validationInputs.push_back(
          MakeWar3ShadowReplayValidationInput(*draw, input));
      continue;
    }

    // Preserve the historical null-draw diagnostic while still routing the
    // entire batch through the same pure validator.
    war3::render::War3ShadowReplayValidationInput missing = {};
    missing.expectedMapEpoch = input.mapEpoch;
    missing.expectedDeviceEpoch = input.deviceEpoch;
    missing.drawMapEpoch = input.mapEpoch;
    missing.drawDeviceEpoch = input.deviceEpoch;
    missing.worldMatrixFinite = true;
    missing.indexed = false;
    missing.vertexCount = 1u;
    validationInputs.push_back(missing);
  }

  const auto batch = war3::render::ValidateWar3ShadowReplayBatch(
      validationInputs.data(), validationInputs.size());
  if (!batch) {
    const War3ShadowCasterDraw* draw =
        batch.failureIndex < replayDraws.size()
            ? replayDraws[batch.failureIndex]
            : nullptr;
    const war3::render::War3ShadowReplayValidationResult result =
        batch.failure;

    ++reconciliation.replayValidationRejectedCount;
    ++reconciliation.replayPartialPreventedCount;
    reconciliation.replayValidationLastReason =
        static_cast<uint32_t>(result.reason);
    reconciliation.replayValidationLastDrawMapEpoch =
        draw != nullptr ? draw->mapEpoch : 0u;
    reconciliation.replayValidationLastExpectedMapEpoch = input.mapEpoch;
    reconciliation.replayValidationLastRequiredEnd = result.requiredEnd;
    reconciliation.replayValidationLastAvailableSize = result.availableSize;
    g_shadowReplayDiagnostics.validationRejectCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_shadowReplayDiagnostics.partialPreventedCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_shadowReplayDiagnostics.lastRejectReason.store(
        static_cast<uint32_t>(result.reason), std::memory_order_release);
    g_shadowReplayDiagnostics.lastOffenderMapEpoch.store(
        draw != nullptr ? draw->mapEpoch : 0u, std::memory_order_release);
    g_shadowReplayDiagnostics.lastRequiredEnd.store(
        result.requiredEnd, std::memory_order_release);
    g_shadowReplayDiagnostics.lastAvailableSize.store(
        result.availableSize, std::memory_order_release);
    g_shadowReplayDiagnostics.lastMinimumVertex.store(
        result.minimumVertex, std::memory_order_release);
    g_shadowReplayDiagnostics.lastMaximumVertex.store(
        result.maximumVertex, std::memory_order_release);
    g_shadowReplayDiagnostics.lastVertexOffset.store(
        draw != nullptr ? draw->vertexOffset : 0, std::memory_order_release);
    g_shadowReplayDiagnostics.lastStage.store(
        draw != nullptr ? static_cast<int32_t>(draw->stage) : -1,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastCategory.store(
        draw != nullptr ? static_cast<uint32_t>(draw->category) : 0u,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastBatchTag.store(
        draw != nullptr ? static_cast<uint32_t>(draw->batchTag) : 0u,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastObjectKind.store(
        draw != nullptr ? static_cast<uint32_t>(draw->objectKind) : 0u,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastRawcode.store(
        draw != nullptr ? draw->rawcode : 0u, std::memory_order_release);
    g_shadowReplayDiagnostics.lastJHandle.store(
        draw != nullptr ? draw->jHandle : 0u, std::memory_order_release);
    g_shadowReplayDiagnostics.lastIndexCount.store(
        draw != nullptr ? draw->indexCount : 0u, std::memory_order_release);
    g_shadowReplayDiagnostics.lastFirstIndex.store(
        draw != nullptr ? draw->firstIndex : 0u, std::memory_order_release);
    g_shadowReplayDiagnostics.lastMinVertexIndex.store(
        draw != nullptr ? draw->minVertexIndex : 0u,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastNumVertices.store(
        draw != nullptr ? draw->numVertices : 0u,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastActualIndexMin.store(
        draw != nullptr ? draw->shadowActualIndexMin : 0u,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastActualIndexMax.store(
        draw != nullptr ? draw->shadowActualIndexMax : 0u,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastActualIndexDomainKnown.store(
        draw != nullptr && draw->shadowActualIndexDomainKnown ? 1u : 0u,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastFullVertexDomainFallback.store(
        draw != nullptr && draw->shadowFullVertexDomainFallback ? 1u : 0u,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastPositionSize.store(
        draw != nullptr ? draw->positionInfo.size : 0u,
        std::memory_order_release);
    m_replayValidationFailedThisFrame = true;

    static uint32_t s_replayRejectLogs = 0u;
    if (s_replayRejectLogs++ < 32u ||
        (s_replayRejectLogs % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK War3ShadowReplay: reject consumer=%s reason=%s "
          "drawEpoch=%llu expectedEpoch=%llu required=%llu available=%llu "
          "vertexDomain=[%lld,%lld] vertexOffset=%d stage=%d category=%u "
          "batchTag=%u objectKind=%u rawcode=%08x jHandle=%08x "
          "indices=%u firstIndex=%u minVertex=%u numVertices=%u "
          "actualDomain=%u[%u,%u] fullFallback=%u positionSize=%llu\n",
          consumer != nullptr ? consumer : "unknown",
          war3::render::War3ShadowReplayRejectReasonName(result.reason),
          static_cast<unsigned long long>(
              draw != nullptr ? draw->mapEpoch : 0u),
          static_cast<unsigned long long>(input.mapEpoch),
          static_cast<unsigned long long>(result.requiredEnd),
          static_cast<unsigned long long>(result.availableSize),
          static_cast<long long>(result.minimumVertex),
          static_cast<long long>(result.maximumVertex),
          draw != nullptr ? draw->vertexOffset : 0,
          draw != nullptr ? static_cast<int>(draw->stage) : -1,
          draw != nullptr ? static_cast<unsigned>(draw->category) : 0u,
          draw != nullptr ? static_cast<unsigned>(draw->batchTag) : 0u,
          draw != nullptr ? static_cast<unsigned>(draw->objectKind) : 0u,
          draw != nullptr ? draw->rawcode : 0u,
          draw != nullptr ? draw->jHandle : 0u,
          draw != nullptr ? draw->indexCount : 0u,
          draw != nullptr ? draw->firstIndex : 0u,
          draw != nullptr ? draw->minVertexIndex : 0u,
          draw != nullptr ? draw->numVertices : 0u,
          draw != nullptr && draw->shadowActualIndexDomainKnown ? 1u : 0u,
          draw != nullptr ? draw->shadowActualIndexMin : 0u,
          draw != nullptr ? draw->shadowActualIndexMax : 0u,
          draw != nullptr && draw->shadowFullVertexDomainFallback ? 1u : 0u,
          static_cast<unsigned long long>(
              draw != nullptr ? draw->positionInfo.size : 0u));
    }
    return false;
  }
  return true;
}

bool War3ShadowReceiverPass::renderShadowMap(const Rc<DxvkCommandList> &ctx,
                                             const War3PipelineInput &input,
                                             const std::vector<
                                                 const War3ShadowCasterDraw*>*
                                                 replayDrawOverride) {
  std::vector<const war3::render::War3TrackedVkPipeline*>
      trackedCasterPipelines;
  const auto trackCasterPipeline = [&] (const ShadowCasterPipeline& pipeline) {
    if (!pipeline.lifetime)
      return;
    const auto* owner = pipeline.lifetime.ptr();
    if (std::find(trackedCasterPipelines.begin(),
                  trackedCasterPipelines.end(), owner) !=
        trackedCasterPipelines.end())
      return;
    // DxvkCommandList releases tracked objects only after GPU completion.
    ctx->track(pipeline.lifetime);
    trackedCasterPipelines.push_back(owner);
  };

  war3::tools::SetGpuFlightBreadcrumb(
      war3::tools::GpuFlightBreadcrumb::CsmPreflight);
  if (!m_volumeSunRenderPathActive)
    war3::tools::ResetGpuFlightCsmWork();
  const uint32_t shadowMapPhaseSampleWeight =
      !m_volumeSunRenderPathActive
          ? War3ShadowPhaseSampleWeight(
                input.frameSerial, 0xd1ec7105f46a4b2bull)
          : 0u;
  War3SampledPhaseRawTiming<kWar3DirectionalShadowMapRawPhaseCount>
      shadowMapPhaseTiming(
          shadowMapPhaseSampleWeight, "DirectionalShadowMapPhaseSample",
          "FramePipeline/DirectionalShadowMapPhaseSample",
          kWar3DirectionalShadowMapRawPhaseNames);
  shadowMapPhaseTiming.enter(static_cast<size_t>(
      War3DirectionalShadowMapRawPhase::EntryAndReplay));

  if (!m_shadowMap || !m_shadowMapSampleView)
  {
    reconciliation.shadowMapRenderSkippedNoResourcesCount = 1u;
    return false;
  }
  // 体积太阳 ortho 路径只画单层 depth，不消费/不写 terrain mask。
  const bool terrainCasterMaskEnabled =
      !m_volumeSunRenderPathActive &&
      ShadowS1TerrainCasterMaskRuntimeEnabled() && m_shadowCasterMask &&
      m_shadowCasterMaskSampleView;
  auto& shadowMapLayout = m_volumeSunRenderPathActive
      ? m_volumeSunShadowLayout
      : m_shadowMapLayout;

  reconciliation.shadowMapDrawnCasters = 0u;
  reconciliation.cascadeCulledCount = 0u;
  reconciliation.shadowMapPreparedDrawCount = 0u;
  reconciliation.shadowMapAlphaTestPreparedCount = 0u;
  reconciliation.shadowMapAlphaPromotedPreparedCount = 0u;
  reconciliation.shadowMapDynamicPreparedCount = 0u;
  reconciliation.shadowMapStaticPreparedCount = 0u;
  reconciliation.shadowMapOtherPreparedCount = 0u;
  reconciliation.shadowMapTerrainDoodadPreparedCount = 0u;
  reconciliation.shadowMapTerrainS1PreparedCount = 0u;
  reconciliation.shadowMapCascade0DrawnCount = 0u;
  reconciliation.shadowMapCascade1DrawnCount = 0u;
  reconciliation.shadowMapCascade2DrawnCount = 0u;
  reconciliation.shadowMapCascade3DrawnCount = 0u;
  reconciliation.shadowMapCascade0CulledCount = 0u;
  reconciliation.shadowMapCascade1CulledCount = 0u;
  reconciliation.shadowMapCascade2CulledCount = 0u;
  reconciliation.shadowMapCascade3CulledCount = 0u;
  reconciliation.terrainBoundsCullMode = 0u;
  reconciliation.terrainBoundsCandidateCount = 0u;
  reconciliation.terrainBoundsProofAcceptedCount = 0u;
  reconciliation.terrainBoundsFailVisibleCount = 0u;
  reconciliation.terrainBoundsWouldCullCount = 0u;
  reconciliation.terrainBoundsAppliedCullCount = 0u;
  reconciliation.terrainBoundsC0WouldCullCount = 0u;
  reconciliation.terrainBoundsC1WouldCullCount = 0u;
  reconciliation.terrainBoundsC2WouldCullCount = 0u;
  reconciliation.terrainBoundsC3WouldCullCount = 0u;
  reconciliation.objectBoundsCandidateCount = 0u;
  reconciliation.objectBoundsProofAcceptedCount = 0u;
  reconciliation.objectBoundsFailVisibleCount = 0u;
  reconciliation.objectBoundsWouldCullCount = 0u;
  reconciliation.objectBoundsAppliedCullCount = 0u;
  reconciliation.unionCullMode = 0u;
  reconciliation.unionCullObserveFrameCount = 0u;
  reconciliation.unionCullCandidateCount = 0u;
  reconciliation.unionCullProofAcceptedCount = 0u;
  reconciliation.unionCullFailVisibleCount = 0u;
  reconciliation.unionCullDynamicConservativeCount = 0u;
  reconciliation.unionCullUnknownOrStaleCount = 0u;
  reconciliation.unionCullC2WouldCullCount = 0u;
  reconciliation.unionCullC3WouldCullCount = 0u;
  reconciliation.unionCullBothFarWouldCullCount = 0u;
  reconciliation.unionCullFalseNegativeCount = 0u;
  reconciliation.unionCullFalsePositiveCount = 0u;
  reconciliation.shadowMapTerrainDoodadCascade0DrawnCount = 0u;
  reconciliation.shadowMapTerrainDoodadCascade1DrawnCount = 0u;
  reconciliation.shadowMapTerrainDoodadCascade2DrawnCount = 0u;
  reconciliation.shadowMapTerrainDoodadCascade3DrawnCount = 0u;
  reconciliation.shadowMapTerrainS1Cascade0DrawnCount = 0u;
  reconciliation.shadowMapTerrainS1Cascade1DrawnCount = 0u;
  reconciliation.shadowMapTerrainS1Cascade2DrawnCount = 0u;
  reconciliation.shadowMapTerrainS1Cascade3DrawnCount = 0u;
  reconciliation.skinnedCasterCount = 0u;
  reconciliation.skinnedPreparedCount = 0u;
  reconciliation.skinnedDrawnCount = 0u;

  const std::vector<const War3ShadowCasterDraw*>* replayDrawsPtr =
      replayDrawOverride;
  if (replayDrawsPtr == nullptr) {
    replayDrawsPtr = &BuildShadowReplayDraws(input.scene, input.frameSerial);
  }
  const auto& replayDraws = *replayDrawsPtr;
  const uint32_t replayCasterCount = static_cast<uint32_t>(
      std::min<size_t>(replayDraws.size(),
                       std::numeric_limits<uint32_t>::max()));
  if (!m_volumeSunRenderPathActive) {
    g_shadowReplayDiagnostics.mapEpoch.store(input.mapEpoch,
                                             std::memory_order_release);
    g_shadowReplayDiagnostics.deviceEpoch.store(input.deviceEpoch,
                                                 std::memory_order_release);
    g_shadowReplayDiagnostics.candidateFrameSerial.store(
        input.frameSerial, std::memory_order_release);
    g_shadowReplayDiagnostics.plannedCasterCount.store(
        replayCasterCount, std::memory_order_release);
    g_shadowReplayDiagnostics.replayCasterCount.store(
        replayCasterCount, std::memory_order_release);
    g_shadowReplayDiagnostics.validatedCasterCount.store(
        0u, std::memory_order_release);
    g_shadowReplayDiagnostics.drawnCasterCount.store(
        0u, std::memory_order_release);
  }
  m_replayValidationFailedThisFrame = false;
  if (!validateShadowReplayDraws(input, replayDraws,
                                 m_volumeSunRenderPathActive
                                     ? "volume-sun"
                                     : "csm-terrain-mask")) {
    return false;
  }
  if (!m_volumeSunRenderPathActive) {
    g_shadowReplayDiagnostics.validatedCasterCount.store(
        replayCasterCount, std::memory_order_release);
  }

  War3RenderSettings defaultSettings = {};
  const War3RenderSettings *settings =
      input.settings ? input.settings.get() : &defaultSettings;
  const bool alphaShadowHashed = settings->shadows.alphaShadowHashed;
  const float alphaShadowFarAlphaRefBias =
      war3::render::SanitizeShadowAlphaFarRefBias(
          settings->shadows.alphaShadowFarAlphaRefBias);

  const uint32_t cascadeCount =
      std::min<uint32_t>(std::max<uint32_t>(m_csmData.cascadeCount, 1u), 4u);
  if (cascadeCount == 0)
    return false;
  for (uint32_t cascade = 0u; cascade < cascadeCount; ++cascade) {
    if (!m_shadowMapLayerViews[cascade] ||
        (terrainCasterMaskEnabled &&
         !m_shadowCasterMaskLayerViews[cascade])) {
      if (!m_volumeSunRenderPathActive) {
        m_replayValidationFailedThisFrame = true;
        reconciliation.replayValidationLastReason = static_cast<uint32_t>(
            war3::render::War3ShadowReplayRejectReason::IncompleteReplayPlan);
        g_shadowReplayDiagnostics.partialPreventedCount.fetch_add(
            1u, std::memory_order_relaxed);
        g_shadowReplayDiagnostics.lastRejectReason.store(
            reconciliation.replayValidationLastReason,
            std::memory_order_release);
      }
      ++reconciliation.replayPartialPreventedCount;
      return false;
    }
  }

  // Admission is provisional until the first shadow rendering scope begins.
  // Matrix allocation, pipeline preflight or cohort validation may still
  // reject the candidate without submitting the expensive replay workload.
  ScopedWar3GpuWorkloadReservation volumeWorkloadReservation(
      m_gpuWorkloadGovernor);
  ScopedWar3GpuWorkloadReservation directionalWorkloadReservation(
      m_gpuWorkloadGovernor);

  // Volume-sun is optional and has no canonical per-cascade culling. Reserve
  // its complete single-layer replay before matrix upload or any command. The
  // main CSM waits for its canonical visibility mask below so normal scenes are
  // not rejected by a replayDraws*4 upper bound.
  if (m_volumeSunRenderPathActive) {
    constexpr auto workloadConsumer =
        war3::render::War3GpuWorkloadConsumer::VolumeSun;
    war3::render::War3GpuWorkloadCost workloadCost = {};
    for (const War3ShadowCasterDraw* draw : replayDraws) {
      if (draw == nullptr ||
          !AddWar3ShadowWorkloadDraw(workloadCost, *draw, cascadeCount)) {
        workloadCost.valid = false;
        break;
      }
    }
    if (!volumeWorkloadReservation.reserve(workloadConsumer, cascadeCount,
                                           workloadCost)) {
      const auto& governor = m_gpuWorkloadGovernor.diagnostics();
      static uint32_t s_workloadRejectLogs = 0u;
      const uint32_t logIndex = s_workloadRejectLogs++;
      if (logIndex < 16u || (logIndex % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3GpuWorkload: reject %s frame=%llu "
            "request(draw=%llu vertex=%llu index=%llu items=%u) "
            "used(draw=%llu vertex=%llu index=%llu) reason=%u\n",
            War3GpuWorkloadConsumerName(workloadConsumer),
            static_cast<unsigned long long>(input.frameSerial),
            static_cast<unsigned long long>(workloadCost.draws),
            static_cast<unsigned long long>(workloadCost.vertices),
            static_cast<unsigned long long>(workloadCost.indices),
            cascadeCount,
            static_cast<unsigned long long>(governor.used.draws),
            static_cast<unsigned long long>(governor.used.vertices),
            static_cast<unsigned long long>(governor.used.indices),
            governor.lastRejectReason);
      }
      return false;
    }
  }

  // 首帧诊断：默认关闭（env DXVK_WAR3_SHADOWMAP_REPLAY_LOG=1 才输出），
  // 避免每次进程启动前 5 帧写日志污染热路径。
  {
    static const bool s_diagLogEnabled = []() {
      const char* env = std::getenv("DXVK_WAR3_SHADOWMAP_REPLAY_LOG");
      return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();
    if (s_diagLogEnabled) {
      static uint32_t s_diagLogCounter = 0;
      if (s_diagLogCounter++ < 5u) {
        WAR3_RENDER_LOG("DXVK ShadowMap[%u]: replayDraws=%u\n",
                        s_diagLogCounter - 1u,
                        static_cast<unsigned>(replayDraws.size()));
      }
    }
  }

  // 1) 上传矩阵 SSBO：骨骼调色板 + 每个 draw 的 worldMatrix（用于静态物体 GPU
  // 端 MVP 计算）
  shadowMapPhaseTiming.enter(static_cast<size_t>(
      War3DirectionalShadowMapRawPhase::MatrixUpload));
  DxvkDescriptorWrite paletteDesc = {};
  paletteDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paletteDesc.buffer = ensureShadowMatrixBuffer(ctx, input, &replayDraws);
  if (paletteDesc.buffer.buffer == VK_NULL_HANDLE) {
    reconciliation.shadowMapRenderSkippedNoMatrixBufferCount = 1u;
    return false;
  }
  reconciliation.shadowMatrixSceneKey = m_shadowMatrixSceneKey;
  reconciliation.shadowMatrixUploadSerial = m_shadowMatrixUploadSerial;
  reconciliation.shadowMatrixBufferObjectPtr =
      War3RcObjectId(m_vertexBlendPaletteBuffer);
  reconciliation.shadowMatrixBufferOffset =
      uint64_t(paletteDesc.buffer.offset);
  reconciliation.shadowMatrixBufferSize = uint64_t(paletteDesc.buffer.size);
  reconciliation.shadowMatrixBufferGpuAddress =
      uint64_t(paletteDesc.buffer.gpuAddress);

  const uint32_t objectBase = m_shadowMatrixObjectBase;

  const auto restoreShadowTargetsToRead = [&]() {
    const VkImageSubresourceRange depthSubresources = {
        VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, cascadeCount};
    const auto depthTransition = shadowMapLayout.plan(
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT);
    VkImageMemoryBarrier2 toRead =
        war3::render::MakeWar3OwnedImageBarrier(
            depthTransition, m_shadowMap->handle(), depthSubresources);
    VkDependencyInfo depthDep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depthDep.imageMemoryBarrierCount = 1u;
    depthDep.pImageMemoryBarriers = &toRead;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depthDep);
    war3::render::CommitWar3OwnedImageLayout(
        shadowMapLayout, depthTransition, *m_shadowMap, depthSubresources);
    if (terrainCasterMaskEnabled) {
      const VkImageSubresourceRange maskSubresources = {
          VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, cascadeCount};
      const auto maskTransition = m_shadowCasterMaskLayout.plan(
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT);
      VkImageMemoryBarrier2 maskToRead =
          war3::render::MakeWar3OwnedImageBarrier(
              maskTransition, m_shadowCasterMask->handle(), maskSubresources);
      VkDependencyInfo maskDep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      maskDep.imageMemoryBarrierCount = 1u;
      maskDep.pImageMemoryBarriers = &maskToRead;
      ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &maskDep);
      war3::render::CommitWar3OwnedImageLayout(
          m_shadowCasterMaskLayout, maskTransition, *m_shadowCasterMask,
          maskSubresources);
    }
  };

  // capture 的冻结缓冲由 transfer 写入，GPU 蒙皮共享输出页由 compute 写入，
  // VS-S1 的 palette 也由 transfer 写入。阴影重放会同时把它们作为
  // vertex/index 输入或 vertex-shader storage 读取，因此在此统一建立可见性。
  shadowMapPhaseTiming.enter(static_cast<size_t>(
      War3DirectionalShadowMapRawPhase::BeginTransitions));
  {
    VkMemoryBarrier2 memBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT |
                               VK_ACCESS_2_SHADER_WRITE_BIT;
    memBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                              VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    memBarrier.dstAccessMask =
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
        VK_ACCESS_2_SHADER_READ_BIT;

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers = &memBarrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  // 2) Transition shadow map to depth attachment layout for rendering
  {
    const VkImageSubresourceRange subresources = {
        VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, cascadeCount};
    const auto transition = shadowMapLayout.plan(
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    VkImageMemoryBarrier2 toDepth =
        war3::render::MakeWar3OwnedImageBarrier(
            transition, m_shadowMap->handle(), subresources);

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &toDepth;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
    war3::render::CommitWar3OwnedImageLayout(
        shadowMapLayout, transition, *m_shadowMap, subresources);
  }
  if (terrainCasterMaskEnabled) {
    const VkImageSubresourceRange subresources = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, cascadeCount};
    const auto transition = m_shadowCasterMaskLayout.plan(
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkImageMemoryBarrier2 toMask =
        war3::render::MakeWar3OwnedImageBarrier(
            transition, m_shadowCasterMask->handle(), subresources);

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &toMask;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
    war3::render::CommitWar3OwnedImageLayout(
        m_shadowCasterMaskLayout, transition, *m_shadowCasterMask,
        subresources);
  }

  shadowMapPhaseTiming.enter(static_cast<size_t>(
      War3DirectionalShadowMapRawPhase::PreparePipelines));
  const VkExtent3D extent = {m_shadowMapResolution, m_shadowMapResolution, 1u};
  uint32_t drawnCasters = 0;

  const uint32_t casterCount =
      static_cast<uint32_t>(replayDraws.size());
  m_shadowPreparedScratch.clear();
  m_shadowPreparedScratch.resize(casterCount);
  auto& prepared = m_shadowPreparedScratch;
  uint32_t skinnedCasterCount = 0;
  uint32_t skinnedPreparedCount = 0;
  uint32_t skinnedInvalidBufferCount = 0;
  uint32_t skinnedInvalidPipelineCount = 0;
  uint32_t preparedDrawCount = 0;
  uint32_t requiredPreparedDrawCount = 0;
  uint32_t alphaTestPreparedCount = 0;
  uint32_t alphaPromotedPreparedCount = 0;
  uint32_t dynamicPreparedCount = 0;
  uint32_t staticPreparedCount = 0;
  uint32_t otherPreparedCount = 0;
  uint32_t terrainDoodadPreparedCount = 0;
  uint32_t terrainS1PreparedCount = 0;
  // Diagnostic-only isolation used by the bridge/ramp visual probe. Keeping
  // this filter at the final CSM replay boundary proves whether Stage13
  // geometry itself is present in the shadow map without changing capture,
  // publication, sorting, or receiver behavior.
  static const int s_debugCasterStage =
      EnvIntOverride("DXVK_WAR3_SHADOW_DEBUG_CASTER_STAGE", 0, 31);

  auto len3 = [](float x, float y, float z) {
    return std::sqrt(x * x + y * y + z * z);
  };

  // 预先准备 Pipeline 与排序 key（与级联无关）
  for (uint32_t i = 0; i < casterCount; i++) {
    const auto &draw = *replayDraws[i];
    if (s_debugCasterStage >= 0 && draw.stage != s_debugCasterStage)
      continue;
    // A GPU-direct input draw deliberately disables the legacy blend binding:
    // the shadow shader consumes immutable source vertices and its palette
    // through gpuSkinInput instead. Count that route as skinned as well, or a
    // high-pressure scene appears to have zero skinned casters while still
    // executing all of their CSM work.
    const bool skinnedDraw =
        draw.vertexBlendEnabled || draw.gpuSkinInput.valid;
    if (skinnedDraw)
      skinnedCasterCount++;
    if (draw.positionInfo.buffer == VK_NULL_HANDLE ||
        draw.positionInfo.size == 0) {
      if (skinnedDraw)
        skinnedInvalidBufferCount++;
      continue;
    }
    if (draw.indexed &&
        (draw.indexInfo.buffer == VK_NULL_HANDLE || draw.indexInfo.size == 0)) {
      if (skinnedDraw)
        skinnedInvalidBufferCount++;
      continue;
    }

    ShadowCasterPipelineKey key = {};
    key.positionFormat = draw.positionFormat;
    key.positionStride = draw.positionStride;
    key.positionOffset = draw.positionOffset;
    key.topology = draw.topology;

    if (draw.vertexBlendEnabled && draw.vertexBlendCount > 0) {
      key.blendWeightFormat = draw.blendWeightFormat;
      key.blendWeightOffset = draw.blendWeightOffset;
    } else {
      key.blendWeightFormat = VK_FORMAT_UNDEFINED;
      key.blendWeightOffset = 0;
    }

    if (draw.vertexBlendEnabled && draw.vertexBlendIndexed) {
      key.blendIndexFormat = draw.blendIndexFormat;
      key.blendIndexOffset = draw.blendIndexOffset;
    } else {
      key.blendIndexFormat = VK_FORMAT_UNDEFINED;
      key.blendIndexOffset = 0;
    }

    key.blendBinding = draw.blendBinding;
    key.blendStride = draw.blendStride;
    key.casterMaskEnabled = false;

    // AlphaBlend alone is not an authoritative cutout contract. Reject it
    // instead of inventing a 0.5 threshold, and reject native alpha-test when
    // any texture/UV backing is incomplete rather than drawing an opaque card.
    const bool alphaPayloadComplete =
        draw.diffuseTexture && draw.HasUsableUvBinding();
    if (draw.alphaBlendEnabled && !draw.alphaTestEnabled)
      continue;
    ++requiredPreparedDrawCount;
    if (draw.alphaTestEnabled && !alphaPayloadComplete)
      continue;
    const bool effectiveAlphaTestShadow =
        draw.alphaTestEnabled && alphaPayloadComplete;
    const bool alphaPromotedShadow = false;

    key.alphaTestEnabled = effectiveAlphaTestShadow;
    if (effectiveAlphaTestShadow) {
      key.uvFormat = draw.uvFormat;
      key.uvOffset = draw.uvOffset;
      key.uvStride = draw.uvStride;
      key.uvBinding = draw.uvBinding;
    }

    PreparedShadowCaster out = {};
    out.pipeline = getShadowCasterPipeline(key);
    if (out.pipeline.pipeline == VK_NULL_HANDLE) {
      if (skinnedDraw)
        skinnedInvalidPipelineCount++;
      continue;
    }

    out.valid = true;
    out.pipelineHash = key.hash();
    out.positionBuffer = draw.positionInfo.buffer;
    out.indexBuffer = draw.indexed ? draw.indexInfo.buffer : VK_NULL_HANDLE;
    // Phase 7.52 AlphaTest 修复：保留 prepare 阶段的 effective alpha-test 决定，
    // 让 pc.flags 写入循环直接复用，不再仅看 draw.alphaTestEnabled。
    out.effectiveAlphaTest = effectiveAlphaTestShadow;
    out.alphaImageView = (effectiveAlphaTestShadow && draw.diffuseTexture)
                             ? draw.textureDescriptor.legacy.image.imageView
                             : VK_NULL_HANDLE;
    // The direct-input contract depends only on immutable replay-draw state.
    // Cache it once here instead of repeating the same probe for every
    // cascade. Per-cascade telemetry remains at the consumption site below.
    const ShadowGpuSkinDirectDecision gpuSkinDirectDecision =
        EvaluateShadowGpuSkinDirectInput(draw);
    out.gpuSkinDirectRequested = gpuSkinDirectDecision.requested;
    out.gpuSkinDirectInputExact = gpuSkinDirectDecision.inputExact;
    out.gpuSkinDirectStateExact = gpuSkinDirectDecision.stateExact;
    preparedDrawCount++;
    war3::render::NoteShadowStageReplayPrepared(draw.stage);
    if (effectiveAlphaTestShadow)
      alphaTestPreparedCount++;
    if (alphaPromotedShadow)
      alphaPromotedPreparedCount++;
    const auto preparedKind =
        static_cast<war3::render::ObjectKind>(draw.objectKind);
    if (skinnedDraw || preparedKind == war3::render::ObjectKind::Unit ||
        preparedKind == war3::render::ObjectKind::Effect) {
      dynamicPreparedCount++;
    } else if (draw.category == War3RenderState::StageCategory::Terrain ||
               preparedKind == war3::render::ObjectKind::Building ||
               preparedKind == war3::render::ObjectKind::Destructible) {
      staticPreparedCount++;
    } else {
      otherPreparedCount++;
    }
    if (skinnedDraw)
      skinnedPreparedCount++;
    if (draw.category == War3RenderState::StageCategory::Terrain) {
      if (draw.stage == 10)
        terrainDoodadPreparedCount++;
      else if (draw.stage == 1)
        terrainS1PreparedCount++;
    }

    prepared[i] = out;
  }

  if (preparedDrawCount != requiredPreparedDrawCount) {
    // Pipeline/material preparation is part of the publication transaction.
    // The old complete contents have not been cleared or drawn over, so put
    // both images back into their sampling layouts and reject the candidate.
    restoreShadowTargetsToRead();
    ++reconciliation.replayValidationRejectedCount;
    ++reconciliation.replayPartialPreventedCount;
    reconciliation.replayValidationLastReason = static_cast<uint32_t>(
        war3::render::War3ShadowReplayRejectReason::IncompleteReplayPlan);
    g_shadowReplayDiagnostics.validationRejectCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_shadowReplayDiagnostics.partialPreventedCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_shadowReplayDiagnostics.lastRejectReason.store(
        reconciliation.replayValidationLastReason,
        std::memory_order_release);
    g_shadowReplayDiagnostics.validatedCasterCount.store(
        preparedDrawCount, std::memory_order_release);
    m_replayValidationFailedThisFrame = true;
    return false;
  }

  shadowMapPhaseTiming.enter(static_cast<size_t>(
      War3DirectionalShadowMapRawPhase::CullAndSortSetup));
  // 每个级联预先计算“世界半径 -> NDC 半径”的缩放（row length）
  struct CascadeCullParams {
    float row0Len = 0.0f;
    float row1Len = 0.0f;
    float row2Len = 0.0f;
  };
  std::array<CascadeCullParams, 4> cullParams = {};
  for (uint32_t c = 0; c < cascadeCount; c++) {
    const Matrix4 &m = m_csmData.cascades[c].lightViewProj;
    cullParams[c].row0Len = len3(m[0].x, m[1].x, m[2].x);
    cullParams[c].row1Len = len3(m[0].y, m[1].y, m[2].y);
    cullParams[c].row2Len = len3(m[0].z, m[1].z, m[2].z);
  }

  static const bool s_disableFarCascadeCull =
      EnvFlagDefault("DXVK_WAR3_CSM_DISABLE_FAR_CASCADE_CULL", false);
  // Generation-backed object bounds remain Observe-only by default.  A
  // physical A/B must prove zero false negatives before this experimental
  // switch may consume C2/C3 visibility decisions.
  static const bool s_objectBoundsCullConsume =
      !war3::internal::kReleaseFreezeExperimentalShadowRoutes &&
      EnvFlagDefault("DXVK_WAR3_OBJECT_BOUNDS_CULL_CONSUME", false);
  const auto terrainBoundsCullMode = m_volumeSunRenderPathActive
      ? war3::render::War3TerrainBoundsCullMode::Off
      : War3TerrainBoundsCullModeRuntime();
  reconciliation.terrainBoundsCullMode =
      static_cast<uint32_t>(terrainBoundsCullMode);

  const auto evaluateBoundsPolicy = [&](
      const War3ShadowCasterDraw& draw) {
    const bool finiteBounds =
        std::isfinite(draw.boundsCenter.x) &&
        std::isfinite(draw.boundsCenter.y) &&
        std::isfinite(draw.boundsCenter.z) &&
        std::isfinite(draw.boundsRadius);
    return war3::render::War3EvaluateBoundsCullEvidence({
        draw.boundsProvenance,
        draw.boundsSourceGeneration,
        draw.boundsFrameSerial,
        input.frameSerial,
        draw.boundsIdentityProven,
        draw.boundsSourceWasSkinned,
        draw.boundsFrameLocalDynamic,
        draw.boundsAnimatedAttachment,
        finiteBounds,
        draw.boundsRadius > 0.0f});
  };

  auto intersectsCascadeUnchecked = [&](const War3ShadowCasterDraw &draw,
                                        uint32_t cascadeIdx) -> bool {
    using war3::render::ObjectKind;

    const bool terrainDraw =
        draw.category == War3RenderState::StageCategory::Terrain;
    if (!(draw.boundsRadius > 0.0f))
      return true;
    // Phase 7.92：适配 War3 RTS 俯视镜头。cascade 0/1 覆盖玩家视野核心区域，
    // 不做 cull 保证近处阴影完整；cascade 2/3 覆盖远处，做 cull 省 draw。
    // A/B：DXVK_WAR3_CSM_DISABLE_FAR_CASCADE_CULL=1 时所有非地形也不剔除，
    // 用于验证高镜头树影/单位是否因 C2/C3 包围球漏杀。
    if ((cascadeIdx < 2u || s_disableFarCascadeCull) && !terrainDraw)
      return true;
    const auto objectKind = static_cast<ObjectKind>(draw.objectKind);
    if constexpr (war3::internal::kShadowCascadeCullDisableForUnits) {
      if (objectKind == ObjectKind::Unit)
        return true;
    }

    const Matrix4 &m = m_csmData.cascades[cascadeIdx].lightViewProj;
    const Vector4 clip = m * draw.boundsCenter;
    const float absW = std::abs(clip.w);
    if (!(absW > 1e-6f))
      return true;

    const float invW = 1.0f / absW;
    const float ndcX = clip.x * invW;
    const float ndcY = clip.y * invW;
    const float ndcZ = clip.z * invW;

    const auto &p = cullParams[cascadeIdx];
    float r = draw.boundsRadius;
    float guard = war3::internal::kShadowCascadeCullGuardBandNdc;
    float zGuard = guard;
    if (draw.vertexBlendEnabled) {
      r = r * war3::internal::kShadowCascadeCullSkinnedRadiusScale +
          war3::internal::kShadowCascadeCullSkinnedExtraRadius;
      guard =
          std::max(guard,
                   war3::internal::kShadowCascadeCullSkinnedExtraGuardNdc);
      zGuard =
          std::max(zGuard,
                   war3::internal::kShadowCascadeCullSkinnedZExtraGuardNdc);
    } else if (objectKind == ObjectKind::Building) {
      r *= war3::internal::kShadowCascadeCullBuildingRadiusScale;
      guard =
          std::max(guard,
                   war3::internal::kShadowCascadeCullBuildingExtraGuardNdc);
      zGuard = std::max(zGuard, guard);
    } else if (terrainDraw) {
      r *= war3::internal::kShadowCascadeCullTerrainRadiusScale;
      guard =
          std::max(guard,
                   war3::internal::kShadowCascadeCullTerrainExtraGuardNdc);
      zGuard = std::max(zGuard, guard);
    } else if (draw.indexCount >= 8192u || draw.vertexCount >= 8192u ||
               draw.numVertices >= 8192u) {
      r *= war3::internal::kShadowCascadeCullLargeDrawRadiusScale;
      guard =
          std::max(guard,
                   war3::internal::kShadowCascadeCullLargeDrawExtraGuardNdc);
      zGuard = std::max(zGuard, guard);
    }
    const float rX = r * p.row0Len * invW;
    const float rY = r * p.row1Len * invW;
    const float rZ = r * p.row2Len * invW;

    if (ndcX + rX < -1.0f - guard || ndcX - rX > 1.0f + guard)
      return false;
    if (ndcY + rY < -1.0f - guard || ndcY - rY > 1.0f + guard)
      return false;
    // Vulkan NDC: z ∈ [0, 1]
    if (ndcZ + rZ < -zGuard || ndcZ - rZ > 1.0f + zGuard)
      return false;

    return true;
  };

  // Bounds mathematics and culling authority are deliberately separate.
  // Guessed scene-node spheres remain useful for Observe telemetry, but can
  // never remove a required caster.  This applies to buildings, trees and
  // animated attachments as well as terrain.
  const auto intersectsCascadeAuthorized = [&]
      (const War3ShadowCasterDraw& draw,
       uint32_t cascadeIdx,
       const war3::render::War3ShadowBoundsCullDecision& boundsPolicy) {
    const bool terrainDraw =
        draw.category == War3RenderState::StageCategory::Terrain;
    if (terrainDraw &&
        !war3::internal::kShadowCascadeCullTerrainWithBounds)
      return true;
    if (!boundsPolicy.mayCull)
      return true;
    return intersectsCascadeUnchecked(draw, cascadeIdx);
  };

  m_shadowSortedDrawIndicesScratch.clear();
  m_shadowSortedDrawIndicesScratch.reserve(casterCount);
  auto& sortedDrawIndices = m_shadowSortedDrawIndicesScratch;
  for (uint32_t i = 0; i < casterCount; i++) {
    if (prepared[i].valid)
      sortedDrawIndices.push_back(i);
  }
  if (sortedDrawIndices.size() > 1) {
    std::sort(sortedDrawIndices.begin(), sortedDrawIndices.end(),
              [&](uint32_t a, uint32_t b) {
                const auto &ka = prepared[a];
                const auto &kb = prepared[b];
                if (ka.pipelineHash != kb.pipelineHash)
                  return ka.pipelineHash < kb.pipelineHash;
                if (ka.alphaImageView != kb.alphaImageView)
                  return ka.alphaImageView < kb.alphaImageView;
                if (ka.positionBuffer != kb.positionBuffer)
                  return ka.positionBuffer < kb.positionBuffer;
                if (ka.indexBuffer != kb.indexBuffer)
                  return ka.indexBuffer < kb.indexBuffer;
                return a < b;
               });
  }

  m_shadowCascadeVisibilityMasksScratch.assign(casterCount, 0u);
  for (const uint32_t drawIndex : sortedDrawIndices) {
    const auto& draw = *replayDraws[drawIndex];
    const bool terrainDraw =
        draw.category == War3RenderState::StageCategory::Terrain;
    const auto boundsPolicy = evaluateBoundsPolicy(draw);
    if (terrainDraw &&
        terrainBoundsCullMode !=
            war3::render::War3TerrainBoundsCullMode::Off) {
      ++reconciliation.terrainBoundsCandidateCount;
      if (boundsPolicy.mayCull)
        ++reconciliation.terrainBoundsProofAcceptedCount;
      else
        ++reconciliation.terrainBoundsFailVisibleCount;
    } else if (!terrainDraw && draw.boundsRadius > 0.0f) {
      ++reconciliation.objectBoundsCandidateCount;
      if (boundsPolicy.mayCull)
        ++reconciliation.objectBoundsProofAcceptedCount;
      else
        ++reconciliation.objectBoundsFailVisibleCount;
    }

    uint8_t actualMask = 0u;
    for (uint32_t c = 0u; c < cascadeCount && c < 4u; ++c) {
      const bool terrainWouldBeVisible =
          !terrainDraw ||
          terrainBoundsCullMode ==
              war3::render::War3TerrainBoundsCullMode::Off ||
          intersectsCascadeAuthorized(draw, c, boundsPolicy);
      if (terrainDraw && boundsPolicy.mayCull &&
          terrainBoundsCullMode !=
              war3::render::War3TerrainBoundsCullMode::Off &&
          !terrainWouldBeVisible) {
        ++reconciliation.terrainBoundsWouldCullCount;
        if (c == 0u)
          ++reconciliation.terrainBoundsC0WouldCullCount;
        else if (c == 1u)
          ++reconciliation.terrainBoundsC1WouldCullCount;
        else if (c == 2u)
          ++reconciliation.terrainBoundsC2WouldCullCount;
        else if (c == 3u)
          ++reconciliation.terrainBoundsC3WouldCullCount;
      }

      const bool consumeTerrainCascade =
          terrainBoundsCullMode ==
              war3::render::War3TerrainBoundsCullMode::Consume &&
          c >= 2u;
      const bool objectWouldBeVisible = terrainDraw ||
          intersectsCascadeAuthorized(draw, c, boundsPolicy);
      if (!terrainDraw && boundsPolicy.mayCull && c >= 2u &&
          !objectWouldBeVisible) {
        ++reconciliation.objectBoundsWouldCullCount;
      }
      const bool consumeObjectCascade =
          s_objectBoundsCullConsume && c >= 2u;
      const bool actualVisible = terrainDraw
          ? !consumeTerrainCascade || terrainWouldBeVisible
          : !consumeObjectCascade || objectWouldBeVisible;
      if (actualVisible) {
        actualMask |= uint8_t(1u << c);
      } else if (terrainDraw) {
        ++reconciliation.terrainBoundsAppliedCullCount;
      } else {
        ++reconciliation.objectBoundsAppliedCullCount;
      }
    }
    m_shadowCascadeVisibilityMasksScratch[drawIndex] = actualMask;
  }
  const auto cascadeVisible = [&](uint32_t drawIndex,
                                  uint32_t cascadeIdx) {
    return drawIndex < m_shadowCascadeVisibilityMasksScratch.size() &&
        cascadeIdx < 4u &&
        (m_shadowCascadeVisibilityMasksScratch[drawIndex] &
         uint8_t(1u << cascadeIdx)) != 0u;
  };

  // Observation-only admission stage for the future joint-consumer mask.
  // It deliberately runs at the final CSM boundary so its prediction can be
  // compared against the canonical culler without changing capture, upload,
  // sorting or any submitted draw. Even mode=Consume remains unadmitted here.
  const auto unionCullMode = War3UnionCullModeRuntime();
  if (unionCullMode != war3::render::War3UnionVisibilityMode::Off) {
    reconciliation.unionCullMode = static_cast<uint32_t>(unionCullMode);
    reconciliation.unionCullObserveFrameCount = 1u;

    const bool cameraCurrent = War3WorldCameraIsFreshForFrame(
        input.scene.worldCamera, input.frameSerial);
    const bool consumerStateCurrent = !s_disableFarCascadeCull;
    const uint32_t firstFarCascade = 2u;

    for (const uint32_t drawIndex : sortedDrawIndices) {
      const auto& draw = *replayDraws[drawIndex];
      const auto objectKind =
          static_cast<war3::render::ObjectKind>(draw.objectKind);
      const bool dynamicOrSkinned =
          draw.vertexBlendEnabled || draw.vertexBlendIndexed ||
          objectKind == war3::render::ObjectKind::Unit ||
          objectKind == war3::render::ObjectKind::Effect;
      if (dynamicOrSkinned) {
        ++reconciliation.unionCullDynamicConservativeCount;
        continue;
      }

      const bool staticRigid =
          objectKind == war3::render::ObjectKind::Building ||
          objectKind == war3::render::ObjectKind::Destructible;
      const bool exactCurrentSource =
          draw.stage == 11 &&
          draw.shadowPartLifecycleState ==
              War3ShadowPartLifecycleState::RequiredCurrent &&
          draw.shadowRenderablePart != nullptr &&
          draw.shadowExactGeometryKeyHash != 0u;
      if (!staticRigid || !exactCurrentSource || !cameraCurrent ||
          !consumerStateCurrent || !(draw.boundsRadius > 0.0f) ||
          m_shadowMapResourceGeneration == 0u) {
        ++reconciliation.unionCullUnknownOrStaleCount;
        continue;
      }

      ++reconciliation.unionCullCandidateCount;
      bool farWouldCull[2] = {false, false};
      for (uint32_t c = firstFarCascade; c < cascadeCount && c < 4u; ++c) {
        war3::render::War3UnionCsmSphereQuery query = {};
        query.mode = unionCullMode;
        query.requestedMask = war3::render::War3UnionConsumerCsm2 |
                              war3::render::War3UnionConsumerCsm3;
        query.cascadeIndex = c;
        query.bounds = {draw.boundsCenter.x, draw.boundsCenter.y,
                        draw.boundsCenter.z, draw.boundsRadius};

        const Matrix4& matrix = m_csmData.cascades[c].lightViewProj;
        for (uint32_t column = 0u; column < 4u; ++column) {
          query.lightViewProjection.columns[column][0] = matrix[column].x;
          query.lightViewProjection.columns[column][1] = matrix[column].y;
          query.lightViewProjection.columns[column][2] = matrix[column].z;
          query.lightViewProjection.columns[column][3] = matrix[column].w;
        }

        query.generations.currentFrameGeneration = input.frameSerial;
        query.generations.candidateFrameGeneration = input.frameSerial;
        query.generations.boundsFrameGeneration = input.frameSerial;
        query.generations.cameraFrameGeneration = input.frameSerial;
        query.generations.consumerStateFrameGeneration = input.frameSerial;
        query.generations.resourceGeneration = m_shadowMapResourceGeneration;
        query.generations.expectedResourceGeneration =
            m_shadowMapResourceGeneration;
        query.identityKnown = true;
        query.exactCurrentFrameSource = true;
        query.boundsKnown = true;
        query.cameraKnown = cameraCurrent;
        query.consumerStateKnown = consumerStateCurrent;
        query.matrixKnown = true;
        query.staticRigidProven = true;
        query.dynamic = false;
        query.skinned = false;
        query.radiusScale = 1.0f;
        query.guardBandNdc = war3::internal::kShadowCascadeCullGuardBandNdc;
        query.depthGuardBandNdc = query.guardBandNdc;
        if (objectKind == war3::render::ObjectKind::Building) {
          query.radiusScale =
              war3::internal::kShadowCascadeCullBuildingRadiusScale;
          query.guardBandNdc = std::max(
              query.guardBandNdc,
              war3::internal::kShadowCascadeCullBuildingExtraGuardNdc);
          query.depthGuardBandNdc = query.guardBandNdc;
        } else if (draw.indexCount >= 8192u ||
                   draw.vertexCount >= 8192u ||
                   draw.numVertices >= 8192u) {
          query.radiusScale =
              war3::internal::kShadowCascadeCullLargeDrawRadiusScale;
          query.guardBandNdc = std::max(
              query.guardBandNdc,
              war3::internal::kShadowCascadeCullLargeDrawExtraGuardNdc);
          query.depthGuardBandNdc = query.guardBandNdc;
        }
        query.consumeAdmissionGranted = false;

        const auto decision =
            war3::render::War3EvaluateConservativeCsmSphere(query);
        if ((decision.proofBits &
             war3::render::War3UnionProofFiniteProjection) != 0u)
          ++reconciliation.unionCullProofAcceptedCount;
        if (decision.failVisible)
          ++reconciliation.unionCullFailVisibleCount;

        const auto consumerBit = war3::render::War3UnionCsmConsumerBit(c);
        const bool predictedVisible =
            (decision.predictedVisibleMask & consumerBit) != 0u;
        const bool canonicalVisible = cascadeVisible(drawIndex, c);
        if (!predictedVisible && canonicalVisible)
          ++reconciliation.unionCullFalseNegativeCount;
        if (predictedVisible && !canonicalVisible)
          ++reconciliation.unionCullFalsePositiveCount;
        farWouldCull[c - firstFarCascade] = !predictedVisible;
        if (!predictedVisible) {
          if (c == 2u)
            ++reconciliation.unionCullC2WouldCullCount;
          else if (c == 3u)
            ++reconciliation.unionCullC3WouldCullCount;
        }
      }
      if (farWouldCull[0] && farWouldCull[1])
        ++reconciliation.unionCullBothFarWouldCullCount;
    }
  }

  m_shadowTerrainMaskDrawIndicesScratch.clear();
  auto& terrainMaskDrawIndices = m_shadowTerrainMaskDrawIndicesScratch;
  if (terrainCasterMaskEnabled) {
    terrainMaskDrawIndices.reserve(sortedDrawIndices.size());
    for (uint32_t i : sortedDrawIndices) {
      const auto& draw = *replayDraws[i];
      if (draw.category == War3RenderState::StageCategory::Terrain &&
          draw.stage == 1) {
        terrainMaskDrawIndices.push_back(i);
      }
    }
  }

  if (!m_volumeSunRenderPathActive) {
    // Charge only canonical prepared draws retained by each cascade. This is
    // the exact work the recording loops below can submit, including the
    // separate Stage1 terrain-mask replay. Reservation remains transactional
    // and precedes the first BeginRendering/clear/draw command.
    war3::render::War3GpuWorkloadCost workloadCost = {};
    uint64_t workloadCascadeItems = 0u;
    for (const uint32_t drawIndex : sortedDrawIndices) {
      const auto& draw = *replayDraws[drawIndex];
      uint32_t visibleCascadeCount = 0u;
      for (uint32_t cascade = 0u; cascade < cascadeCount; ++cascade) {
        if (cascadeVisible(drawIndex, cascade))
          ++visibleCascadeCount;
      }
      if (visibleCascadeCount == 0u)
        continue;
      workloadCascadeItems += visibleCascadeCount;
      if (!AddWar3ShadowWorkloadDraw(workloadCost, draw,
                                     visibleCascadeCount)) {
        workloadCost.valid = false;
        break;
      }
      if (terrainCasterMaskEnabled &&
          draw.category == War3RenderState::StageCategory::Terrain &&
          draw.stage == 1 &&
          !AddWar3ShadowWorkloadDraw(workloadCost, draw,
                                     visibleCascadeCount)) {
        workloadCost.valid = false;
        break;
      }
    }
    if (!directionalWorkloadReservation.reserve(
            war3::render::War3GpuWorkloadConsumer::DirectionalCsm,
            std::max<uint64_t>(workloadCascadeItems, 1u), workloadCost)) {
      restoreShadowTargetsToRead();
      m_workloadGovernorRejectedThisFrame = true;
      const auto& governor = m_gpuWorkloadGovernor.diagnostics();
      static uint32_t s_directionalWorkloadRejectLogs = 0u;
      const uint32_t logIndex = s_directionalWorkloadRejectLogs++;
      if (logIndex < 16u || (logIndex % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3GpuWorkload: reject directional-csm frame=%llu "
            "request(draw=%llu vertex=%llu index=%llu items=%llu) "
            "used(draw=%llu vertex=%llu index=%llu) reason=%u\n",
            static_cast<unsigned long long>(input.frameSerial),
            static_cast<unsigned long long>(workloadCost.draws),
            static_cast<unsigned long long>(workloadCost.vertices),
            static_cast<unsigned long long>(workloadCost.indices),
            static_cast<unsigned long long>(workloadCascadeItems),
            static_cast<unsigned long long>(governor.used.draws),
            static_cast<unsigned long long>(governor.used.vertices),
            static_cast<unsigned long long>(governor.used.indices),
            governor.lastRejectReason);
      }
      return false;
    }
  }

  m_shadowDrawIndicesScratch.clear();
  m_shadowDrawIndicesScratch.reserve(sortedDrawIndices.size());
  auto& drawIndices = m_shadowDrawIndicesScratch;
  std::array<uint32_t, 4> culledPerCascade = {};
  std::array<uint32_t, 4> drawnPerCascade = {};
  std::array<uint32_t, 4> skinnedCulledPerCascade = {};
  std::array<uint32_t, 4> skinnedDrawnPerCascade = {};
  std::array<uint32_t, 4> terrainDoodadDrawnPerCascade = {};
  std::array<uint32_t, 4> terrainS1DrawnPerCascade = {};
  const bool csmDescriptorReuseEnabled =
      War3CsmDescriptorReuseEnabled() && m_device->canUseDescriptorBuffer();
  const bool csmDescriptorReuseVerifierEnabled =
      War3CsmDescriptorReuseVerifierEnabled();
  const bool collectCsmDescriptorReuseCounters =
      shadowMapPhaseSampleWeight != 0u ||
      csmDescriptorReuseVerifierEnabled;
  uint32_t csmDescriptorRequestCount = 0u;
  uint32_t csmDescriptorFullDrawBindCount = 0u;
  uint32_t csmDescriptorPushOnlyReuseCount = 0u;
  uint32_t csmDescriptorColdMissCount = 0u;
  uint32_t csmDescriptorLayoutMissCount = 0u;
  uint32_t csmDescriptorBufferMissCount = 0u;
  uint32_t csmDescriptorAlphaMissCount = 0u;
  uint32_t csmDescriptorInvalidationCount = 0u;
  uint32_t csmDescriptorDirectBypassCount = 0u;
  uint32_t csmDescriptorDirectClearBindCount = 0u;
  uint32_t csmDescriptorVerifierMismatchCount = 0u;

  for (uint32_t c = 0; c < cascadeCount; c++) {
    war3::tools::SetGpuFlightBreadcrumb(
        war3::tools::GpuFlightBreadcrumb::CsmCascade, c);
    shadowMapPhaseTiming.enter(static_cast<size_t>(
        War3DirectionalShadowMapRawPhase::CascadeCull));
    if (!m_shadowMapLayerViews[c])
      continue;

    drawIndices.clear();
    uint32_t culled = 0;
    for (uint32_t i : sortedDrawIndices) {
      const auto &draw = *replayDraws[i];
      if (!cascadeVisible(i, c)) {
        culled++;
        if (draw.vertexBlendEnabled)
          skinnedCulledPerCascade[c]++;
        continue;
      }
      drawIndices.push_back(i);
    }

    culledPerCascade[c] = culled;

    shadowMapPhaseTiming.enter(static_cast<size_t>(
        War3DirectionalShadowMapRawPhase::CascadeRecord));
    const float alphaRefBiasCascade =
        war3::render::ShadowAlphaRefBiasForCascade(
            alphaShadowFarAlphaRefBias, c, cascadeCount);

    VkClearValue clearValue = {};
    clearValue.depthStencil.depth = 1.0f;
    clearValue.depthStencil.stencil = 0;

    VkRenderingAttachmentInfo depthAttachment = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = m_shadowMapLayerViews[c]->handle();
    depthAttachment.imageLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue = clearValue;

    VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea.offset = {0u, 0u};
    renderInfo.renderArea.extent = {extent.width, extent.height};
    renderInfo.layerCount = 1u;
    renderInfo.colorAttachmentCount = 0u;
    renderInfo.pColorAttachments = nullptr;
    renderInfo.pDepthAttachment = &depthAttachment;

    volumeWorkloadReservation.commit();
    directionalWorkloadReservation.commit();
    ctx->cmdBeginRendering(&renderInfo);

    // D3D-style NDC needs a negative viewport height (receiver shader assumes
    // this).
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = float(extent.height);
    viewport.width = float(extent.width);
    viewport.height = -float(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {extent.width, extent.height};

    ctx->cmdSetViewport(1, &viewport);
    ctx->cmdSetScissor(1, &scissor);

    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkBuffer boundVb0 = VK_NULL_HANDLE;
    VkDeviceSize boundVb0Offset = 0;
    VkDeviceSize boundVb0Size = 0;
    VkDeviceSize boundVb0Stride = 0;
    VkBuffer boundVb1 = VK_NULL_HANDLE;
    VkDeviceSize boundVb1Offset = 0;
    VkDeviceSize boundVb1Size = 0;
    VkDeviceSize boundVb1Stride = 0;
    VkBuffer boundVb2 = VK_NULL_HANDLE;
    VkDeviceSize boundVb2Offset = 0;
    VkDeviceSize boundVb2Size = 0;
    VkDeviceSize boundVb2Stride = 0;
    uint32_t boundVbCount = 0;

    VkBuffer boundIb = VK_NULL_HANDLE;
    VkDeviceSize boundIbOffset = 0;
    VkDeviceSize boundIbSize = 0;
    VkIndexType boundIbType = VK_INDEX_TYPE_UINT16;

    uint32_t cascadeDrawn = 0;
    uint64_t cascadeTriangles = 0u;
    bool csmDescriptorCacheValid = false;
    War3CsmDescriptorSignature csmDescriptorCacheKey = {};
    std::array<DxvkDescriptorWrite, 5> csmDescriptorCacheWrites = {};

    for (uint32_t idx : drawIndices) {
      const auto &draw = *replayDraws[idx];
      auto &prep = prepared[idx];
      const bool gpuSkinDirect =
          prep.gpuSkinDirectRequested &&
          prep.gpuSkinDirectInputExact &&
          prep.gpuSkinDirectStateExact;
      if (prep.gpuSkinDirectRequested) {
        ++m_gpuSkinVsShadowDirectAttempts;
        if (!prep.gpuSkinDirectInputExact)
          ++m_gpuSkinVsShadowDirectInputRejects;
        else if (!prep.gpuSkinDirectStateExact)
          ++m_gpuSkinVsShadowDirectStateRejects;
      }

      ShadowCasterPushConstants pc = {};
      pc.blendCount = draw.vertexBlendCount;
      pc.flags = 0u;
      pc.mvp = m_csmData.cascades[c].lightViewProj;
      const bool s1TerrainCaster =
          draw.category == War3RenderState::StageCategory::Terrain &&
          draw.stage == 1;
      if (s1TerrainCaster) {
        pc.flags |= kShadowCasterFlagStage1Terrain;
        if (war3::internal::kShadowS1TerrainCasterDepthBiasEnabled) {
          pc.terrainDepthBias =
              war3::internal::kShadowS1TerrainCasterDepthBiasNdc;
        }
      }

      if (draw.vertexBlendEnabled) {
        pc.flags |= kShadowCasterFlagUseBlend;
        if (draw.vertexBlendIndexed)
          pc.flags |= kShadowCasterFlagIndexedBlend;
        pc.paletteOffset = draw.paletteIndex * 256u;
      } else {
        // 非混合物体：worldMatrix 放在矩阵 SSBO 末尾，按 drawIndex 取用
        pc.paletteOffset = objectBase + idx;
      }

      if (gpuSkinDirect) {
        pc.flags |= kShadowCasterFlagGpuSkinDirectInput |
                    PackShadowCasterGpuSkinMetadata(
                        draw.gpuSkinInput.desc);
        if (draw.gpuSkinInput.irreversible)
          pc.flags |= kShadowCasterFlagGpuSkinNoFallback;
        pc.blendCount = draw.gpuSkinInput.desc.paletteMatrixCount;
        pc.padding[1] = draw.gpuSkinInput.desc.vertexCount;
      }

      if (prep.effectiveAlphaTest && draw.diffuseTexture) {
        pc.flags |= kShadowCasterFlagAlphaTest;
        if (alphaShadowHashed)
          pc.flags |= kShadowCasterFlagHashAlpha;
        pc.alphaRef =
            std::clamp(draw.alphaRef + alphaRefBiasCascade, 0.0f, 1.0f);
        pc.samplerIndex = draw.diffuseSamplerIndex;
      }

      if (prep.pipeline.pipeline != boundPipeline) {
        trackCasterPipeline(prep.pipeline);
        ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                             VK_PIPELINE_BIND_POINT_GRAPHICS,
                             prep.pipeline.pipeline);
        boundPipeline = prep.pipeline.pipeline;
      }

      std::array<DxvkDescriptorWrite, 5> descriptors = {};
      descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      descriptors[0].buffer = paletteDesc.buffer;

      if (prep.effectiveAlphaTest && draw.diffuseTexture) {
        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = &draw.textureDescriptor;
        ctx->track(draw.diffuseTexture->image(), DxvkAccess::Read);
      } else {
        descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[1].descriptor = nullptr;
      }
      descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      descriptors[2].descriptor = nullptr;
      SetShadowGpuSkinStorageDescriptors(
          descriptors, paletteDesc.buffer, draw, gpuSkinDirect);

      if (collectCsmDescriptorReuseCounters)
        ++csmDescriptorRequestCount;

      bool pushOnlyDescriptorReuse = false;
      if (gpuSkinDirect) {
        if (collectCsmDescriptorReuseCounters)
          ++csmDescriptorDirectBypassCount;
        if (csmDescriptorCacheValid) {
          csmDescriptorCacheValid = false;
          if (collectCsmDescriptorReuseCounters)
            ++csmDescriptorInvalidationCount;
        }
        ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
                           prep.pipeline.layout, descriptors.size(),
                           descriptors.data(), sizeof(pc), &pc);
        if (collectCsmDescriptorReuseCounters)
          ++csmDescriptorFullDrawBindCount;
      } else if (csmDescriptorReuseEnabled) {
        const War3CsmDescriptorSignature descriptorKey =
            War3CsmDescriptorKey(prep.pipeline.layout, descriptors);
        if (!csmDescriptorCacheValid) {
          if (collectCsmDescriptorReuseCounters)
            ++csmDescriptorColdMissCount;
        } else if (csmDescriptorCacheKey.layout != descriptorKey.layout) {
          if (collectCsmDescriptorReuseCounters) {
            ++csmDescriptorLayoutMissCount;
            ++csmDescriptorInvalidationCount;
          }
        } else if (!War3CsmDescriptorBufferKeysEqual(
                       csmDescriptorCacheKey, descriptorKey)) {
          if (collectCsmDescriptorReuseCounters) {
            ++csmDescriptorBufferMissCount;
            ++csmDescriptorInvalidationCount;
          }
        } else if (!War3CsmDescriptorAlphaKeysEqual(
                       csmDescriptorCacheKey, descriptorKey)) {
          if (collectCsmDescriptorReuseCounters) {
            ++csmDescriptorAlphaMissCount;
            ++csmDescriptorInvalidationCount;
          }
        } else {
          pushOnlyDescriptorReuse = true;
          if (csmDescriptorReuseVerifierEnabled &&
              !War3CsmDescriptorWritesEquivalent(
                  csmDescriptorCacheWrites, descriptors)) {
            pushOnlyDescriptorReuse = false;
            ++csmDescriptorVerifierMismatchCount;
            if (War3CsmDescriptorReuseVerifierAssertEnabled())
              std::abort();
          }
        }

        if (pushOnlyDescriptorReuse) {
          ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
                             prep.pipeline.layout, 0u, nullptr, sizeof(pc),
                             &pc);
          if (collectCsmDescriptorReuseCounters)
            ++csmDescriptorPushOnlyReuseCount;
        } else {
          ctx->bindResources(DxvkCmdBuffer::ExecBuffer,
                             prep.pipeline.layout, descriptors.size(),
                             descriptors.data(), sizeof(pc), &pc);
          if (collectCsmDescriptorReuseCounters)
            ++csmDescriptorFullDrawBindCount;
          csmDescriptorCacheKey = descriptorKey;
          csmDescriptorCacheWrites = descriptors;
          csmDescriptorCacheValid = true;
        }
      } else {
        ctx->bindResources(DxvkCmdBuffer::ExecBuffer, prep.pipeline.layout,
                           descriptors.size(), descriptors.data(), sizeof(pc),
                           &pc);
        if (collectCsmDescriptorReuseCounters)
          ++csmDescriptorFullDrawBindCount;
      }

      // All cascades are recorded into the same command list. Keep each
      // caster's backing storage alive on its first actual draw only; repeated
      // Rc tracking in later cascades merely appended duplicate references to
      // the command-list object tracker.
      if (!prep.lifetimeResourcesTracked) {
        if (draw.positionStorage.ptr() != nullptr)
          ctx->track(draw.positionStorage);
        if (draw.indexStorage.ptr() != nullptr &&
            draw.indexStorage.ptr() != draw.positionStorage.ptr())
          ctx->track(draw.indexStorage);
        if (draw.blendStorage.ptr() != nullptr)
          ctx->track(draw.blendStorage);
        if (prep.effectiveAlphaTest && draw.uvBinding != 0u &&
            draw.uvStorage.ptr() != nullptr &&
            draw.uvStorage.ptr() != draw.positionStorage.ptr() &&
            draw.uvStorage.ptr() != draw.blendStorage.ptr())
          ctx->track(draw.uvStorage);
        if (gpuSkinDirect) {
          ctx->track(draw.gpuSkinInput.staticSource.buffer());
          if (draw.gpuSkinInput.palette.buffer().ptr() !=
              draw.gpuSkinInput.staticSource.buffer().ptr())
            ctx->track(draw.gpuSkinInput.palette.buffer());
        }
        prep.lifetimeResourcesTracked = true;
      }

      // Bind vertex buffers（尽量去重）
      VkBuffer vb0 = draw.positionInfo.buffer;
      VkDeviceSize vb0Off = draw.positionInfo.offset;
      VkDeviceSize vb0Size = draw.positionInfo.size;
      VkDeviceSize vb0Stride = draw.positionStride;

      uint32_t vbCount = 1;
      VkBuffer vb1 = VK_NULL_HANDLE;
      VkDeviceSize vb1Off = 0;
      VkDeviceSize vb1Size = 0;
      VkDeviceSize vb1Stride = 0;

      if (draw.blendBinding == 1) {
        vbCount = 2;
        vb1 = draw.blendInfo.buffer;
        vb1Off = draw.blendInfo.offset;
        vb1Size = draw.blendInfo.size;
        vb1Stride = draw.blendStride;
      } else if (prep.effectiveAlphaTest && draw.uvBinding == 1u) {
        vbCount = 2;
        vb1 = draw.uvInfo.buffer;
        vb1Off = draw.uvInfo.offset;
        vb1Size = draw.uvInfo.size;
        vb1Stride = draw.uvStride;
      }

      const bool vbDirty =
          boundVbCount != vbCount || boundVb0 != vb0 ||
          boundVb0Offset != vb0Off || boundVb0Size != vb0Size ||
          boundVb0Stride != vb0Stride || boundVb1 != vb1 ||
          boundVb1Offset != vb1Off || boundVb1Size != vb1Size ||
          boundVb1Stride != vb1Stride;

      if (vbDirty) {
        VkBuffer vbs[2] = {vb0, vb1};
        VkDeviceSize offsets[2] = {vb0Off, vb1Off};
        VkDeviceSize sizes[2] = {vb0Size, vb1Size};
        VkDeviceSize strides[2] = {vb0Stride, vb1Stride};
        ctx->cmdBindVertexBuffers(0, vbCount, vbs, offsets, sizes, strides);

        boundVbCount = vbCount;
        boundVb0 = vb0;
        boundVb0Offset = vb0Off;
        boundVb0Size = vb0Size;
        boundVb0Stride = vb0Stride;
        boundVb1 = vb1;
        boundVb1Offset = vb1Off;
        boundVb1Size = vb1Size;
        boundVb1Stride = vb1Stride;
      }

      if (prep.effectiveAlphaTest && draw.uvBinding == 2u) {
        const bool uvDirty =
            boundVb2 != draw.uvInfo.buffer ||
            boundVb2Offset != draw.uvInfo.offset ||
            boundVb2Size != draw.uvInfo.size ||
            boundVb2Stride != draw.uvStride;
        if (uvDirty) {
          const VkBuffer uvBuffer = draw.uvInfo.buffer;
          const VkDeviceSize uvOffset = draw.uvInfo.offset;
          const VkDeviceSize uvSize = draw.uvInfo.size;
          const VkDeviceSize uvStride = draw.uvStride;
          ctx->cmdBindVertexBuffers(2u, 1u, &uvBuffer, &uvOffset, &uvSize,
                                    &uvStride);
          boundVb2 = uvBuffer;
          boundVb2Offset = uvOffset;
          boundVb2Size = uvSize;
          boundVb2Stride = uvStride;
        }
      }

      if (draw.indexed) {
        const bool ibDirty = boundIb != draw.indexInfo.buffer ||
                             boundIbOffset != draw.indexInfo.offset ||
                             boundIbSize != draw.indexInfo.size ||
                             boundIbType != draw.indexType;

        if (ibDirty) {
          ctx->cmdBindIndexBuffer2(draw.indexInfo.buffer, draw.indexInfo.offset,
                                   draw.indexInfo.size, draw.indexType);
          boundIb = draw.indexInfo.buffer;
          boundIbOffset = draw.indexInfo.offset;
          boundIbSize = draw.indexInfo.size;
          boundIbType = draw.indexType;
        }

        ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
                            draw.vertexOffset, 0);
        cascadeTriangles += uint64_t(draw.indexCount / 3u);
      } else {
        ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);
        cascadeTriangles += uint64_t(draw.vertexCount / 3u);
      }

      if (gpuSkinDirect) {
        // 每个 direct draw 后立即恢复有效 storage fallback，并清除私有 flag。
        // 这样即使后续复用兼容 layout，也不能继承上一 draw 的输入租约。
        auto clearedDescriptors = descriptors;
        SetShadowGpuSkinStorageDescriptors(
            clearedDescriptors, paletteDesc.buffer, draw, false);
        auto clearedPc = pc;
        clearedPc.flags &= ~(kShadowCasterFlagGpuSkinDirectInput |
                             kShadowCasterFlagGpuSkinNoFallback |
                             kShadowCasterGpuSkinMetadataMask);
        clearedPc.blendCount = draw.vertexBlendCount;
        clearedPc.padding[1] = 0u;
        ctx->bindResources(DxvkCmdBuffer::ExecBuffer, prep.pipeline.layout,
                           clearedDescriptors.size(),
                           clearedDescriptors.data(), sizeof(clearedPc),
                           &clearedPc);
        if (collectCsmDescriptorReuseCounters)
          ++csmDescriptorDirectClearBindCount;
        csmDescriptorCacheValid = false;
        ++m_gpuSkinVsShadowDirectDrawsSubmitted;
        ++m_gpuSkinVsShadowDirectBindingsCleared;
        ++m_gpuSkinVsShadowReplayDirectional;
      }

      drawnCasters++;
      cascadeDrawn++;
      war3::render::NoteShadowStageCascadeDrawn(draw.stage, c);
      if (draw.category == War3RenderState::StageCategory::Terrain) {
        if (draw.stage == 10)
          terrainDoodadDrawnPerCascade[c]++;
        else if (draw.stage == 1)
          terrainS1DrawnPerCascade[c]++;
      }
      if (draw.vertexBlendEnabled || draw.gpuSkinInput.valid)
        skinnedDrawnPerCascade[c]++;
    }

    drawnPerCascade[c] = cascadeDrawn;
    if (!m_volumeSunRenderPathActive) {
      war3::tools::SetGpuFlightCsmCascadeWork(
          c, cascadeDrawn, cascadeTriangles);
    }
    ctx->cmdEndRendering();
  }

  shadowMapPhaseTiming.enter(static_cast<size_t>(
      War3DirectionalShadowMapRawPhase::TerrainMask));
  war3::tools::SetGpuFlightBreadcrumb(
      war3::tools::GpuFlightBreadcrumb::CsmTerrainMask);
  // S1 terrain is kept in the CSM depth map only as a blocker so flying/unit
  // shadows stop at the first cliff/ground surface. A separate tiny mask pass
  // records where that nearest blocker is terrain; the receiver then ignores
  // terrain-as-caster pixels so the terrain does not visibly project onto itself.
  if (terrainCasterMaskEnabled) {
    // The mask pass opens the same depth attachment with LOAD and performs a
    // read-only depth test. Dynamic-rendering instances do not create an
    // implicit attachment dependency, so make the main CSM writes available
    // before any early/late depth reads in the second instance.
    VkImageMemoryBarrier2 mainDepthToTerrainMask = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    mainDepthToTerrainMask.srcStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    mainDepthToTerrainMask.srcAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mainDepthToTerrainMask.dstStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    mainDepthToTerrainMask.dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    mainDepthToTerrainMask.oldLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    mainDepthToTerrainMask.newLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    mainDepthToTerrainMask.image = m_shadowMap->handle();
    mainDepthToTerrainMask.subresourceRange = {
        VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, cascadeCount};

    VkDependencyInfo mainDepthToTerrainMaskDependency = {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    mainDepthToTerrainMaskDependency.imageMemoryBarrierCount = 1u;
    mainDepthToTerrainMaskDependency.pImageMemoryBarriers =
        &mainDepthToTerrainMask;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer,
                            &mainDepthToTerrainMaskDependency);

    uint32_t terrainMaskDraws = 0u;
    for (uint32_t c = 0; c < cascadeCount; c++) {
      if (!m_shadowMapLayerViews[c] || !m_shadowCasterMaskLayerViews[c])
        continue;

      const float alphaRefBiasCascade =
          war3::render::ShadowAlphaRefBiasForCascade(
              alphaShadowFarAlphaRefBias, c, cascadeCount);

      VkClearValue maskClearValue = {};
      maskClearValue.color.float32[0] = 0.0f;
      maskClearValue.color.float32[1] = 0.0f;
      maskClearValue.color.float32[2] = 0.0f;
      maskClearValue.color.float32[3] = 0.0f;

      VkRenderingAttachmentInfo maskAttachment = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      maskAttachment.imageView = m_shadowCasterMaskLayerViews[c]->handle();
      maskAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      maskAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      maskAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      maskAttachment.clearValue = maskClearValue;

      VkRenderingAttachmentInfo depthAttachment = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      depthAttachment.imageView = m_shadowMapLayerViews[c]->handle();
      depthAttachment.imageLayout =
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      renderInfo.renderArea.offset = {0u, 0u};
      renderInfo.renderArea.extent = {extent.width, extent.height};
      renderInfo.layerCount = 1u;
      renderInfo.colorAttachmentCount = 1u;
      renderInfo.pColorAttachments = &maskAttachment;
      renderInfo.pDepthAttachment = &depthAttachment;

      ctx->cmdBeginRendering(&renderInfo);

      VkViewport viewport = {};
      viewport.x = 0.0f;
      viewport.y = float(extent.height);
      viewport.width = float(extent.width);
      viewport.height = -float(extent.height);
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;

      VkRect2D scissor = {};
      scissor.offset = {0, 0};
      scissor.extent = {extent.width, extent.height};

      ctx->cmdSetViewport(1, &viewport);
      ctx->cmdSetScissor(1, &scissor);

      VkPipeline boundPipeline = VK_NULL_HANDLE;
      VkBuffer boundVb0 = VK_NULL_HANDLE;
      VkDeviceSize boundVb0Offset = 0;
      VkDeviceSize boundVb0Size = 0;
      VkDeviceSize boundVb0Stride = 0;

      VkBuffer boundIb = VK_NULL_HANDLE;
      VkDeviceSize boundIbOffset = 0;
      VkDeviceSize boundIbSize = 0;
      VkIndexType boundIbType = VK_INDEX_TYPE_UINT16;

      for (uint32_t idx : terrainMaskDrawIndices) {
        const auto &draw = *replayDraws[idx];
        const auto &prep = prepared[idx];
        if (!prep.valid)
          continue;
        if (!cascadeVisible(idx, c))
          continue;

        ShadowCasterPipelineKey key = {};
        key.positionFormat = draw.positionFormat;
        key.positionStride = draw.positionStride;
        key.positionOffset = draw.positionOffset;
        key.topology = draw.topology;
        key.blendWeightFormat = VK_FORMAT_UNDEFINED;
        key.blendWeightOffset = 0;
        key.blendIndexFormat = VK_FORMAT_UNDEFINED;
        key.blendIndexOffset = 0;
        key.blendBinding = draw.blendBinding;
        key.blendStride = draw.blendStride;
        key.casterMaskEnabled = true;

        if (prep.effectiveAlphaTest) {
          key.alphaTestEnabled = true;
          key.uvFormat = draw.uvFormat;
          key.uvOffset = draw.uvOffset;
          key.uvStride = draw.uvStride;
          key.uvBinding = draw.uvBinding;
        }

        ShadowCasterPipeline pipeline = getShadowCasterPipeline(key);
        if (pipeline.pipeline == VK_NULL_HANDLE)
          continue;

        ShadowCasterPushConstants pc = {};
        pc.blendCount = draw.vertexBlendCount;
        pc.flags = kShadowCasterFlagStage1Terrain;
        pc.mvp = m_csmData.cascades[c].lightViewProj;
        pc.paletteOffset = objectBase + idx;
        if (war3::internal::kShadowS1TerrainCasterDepthBiasEnabled) {
          pc.terrainDepthBias =
              war3::internal::kShadowS1TerrainCasterDepthBiasNdc;
        }

        if (prep.effectiveAlphaTest && draw.diffuseTexture) {
          pc.flags |= kShadowCasterFlagAlphaTest;
          if (alphaShadowHashed)
            pc.flags |= kShadowCasterFlagHashAlpha;
          pc.alphaRef =
              std::clamp(draw.alphaRef + alphaRefBiasCascade, 0.0f, 1.0f);
          pc.samplerIndex = draw.diffuseSamplerIndex;
        }

        if (pipeline.pipeline != boundPipeline) {
          trackCasterPipeline(pipeline);
          ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                               VK_PIPELINE_BIND_POINT_GRAPHICS,
                               pipeline.pipeline);
          boundPipeline = pipeline.pipeline;
        }

        std::array<DxvkDescriptorWrite, 5> descriptors = {};
        descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptors[0].buffer = paletteDesc.buffer;
        if (prep.effectiveAlphaTest && draw.diffuseTexture) {
          descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          descriptors[1].descriptor = &draw.textureDescriptor;
          ctx->track(draw.diffuseTexture->image(), DxvkAccess::Read);
        } else {
          descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          descriptors[1].descriptor = nullptr;
        }
        descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[2].descriptor = nullptr;
        SetShadowGpuSkinStorageDescriptors(
            descriptors, paletteDesc.buffer, draw, false);

        ctx->bindResources(DxvkCmdBuffer::ExecBuffer, pipeline.layout,
                           descriptors.size(), descriptors.data(), sizeof(pc),
                           &pc);

        if (draw.positionStorage.ptr() != nullptr)
          ctx->track(draw.positionStorage);
        if (draw.indexStorage.ptr() != nullptr &&
            draw.indexStorage.ptr() != draw.positionStorage.ptr())
          ctx->track(draw.indexStorage);
        if (draw.blendStorage.ptr() != nullptr &&
            draw.blendStorage.ptr() != draw.positionStorage.ptr())
          ctx->track(draw.blendStorage);
        if (prep.effectiveAlphaTest && draw.uvBinding != 0u &&
            draw.uvStorage.ptr() != nullptr &&
            draw.uvStorage.ptr() != draw.positionStorage.ptr() &&
            draw.uvStorage.ptr() != draw.blendStorage.ptr())
          ctx->track(draw.uvStorage);

        VkBuffer vb0 = draw.positionInfo.buffer;
        VkDeviceSize vb0Off = draw.positionInfo.offset;
        VkDeviceSize vb0Size = draw.positionInfo.size;
        VkDeviceSize vb0Stride = draw.positionStride;
        const bool vbDirty =
            boundVb0 != vb0 || boundVb0Offset != vb0Off ||
            boundVb0Size != vb0Size || boundVb0Stride != vb0Stride;
        if (vbDirty) {
          ctx->cmdBindVertexBuffers(0, 1, &vb0, &vb0Off, &vb0Size,
                                    &vb0Stride);
          boundVb0 = vb0;
          boundVb0Offset = vb0Off;
          boundVb0Size = vb0Size;
          boundVb0Stride = vb0Stride;
        }
        if (prep.effectiveAlphaTest && draw.uvBinding != 0u) {
          const VkBuffer uvBuffer = draw.uvInfo.buffer;
          const VkDeviceSize uvOffset = draw.uvInfo.offset;
          const VkDeviceSize uvSize = draw.uvInfo.size;
          const VkDeviceSize uvStride = draw.uvStride;
          ctx->cmdBindVertexBuffers(draw.uvBinding, 1u, &uvBuffer, &uvOffset,
                                    &uvSize, &uvStride);
        }

        if (draw.indexed) {
          const bool ibDirty = boundIb != draw.indexInfo.buffer ||
                               boundIbOffset != draw.indexInfo.offset ||
                               boundIbSize != draw.indexInfo.size ||
                               boundIbType != draw.indexType;
          if (ibDirty) {
            ctx->cmdBindIndexBuffer2(draw.indexInfo.buffer,
                                     draw.indexInfo.offset,
                                     draw.indexInfo.size, draw.indexType);
            boundIb = draw.indexInfo.buffer;
            boundIbOffset = draw.indexInfo.offset;
            boundIbSize = draw.indexInfo.size;
            boundIbType = draw.indexType;
          }

          ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
                              draw.vertexOffset, 0);
        } else {
          ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);
        }
        terrainMaskDraws++;
      }

      ctx->cmdEndRendering();
    }

    static uint32_t s_maskLogCounter = 0u;
    if (war3dbg::RenderLogEnabled() &&
        (s_maskLogCounter++ < 5u || (s_maskLogCounter % 300u) == 0u)) {
      WAR3_RENDER_LOG("DXVK ShadowMap: S1 terrain mask draws=%u\n",
                      terrainMaskDraws);
    }
  }

  shadowMapPhaseTiming.enter(static_cast<size_t>(
      War3DirectionalShadowMapRawPhase::FinalizeAndReadTransition));
  if (casterCount > 0) {
    static uint32_t s_logDrawn = 0;
    const bool hasSkinnedCasters = skinnedCasterCount > 0u;
    // Phase 7.2: 对账计数器（每帧填充）
    reconciliation.shadowMapDrawnCasters = drawnCasters;
    reconciliation.skinnedCasterCount = skinnedCasterCount;
    reconciliation.skinnedPreparedCount = skinnedPreparedCount;
    reconciliation.skinnedInvalidBufferCount = skinnedInvalidBufferCount;
    reconciliation.skinnedInvalidPipelineCount = skinnedInvalidPipelineCount;
    reconciliation.shadowMapPreparedDrawCount = preparedDrawCount;
    reconciliation.shadowMapAlphaTestPreparedCount = alphaTestPreparedCount;
    reconciliation.shadowMapAlphaPromotedPreparedCount =
        alphaPromotedPreparedCount;
    reconciliation.shadowMapDynamicPreparedCount = dynamicPreparedCount;
    reconciliation.shadowMapStaticPreparedCount = staticPreparedCount;
    reconciliation.shadowMapOtherPreparedCount = otherPreparedCount;
    reconciliation.shadowMapTerrainDoodadPreparedCount =
        terrainDoodadPreparedCount;
    reconciliation.shadowMapTerrainS1PreparedCount =
        terrainS1PreparedCount;
    uint32_t totalCulled = 0u;
    uint32_t totalSkinnedDrawn = 0u;
    for (uint32_t c = 0; c < cascadeCount; ++c) {
      totalCulled += culledPerCascade[c];
      totalSkinnedDrawn += skinnedDrawnPerCascade[c];
    }
    reconciliation.cascadeCulledCount = totalCulled;
    reconciliation.skinnedDrawnCount = totalSkinnedDrawn;
    reconciliation.shadowMapCascade0DrawnCount = drawnPerCascade[0];
    reconciliation.shadowMapCascade1DrawnCount = drawnPerCascade[1];
    reconciliation.shadowMapCascade2DrawnCount = drawnPerCascade[2];
    reconciliation.shadowMapCascade3DrawnCount = drawnPerCascade[3];
    reconciliation.shadowMapCascade0CulledCount = culledPerCascade[0];
    reconciliation.shadowMapCascade1CulledCount = culledPerCascade[1];
    reconciliation.shadowMapCascade2CulledCount = culledPerCascade[2];
    reconciliation.shadowMapCascade3CulledCount = culledPerCascade[3];
    reconciliation.shadowMapTerrainDoodadCascade0DrawnCount =
        terrainDoodadDrawnPerCascade[0];
    reconciliation.shadowMapTerrainDoodadCascade1DrawnCount =
        terrainDoodadDrawnPerCascade[1];
    reconciliation.shadowMapTerrainDoodadCascade2DrawnCount =
        terrainDoodadDrawnPerCascade[2];
    reconciliation.shadowMapTerrainDoodadCascade3DrawnCount =
        terrainDoodadDrawnPerCascade[3];
    reconciliation.shadowMapTerrainS1Cascade0DrawnCount =
        terrainS1DrawnPerCascade[0];
    reconciliation.shadowMapTerrainS1Cascade1DrawnCount =
        terrainS1DrawnPerCascade[1];
    reconciliation.shadowMapTerrainS1Cascade2DrawnCount =
        terrainS1DrawnPerCascade[2];
    reconciliation.shadowMapTerrainS1Cascade3DrawnCount =
        terrainS1DrawnPerCascade[3];
    // 前5帧强制日志，验证 renderShadowMap 实际画了多少 caster
    if (war3dbg::RenderLogEnabled()) {
      const uint32_t logIndex = s_logDrawn++;
      if (logIndex < 5u ||
          (hasSkinnedCasters &&
           (logIndex < 12u || (logIndex % 120u) == 0u)) ||
          (!hasSkinnedCasters && (logIndex % 300u) == 0u)) {
        WAR3_RENDER_LOG(
            "DXVK ShadowMap: DrawCalls=%u Casters=%u | C0=%u(-%u) C1=%u(-%u) "
            "C2=%u(-%u) C3=%u(-%u) | Skinned=%u prep=%u badBuf=%u badPipe=%u "
            "draw=%u/%u/%u/%u cull=%u/%u/%u/%u\n",
            drawnCasters, casterCount, drawnPerCascade[0], culledPerCascade[0],
            drawnPerCascade[1], culledPerCascade[1], drawnPerCascade[2],
            culledPerCascade[2], drawnPerCascade[3], culledPerCascade[3],
            skinnedCasterCount, skinnedPreparedCount,
            skinnedInvalidBufferCount, skinnedInvalidPipelineCount,
            skinnedDrawnPerCascade[0], skinnedDrawnPerCascade[1],
            skinnedDrawnPerCascade[2], skinnedDrawnPerCascade[3],
            skinnedCulledPerCascade[0], skinnedCulledPerCascade[1],
            skinnedCulledPerCascade[2], skinnedCulledPerCascade[3]);
      }
    }
  }

  // 3) Transition shadow map back to read-only for sampling in receiver shader
  {
    const VkImageSubresourceRange subresources = {
        VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, cascadeCount};
    const auto transition = shadowMapLayout.plan(
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT);
    VkImageMemoryBarrier2 toRead =
        war3::render::MakeWar3OwnedImageBarrier(
            transition, m_shadowMap->handle(), subresources);

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &toRead;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
    war3::render::CommitWar3OwnedImageLayout(
        shadowMapLayout, transition, *m_shadowMap, subresources);
  }
  if (terrainCasterMaskEnabled) {
    const VkImageSubresourceRange subresources = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, cascadeCount};
    const auto transition = m_shadowCasterMaskLayout.plan(
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT);
    VkImageMemoryBarrier2 maskToRead =
        war3::render::MakeWar3OwnedImageBarrier(
            transition, m_shadowCasterMask->handle(), subresources);

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &maskToRead;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
    war3::render::CommitWar3OwnedImageLayout(
        m_shadowCasterMaskLayout, transition, *m_shadowCasterMask,
        subresources);
  }

  ctx->track(m_shadowMap, DxvkAccess::Write);
  if (terrainCasterMaskEnabled)
    ctx->track(m_shadowCasterMask, DxvkAccess::Write);
  if (shadowMapPhaseSampleWeight != 0u) {
    auto& perf = war3::War3PerfMonitor::instance();
    const auto publishDescriptorCounter =
        [&](const char* name, uint32_t count) {
          if (count == 0u)
            return;
          perf.addCpuSample(
              name, 0.0,
              "FramePipeline/DirectionalShadowMapPhaseSample/CascadeRecord",
              count * shadowMapPhaseSampleWeight);
        };
    publishDescriptorCounter("DescriptorRequest",
                             csmDescriptorRequestCount);
    publishDescriptorCounter("DescriptorFullDrawBind",
                             csmDescriptorFullDrawBindCount);
    publishDescriptorCounter("DescriptorPushOnlyReuse",
                             csmDescriptorPushOnlyReuseCount);
    publishDescriptorCounter("DescriptorColdMiss",
                             csmDescriptorColdMissCount);
    publishDescriptorCounter("DescriptorLayoutMiss",
                             csmDescriptorLayoutMissCount);
    publishDescriptorCounter("DescriptorBufferMiss",
                             csmDescriptorBufferMissCount);
    publishDescriptorCounter("DescriptorAlphaMiss",
                             csmDescriptorAlphaMissCount);
    publishDescriptorCounter("DescriptorInvalidation",
                             csmDescriptorInvalidationCount);
    publishDescriptorCounter("DescriptorDirectBypass",
                             csmDescriptorDirectBypassCount);
    publishDescriptorCounter("DescriptorDirectClearBind",
                             csmDescriptorDirectClearBindCount);
    publishDescriptorCounter("DescriptorVerifierMismatch",
                             csmDescriptorVerifierMismatchCount);
  }
  reconciliation.shadowMapExecutedThisFrame = 1u;
  reconciliation.shadowMapRenderSerial = ++m_shadowMapRenderSerial;
  if (!m_volumeSunRenderPathActive) {
    g_shadowReplayDiagnostics.drawnCasterCount.store(
        preparedDrawCount, std::memory_order_release);
  }
  return true;
}

namespace {
// A2 Worker_Prepare：默认开启，可用 DXVK_WAR3_WORKER_PREPARE=0 回退同步路径。
bool War3WorkerPrepareEnabled() {
  static const bool enabled = []() -> bool {
    if (const char *env = std::getenv("DXVK_WAR3_WORKER_PREPARE"))
      return env[0] != '0';
    return true;
  }();
  return enabled;
}

enum class War3PointShadowPersistentMode : uint32_t {
  Off = 0u,
  Observe = 1u,
  Consume = 2u,
};

War3PointShadowPersistentMode PointShadowPersistentMode() {
  if constexpr (war3::internal::kReleaseFreezeExperimentalShadowRoutes)
    return War3PointShadowPersistentMode::Off;
  static const War3PointShadowPersistentMode mode = [] {
    const char *env =
        std::getenv("DXVK_WAR3_POINT_SHADOW_PERSISTENT_PREPARE_MODE");
    if (!env || env[0] == '\0' || env[0] == '0')
      return War3PointShadowPersistentMode::Off;
    if (std::strcmp(env, "2") == 0 || std::strcmp(env, "consume") == 0 ||
        std::strcmp(env, "Consume") == 0)
      return War3PointShadowPersistentMode::Consume;
    if (std::strcmp(env, "1") == 0 || std::strcmp(env, "observe") == 0 ||
        std::strcmp(env, "Observe") == 0)
      return War3PointShadowPersistentMode::Observe;
    return War3PointShadowPersistentMode::Off;
  }();
  return mode;
}

struct PointShadowSealBuilder {
  uint64_t value = 0xcbf29ce484222325ull;

  void mixU64(uint64_t token) noexcept {
    value ^= token + 0x9e3779b97f4a7c15ull + (value << 6u) +
        (value >> 2u);
  }

  void mixF32(float token) noexcept {
    uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(token));
    std::memcpy(&bits, &token, sizeof(bits));
    mixU64(bits);
  }

  uint64_t finish() const noexcept {
    return value != 0u ? value : 1u;
  }
};

template <typename Handle>
uint64_t PointShadowHandleIdentity(Handle handle) noexcept {
  uint64_t result = 0u;
  static_assert(sizeof(handle) <= sizeof(result));
  std::memcpy(&result, &handle, sizeof(handle));
  return result;
}

bool PointShadowF32Exact(float lhs, float rhs) noexcept {
  uint32_t lhsBits = 0u;
  uint32_t rhsBits = 0u;
  std::memcpy(&lhsBits, &lhs, sizeof(lhsBits));
  std::memcpy(&rhsBits, &rhs, sizeof(rhsBits));
  return lhsBits == rhsBits;
}

war3::render::War3PointShadowCpuMatrix4 FreezePointShadowMatrix(
    const Matrix4 &matrix) noexcept {
  war3::render::War3PointShadowCpuMatrix4 result = {};
  for (uint32_t lane = 0u; lane < 4u; ++lane) {
    result.vectors[lane][0] = matrix[lane].x;
    result.vectors[lane][1] = matrix[lane].y;
    result.vectors[lane][2] = matrix[lane].z;
    result.vectors[lane][3] = matrix[lane].w;
  }
  return result;
}

Matrix4 ThawPointShadowMatrix(
    const war3::render::War3PointShadowCpuMatrix4 &matrix) noexcept {
  Matrix4 result;
  for (uint32_t lane = 0u; lane < 4u; ++lane) {
    result[lane] = Vector4(matrix.vectors[lane][0], matrix.vectors[lane][1],
                           matrix.vectors[lane][2], matrix.vectors[lane][3]);
  }
  return result;
}

war3::render::War3PointShadowCpuCaster FreezePointShadowCaster(
    const War3ShadowCasterDraw &draw) noexcept {
  using Caster = war3::render::War3PointShadowCpuCaster;
  Caster result = {};
  result.boundsCenter = {draw.boundsCenter.x, draw.boundsCenter.y,
                         draw.boundsCenter.z, draw.boundsCenter.w};
  result.boundsRadius = draw.boundsRadius;
  result.worldMatrix = FreezePointShadowMatrix(draw.worldMatrix);
  result.vertexCount = draw.vertexCount;
  result.indexCount = draw.indexCount;
  result.positionFormat = static_cast<uint32_t>(draw.positionFormat);
  result.positionStride = draw.positionStride;
  result.positionOffset = draw.positionOffset;
  result.indexType = static_cast<uint32_t>(draw.indexType);
  result.vertexBlendCount = draw.vertexBlendCount;
  result.paletteIndex = draw.paletteIndex;
  result.blendWeightFormat = static_cast<uint32_t>(draw.blendWeightFormat);
  result.blendWeightOffset = draw.blendWeightOffset;
  result.blendIndexFormat = static_cast<uint32_t>(draw.blendIndexFormat);
  result.blendIndexOffset = draw.blendIndexOffset;
  result.blendBinding = draw.blendBinding;
  result.blendStride = draw.blendStride;
  result.topology = static_cast<uint32_t>(draw.topology);
  result.firstIndex = draw.firstIndex;
  result.vertexOffset = draw.vertexOffset;
  result.firstVertex = draw.firstVertex;
  result.minVertexIndex = draw.minVertexIndex;
  result.numVertices = draw.numVertices;
  result.positionStorageIdentity =
      reinterpret_cast<uintptr_t>(draw.positionStorage.ptr());
  result.positionBufferIdentity =
      PointShadowHandleIdentity(draw.positionInfo.buffer);
  result.positionBufferOffset = draw.positionInfo.offset;
  result.positionBufferSize = draw.positionInfo.size;
  result.indexStorageIdentity =
      reinterpret_cast<uintptr_t>(draw.indexStorage.ptr());
  result.indexBufferIdentity = PointShadowHandleIdentity(draw.indexInfo.buffer);
  result.indexBufferOffset = draw.indexInfo.offset;
  result.indexBufferSize = draw.indexInfo.size;
  result.blendStorageIdentity =
      reinterpret_cast<uintptr_t>(draw.blendStorage.ptr());
  result.blendBufferIdentity = PointShadowHandleIdentity(draw.blendInfo.buffer);
  result.blendBufferOffset = draw.blendInfo.offset;
  result.blendBufferSize = draw.blendInfo.size;
  result.alphaRef = draw.alphaRef;
  result.uvFormat = static_cast<uint32_t>(draw.uvFormat);
  result.uvStride = draw.uvStride;
  result.uvOffset = draw.uvOffset;
  result.uvBinding = draw.uvBinding;
  result.uvStorageIdentity =
      reinterpret_cast<uintptr_t>(draw.uvStorage.ptr());
  result.uvBufferIdentity = PointShadowHandleIdentity(draw.uvInfo.buffer);
  result.uvBufferOffset = draw.uvInfo.offset;
  result.uvBufferSize = draw.uvInfo.size;
  result.diffuseTextureIdentity =
      reinterpret_cast<uintptr_t>(draw.diffuseTexture.ptr());
  result.diffuseSamplerIdentity =
      reinterpret_cast<uintptr_t>(draw.diffuseSampler.ptr());
  result.diffuseSamplerIndex = draw.diffuseSamplerIndex;
  result.indexed = draw.indexed;
  result.vertexBlendEnabled = draw.vertexBlendEnabled;
  result.vertexBlendIndexed = draw.vertexBlendIndexed;
  result.alphaTestEnabled = draw.alphaTestEnabled;
  result.alphaBlendEnabled = draw.alphaBlendEnabled;
  // This declaration is minted only after every field above has been copied
  // from the sealed current-frame replay row by the renderer owner.
  result.frozenComplete = true;
  return result;
}

bool PointShadowMatrixExact(
    const war3::render::War3PointShadowCpuMatrix4 &lhs,
    const war3::render::War3PointShadowCpuMatrix4 &rhs) noexcept {
  for (uint32_t lane = 0u; lane < 4u; ++lane) {
    for (uint32_t component = 0u; component < 4u; ++component) {
      if (!PointShadowF32Exact(lhs.vectors[lane][component],
                               rhs.vectors[lane][component]))
        return false;
    }
  }
  return true;
}

bool PointShadowMatrixFinite(
    const war3::render::War3PointShadowCpuMatrix4 &matrix) noexcept {
  for (uint32_t lane = 0u; lane < 4u; ++lane) {
    for (uint32_t component = 0u; component < 4u; ++component) {
      if (!std::isfinite(matrix.vectors[lane][component]))
        return false;
    }
  }
  return true;
}

bool PointShadowCasterExact(
    const war3::render::War3PointShadowCpuCaster &lhs,
    const war3::render::War3PointShadowCpuCaster &rhs) noexcept {
  const bool scalarExact =
      PointShadowF32Exact(lhs.boundsCenter.x, rhs.boundsCenter.x) &&
      PointShadowF32Exact(lhs.boundsCenter.y, rhs.boundsCenter.y) &&
      PointShadowF32Exact(lhs.boundsCenter.z, rhs.boundsCenter.z) &&
      PointShadowF32Exact(lhs.boundsCenter.w, rhs.boundsCenter.w) &&
      PointShadowF32Exact(lhs.boundsRadius, rhs.boundsRadius) &&
      PointShadowF32Exact(lhs.alphaRef, rhs.alphaRef);
  if (!scalarExact || !PointShadowMatrixExact(lhs.worldMatrix, rhs.worldMatrix))
    return false;
  return lhs.vertexCount == rhs.vertexCount &&
      lhs.indexCount == rhs.indexCount &&
      lhs.positionFormat == rhs.positionFormat &&
      lhs.positionStride == rhs.positionStride &&
      lhs.positionOffset == rhs.positionOffset &&
      lhs.indexType == rhs.indexType &&
      lhs.vertexBlendCount == rhs.vertexBlendCount &&
      lhs.paletteIndex == rhs.paletteIndex &&
      lhs.blendWeightFormat == rhs.blendWeightFormat &&
      lhs.blendWeightOffset == rhs.blendWeightOffset &&
      lhs.blendIndexFormat == rhs.blendIndexFormat &&
      lhs.blendIndexOffset == rhs.blendIndexOffset &&
      lhs.blendBinding == rhs.blendBinding &&
      lhs.blendStride == rhs.blendStride &&
      lhs.topology == rhs.topology && lhs.firstIndex == rhs.firstIndex &&
      lhs.vertexOffset == rhs.vertexOffset &&
      lhs.firstVertex == rhs.firstVertex &&
      lhs.minVertexIndex == rhs.minVertexIndex &&
      lhs.numVertices == rhs.numVertices &&
      lhs.positionStorageIdentity == rhs.positionStorageIdentity &&
      lhs.positionBufferIdentity == rhs.positionBufferIdentity &&
      lhs.positionBufferOffset == rhs.positionBufferOffset &&
      lhs.positionBufferSize == rhs.positionBufferSize &&
      lhs.indexStorageIdentity == rhs.indexStorageIdentity &&
      lhs.indexBufferIdentity == rhs.indexBufferIdentity &&
      lhs.indexBufferOffset == rhs.indexBufferOffset &&
      lhs.indexBufferSize == rhs.indexBufferSize &&
      lhs.blendStorageIdentity == rhs.blendStorageIdentity &&
      lhs.blendBufferIdentity == rhs.blendBufferIdentity &&
      lhs.blendBufferOffset == rhs.blendBufferOffset &&
      lhs.blendBufferSize == rhs.blendBufferSize &&
      lhs.uvFormat == rhs.uvFormat && lhs.uvStride == rhs.uvStride &&
      lhs.uvOffset == rhs.uvOffset && lhs.uvBinding == rhs.uvBinding &&
      lhs.uvStorageIdentity == rhs.uvStorageIdentity &&
      lhs.uvBufferIdentity == rhs.uvBufferIdentity &&
      lhs.uvBufferOffset == rhs.uvBufferOffset &&
      lhs.uvBufferSize == rhs.uvBufferSize &&
      lhs.diffuseTextureIdentity == rhs.diffuseTextureIdentity &&
      lhs.diffuseSamplerIdentity == rhs.diffuseSamplerIdentity &&
      lhs.diffuseSamplerIndex == rhs.diffuseSamplerIndex &&
      lhs.indexed == rhs.indexed &&
      lhs.vertexBlendEnabled == rhs.vertexBlendEnabled &&
      lhs.vertexBlendIndexed == rhs.vertexBlendIndexed &&
      lhs.alphaTestEnabled == rhs.alphaTestEnabled &&
      lhs.alphaBlendEnabled == rhs.alphaBlendEnabled &&
      lhs.frozenComplete == rhs.frozenComplete;
}

void MixPointShadowCasterSeal(
    PointShadowSealBuilder &seal,
    const war3::render::War3PointShadowCpuCaster &caster) noexcept {
  // Do not hash object padding. Mix the exact fields through a second,
  // padding-free byte representation produced by the same explicit equality
  // contract. The full equality is still repeated before Consume; this digest
  // is only the early replay-generation seal.
  seal.mixF32(caster.boundsCenter.x);
  seal.mixF32(caster.boundsCenter.y);
  seal.mixF32(caster.boundsCenter.z);
  seal.mixF32(caster.boundsCenter.w);
  seal.mixF32(caster.boundsRadius);
  seal.mixU64(caster.vertexCount);
  seal.mixU64(caster.indexCount);
  seal.mixU64(caster.positionFormat);
  seal.mixU64(caster.positionStride);
  seal.mixU64(caster.positionOffset);
  seal.mixU64(caster.indexType);
  seal.mixU64(caster.vertexBlendCount);
  seal.mixU64(caster.paletteIndex);
  seal.mixU64(caster.blendWeightFormat);
  seal.mixU64(caster.blendWeightOffset);
  seal.mixU64(caster.blendIndexFormat);
  seal.mixU64(caster.blendIndexOffset);
  seal.mixU64(caster.blendBinding);
  seal.mixU64(caster.blendStride);
  seal.mixU64(caster.topology);
  seal.mixU64(caster.firstIndex);
  seal.mixU64(static_cast<uint32_t>(caster.vertexOffset));
  seal.mixU64(caster.firstVertex);
  seal.mixU64(caster.minVertexIndex);
  seal.mixU64(caster.numVertices);
  seal.mixU64(caster.positionStorageIdentity);
  seal.mixU64(caster.positionBufferIdentity);
  seal.mixU64(caster.positionBufferOffset);
  seal.mixU64(caster.positionBufferSize);
  seal.mixU64(caster.indexStorageIdentity);
  seal.mixU64(caster.indexBufferIdentity);
  seal.mixU64(caster.indexBufferOffset);
  seal.mixU64(caster.indexBufferSize);
  seal.mixU64(caster.blendStorageIdentity);
  seal.mixU64(caster.blendBufferIdentity);
  seal.mixU64(caster.blendBufferOffset);
  seal.mixU64(caster.blendBufferSize);
  seal.mixF32(caster.alphaRef);
  seal.mixU64(caster.uvFormat);
  seal.mixU64(caster.uvStride);
  seal.mixU64(caster.uvOffset);
  seal.mixU64(caster.uvBinding);
  seal.mixU64(caster.uvStorageIdentity);
  seal.mixU64(caster.uvBufferIdentity);
  seal.mixU64(caster.uvBufferOffset);
  seal.mixU64(caster.uvBufferSize);
  seal.mixU64(caster.diffuseTextureIdentity);
  seal.mixU64(caster.diffuseSamplerIdentity);
  seal.mixU64(caster.diffuseSamplerIndex);
  seal.mixU64(caster.indexed ? 1u : 0u);
  seal.mixU64(caster.vertexBlendEnabled ? 1u : 0u);
  seal.mixU64(caster.vertexBlendIndexed ? 1u : 0u);
  seal.mixU64(caster.alphaTestEnabled ? 1u : 0u);
  seal.mixU64(caster.alphaBlendEnabled ? 1u : 0u);
  seal.mixU64(caster.frozenComplete ? 1u : 0u);
  for (uint32_t lane = 0u; lane < 4u; ++lane) {
    for (uint32_t component = 0u; component < 4u; ++component)
      seal.mixF32(caster.worldMatrix.vectors[lane][component]);
  }
}

war3::render::War3PointShadowCpuPlanSettings FreezePointShadowSettings(
    const War3RenderSettings &settings) noexcept {
  war3::render::War3PointShadowCpuPlanSettings result = {};
  result.pointLightsEnabled = settings.shadows.pointLightsEnabled;
  result.pointShadowEnabled = settings.shadows.pointShadowEnabled;
  result.pointShadowFaceCulling = settings.shadows.pointShadowFaceCulling;
  result.pointShadowTemporalReuse = settings.shadows.pointShadowTemporalReuse;
  result.alphaShadowHashed = settings.shadows.alphaShadowHashed;
  result.pointShadowResolution = settings.shadows.pointShadowResolution;
  result.pointShadowMaxLights = settings.shadows.pointShadowMaxLights;
  result.pointShadowMaxFacesPerFrame =
      settings.shadows.pointShadowMaxFacesPerFrame;
  result.pointShadowMaxCastersPerFace =
      settings.shadows.pointShadowMaxCastersPerFace;
  result.pointShadowUpdatePeriod = settings.shadows.pointShadowUpdatePeriod;
  result.pointShadowCasterCullPadding =
      settings.shadows.pointShadowCasterCullPadding;
  return result;
}

bool PointShadowSettingsExact(
    const war3::render::War3PointShadowCpuPlanSettings &lhs,
    const war3::render::War3PointShadowCpuPlanSettings &rhs) noexcept {
  return lhs.pointLightsEnabled == rhs.pointLightsEnabled &&
      lhs.pointShadowEnabled == rhs.pointShadowEnabled &&
      lhs.pointShadowFaceCulling == rhs.pointShadowFaceCulling &&
      lhs.pointShadowTemporalReuse == rhs.pointShadowTemporalReuse &&
      lhs.alphaShadowHashed == rhs.alphaShadowHashed &&
      lhs.pointShadowResolution == rhs.pointShadowResolution &&
      lhs.pointShadowMaxLights == rhs.pointShadowMaxLights &&
      lhs.pointShadowMaxFacesPerFrame == rhs.pointShadowMaxFacesPerFrame &&
      lhs.pointShadowMaxCastersPerFace == rhs.pointShadowMaxCastersPerFace &&
      lhs.pointShadowUpdatePeriod == rhs.pointShadowUpdatePeriod &&
      PointShadowF32Exact(lhs.pointShadowCasterCullPadding,
                          rhs.pointShadowCasterCullPadding);
}

uint64_t PointShadowPolicySeal(
    const war3::render::War3PointShadowCpuPlanSettings &settings) noexcept {
  PointShadowSealBuilder seal;
  seal.mixU64(settings.pointLightsEnabled ? 1u : 0u);
  seal.mixU64(settings.pointShadowEnabled ? 1u : 0u);
  seal.mixU64(settings.pointShadowFaceCulling ? 1u : 0u);
  seal.mixU64(settings.pointShadowTemporalReuse ? 1u : 0u);
  seal.mixU64(settings.alphaShadowHashed ? 1u : 0u);
  seal.mixU64(settings.pointShadowResolution);
  seal.mixU64(settings.pointShadowMaxLights);
  seal.mixU64(settings.pointShadowMaxFacesPerFrame);
  seal.mixU64(settings.pointShadowMaxCastersPerFace);
  seal.mixU64(settings.pointShadowUpdatePeriod);
  seal.mixF32(settings.pointShadowCasterCullPadding);
  return seal.finish();
}

bool PointShadowHistoryExact(
    const war3::render::War3PointShadowCpuHistory &lhs,
    const war3::render::War3PointShadowCpuHistory &rhs) noexcept {
  return lhs.cubeAllocated == rhs.cubeAllocated &&
      lhs.readyLightCount == rhs.readyLightCount &&
      lhs.publishedContentSignature == rhs.publishedContentSignature &&
      lhs.temporalAge == rhs.temporalAge &&
      lhs.faceValidMask == rhs.faceValidMask && lhs.faceAge == rhs.faceAge;
}

uint64_t PointShadowLifecycleSeal(
    uint64_t rendererEpoch,
    const war3::render::War3PointShadowCpuHistory &history) noexcept {
  PointShadowSealBuilder seal;
  seal.mixU64(rendererEpoch);
  seal.mixU64(history.cubeAllocated ? 1u : 0u);
  seal.mixU64(history.readyLightCount);
  seal.mixU64(history.publishedContentSignature);
  seal.mixU64(history.temporalAge);
  for (uint32_t light = 0u;
       light < war3::render::kWar3PointShadowCpuPlanMaxLights; ++light) {
    seal.mixU64(history.faceValidMask[light]);
    for (uint32_t face = 0u; face < 6u; ++face)
      seal.mixU64(history.faceAge[light][face]);
  }
  return seal.finish();
}

war3::render::War3PointShadowCpuLight FreezePointShadowLight(
    const War3PointLight &light) noexcept {
  war3::render::War3PointShadowCpuLight result = {};
  result.position = {light.position.x, light.position.y, light.position.z,
                     light.position.w};
  result.shadowIntensity = light.params.x;
  result.id = light.id;
  return result;
}

bool PointShadowLightExact(
    const war3::render::War3PointShadowCpuLight &lhs,
    const war3::render::War3PointShadowCpuLight &rhs) noexcept {
  return lhs.id == rhs.id &&
      PointShadowF32Exact(lhs.position.x, rhs.position.x) &&
      PointShadowF32Exact(lhs.position.y, rhs.position.y) &&
      PointShadowF32Exact(lhs.position.z, rhs.position.z) &&
      PointShadowF32Exact(lhs.position.w, rhs.position.w) &&
      PointShadowF32Exact(lhs.shadowIntensity, rhs.shadowIntensity);
}

static const struct {
  Vector4 dir;
  Vector4 up;
  Vector4 right; // cross(up, dir), precomputed for CPU face culling
} kPointShadowFaceParams[6] = {
    {{1, 0, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}},    // +X
    {{-1, 0, 0, 0}, {0, -1, 0, 0}, {0, 0, -1, 0}},  // -X
    {{0, 1, 0, 0}, {0, 0, 1, 0}, {-1, 0, 0, 0}},    // +Y
    {{0, -1, 0, 0}, {0, 0, -1, 0}, {-1, 0, 0, 0}},  // -Y
    {{0, 0, 1, 0}, {0, -1, 0, 0}, {-1, 0, 0, 0}},   // +Z
    {{0, 0, -1, 0}, {0, -1, 0, 0}, {1, 0, 0, 0}},   // -Z
};
} // namespace

void War3ShadowReceiverPass::resetPointShadowCpuPlanPreservingCapacity() {
  m_pointShadowCpuPlan.ready = false;
  m_pointShadowCpuPlan.shouldRender = false;
  m_pointShadowCpuPlan.reusePublished = false;
  m_pointShadowCpuPlan.forceFullFaceUpdate = false;
  m_pointShadowCpuPlan.failed = false;
  m_pointShadowCpuPlan.shadowLightCount = 0u;
  m_pointShadowCpuPlan.resourceCapacityLights = 1u;
  m_pointShadowCpuPlan.maxFacesPerFrame = 6u;
  m_pointShadowCpuPlan.resolution = 1024u;
  m_pointShadowCpuPlan.maxCastersPerFace = 0u;
  m_pointShadowCpuPlan.lightGeneration = 0u;
  m_pointShadowCpuPlan.lightFrameSerial = 0u;
  m_pointShadowCpuPlan.contentSignature = 0u;
  m_pointShadowCpuPlan.updateMask.fill(0u);
  m_pointShadowCpuPlan.faceCandidateCount.fill(0u);
  m_pointShadowCpuPlan.faceKeptCount.fill(0u);
  m_pointShadowCpuPlan.faceDroppedCount.fill(0u);
  for (auto &faceCasters : m_pointShadowCpuPlan.faceCasters)
    faceCasters.clear();
}

void War3ShadowReceiverPass::invalidatePointShadowPublishedState() {
  m_pointShadowReady.fill(false);
  m_pointShadowReadyCount = 0u;
  m_pointShadowTemporalAge = 0u;
  m_pointShadowFaceValidMask.fill(0u);
  for (auto &ages : m_pointShadowFaceAge)
    ages.fill(0u);
  m_pointShadowContentSignature = 0u;
  m_pointShadowPublishedLightGeneration = 0u;
  m_pointShadowPublishedFrameSerial = 0u;
  m_pointShadowPublishedMapEpoch = 0u;
  m_pointShadowPublishedDeviceEpoch = 0u;
  m_pointShadowPublishedResourceGeneration = 0u;
  m_pointShadowPublishedPolicyRevision = 0u;
  m_pointShadowPublishedLightCount = 0u;
  m_pointShadowPublishedLightIds.fill(0);
}

bool War3ShadowReceiverPass::pointShadowPublishedStateMatchesCurrentPlan()
    const {
  const bool publicationNamesCurrentFrame =
      m_pointShadowPublishedFrameSerial != 0u &&
      m_pointShadowPublishedFrameSerial ==
          m_pointShadowCpuPlan.lightFrameSerial;
  const bool explicitTemporalReuse =
      m_pointShadowCpuPlan.reusePublished &&
      m_pointShadowPublishedFrameSerial != 0u &&
      m_pointShadowPublishedFrameSerial <
          m_pointShadowCpuPlan.lightFrameSerial;
  return m_pointShadowCube && m_pointShadowCubeView &&
         m_pointShadowPublishedMapEpoch == m_shadowMapEpoch &&
         m_pointShadowPublishedDeviceEpoch == m_shadowDeviceEpoch &&
         m_pointShadowPublishedResourceGeneration != 0u &&
         m_pointShadowPublishedResourceGeneration ==
             m_pointShadowResourceGeneration &&
         m_pointShadowReadyCount > 0u &&
         m_pointShadowCpuPlan.ready &&
         !m_pointShadowCpuPlan.failed &&
         m_pointShadowCpuPlan.lightGeneration != 0u &&
         (publicationNamesCurrentFrame || explicitTemporalReuse) &&
         m_pointShadowCpuPlan.contentSignature ==
             m_pointShadowContentSignature &&
         m_pointShadowCpuPlan.lightGeneration ==
             m_pointShadowPublishedLightGeneration;
}

bool War3ShadowReceiverPass::holdPointShadowLastCompleteAfterBudgetReject(
    const War3PipelineInput& input,
    const War3PointLightFrameSnapshot& lightSnapshot,
    const War3RenderSettings& settings) {
  using war3::render::War3GpuPointShadowPublicationIdentity;
  constexpr uint32_t kCompleteFaceMask = 0x3fu;

  War3GpuPointShadowPublicationIdentity current = {};
  current.mapEpoch = input.mapEpoch;
  current.deviceEpoch = input.deviceEpoch;
  current.resourceGeneration = m_pointShadowResourceGeneration;
  current.lightGeneration = lightSnapshot.generation;
  current.settingsRevision =
      PointShadowPolicySeal(FreezePointShadowSettings(settings));
  current.resolution = m_pointShadowCpuPlan.resolution;
  current.capacityLights = m_pointShadowCpuPlan.resourceCapacityLights;
  current.lightCount = m_pointShadowCpuPlan.shadowLightCount;
  current.complete = current.lightCount != 0u &&
      current.lightCount <= kMaxPointShadowLights &&
      current.lightCount <= lightSnapshot.shadowCount &&
      lightSnapshot.frameSerial == input.frameSerial &&
      input.mapEpoch == m_shadowMapEpoch &&
      input.deviceEpoch == m_shadowDeviceEpoch;

  War3GpuPointShadowPublicationIdentity published = {};
  published.mapEpoch = m_pointShadowPublishedMapEpoch;
  published.deviceEpoch = m_pointShadowPublishedDeviceEpoch;
  published.resourceGeneration = m_pointShadowPublishedResourceGeneration;
  published.lightGeneration = m_pointShadowPublishedLightGeneration;
  published.settingsRevision = m_pointShadowPublishedPolicyRevision;
  published.resolution = m_pointShadowResolution;
  published.capacityLights = m_pointShadowCapacityLights;
  published.lightCount = m_pointShadowPublishedLightCount;
  published.complete = published.lightCount != 0u &&
      published.lightCount <= kMaxPointShadowLights &&
      m_pointShadowReadyCount >= published.lightCount;

  const uint32_t currentCount = std::min<uint32_t>(
      current.lightCount, War3GpuPointShadowPublicationIdentity::kMaxLights);
  for (uint32_t light = 0u; light < currentCount; ++light) {
    const auto& source = lightSnapshot.lights[light];
    current.lights[light] = {
        source.id, source.position.x, source.position.y, source.position.z,
        std::max(source.position.w, 1.0f),
        std::clamp(source.params.x, 0.0f, 1.0f)};
  }

  const uint32_t publishedCount = std::min<uint32_t>(
      published.lightCount,
      War3GpuPointShadowPublicationIdentity::kMaxLights);
  for (uint32_t light = 0u; light < publishedCount; ++light) {
    const auto& source = m_pointShadowData[light];
    published.lights[light] = {
        m_pointShadowPublishedLightIds[light], source.lightPos.x,
        source.lightPos.y, source.lightPos.z, source.lightPos.w,
        source.shadowIntensity};
    published.complete = published.complete && m_pointShadowReady[light] &&
        (m_pointShadowFaceValidMask[light] & kCompleteFaceMask) ==
            kCompleteFaceMask;
  }

  if (!war3::render::War3GpuCanHoldPointShadowLastComplete(current,
                                                            published))
    return false;

  // Turn this rejected update into an explicit reference to the immutable
  // last-complete cube. No command was recorded and no face validity changed.
  m_pointShadowCpuPlan.ready = true;
  m_pointShadowCpuPlan.failed = false;
  m_pointShadowCpuPlan.shouldRender = false;
  m_pointShadowCpuPlan.reusePublished = true;
  m_pointShadowCpuPlan.forceFullFaceUpdate = false;
  m_pointShadowCpuPlan.shadowLightCount = m_pointShadowPublishedLightCount;
  m_pointShadowCpuPlan.resourceCapacityLights = m_pointShadowCapacityLights;
  m_pointShadowCpuPlan.resolution = m_pointShadowResolution;
  m_pointShadowCpuPlan.lightGeneration = lightSnapshot.generation;
  m_pointShadowCpuPlan.lightFrameSerial = input.frameSerial;
  m_pointShadowCpuPlan.contentSignature = m_pointShadowContentSignature;
  m_pointShadowCpuPlan.updateMask.fill(0u);
  m_pointShadowCpuPlan.faceCandidateCount.fill(0u);
  m_pointShadowCpuPlan.faceKeptCount.fill(0u);
  m_pointShadowCpuPlan.faceDroppedCount.fill(0u);
  for (auto& faceCasters : m_pointShadowCpuPlan.faceCasters)
    faceCasters.clear();
  if (m_pointShadowTemporalAge != std::numeric_limits<uint32_t>::max())
    ++m_pointShadowTemporalAge;
  return pointShadowPublishedStateMatchesCurrentPlan();
}

void War3ShadowReceiverPass::waitPointShadowCpuPrepare() {
  if (m_pointShadowPrepareFuture.valid()) {
    try {
      // get(), unlike wait(), propagates worker failures. Never let a partially
      // written plan advance to GPU recording.
      m_pointShadowPrepareFuture.get();
    } catch (const DxvkError &e) {
      const uint64_t failedGeneration =
          m_pointShadowCpuPlan.lightGeneration;
      const uint64_t failedFrameSerial =
          m_pointShadowCpuPlan.lightFrameSerial;
      m_pointShadowCpuPlan = {};
      m_pointShadowCpuPlan.ready = true;
      m_pointShadowCpuPlan.failed = true;
      m_pointShadowCpuPlan.lightGeneration = failedGeneration;
      m_pointShadowCpuPlan.lightFrameSerial = failedFrameSerial;
      invalidatePointShadowPublishedState();
      static uint32_t s_dxvkWorkerFailureLogs = 0u;
      if (s_dxvkWorkerFailureLogs++ < 16u ||
          (s_dxvkWorkerFailureLogs % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK PointShadow: Worker_Prepare failed; point shadows disabled "
            "for this frame (%s)\n",
            e.message().c_str());
      }
    } catch (const std::exception &e) {
      const uint64_t failedGeneration =
          m_pointShadowCpuPlan.lightGeneration;
      const uint64_t failedFrameSerial =
          m_pointShadowCpuPlan.lightFrameSerial;
      m_pointShadowCpuPlan = {};
      m_pointShadowCpuPlan.ready = true;
      m_pointShadowCpuPlan.failed = true;
      m_pointShadowCpuPlan.lightGeneration = failedGeneration;
      m_pointShadowCpuPlan.lightFrameSerial = failedFrameSerial;
      invalidatePointShadowPublishedState();
      static uint32_t s_workerFailureLogs = 0u;
      if (s_workerFailureLogs++ < 16u ||
          (s_workerFailureLogs % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK PointShadow: Worker_Prepare failed; point shadows disabled "
            "for this frame (%s)\n",
            e.what());
      }
    } catch (...) {
      const uint64_t failedGeneration =
          m_pointShadowCpuPlan.lightGeneration;
      const uint64_t failedFrameSerial =
          m_pointShadowCpuPlan.lightFrameSerial;
      m_pointShadowCpuPlan = {};
      m_pointShadowCpuPlan.ready = true;
      m_pointShadowCpuPlan.failed = true;
      m_pointShadowCpuPlan.lightGeneration = failedGeneration;
      m_pointShadowCpuPlan.lightFrameSerial = failedFrameSerial;
      invalidatePointShadowPublishedState();
      static uint32_t s_unknownWorkerFailureLogs = 0u;
      if (s_unknownWorkerFailureLogs++ < 16u ||
          (s_unknownWorkerFailureLogs % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK PointShadow: Worker_Prepare failed; point shadows disabled "
            "for this frame (unknown exception)\n");
      }
    }
    m_pointShadowPrepareFuture = {};
  }
  m_pointShadowPrepareRunning.store(false, std::memory_order_release);
}

void War3ShadowReceiverPass::recyclePointShadowPersistentStorage(
    war3::render::War3PointShadowCpuPlanOwnedStorage &&storage) {
  m_pointShadowPersistentRequestScratch.payload.storage = std::move(storage);
}

void War3ShadowReceiverPass::beginPointShadowPersistentPrepare(
    const War3PipelineInput &input,
    const War3PointLightFrameSnapshot &lightSnapshot,
    const std::vector<const War3ShadowCasterDraw *> *replayDraws) {
  using namespace war3::render;

  ++m_pointShadowPersistentBeginAttempts;
  const auto rejectAdmission =
      [this](PointShadowPersistentBeginRejectReason reason) noexcept {
        m_pointShadowPersistentLastBeginRejectReason = reason;
      };
  if (PointShadowPersistentMode() == War3PointShadowPersistentMode::Off) {
    rejectAdmission(PointShadowPersistentBeginRejectReason::ModeOff);
    return;
  }
  if (!War3WorkerPrepareEnabled()) {
    rejectAdmission(
        PointShadowPersistentBeginRejectReason::WorkerPrepareDisabled);
    return;
  }
  if (!input.settings) {
    rejectAdmission(PointShadowPersistentBeginRejectReason::MissingSettings);
    return;
  }
  if (!input.settings->shadows.pointLightsEnabled ||
      !input.settings->shadows.pointShadowEnabled ||
      input.settings->shadows.pointShadowMaxLights == 0u) {
    rejectAdmission(PointShadowPersistentBeginRejectReason::PointShadowDisabled);
    return;
  }
  if (!lightSnapshot.hasAny) {
    rejectAdmission(PointShadowPersistentBeginRejectReason::NoPointLights);
    return;
  }
  if (lightSnapshot.shadowCount == 0u) {
    rejectAdmission(
        PointShadowPersistentBeginRejectReason::NoShadowCastingLights);
    return;
  }
  if (!replayDraws || replayDraws->empty()) {
    rejectAdmission(PointShadowPersistentBeginRejectReason::NoReplayDraws);
    return;
  }
  if (lightSnapshot.generation == 0u ||
      lightSnapshot.frameSerial == 0u) {
    rejectAdmission(
        PointShadowPersistentBeginRejectReason::InvalidLightSnapshot);
    return;
  }
  if (input.frameSerial == 0u ||
      lightSnapshot.frameSerial != input.frameSerial) {
    rejectAdmission(PointShadowPersistentBeginRejectReason::InvalidFrameSerial);
    return;
  }
  if (m_pointShadowPersistentRendererEpoch == 0u) {
    rejectAdmission(
        PointShadowPersistentBeginRejectReason::InvalidRendererEpoch);
    return;
  }

  ++m_pointShadowPersistentBeginEligible;
  m_pointShadowPersistentLastBeginRejectReason =
      PointShadowPersistentBeginRejectReason::None;

  if (!m_pointShadowPersistentWorker) {
    try {
      m_pointShadowPersistentWorker =
          std::make_unique<PointShadowPersistentPrepareWorker>();
    } catch (...) {
      rejectAdmission(
          PointShadowPersistentBeginRejectReason::WorkerCreateFailed);
      ++m_pointShadowPersistentRejectedFallback;
      return;
    }
    ++m_pointShadowPersistentWorkerCreateCount;
    if (!m_pointShadowPersistentWorker->available()) {
      rejectAdmission(PointShadowPersistentBeginRejectReason::WorkerUnavailable);
      ++m_pointShadowPersistentRejectedFallback;
      return;
    }
    WAR3_RENDER_LOG(
        "DXVK PointShadow: persistent prepare worker created mode=%u "
        "(release default remains Off)\n",
        static_cast<uint32_t>(PointShadowPersistentMode()));
  }

  // A prior frame may have taken longer than its same-frame deadline. Never
  // wait for it and never overwrite its mailbox slot. Reclaim an exact ready
  // payload, discard terminal failures, or leave the worker busy and use the
  // canonical synchronous path for this frame.
  if (m_pointShadowPersistentPending) {
    auto previous = m_pointShadowPersistentWorker->tryCollectExact(
        m_pointShadowPersistentPendingGeneration);
    if (previous.state == War3PointShadowPrepareResultState::NotReady) {
      rejectAdmission(
          PointShadowPersistentBeginRejectReason::PreviousJobNotReady);
      ++m_pointShadowPersistentRejectedFallback;
      return;
    }
    m_pointShadowPersistentPending = false;
    if (previous.state == War3PointShadowPrepareResultState::Ready &&
        previous.payload.has_value()) {
      recyclePointShadowPersistentStorage(
          std::move(previous.payload->storage));
    }
  }

  if (m_pointShadowPersistentJobSerial ==
      std::numeric_limits<uint64_t>::max()) {
    rejectAdmission(
        PointShadowPersistentBeginRejectReason::JobSerialExhausted);
    ++m_pointShadowPersistentRejectedFallback;
    return;
  }
  if (replayDraws->size() >
      size_t(std::numeric_limits<uint32_t>::max())) {
    rejectAdmission(
        PointShadowPersistentBeginRejectReason::ReplayDrawCountOverflow);
    ++m_pointShadowPersistentRejectedFallback;
    return;
  }

  PointShadowPersistentRequest &request =
      m_pointShadowPersistentRequestScratch;
  try {
    request.generation = {
        ++m_pointShadowPersistentJobSerial,
        m_pointShadowPersistentRendererEpoch,
        input.frameSerial,
        lightSnapshot.generation,
    };
    auto &payload = request.payload;
    payload.settings = FreezePointShadowSettings(*input.settings);
    payload.hasAnyLight = lightSnapshot.hasAny;
    payload.shadowLightCount = lightSnapshot.shadowCount;
    payload.dynamicPoseSignature =
        input.scene.shadowStats.dynamicPoseSignature;
    payload.dynamicPoseCount = input.scene.shadowStats.dynamicPoseCount;
    payload.dynamicSkinnedOutputCount =
        input.scene.shadowStats.dynamicSkinnedOutputCount;
    payload.history = {};
    payload.history.cubeAllocated =
        m_pointShadowCube && m_pointShadowCubeView;
    payload.history.readyLightCount = m_pointShadowReadyCount;
    payload.history.publishedContentSignature =
        m_pointShadowContentSignature;
    payload.history.temporalAge = m_pointShadowTemporalAge;
    payload.history.faceValidMask = m_pointShadowFaceValidMask;
    payload.history.faceAge = m_pointShadowFaceAge;

    payload.lights.fill({});
    const uint32_t frozenLightCount = std::min<uint32_t>(
        lightSnapshot.shadowCount, kMaxPointShadowLights);
    for (uint32_t light = 0u; light < frozenLightCount; ++light) {
      payload.lights[light] =
          FreezePointShadowLight(lightSnapshot.lights[light]);
    }

    auto &storage = payload.storage;
    storage.paletteHashes.clear();
    storage.paletteHashes.reserve(input.scene.shadowPalettes.size());
    for (const auto &palette : input.scene.shadowPalettes)
      storage.paletteHashes.push_back(palette.hash);
    storage.casters.clear();
    storage.casters.reserve(replayDraws->size());
    storage.rangeCandidateIndices.clear();
    storage.rankedCandidates.clear();
    for (auto &face : storage.faceCasters)
      face.clear();

    PointShadowSealBuilder replaySeal;
    replaySeal.mixU64(input.frameSerial);
    replaySeal.mixU64(lightSnapshot.generation);
    replaySeal.mixU64(payload.dynamicPoseSignature);
    replaySeal.mixU64(payload.dynamicPoseCount);
    replaySeal.mixU64(payload.dynamicSkinnedOutputCount);
    replaySeal.mixU64(replayDraws->size());
    for (const War3ShadowCasterDraw *draw : *replayDraws) {
      if (!draw) {
        request.generation = {};
        payload.seal = {};
        rejectAdmission(PointShadowPersistentBeginRejectReason::NullCaster);
        ++m_pointShadowPersistentRejectedFallback;
        return;
      }
      storage.casters.push_back(FreezePointShadowCaster(*draw));
      MixPointShadowCasterSeal(replaySeal, storage.casters.back());
    }
    replaySeal.mixU64(storage.paletteHashes.size());
    for (uint64_t paletteHash : storage.paletteHashes)
      replaySeal.mixU64(paletteHash);

    payload.seal.replayGeneration = replaySeal.finish();
    payload.seal.policyRevision = PointShadowPolicySeal(payload.settings);
    payload.seal.lifecycleGeneration = PointShadowLifecycleSeal(
        m_pointShadowPersistentRendererEpoch, payload.history);

    const auto expectedGeneration = request.generation;
    const auto expectedSeal = payload.seal;
    const auto expectedSettings = payload.settings;
    const auto expectedHistory = payload.history;
    const auto expectedLights = payload.lights;
    const uint64_t expectedDynamicPoseSignature =
        payload.dynamicPoseSignature;
    const uint32_t expectedDynamicPoseCount = payload.dynamicPoseCount;
    const uint32_t expectedDynamicSkinnedOutputCount =
        payload.dynamicSkinnedOutputCount;

    const War3PointShadowPrepareSubmitStatus submitted =
        m_pointShadowPersistentWorker->submit(request);
    if (submitted == War3PointShadowPrepareSubmitStatus::Accepted) {
      m_pointShadowPersistentPendingGeneration = expectedGeneration;
      m_pointShadowPersistentPendingSeal = expectedSeal;
      m_pointShadowPersistentExpectedSettings = expectedSettings;
      m_pointShadowPersistentExpectedHistory = expectedHistory;
      m_pointShadowPersistentExpectedLights = expectedLights;
      m_pointShadowPersistentExpectedLightCount = frozenLightCount;
      m_pointShadowPersistentExpectedDynamicPoseSignature =
          expectedDynamicPoseSignature;
      m_pointShadowPersistentExpectedDynamicPoseCount =
          expectedDynamicPoseCount;
      m_pointShadowPersistentExpectedDynamicSkinnedOutputCount =
          expectedDynamicSkinnedOutputCount;
      m_pointShadowPersistentPending = true;
      m_pointShadowPersistentLastBeginRejectReason =
          PointShadowPersistentBeginRejectReason::None;
      ++m_pointShadowPersistentAccepted;
      return;
    }

    // submit(Request&) moves only on Accepted. The exact storage is still ours
    // here and remains reusable after the canonical same-frame fallback.
    request.generation = {};
    payload.seal = {};
    switch (submitted) {
      case War3PointShadowPrepareSubmitStatus::InvalidGeneration:
        rejectAdmission(
            PointShadowPersistentBeginRejectReason::SubmitInvalidGeneration);
        break;
      case War3PointShadowPrepareSubmitStatus::StaleGeneration:
        rejectAdmission(
            PointShadowPersistentBeginRejectReason::SubmitStaleGeneration);
        break;
      case War3PointShadowPrepareSubmitStatus::Busy:
        rejectAdmission(PointShadowPersistentBeginRejectReason::SubmitBusy);
        break;
      case War3PointShadowPrepareSubmitStatus::Stopping:
        rejectAdmission(PointShadowPersistentBeginRejectReason::SubmitStopping);
        break;
      case War3PointShadowPrepareSubmitStatus::Unavailable:
        rejectAdmission(
            PointShadowPersistentBeginRejectReason::SubmitUnavailable);
        break;
      case War3PointShadowPrepareSubmitStatus::Accepted:
        break;
    }
    ++m_pointShadowPersistentRejectedFallback;
  } catch (const std::bad_alloc &) {
    // Every allocation in the owner freeze transaction is above this boundary.
    // The request has not crossed submit, so no worker generation is pending and
    // renderPointShadow will build the canonical plan in this same frame.
    request.generation = {};
    request.payload.seal = {};
    m_pointShadowPersistentPending = false;
    rejectAdmission(PointShadowPersistentBeginRejectReason::AllocationFailure);
    ++m_pointShadowPersistentRejectedFallback;
  } catch (...) {
    // length_error and any future throwing copy/freeze helper follow the same
    // fail-closed path. Partially filled vectors remain renderer-owned scratch.
    request.generation = {};
    request.payload.seal = {};
    m_pointShadowPersistentPending = false;
    rejectAdmission(
        PointShadowPersistentBeginRejectReason::UnexpectedException);
    ++m_pointShadowPersistentRejectedFallback;
  }
}

std::optional<War3ShadowReceiverPass::PointShadowPersistentResultPayload>
War3ShadowReceiverPass::tryCollectPointShadowPersistentProposal(
    const War3PipelineInput &input,
    const War3PointLightFrameSnapshot &lightSnapshot,
    const std::vector<const War3ShadowCasterDraw *> *replayDraws) {
  using namespace war3::render;
  if (!m_pointShadowPersistentPending || !m_pointShadowPersistentWorker ||
      !replayDraws || !input.settings)
    return std::nullopt;
  if (m_pointShadowPersistentPendingGeneration.frameSerial !=
          input.frameSerial ||
      m_pointShadowPersistentPendingGeneration.lightGeneration !=
          lightSnapshot.generation ||
      lightSnapshot.frameSerial != input.frameSerial) {
    ++m_pointShadowLateResultRejectCount;
    g_shadowReplayDiagnostics.pointLateResultRejectCount.store(
        m_pointShadowLateResultRejectCount, std::memory_order_release);
    return std::nullopt;
  }

  auto completed = m_pointShadowPersistentWorker->tryCollectExact(
      m_pointShadowPersistentPendingGeneration);
  if (completed.state == War3PointShadowPrepareResultState::NotReady) {
    ++m_pointShadowPersistentDeadlineFallback;
    return std::nullopt;
  }
  m_pointShadowPersistentPending = false;
  if (completed.state != War3PointShadowPrepareResultState::Ready ||
      !completed.payload.has_value()) {
    ++m_pointShadowPersistentRejectedFallback;
    return std::nullopt;
  }

  PointShadowPersistentResultPayload proposal =
      std::move(*completed.payload);
  const auto rejectAndRecycle = [&]()
      -> std::optional<PointShadowPersistentResultPayload> {
    recyclePointShadowPersistentStorage(std::move(proposal.storage));
    ++m_pointShadowPersistentRejectedFallback;
    return std::nullopt;
  };

  if (proposal.seal != m_pointShadowPersistentPendingSeal ||
      !proposal.seal.valid() || proposal.ownerMustInvalidatePublication)
    return rejectAndRecycle();
  if (proposal.disposition != War3PointShadowCpuPlanDisposition::Render &&
      proposal.disposition !=
          War3PointShadowCpuPlanDisposition::ReusePublished)
    return rejectAndRecycle();
  if (proposal.shouldRender == proposal.reusePublished ||
      proposal.shadowLightCount > kMaxPointShadowLights ||
      proposal.storage.casters.size() != replayDraws->size())
    return rejectAndRecycle();

  const War3PointShadowCpuPlanSettings currentSettings =
      FreezePointShadowSettings(*input.settings);
  if (!PointShadowSettingsExact(currentSettings,
                                 m_pointShadowPersistentExpectedSettings) ||
      PointShadowPolicySeal(currentSettings) !=
          m_pointShadowPersistentPendingSeal.policyRevision)
    return rejectAndRecycle();
  if (input.scene.shadowStats.dynamicPoseSignature !=
          m_pointShadowPersistentExpectedDynamicPoseSignature ||
      input.scene.shadowStats.dynamicPoseCount !=
          m_pointShadowPersistentExpectedDynamicPoseCount ||
      input.scene.shadowStats.dynamicSkinnedOutputCount !=
          m_pointShadowPersistentExpectedDynamicSkinnedOutputCount)
    return rejectAndRecycle();
  const uint32_t expectedResolution = std::clamp<uint32_t>(
      currentSettings.pointShadowResolution, 128u, 2048u);
  const uint32_t expectedRequestedLights = std::clamp<uint32_t>(
      currentSettings.pointShadowMaxLights, 1u,
      kMaxPointShadowLights);
  const uint32_t expectedCapacity = War3ResolvePointShadowCpuPlanCapacity(
      expectedResolution, expectedRequestedLights);
  const uint32_t expectedShadowLights = std::min<uint32_t>(
      {kMaxPointShadowLights, expectedCapacity,
       lightSnapshot.shadowCount});
  if (proposal.resolution != expectedResolution ||
      proposal.resourceCapacityLights != expectedCapacity ||
      proposal.shadowLightCount != expectedShadowLights ||
      proposal.maxCastersPerFace !=
          currentSettings.pointShadowMaxCastersPerFace)
    return rejectAndRecycle();

  War3PointShadowCpuHistory currentHistory = {};
  currentHistory.cubeAllocated = m_pointShadowCube && m_pointShadowCubeView;
  currentHistory.readyLightCount = m_pointShadowReadyCount;
  currentHistory.publishedContentSignature = m_pointShadowContentSignature;
  currentHistory.temporalAge = m_pointShadowTemporalAge;
  currentHistory.faceValidMask = m_pointShadowFaceValidMask;
  currentHistory.faceAge = m_pointShadowFaceAge;
  if (!PointShadowHistoryExact(currentHistory,
                               m_pointShadowPersistentExpectedHistory) ||
      PointShadowLifecycleSeal(m_pointShadowPersistentRendererEpoch,
                               currentHistory) !=
          m_pointShadowPersistentPendingSeal.lifecycleGeneration)
    return rejectAndRecycle();

  if (m_pointShadowPersistentExpectedLightCount !=
      std::min<uint32_t>(lightSnapshot.shadowCount,
                         kMaxPointShadowLights))
    return rejectAndRecycle();
  for (uint32_t light = 0u;
       light < m_pointShadowPersistentExpectedLightCount; ++light) {
    if (!PointShadowLightExact(
            FreezePointShadowLight(lightSnapshot.lights[light]),
            m_pointShadowPersistentExpectedLights[light]))
      return rejectAndRecycle();
  }

  PointShadowSealBuilder replaySeal;
  replaySeal.mixU64(input.frameSerial);
  replaySeal.mixU64(lightSnapshot.generation);
  replaySeal.mixU64(input.scene.shadowStats.dynamicPoseSignature);
  replaySeal.mixU64(input.scene.shadowStats.dynamicPoseCount);
  replaySeal.mixU64(input.scene.shadowStats.dynamicSkinnedOutputCount);
  replaySeal.mixU64(replayDraws->size());
  bool frozenCasterUsesVertexBlend = false;
  for (size_t caster = 0u; caster < replayDraws->size(); ++caster) {
    const War3ShadowCasterDraw *draw = (*replayDraws)[caster];
    if (!draw)
      return rejectAndRecycle();
    const War3PointShadowCpuCaster frozen = FreezePointShadowCaster(*draw);
    if (!PointShadowCasterExact(frozen, proposal.storage.casters[caster]))
      return rejectAndRecycle();
    frozenCasterUsesVertexBlend =
        frozenCasterUsesVertexBlend || frozen.vertexBlendEnabled;
    MixPointShadowCasterSeal(replaySeal, frozen);
  }
  replaySeal.mixU64(input.scene.shadowPalettes.size());
  if (proposal.storage.paletteHashes.size() !=
      input.scene.shadowPalettes.size())
    return rejectAndRecycle();
  for (size_t palette = 0u; palette <
       input.scene.shadowPalettes.size(); ++palette) {
    const uint64_t currentHash = input.scene.shadowPalettes[palette].hash;
    if (proposal.storage.paletteHashes[palette] != currentHash)
      return rejectAndRecycle();
    replaySeal.mixU64(currentHash);
  }
  if (replaySeal.finish() !=
      m_pointShadowPersistentPendingSeal.replayGeneration)
    return rejectAndRecycle();

  if (proposal.disposition ==
      War3PointShadowCpuPlanDisposition::ReusePublished) {
    if (!War3PointShadowCpuReuseProposalExact(
            proposal, currentSettings, currentHistory,
            m_pointShadowPersistentExpectedDynamicPoseCount,
            m_pointShadowPersistentExpectedDynamicSkinnedOutputCount,
            frozenCasterUsesVertexBlend))
      return rejectAndRecycle();

    // A reuse proposal deliberately contains no regenerated light matrices or
    // face lists. Prove that the live publication still names the exact frozen
    // lights before allowing adoption to advance only the temporal age.
    if (m_pointShadowPublishedLightGeneration != lightSnapshot.generation ||
        m_pointShadowPublishedFrameSerial == 0u ||
        m_pointShadowPublishedFrameSerial >= input.frameSerial ||
        m_pointShadowPublishedLightCount != proposal.shadowLightCount)
      return rejectAndRecycle();
    for (uint32_t light = 0u; light < proposal.shadowLightCount; ++light) {
      const auto &expected = m_pointShadowPersistentExpectedLights[light];
      const auto &live = m_pointShadowData[light];
      if (m_pointShadowPublishedLightIds[light] != expected.id ||
          !PointShadowF32Exact(live.lightPos.x, expected.position.x) ||
          !PointShadowF32Exact(live.lightPos.y, expected.position.y) ||
          !PointShadowF32Exact(live.lightPos.z, expected.position.z) ||
          !PointShadowF32Exact(live.lightPos.w,
                               std::max(expected.position.w, 1.0f)) ||
          !PointShadowF32Exact(
              live.shadowIntensity,
              std::clamp(expected.shadowIntensity, 0.0f, 1.0f)))
        return rejectAndRecycle();
    }
    for (uint32_t light = 0u; light < kMaxPointShadowLights; ++light) {
      if (proposal.updateMask[light] != 0u)
        return rejectAndRecycle();
    }
    for (size_t face = 0u; face < proposal.storage.faceCasters.size(); ++face) {
      if (proposal.faceCandidateCount[face] != 0u ||
          proposal.faceKeptCount[face] != 0u ||
          proposal.faceDroppedCount[face] != 0u ||
          !proposal.storage.faceCasters[face].empty())
        return rejectAndRecycle();
    }
    return std::optional<PointShadowPersistentResultPayload>(
        std::in_place, std::move(proposal));
  }

  if (proposal.disposition != War3PointShadowCpuPlanDisposition::Render ||
      !proposal.shouldRender || proposal.reusePublished ||
      proposal.maxFacesPerFrame == 0u ||
      proposal.maxFacesPerFrame > 6u ||
      proposal.nextTemporalAge != 0u ||
      proposal.forceFullFaceUpdate !=
          proposal.ownerMustClearFaceValidityBeforeRecord)
    return rejectAndRecycle();

  for (size_t face = 0u; face < proposal.storage.faceCasters.size(); ++face) {
    const uint32_t light = static_cast<uint32_t>(face / 6u);
    if ((proposal.updateMask[light] & 0xc0u) != 0u ||
        proposal.faceKeptCount[face] !=
            proposal.storage.faceCasters[face].size() ||
        proposal.faceCandidateCount[face] <
            proposal.faceKeptCount[face] ||
        proposal.faceDroppedCount[face] !=
            proposal.faceCandidateCount[face] -
                proposal.faceKeptCount[face])
      return rejectAndRecycle();
    if ((proposal.updateMask[face / 6u] &
         uint8_t(1u << (face % 6u))) == 0u &&
        !proposal.storage.faceCasters[face].empty())
      return rejectAndRecycle();
    for (uint32_t caster : proposal.storage.faceCasters[face]) {
      if (caster >= proposal.storage.casters.size())
        return rejectAndRecycle();
    }
  }
  for (uint32_t light = 0u; light < proposal.shadowLightCount; ++light) {
    const auto &plan = proposal.lights[light];
    if (!std::isfinite(plan.lightPositionRange.x) ||
        !std::isfinite(plan.lightPositionRange.y) ||
        !std::isfinite(plan.lightPositionRange.z) ||
        !std::isfinite(plan.lightPositionRange.w) ||
        !std::isfinite(plan.shadowIntensity))
      return rejectAndRecycle();
    for (uint32_t face = 0u; face < 6u; ++face) {
      if (!PointShadowMatrixFinite(plan.faceViewProjection[face]))
        return rejectAndRecycle();
    }
  }
  return std::optional<PointShadowPersistentResultPayload>(
      std::in_place, std::move(proposal));
}

bool War3ShadowReceiverPass::adoptPointShadowPersistentProposal(
    PointShadowPersistentResultPayload &proposal,
    const War3PointLightFrameSnapshot &lightSnapshot) {
  using namespace war3::render;
  if (proposal.ownerMustInvalidatePublication)
    return false;
  if (proposal.disposition ==
      War3PointShadowCpuPlanDisposition::ReusePublished) {
    return adoptPointShadowPersistentReuseProposal(proposal, lightSnapshot);
  }
  if (proposal.disposition == War3PointShadowCpuPlanDisposition::Render)
    return adoptPointShadowPersistentRenderProposal(proposal, lightSnapshot);
  return false;
}

bool War3ShadowReceiverPass::adoptPointShadowPersistentReuseProposal(
    PointShadowPersistentResultPayload &proposal,
    const War3PointLightFrameSnapshot &lightSnapshot) {
  using namespace war3::render;
  if (proposal.disposition !=
          War3PointShadowCpuPlanDisposition::ReusePublished ||
      proposal.shouldRender || !proposal.reusePublished ||
      proposal.forceFullFaceUpdate ||
      proposal.ownerMustClearFaceValidityBeforeRecord ||
      proposal.ownerMustInvalidatePublication ||
      proposal.contentSignature == 0u || proposal.nextTemporalAge == 0u)
    return false;

  // ReusePublished is not a render payload: its light matrices and face lists
  // are intentionally default-initialized. Preserve the live publication and
  // advance only the exact semantic tuple proven by collection above.
  m_pointShadowCpuPlan.ready = true;
  m_pointShadowCpuPlan.failed = false;
  m_pointShadowCpuPlan.shouldRender = false;
  m_pointShadowCpuPlan.reusePublished = true;
  m_pointShadowCpuPlan.forceFullFaceUpdate = false;
  m_pointShadowCpuPlan.shadowLightCount = proposal.shadowLightCount;
  m_pointShadowCpuPlan.resourceCapacityLights =
      proposal.resourceCapacityLights;
  m_pointShadowCpuPlan.resolution = proposal.resolution;
  m_pointShadowCpuPlan.maxCastersPerFace = proposal.maxCastersPerFace;
  m_pointShadowCpuPlan.lightGeneration = lightSnapshot.generation;
  m_pointShadowCpuPlan.lightFrameSerial = lightSnapshot.frameSerial;
  m_pointShadowCpuPlan.contentSignature = proposal.contentSignature;
  m_pointShadowTemporalAge = proposal.nextTemporalAge;

  recyclePointShadowPersistentStorage(std::move(proposal.storage));
  ++m_pointShadowPersistentConsumed;
  return true;
}

bool War3ShadowReceiverPass::adoptPointShadowPersistentRenderProposal(
    PointShadowPersistentResultPayload &proposal,
    const War3PointLightFrameSnapshot &lightSnapshot) {
  using namespace war3::render;
  if (proposal.disposition != War3PointShadowCpuPlanDisposition::Render ||
      !proposal.shouldRender || proposal.reusePublished ||
      proposal.ownerMustInvalidatePublication)
    return false;

  m_pointShadowCpuPlan.ready = true;
  m_pointShadowCpuPlan.failed = false;
  m_pointShadowCpuPlan.shouldRender = proposal.shouldRender;
  m_pointShadowCpuPlan.reusePublished = proposal.reusePublished;
  m_pointShadowCpuPlan.forceFullFaceUpdate =
      proposal.forceFullFaceUpdate;
  m_pointShadowCpuPlan.shadowLightCount = proposal.shadowLightCount;
  m_pointShadowCpuPlan.resourceCapacityLights =
      proposal.resourceCapacityLights;
  m_pointShadowCpuPlan.maxFacesPerFrame = proposal.maxFacesPerFrame;
  m_pointShadowCpuPlan.resolution = proposal.resolution;
  m_pointShadowCpuPlan.maxCastersPerFace = proposal.maxCastersPerFace;
  m_pointShadowCpuPlan.lightGeneration = lightSnapshot.generation;
  m_pointShadowCpuPlan.lightFrameSerial = lightSnapshot.frameSerial;
  m_pointShadowCpuPlan.contentSignature = proposal.contentSignature;
  m_pointShadowCpuPlan.updateMask = proposal.updateMask;
  m_pointShadowCpuPlan.faceCandidateCount = proposal.faceCandidateCount;
  m_pointShadowCpuPlan.faceKeptCount = proposal.faceKeptCount;
  m_pointShadowCpuPlan.faceDroppedCount = proposal.faceDroppedCount;
  m_pointShadowTemporalAge = proposal.nextTemporalAge;

  for (uint32_t light = 0u; light < proposal.shadowLightCount; ++light) {
    const auto &source = proposal.lights[light];
    m_pointShadowData[light].lightPos =
        Vector4(source.lightPositionRange.x, source.lightPositionRange.y,
                source.lightPositionRange.z, source.lightPositionRange.w);
    m_pointShadowData[light].shadowIntensity = source.shadowIntensity;
    for (uint32_t face = 0u; face < 6u; ++face) {
      m_pointShadowData[light].faceViewProj[face] =
          ThawPointShadowMatrix(source.faceViewProjection[face]);
    }
    if (proposal.ownerMustClearFaceValidityBeforeRecord)
      m_pointShadowFaceValidMask[light] = 0u;
  }

  auto storage = std::move(proposal.storage);
  for (size_t face = 0u; face < storage.faceCasters.size(); ++face) {
    m_pointShadowCpuPlan.faceCasters[face].swap(storage.faceCasters[face]);
  }
  recyclePointShadowPersistentStorage(std::move(storage));
  ++m_pointShadowPersistentConsumed;
  return true;
}

bool War3ShadowReceiverPass::pointShadowPersistentProposalMatchesCanonical(
    const PointShadowPersistentResultPayload &proposal) const {
  using namespace war3::render;
  if (proposal.disposition ==
      War3PointShadowCpuPlanDisposition::ReusePublished) {
    if (!m_pointShadowCpuPlan.ready || m_pointShadowCpuPlan.failed ||
        m_pointShadowCpuPlan.shouldRender ||
        !m_pointShadowCpuPlan.reusePublished ||
        m_pointShadowCpuPlan.forceFullFaceUpdate ||
        m_pointShadowCpuPlan.contentSignature != proposal.contentSignature ||
        m_pointShadowTemporalAge != proposal.nextTemporalAge ||
        m_pointShadowPublishedLightCount != proposal.shadowLightCount ||
        !pointShadowPublishedStateMatchesCurrentPlan())
      return false;
    for (uint32_t light = 0u; light < proposal.shadowLightCount; ++light) {
      const auto &expected = m_pointShadowPersistentExpectedLights[light];
      const auto &actual = m_pointShadowData[light];
      if (m_pointShadowPublishedLightIds[light] != expected.id ||
          !PointShadowF32Exact(actual.lightPos.x, expected.position.x) ||
          !PointShadowF32Exact(actual.lightPos.y, expected.position.y) ||
          !PointShadowF32Exact(actual.lightPos.z, expected.position.z) ||
          !PointShadowF32Exact(actual.lightPos.w,
                               std::max(expected.position.w, 1.0f)) ||
          !PointShadowF32Exact(
              actual.shadowIntensity,
              std::clamp(expected.shadowIntensity, 0.0f, 1.0f)))
        return false;
    }
    return true;
  }

  if (proposal.disposition != War3PointShadowCpuPlanDisposition::Render)
    return false;
  if (!m_pointShadowCpuPlan.ready || m_pointShadowCpuPlan.failed ||
      m_pointShadowCpuPlan.shouldRender != proposal.shouldRender ||
      m_pointShadowCpuPlan.reusePublished != proposal.reusePublished ||
      m_pointShadowCpuPlan.forceFullFaceUpdate !=
          proposal.forceFullFaceUpdate ||
      m_pointShadowCpuPlan.shadowLightCount != proposal.shadowLightCount ||
      m_pointShadowCpuPlan.resourceCapacityLights !=
          proposal.resourceCapacityLights ||
      m_pointShadowCpuPlan.maxFacesPerFrame != proposal.maxFacesPerFrame ||
      m_pointShadowCpuPlan.resolution != proposal.resolution ||
      m_pointShadowCpuPlan.maxCastersPerFace !=
          proposal.maxCastersPerFace ||
      m_pointShadowCpuPlan.contentSignature != proposal.contentSignature ||
      m_pointShadowCpuPlan.updateMask != proposal.updateMask ||
      m_pointShadowCpuPlan.faceCandidateCount !=
          proposal.faceCandidateCount ||
      m_pointShadowCpuPlan.faceKeptCount != proposal.faceKeptCount ||
      m_pointShadowCpuPlan.faceDroppedCount != proposal.faceDroppedCount ||
      m_pointShadowTemporalAge != proposal.nextTemporalAge)
    return false;

  for (uint32_t light = 0u; light < proposal.shadowLightCount; ++light) {
    const auto &expected = proposal.lights[light];
    const auto &actual = m_pointShadowData[light];
    if (!PointShadowF32Exact(actual.lightPos.x,
                             expected.lightPositionRange.x) ||
        !PointShadowF32Exact(actual.lightPos.y,
                             expected.lightPositionRange.y) ||
        !PointShadowF32Exact(actual.lightPos.z,
                             expected.lightPositionRange.z) ||
        !PointShadowF32Exact(actual.lightPos.w,
                             expected.lightPositionRange.w) ||
        !PointShadowF32Exact(actual.shadowIntensity,
                             expected.shadowIntensity))
      return false;
    for (uint32_t face = 0u; face < 6u; ++face) {
      if (!PointShadowMatrixExact(
              FreezePointShadowMatrix(actual.faceViewProj[face]),
              expected.faceViewProjection[face]))
        return false;
    }
  }
  for (size_t face = 0u; face < proposal.storage.faceCasters.size(); ++face) {
    if (m_pointShadowCpuPlan.faceCasters[face] !=
        proposal.storage.faceCasters[face])
      return false;
  }
  return true;
}

void War3ShadowReceiverPass::beginPointShadowCpuPrepare(
    const War3PipelineInput &input,
    const War3PointLightFrameSnapshot &lightSnapshot,
    const std::vector<const War3ShadowCasterDraw *> *replayDraws) {
  waitPointShadowCpuPrepare();
  resetPointShadowCpuPlanPreservingCapacity();
  // Name the attempted snapshot before std::async. If launch or worker
  // execution fails, renderPointShadow can fail closed for this exact frame
  // without mistaking the failed plan for an old point-only plan that merely
  // needs synchronous preparation.
  m_pointShadowCpuPlan.lightGeneration = lightSnapshot.generation;
  m_pointShadowCpuPlan.lightFrameSerial = lightSnapshot.frameSerial;
  if (War3WorkerPrepareEnabled() &&
      PointShadowPersistentMode() != War3PointShadowPersistentMode::Off) {
    beginPointShadowPersistentPrepare(input, lightSnapshot, replayDraws);
    return;
  }
  if (!War3WorkerPrepareEnabled() || !replayDraws || replayDraws->empty())
    return;
  if (!input.settings || !input.settings->shadows.pointLightsEnabled ||
      !input.settings->shadows.pointShadowEnabled ||
      input.settings->shadows.pointShadowMaxLights == 0u ||
      !lightSnapshot.hasAny)
    return;

  m_pointShadowPrepareRunning.store(true, std::memory_order_release);
  // Capture settings/frame/camera and the immutable frame light snapshot.
  // replayDraws remains valid because Run owns a scope-exit wait guard.
  //
  // 2026-07-21 优化：原实现先按值拷贝整个 War3PipelineInput（caster 大结构
  // vector + 每条约 16 KB 的 shadowPalettes 矩阵），lambda 再按值捕获第二份；
  // 而 preparePointShadowCpuPlan 只读 settings、3 个 shadowStats 字段与
  // palette hash。现改为构造小 POD 并 move 进 lambda，每帧省两次 MB 级深拷贝。
  War3PointShadowCpuPlanInput workerInput;
  if (input.settings)
    workerInput.settings = *input.settings;
  workerInput.frameSerial = input.frameSerial;
  workerInput.dynamicPoseSignature = input.scene.shadowStats.dynamicPoseSignature;
  workerInput.dynamicPoseCount = input.scene.shadowStats.dynamicPoseCount;
  workerInput.dynamicSkinnedOutputCount =
      input.scene.shadowStats.dynamicSkinnedOutputCount;
  workerInput.paletteHashes.reserve(input.scene.shadowPalettes.size());
  for (const auto &palette : input.scene.shadowPalettes)
    workerInput.paletteHashes.push_back(palette.hash);
  // worker 路径总是带 draws，无需 sceneForReplayFallback。
  const auto *draws = replayDraws;
  try {
    m_pointShadowPrepareFuture = std::async(
        std::launch::async,
        [this, workerInput = std::move(workerInput), lightSnapshot, draws]() {
          auto scope = war3::War3PerfMonitor::instance().scope(
              "PointShadow/WorkerPrepare", Rc<DxvkCommandList>());
          preparePointShadowCpuPlan(workerInput, lightSnapshot, draws);
        });
  } catch (const std::exception &e) {
    m_pointShadowPrepareRunning.store(false, std::memory_order_release);
    m_pointShadowCpuPlan = {};
    m_pointShadowCpuPlan.ready = true;
    m_pointShadowCpuPlan.failed = true;
    m_pointShadowCpuPlan.lightGeneration = lightSnapshot.generation;
    m_pointShadowCpuPlan.lightFrameSerial = lightSnapshot.frameSerial;
    invalidatePointShadowPublishedState();
    static uint32_t s_launchFailureLogs = 0u;
    if (s_launchFailureLogs++ < 16u || (s_launchFailureLogs % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK PointShadow: failed to launch Worker_Prepare; skip frame (%s)\n",
          e.what());
    }
  } catch (...) {
    m_pointShadowPrepareRunning.store(false, std::memory_order_release);
    m_pointShadowCpuPlan = {};
    m_pointShadowCpuPlan.ready = true;
    m_pointShadowCpuPlan.failed = true;
    m_pointShadowCpuPlan.lightGeneration = lightSnapshot.generation;
    m_pointShadowCpuPlan.lightFrameSerial = lightSnapshot.frameSerial;
    invalidatePointShadowPublishedState();
  }
}

// Keep the whole point-shadow D32 cube array within a predictable 96 MiB
// budget. This permits four 1024 cubes (24 MiB each), while 2048 is deliberately
// restricted to one light. A user selecting more lights still receives direct
// lighting; only the shadowed prefix is budget-clamped.
constexpr uint64_t kPointShadowResourceBudgetBytes = 96ull * 1024ull * 1024ull;

uint32_t ResolvePointShadowCapacity(uint32_t resolution,
                                    uint32_t requestedLights) {
  resolution = std::clamp<uint32_t>(resolution, 128u, 2048u);
  requestedLights =
      std::clamp<uint32_t>(requestedLights, 1u,
                           War3PointLightFrameSnapshot::kMaxShadowLights);
  const uint64_t bytesPerLight = uint64_t(resolution) * uint64_t(resolution) *
                                 4ull * 6ull;
  const uint64_t budgetLights =
      std::max<uint64_t>(1ull, kPointShadowResourceBudgetBytes / bytesPerLight);
  return std::min<uint32_t>(requestedLights, static_cast<uint32_t>(
                                                std::min<uint64_t>(
                                                    budgetLights,
                                                    War3PointLightFrameSnapshot::
                                                        kMaxShadowLights)));
}

// 点光 Cube Shadow CPU 计划：签名 / face budget / range+face caster 列表。
// 可在 worker 线程执行；禁止触碰 GPU 命令与 Vulkan 资源。
bool War3ShadowReceiverPass::preparePointShadowCpuPlan(
    const War3PointShadowCpuPlanInput &input,
    const War3PointLightFrameSnapshot &lightSnapshot,
    const std::vector<const War3ShadowCasterDraw *> *replayDrawsOverride) {
  auto cpuScope =
      war3::War3PerfMonitor::instance().scope("PointShadow/PrepareCpu",
                                              Rc<DxvkCommandList>());
  resetPointShadowCpuPlanPreservingCapacity();
  m_pointShadowCpuPlan.ready = true;

  const War3RenderSettings *settings = &input.settings;
  m_pointShadowCpuPlan.lightGeneration = lightSnapshot.generation;
  m_pointShadowCpuPlan.lightFrameSerial = lightSnapshot.frameSerial;
  if (!settings->shadows.pointLightsEnabled ||
      !settings->shadows.pointShadowEnabled ||
      settings->shadows.pointShadowMaxLights == 0u ||
      !lightSnapshot.hasAny) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return false;
  }

  const std::vector<const War3ShadowCasterDraw *> *replayDrawsPtr =
      replayDrawsOverride;
  if (replayDrawsPtr == nullptr && input.sceneForReplayFallback != nullptr) {
    replayDrawsPtr = &BuildShadowReplayDraws(*input.sceneForReplayFallback,
                                             input.frameSerial);
  }
  if (replayDrawsPtr == nullptr) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return false;
  }
  const auto &replayDraws = *replayDrawsPtr;
  if (replayDraws.empty()) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return false;
  }

  const bool faceCulling = settings->shadows.pointShadowFaceCulling;
  const float cullPadding =
      std::max(1.0f, settings->shadows.pointShadowCasterCullPadding);
  const uint32_t maxCastersPerFace =
      settings->shadows.pointShadowMaxCastersPerFace;
  const uint32_t pointShadowResolution =
      std::clamp<uint32_t>(settings->shadows.pointShadowResolution, 128u, 2048u);
  const uint32_t requestedPointShadowLights = std::clamp<uint32_t>(
      settings->shadows.pointShadowMaxLights, 1u, kMaxPointShadowLights);
  const uint32_t pointShadowCapacityLights = ResolvePointShadowCapacity(
      pointShadowResolution, requestedPointShadowLights);
  if (pointShadowCapacityLights < requestedPointShadowLights) {
    static uint32_t s_budgetClampLogs = 0u;
    if (s_budgetClampLogs++ < 8u || (s_budgetClampLogs % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK PointShadow: memory budget clamps %u requested shadow lights "
          "to %u at %u resolution (budget=96 MiB)\n",
          requestedPointShadowLights, pointShadowCapacityLights,
          pointShadowResolution);
    }
  }
  const uint32_t shadowLightCount = std::min<uint32_t>(
      {kMaxPointShadowLights, pointShadowCapacityLights,
       lightSnapshot.shadowCount});
  if (shadowLightCount == 0u) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return false;
  }

  const uint32_t configuredMaxFacesPerFrame =
      settings->shadows.pointShadowMaxFacesPerFrame;
  const bool needsIncrementalContentSignature =
      (settings->shadows.pointShadowTemporalReuse &&
       settings->shadows.pointShadowUpdatePeriod > 1u) ||
      (configuredMaxFacesPerFrame > 0u &&
       configuredMaxFacesPerFrame < 6u);

  uint64_t contentSignature = 0xcbf29ce484222325ull;
  auto mixU64 = [&](uint64_t v) {
    contentSignature ^= v + 0x9e3779b97f4a7c15ull + (contentSignature << 6) +
                        (contentSignature >> 2);
  };
  auto mixF32 = [&](float v) {
    uint32_t bits = 0u;
    std::memcpy(&bits, &v, sizeof(bits));
    mixU64(bits);
  };
  auto mixMatrix = [&](const Matrix4 &matrix) {
    for (uint32_t row = 0u; row < 4u; ++row) {
      mixF32(matrix[row].x);
      mixF32(matrix[row].y);
      mixF32(matrix[row].z);
      mixF32(matrix[row].w);
    }
  };
  auto mixHandle = [&](auto handle) {
    uint64_t bits = 0u;
    static_assert(sizeof(handle) <= sizeof(bits));
    std::memcpy(&bits, &handle, sizeof(handle));
    mixU64(bits);
  };
  mixU64(shadowLightCount);
  // A manager mutation is an explicit light-content generation boundary. Even
  // if authored values happen to compare bit-identical, do not silently carry
  // a cube publication across that boundary: force a complete six-face update
  // and publish the new generation only after recording succeeds.
  mixU64(lightSnapshot.generation);
  // Resource/selection policy is part of the history contract. In particular,
  // a resolution change recreates the image and therefore requires all six
  // faces to be initialized before temporal face budgeting can resume.
  mixU64(pointShadowResolution);
  mixU64(pointShadowCapacityLights);
  mixU64(faceCulling ? 1u : 0u);
  mixU64(maxCastersPerFace);
  mixF32(cullPadding);
  for (uint32_t i = 0; i < shadowLightCount; ++i) {
    const auto &L = lightSnapshot.lights[i];
    mixU64(static_cast<uint64_t>(L.id));
    mixF32(L.position.x);
    mixF32(L.position.y);
    mixF32(L.position.z);
    mixF32(L.position.w);
    mixF32(L.params.x);
  }
  bool hasDynamicCaster = false;
  if (needsIncrementalContentSignature) {
    // Only history-reusing modes need an O(casters + palettes) content hash.
    // The quality default redraws all six faces every frame, so hashing the
    // same complete input first cannot affect its output or validity.
    mixU64(static_cast<uint64_t>(replayDraws.size()));
    mixU64(settings->shadows.alphaShadowHashed ? 1u : 0u);
    mixU64(input.dynamicPoseSignature);
    for (const uint64_t paletteHash : input.paletteHashes)
      mixU64(paletteHash);
    hasDynamicCaster = input.dynamicPoseCount > 0u ||
                       input.dynamicSkinnedOutputCount > 0u;
    for (const auto *drawPtr : replayDraws) {
      const auto &d = *drawPtr;
      mixF32(d.boundsCenter.x);
      mixF32(d.boundsCenter.y);
      mixF32(d.boundsCenter.z);
      mixF32(d.boundsRadius);
      mixU64(d.vertexCount);
      mixU64(d.indexCount);
      mixU64(d.indexed ? 1u : 0u);
      mixU64(static_cast<uint64_t>(d.positionFormat));
      mixU64(d.positionStride);
      mixU64(d.positionOffset);
      mixU64(static_cast<uint64_t>(d.indexType));
      mixU64(d.vertexBlendEnabled ? 1u : 0u);
      mixU64(d.vertexBlendIndexed ? 1u : 0u);
      mixU64(d.vertexBlendCount);
      mixU64(d.paletteIndex);
      mixU64(static_cast<uint64_t>(d.blendWeightFormat));
      mixU64(d.blendWeightOffset);
      mixU64(static_cast<uint64_t>(d.blendIndexFormat));
      mixU64(d.blendIndexOffset);
      mixU64(d.blendBinding);
      mixU64(d.blendStride);
      mixU64(static_cast<uint64_t>(d.topology));
      mixU64(d.firstIndex);
      mixU64(static_cast<uint32_t>(d.vertexOffset));
      mixU64(d.firstVertex);
      mixU64(d.minVertexIndex);
      mixU64(d.numVertices);
      mixU64(reinterpret_cast<uintptr_t>(d.positionStorage.ptr()));
      mixHandle(d.positionInfo.buffer);
      mixU64(d.positionInfo.offset);
      mixU64(d.positionInfo.size);
      mixU64(reinterpret_cast<uintptr_t>(d.indexStorage.ptr()));
      mixHandle(d.indexInfo.buffer);
      mixU64(d.indexInfo.offset);
      mixU64(d.indexInfo.size);
      mixU64(reinterpret_cast<uintptr_t>(d.blendStorage.ptr()));
      mixHandle(d.blendInfo.buffer);
      mixU64(d.blendInfo.offset);
      mixU64(d.blendInfo.size);
      mixU64(d.alphaTestEnabled ? 1u : 0u);
      mixU64(d.alphaBlendEnabled ? 1u : 0u);
      mixF32(d.alphaRef);
      mixU64(static_cast<uint64_t>(d.uvFormat));
      mixU64(d.uvStride);
      mixU64(d.uvOffset);
      mixU64(d.uvBinding);
      mixU64(reinterpret_cast<uintptr_t>(d.uvStorage.ptr()));
      mixHandle(d.uvInfo.buffer);
      mixU64(d.uvInfo.offset);
      mixU64(d.uvInfo.size);
      mixU64(reinterpret_cast<uintptr_t>(d.diffuseTexture.ptr()));
      mixU64(reinterpret_cast<uintptr_t>(d.diffuseSampler.ptr()));
      mixU64(d.diffuseSamplerIndex);
      mixMatrix(d.worldMatrix);
      hasDynamicCaster = hasDynamicCaster || d.vertexBlendEnabled;
    }
  }
  // Keep the exact current signature even when temporal reuse skips GPU work.
  // The receiver uses this plan-vs-publication tuple to reject an old cube after
  // worker launch failure, empty replay, resource failure, or light reordering.
  m_pointShadowCpuPlan.contentSignature = contentSignature;

  const bool signatureUnchanged =
      m_pointShadowCube && m_pointShadowReadyCount > 0u &&
      contentSignature == m_pointShadowContentSignature;
  const bool temporalReuse = settings->shadows.pointShadowTemporalReuse &&
                             settings->shadows.pointShadowUpdatePeriod > 1u &&
                             signatureUnchanged && !hasDynamicCaster;
  if (temporalReuse) {
    const uint32_t period =
        std::max(1u, settings->shadows.pointShadowUpdatePeriod);
    if ((m_pointShadowTemporalAge + 1u) < period) {
      ++m_pointShadowTemporalAge;
      m_pointShadowCpuPlan.shouldRender = false;
      m_pointShadowCpuPlan.reusePublished = true;
      return false;
    }
  }

  // A moving rigid transform changes the signature above. Skinned/dynamic-pose
  // casters additionally force all six faces every frame because their vertex
  // output may change even when conservative bounds and palette identity do not.
  const bool forceFullFaceUpdate = !signatureUnchanged || hasDynamicCaster;
  m_pointShadowTemporalAge = 0;

  uint32_t maxFacesPerFrame = configuredMaxFacesPerFrame;
  if (maxFacesPerFrame == 0u || maxFacesPerFrame >= 6u || forceFullFaceUpdate)
    maxFacesPerFrame = 6u;

  m_pointShadowCpuPlan.shouldRender = true;
  m_pointShadowCpuPlan.forceFullFaceUpdate = forceFullFaceUpdate;
  m_pointShadowCpuPlan.shadowLightCount = shadowLightCount;
  m_pointShadowCpuPlan.resourceCapacityLights = pointShadowCapacityLights;
  m_pointShadowCpuPlan.maxFacesPerFrame = maxFacesPerFrame;
  m_pointShadowCpuPlan.resolution = pointShadowResolution;
  m_pointShadowCpuPlan.maxCastersPerFace = maxCastersPerFace;

  auto casterInLightRange = [&](const War3ShadowCasterDraw &draw,
                                const Vector4 &lightPosRange) -> bool {
    // 0/negative/NaN 都表示 unknown bounds；不得凭 world origin + 猜测半径
    // 做 range cull，否则会在进入 face 保守路径前丢掉真实 caster。
    if (!(draw.boundsRadius > 0.0f))
      return true;
    const float range = std::max(lightPosRange.w, 1.0f) * cullPadding;
    const float cx = draw.boundsCenter.x;
    const float cy = draw.boundsCenter.y;
    const float cz = draw.boundsCenter.z;
    const float dx = cx - lightPosRange.x;
    const float dy = cy - lightPosRange.y;
    const float dz = cz - lightPosRange.z;
    const float reach = range + draw.boundsRadius;
    return dx * dx + dy * dy + dz * dz <= reach * reach;
  };

  auto casterOnFace = [&](const War3ShadowCasterDraw &draw,
                          const Vector4 &lightPosRange,
                          const Vector4 &faceDir,
                          const Vector4 &faceUp,
                          const Vector4 &faceRight) -> bool {
    if (!faceCulling)
      return true;
    if (!(draw.boundsRadius > 0.0f))
      return true;
    const float cx = draw.boundsCenter.x;
    const float cy = draw.boundsCenter.y;
    const float cz = draw.boundsCenter.z;
    const float dx = cx - lightPosRange.x;
    const float dy = cy - lightPosRange.y;
    const float dz = cz - lightPosRange.z;
    const float radius = draw.boundsRadius;

    const float viewX = dx * faceRight.x + dy * faceRight.y + dz * faceRight.z;
    const float viewY = dx * faceUp.x + dy * faceUp.y + dz * faceUp.z;
    const float viewZ = dx * faceDir.x + dy * faceDir.y + dz * faceDir.z;
    if (viewZ < -radius)
      return false;

    // 90-degree square pyramid: z +/- x >= 0 and z +/- y >= 0.
    // Plane normals have length sqrt(2), so expand each side by r*sqrt(2)
    // for a conservative sphere/frustum test. This removes off-face casters
    // before the nearest-N cap without clipping bounds that cross a seam.
    constexpr float kSqrt2 = 1.41421356237f;
    const float sideMargin = radius * kSqrt2;
    return std::abs(viewX) <= viewZ + sideMargin &&
           std::abs(viewY) <= viewZ + sideMargin;
  };

  for (uint32_t lightIndex = 0; lightIndex < shadowLightCount; ++lightIndex) {
    const War3PointLight &light = lightSnapshot.lights[lightIndex];
    const float range = std::max(light.position.w, 1.0f);
    auto &shadowData = m_pointShadowData[lightIndex];
    shadowData.lightPos =
        Vector4(light.position.x, light.position.y, light.position.z, range);
    shadowData.shadowIntensity = std::clamp(light.params.x, 0.0f, 1.0f);

    const float nearZ = 1.0f;
    const float farZ = std::max(range, nearZ + 1.0f);
    const float fov = 1.5707963f;
    const float tanHalf = std::tan(fov * 0.5f);
    Matrix4 proj;
    // Vulkan cube sampling expects the horizontal axis of every face to be
    // opposite to the one produced by our row-vector LH look-at basis.  The
    // usual cube face direction/up table is otherwise correct, so flip clip X
    // once in the projection instead of reflecting world-space sample rays.
    proj[0] = Vector4(-1.0f / tanHalf, 0.0f, 0.0f, 0.0f);
    proj[1] = Vector4(0.0f, 1.0f / tanHalf, 0.0f, 0.0f);
    proj[2] = Vector4(0.0f, 0.0f, farZ / (farZ - nearZ), 1.0f);
    proj[3] = Vector4(0.0f, 0.0f, -(nearZ * farZ) / (farZ - nearZ), 0.0f);

    Vector4 eye = Vector4(shadowData.lightPos.x, shadowData.lightPos.y,
                          shadowData.lightPos.z, 1.0f);
    for (int face = 0; face < 6; face++) {
      Vector4 target = Vector4(eye.x + kPointShadowFaceParams[face].dir.x,
                               eye.y + kPointShadowFaceParams[face].dir.y,
                               eye.z + kPointShadowFaceParams[face].dir.z, 1.0f);
      Matrix4 view =
          MakeLookAtLH(eye, target, kPointShadowFaceParams[face].up);
      // Matrix4::operator* stores the raw product in reverse order. Match the
      // camera/CSM convention (`proj * view` in C++) so the row-vector shader
      // receives raw view*proj; the old order projected even a face-forward
      // point outside its cube-face frustum whenever the light was translated.
      shadowData.faceViewProj[face] = proj * view;
    }
    if (forceFullFaceUpdate)
      m_pointShadowFaceValidMask[lightIndex] = 0;

    // range 预过滤
    m_pointShadowCasterIndicesScratch.clear();
    m_pointShadowCasterIndicesScratch.reserve(replayDraws.size());
    for (uint32_t drawIdx = 0; drawIdx < replayDraws.size(); ++drawIdx) {
      const auto &draw = *replayDraws[drawIdx];
      if (draw.positionInfo.buffer == VK_NULL_HANDLE ||
          draw.positionInfo.size == 0)
        continue;
      if (draw.indexed &&
          (draw.indexInfo.buffer == VK_NULL_HANDLE || draw.indexInfo.size == 0))
        continue;
      if (!casterInLightRange(draw, shadowData.lightPos))
        continue;
      m_pointShadowCasterIndicesScratch.push_back(drawIdx);
    }

    uint8_t updateMask = 0;
    if (maxFacesPerFrame >= 6u) {
      updateMask = 0x3Fu;
    } else {
      for (uint32_t pick = 0; pick < maxFacesPerFrame; ++pick) {
        int bestFace = -1;
        uint32_t bestAge = 0;
        for (uint32_t face = 0; face < 6u; ++face) {
          if (updateMask & (1u << face))
            continue;
          const bool invalid =
              (m_pointShadowFaceValidMask[lightIndex] & (1u << face)) == 0;
          const uint32_t age = m_pointShadowFaceAge[lightIndex][face] +
                               (invalid ? 1000u : 0u);
          if (bestFace < 0 || age > bestAge) {
            bestFace = int(face);
            bestAge = age;
          }
        }
        if (bestFace < 0)
          break;
        updateMask |= uint8_t(1u << bestFace);
      }
    }
    m_pointShadowCpuPlan.updateMask[lightIndex] = updateMask;

    for (uint32_t face = 0; face < 6u; ++face) {
      const size_t facePlanIndex = lightIndex * 6u + face;
      auto &outFace = m_pointShadowCpuPlan.faceCasters[facePlanIndex];
      outFace.clear();
      if ((updateMask & (1u << face)) == 0u)
        continue;

      // Quality-default unlimited mode keeps every face-visible caster. The
      // source indices are already ascending and the capped path sorts back
      // to that same replay order, so distance ranking/partition/sort would be
      // pure overhead here (including one sqrt per caster per cube face).
      if (maxCastersPerFace == 0u) {
        outFace.reserve(m_pointShadowCasterIndicesScratch.size());
        for (uint32_t drawIdx : m_pointShadowCasterIndicesScratch) {
          const auto &draw = *replayDraws[drawIdx];
          if (casterOnFace(draw, shadowData.lightPos,
                           kPointShadowFaceParams[face].dir,
                           kPointShadowFaceParams[face].up,
                           kPointShadowFaceParams[face].right))
            outFace.push_back(drawIdx);
        }
        const uint32_t keptCount = static_cast<uint32_t>(
            std::min<size_t>(outFace.size(),
                             std::numeric_limits<uint32_t>::max()));
        m_pointShadowCpuPlan.faceCandidateCount[facePlanIndex] = keptCount;
        m_pointShadowCpuPlan.faceKeptCount[facePlanIndex] = keptCount;
        m_pointShadowCpuPlan.faceDroppedCount[facePlanIndex] = 0u;
        continue;
      }

      struct Cand {
        uint32_t idx = 0;
        // Rank known bounds by nearest surface rather than center distance.
        // This keeps large buildings/terrain chunks from being displaced by a
        // small object's closer center even when the large caster reaches much
        // nearer to the light and covers more of the face.
        float surfaceDistSq = 0.0f;
        bool pinned = false;
      };
      thread_local std::vector<Cand> s_cands;
      s_cands.clear();
      s_cands.reserve(m_pointShadowCasterIndicesScratch.size());
      for (uint32_t drawIdx : m_pointShadowCasterIndicesScratch) {
        const auto &draw = *replayDraws[drawIdx];
        if (!casterOnFace(draw, shadowData.lightPos,
                          kPointShadowFaceParams[face].dir,
                          kPointShadowFaceParams[face].up,
                          kPointShadowFaceParams[face].right))
          continue;
        const bool unknownBounds = !(draw.boundsRadius > 0.0f);
        const float cx = unknownBounds ? draw.worldMatrix[3].x
                                       : draw.boundsCenter.x;
        const float cy = unknownBounds ? draw.worldMatrix[3].y
                                       : draw.boundsCenter.y;
        const float cz = unknownBounds ? draw.worldMatrix[3].z
                                       : draw.boundsCenter.z;
        const float dx = cx - shadowData.lightPos.x;
        const float dy = cy - shadowData.lightPos.y;
        const float dz = cz - shadowData.lightPos.z;
        const float centerDist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float surfaceDist = unknownBounds
                                      ? 0.0f
                                      : std::max(centerDist - draw.boundsRadius,
                                                 0.0f);
        // Unknown bounds cannot be proven unimportant and therefore never
        // consume the known-caster cap. They remain pinned in the face list.
        s_cands.push_back(Cand{drawIdx, surfaceDist * surfaceDist,
                               unknownBounds});
      }
      const uint32_t candidateCount = static_cast<uint32_t>(
          std::min<size_t>(s_cands.size(),
                           std::numeric_limits<uint32_t>::max()));
      m_pointShadowCpuPlan.faceCandidateCount[facePlanIndex] = candidateCount;
      // Keep unknown bounds unconditionally, but still cap the known subset.
      // The previous all-or-nothing rule disabled the cap for an entire face as
      // soon as one unknown draw appeared.
      const auto knownBegin = std::stable_partition(
          s_cands.begin(), s_cands.end(),
          [](const Cand &cand) { return cand.pinned; });
      const size_t pinnedCount = size_t(knownBegin - s_cands.begin());
      const size_t knownCount = s_cands.size() - pinnedCount;
      if (maxCastersPerFace > 0u && knownCount > maxCastersPerFace) {
        const auto keepEnd = knownBegin + maxCastersPerFace;
        std::nth_element(
            knownBegin, keepEnd, s_cands.end(), [](const Cand &a, const Cand &b) {
              if (a.surfaceDistSq != b.surfaceDistSq)
                return a.surfaceDistSq < b.surfaceDistSq;
              return a.idx < b.idx;
            });
        s_cands.resize(pinnedCount + maxCastersPerFace);
      }
      // nth_element 会打乱候选，提交前恢复 replay draw 顺序。
      std::sort(s_cands.begin(), s_cands.end(),
                [](const Cand &a, const Cand &b) { return a.idx < b.idx; });
      const uint32_t keptCount = static_cast<uint32_t>(
          std::min<size_t>(s_cands.size(),
                           std::numeric_limits<uint32_t>::max()));
      m_pointShadowCpuPlan.faceKeptCount[facePlanIndex] = keptCount;
      m_pointShadowCpuPlan.faceDroppedCount[facePlanIndex] =
          candidateCount - std::min(candidateCount, keptCount);
      outFace.reserve(s_cands.size());
      for (const auto &c : s_cands)
        outFace.push_back(c.idx);
    }
  }

  return true;
}

// 点光 Cube Shadow：与 CSM 完全隔离的附加路径。
// CPU 策略：
// 1) 灯光快照复用（无 vector 堆分配）
// 2) 每灯一次 range 预过滤 caster
// 3) 每 face 90° square-pyramid 保守球体剔除；只有显式性能 cap 才近距截断
// 4) 签名稳定时隔帧复用 cube depth（不重画）
// 5) A2：可与 CSM GPU 录制重叠的 Worker_Prepare
void War3ShadowReceiverPass::renderPointShadow(
    const Rc<DxvkCommandList> &ctx, const War3PipelineInput &input,
    const War3PointLightFrameSnapshot &lightSnapshot,
    const std::vector<const War3ShadowCasterDraw *> *replayDrawsOverride) {
  std::vector<const war3::render::War3TrackedVkPipeline*>
      trackedCasterPipelines;
  const auto trackCasterPipeline = [&] (const ShadowCasterPipeline& pipeline) {
    if (!pipeline.lifetime)
      return;
    const auto* owner = pipeline.lifetime.ptr();
    if (std::find(trackedCasterPipelines.begin(),
                  trackedCasterPipelines.end(), owner) !=
        trackedCasterPipelines.end())
      return;
    ctx->track(pipeline.lifetime);
    trackedCasterPipelines.push_back(owner);
  };

  war3::tools::SetGpuFlightBreadcrumb(
      war3::tools::GpuFlightBreadcrumb::PointShadowPlan);
  war3::tools::ResetGpuFlightPointShadowWork(0u);
  auto perfScope = war3::War3PerfMonitor::instance().scope("PointShadow", ctx);

  waitPointShadowCpuPrepare();
  const War3PointShadowPersistentMode persistentMode =
      PointShadowPersistentMode();
  std::optional<PointShadowPersistentResultPayload> persistentProposal;
  if (persistentMode != War3PointShadowPersistentMode::Off) {
    persistentProposal = tryCollectPointShadowPersistentProposal(
        input, lightSnapshot, replayDrawsOverride);
    if (persistentMode == War3PointShadowPersistentMode::Consume &&
        persistentProposal.has_value()) {
      if (adoptPointShadowPersistentProposal(*persistentProposal,
                                              lightSnapshot)) {
        persistentProposal.reset();
      } else {
        recyclePointShadowPersistentStorage(
            std::move(persistentProposal->storage));
        persistentProposal.reset();
        ++m_pointShadowPersistentRejectedFallback;
      }
    }
  }
  const bool planNamesCurrentSnapshot =
      m_pointShadowCpuPlan.ready &&
      m_pointShadowCpuPlan.lightGeneration == lightSnapshot.generation &&
      m_pointShadowCpuPlan.lightFrameSerial == lightSnapshot.frameSerial;
  // Worker_Prepare normally refreshes the plan while CSM records. Point-only
  // frames do not enter that CSM block, so their prior plan must be rebuilt
  // synchronously instead of being replayed forever with stale light/caster
  // slots. A named failure for this exact frame remains fail-closed below.
  if (!planNamesCurrentSnapshot) {
    // 同步兜底路径：同样用小 POD 避免 War3PipelineInput 深拷贝（每次仅
    // settings 值拷贝 + palette hash 收集，远小于 scene 深拷贝）。
    War3PointShadowCpuPlanInput syncInput;
    if (input.settings)
      syncInput.settings = *input.settings;
    syncInput.frameSerial = input.frameSerial;
    syncInput.dynamicPoseSignature =
        input.scene.shadowStats.dynamicPoseSignature;
    syncInput.dynamicPoseCount = input.scene.shadowStats.dynamicPoseCount;
    syncInput.dynamicSkinnedOutputCount =
        input.scene.shadowStats.dynamicSkinnedOutputCount;
    syncInput.paletteHashes.reserve(input.scene.shadowPalettes.size());
    for (const auto &palette : input.scene.shadowPalettes)
      syncInput.paletteHashes.push_back(palette.hash);
    syncInput.sceneForReplayFallback = &input.scene;
    preparePointShadowCpuPlan(syncInput, lightSnapshot, replayDrawsOverride);
  }
  if (persistentMode == War3PointShadowPersistentMode::Observe &&
      persistentProposal.has_value()) {
    if (pointShadowPersistentProposalMatchesCanonical(*persistentProposal))
      ++m_pointShadowPersistentObserveMatch;
    else
      ++m_pointShadowPersistentObserveMismatch;
    const uint64_t observed = m_pointShadowPersistentObserveMatch +
        m_pointShadowPersistentObserveMismatch;
    if (observed <= 16u || (observed % 240u) == 0u) {
      const auto workerDiagnostics =
          m_pointShadowPersistentWorker
              ? m_pointShadowPersistentWorker->diagnostics()
              : war3::render::War3PointShadowPrepareWorkerDiagnostics{};
      WAR3_RENDER_LOG(
          "DXVK PointShadow: persistent Observe exact=%llu mismatch=%llu "
          "accepted=%llu deadlineFallback=%llu rejectedFallback=%llu "
          "workerReady=%llu workerFailed=%llu busy=%llu\n",
          static_cast<unsigned long long>(
              m_pointShadowPersistentObserveMatch),
          static_cast<unsigned long long>(
              m_pointShadowPersistentObserveMismatch),
          static_cast<unsigned long long>(m_pointShadowPersistentAccepted),
          static_cast<unsigned long long>(
              m_pointShadowPersistentDeadlineFallback),
          static_cast<unsigned long long>(
              m_pointShadowPersistentRejectedFallback),
          static_cast<unsigned long long>(workerDiagnostics.readyJobs),
          static_cast<unsigned long long>(workerDiagnostics.failedJobs),
          static_cast<unsigned long long>(workerDiagnostics.busyRejections));
    }
    recyclePointShadowPersistentStorage(
        std::move(persistentProposal->storage));
    persistentProposal.reset();
  }
  if (!m_pointShadowCpuPlan.ready) {
    invalidatePointShadowPublishedState();
    return;
  }
  if (m_pointShadowCpuPlan.failed) {
    invalidatePointShadowPublishedState();
    return;
  }
  if (m_pointShadowCpuPlan.lightGeneration != lightSnapshot.generation ||
      m_pointShadowCpuPlan.lightFrameSerial != lightSnapshot.frameSerial) {
    const bool hadNamedSnapshot =
        m_pointShadowCpuPlan.lightGeneration != 0u ||
        m_pointShadowCpuPlan.lightFrameSerial != 0u;
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    static uint32_t s_planSnapshotMismatchLogs = 0u;
    if (hadNamedSnapshot &&
        (s_planSnapshotMismatchLogs++ < 16u ||
         (s_planSnapshotMismatchLogs % 240u) == 0u)) {
      WAR3_RENDER_LOG(
          "DXVK PointShadow: reject stale CPU plan planGen=%llu "
          "snapshotGen=%llu planFrame=%llu snapshotFrame=%llu\n",
          static_cast<unsigned long long>(
              m_pointShadowCpuPlan.lightGeneration),
          static_cast<unsigned long long>(lightSnapshot.generation),
          static_cast<unsigned long long>(
              m_pointShadowCpuPlan.lightFrameSerial),
          static_cast<unsigned long long>(lightSnapshot.frameSerial));
    }
    return;
  }
  if (!m_pointShadowCpuPlan.shouldRender) {
    // Only an exact semantic match may intentionally reuse the old cube.
    // Failure/no-light plans carry no matching signature and therefore revoke
    // publication instead of pairing old face depths with the current lights.
    if (!pointShadowPublishedStateMatchesCurrentPlan())
      invalidatePointShadowPublishedState();
    return;
  }

  {
    uint64_t candidateTotal = 0u;
    uint64_t keptTotal = 0u;
    uint64_t droppedTotal = 0u;
    uint32_t hitFaces = 0u;
    uint32_t worstFaceIndex = 0u;
    uint32_t worstDropped = 0u;
    for (uint32_t faceIndex = 0u;
         faceIndex < m_pointShadowCpuPlan.faceCandidateCount.size();
         ++faceIndex) {
      const uint32_t candidates =
          m_pointShadowCpuPlan.faceCandidateCount[faceIndex];
      const uint32_t kept = m_pointShadowCpuPlan.faceKeptCount[faceIndex];
      const uint32_t dropped =
          m_pointShadowCpuPlan.faceDroppedCount[faceIndex];
      candidateTotal += candidates;
      keptTotal += kept;
      droppedTotal += dropped;
      hitFaces += dropped > 0u ? 1u : 0u;
      if (dropped > worstDropped) {
        worstDropped = dropped;
        worstFaceIndex = faceIndex;
      }
    }

    static uint32_t s_facePlanDiagRuns = 0u;
    const uint32_t diagRun = s_facePlanDiagRuns++;
    const bool shouldLog = diagRun < 8u ||
        ((diagRun % 600u) == 0u) ||
        (droppedTotal > 0u && (diagRun % 120u) == 0u);
    if (shouldLog) {
      WAR3_RENDER_LOG(
          "DXVK PointShadow: face caster plan cap=%u candidates=%llu "
          "kept=%llu dropped=%llu hitFaces=%u worstLight=%u "
          "worstFace=%u worstDropped=%u\n",
          m_pointShadowCpuPlan.maxCastersPerFace,
          static_cast<unsigned long long>(candidateTotal),
          static_cast<unsigned long long>(keptTotal),
          static_cast<unsigned long long>(droppedTotal), hitFaces,
          worstFaceIndex / 6u, worstFaceIndex % 6u, worstDropped);
    }
  }

  War3RenderSettings defaultSettings = {};
  const War3RenderSettings *settings =
      input.settings ? input.settings.get() : &defaultSettings;
  const bool alphaShadowHashed = settings->shadows.alphaShadowHashed;

  const std::vector<const War3ShadowCasterDraw *> *replayDrawsPtr =
      replayDrawsOverride;
  if (replayDrawsPtr == nullptr) {
    replayDrawsPtr = &BuildShadowReplayDraws(input.scene, input.frameSerial);
  }
  const auto &replayDraws = *replayDrawsPtr;
  if (replayDraws.empty()) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return;
  }

  if (!validateShadowReplayDraws(input, replayDraws, "point-shadow")) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return;
  }

  const uint32_t shadowLightCount = m_pointShadowCpuPlan.shadowLightCount;
  war3::tools::ResetGpuFlightPointShadowWork(shadowLightCount);
  for (uint32_t light = 0u; light < shadowLightCount; ++light) {
    for (uint32_t face = 0u; face < 6u; ++face) {
      const uint32_t index = light * 6u + face;
      war3::tools::SetGpuFlightPointShadowFacePlan(
          light, face, m_pointShadowCpuPlan.faceCandidateCount[index],
          m_pointShadowCpuPlan.faceKeptCount[index]);
    }
  }
  const uint32_t pointShadowResolution = m_pointShadowCpuPlan.resolution;
  const uint32_t pointShadowCapacityLights =
      m_pointShadowCpuPlan.resourceCapacityLights;
  constexpr uint64_t kPointShadowResourceRetryRuns = 120u;
  const bool sameFailedRequest =
      m_pointShadowFailedResolution == pointShadowResolution &&
      m_pointShadowFailedCapacityLights == pointShadowCapacityLights;
  if (sameFailedRequest &&
      m_pointShadowRunSerial < m_pointShadowResourceRetryAfterSerial) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return;
  }
  if (!sameFailedRequest) {
    m_pointShadowFailedResolution = 0u;
    m_pointShadowFailedCapacityLights = 0u;
    m_pointShadowResourceRetryAfterSerial = 0u;
  }
  try {
    ensurePointShadowResources(pointShadowResolution,
                               pointShadowCapacityLights);
    m_pointShadowFailedResolution = 0u;
    m_pointShadowFailedCapacityLights = 0u;
    m_pointShadowResourceRetryAfterSerial = 0u;
  } catch (const DxvkError &e) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    m_pointShadowFailedResolution = pointShadowResolution;
    m_pointShadowFailedCapacityLights = pointShadowCapacityLights;
    m_pointShadowResourceRetryAfterSerial =
        m_pointShadowRunSerial + kPointShadowResourceRetryRuns;
    static uint32_t s_dxvkResourceFailureLogs = 0u;
    if (s_dxvkResourceFailureLogs++ < 16u ||
        (s_dxvkResourceFailureLogs % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK PointShadow: resource ensure failed at %u x %u lights; "
          "direct lighting remains active (%s)\n",
          pointShadowResolution, pointShadowCapacityLights,
          e.message().c_str());
    }
    return;
  } catch (const std::exception &e) {
    // Resource creation is optional. Preserve the rest of the receiver pass and
    // disable cube sampling for this frame rather than letting Pipeline disable
    // all directional shadows/outline after an allocation failure.
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    m_pointShadowFailedResolution = pointShadowResolution;
    m_pointShadowFailedCapacityLights = pointShadowCapacityLights;
    m_pointShadowResourceRetryAfterSerial =
        m_pointShadowRunSerial + kPointShadowResourceRetryRuns;
    static uint32_t s_resourceFailureLogs = 0u;
    if (s_resourceFailureLogs++ < 16u ||
        (s_resourceFailureLogs % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK PointShadow: resource ensure failed at %u x %u lights; "
          "direct lighting remains active (%s)\n",
          pointShadowResolution, pointShadowCapacityLights, e.what());
    }
    return;
  } catch (...) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    m_pointShadowFailedResolution = pointShadowResolution;
    m_pointShadowFailedCapacityLights = pointShadowCapacityLights;
    m_pointShadowResourceRetryAfterSerial =
        m_pointShadowRunSerial + kPointShadowResourceRetryRuns;
    return;
  }
  if (!m_pointShadowCube) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return;
  }
  if (shadowLightCount == 0u ||
      shadowLightCount > m_pointShadowCapacityLights) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return;
  }

  // Validate every face/caster/pipeline cohort before the first cube layer is
  // transitioned or cleared. A point-shadow candidate is one transaction:
  // missing views, stale plan indices, alpha payload gaps or pipeline failure
  // revoke publication instead of leaving a partially refreshed cube.
  bool pointReplayPlanComplete = true;
  for (uint32_t lightIndex = 0u;
       pointReplayPlanComplete && lightIndex < shadowLightCount;
       ++lightIndex) {
    const uint8_t updateMask = m_pointShadowCpuPlan.updateMask[lightIndex];
    for (uint32_t face = 0u; pointReplayPlanComplete && face < 6u; ++face) {
      if ((updateMask & (1u << face)) == 0u)
        continue;
      const uint32_t faceLayer = lightIndex * 6u + face;
      if (faceLayer >= m_pointShadowCapacityLights * 6u ||
          !m_pointShadowFaceViews[faceLayer]) {
        pointReplayPlanComplete = false;
        break;
      }
      const auto& faceCasters =
          m_pointShadowCpuPlan.faceCasters[lightIndex * 6u + face];
      for (uint32_t drawIdx : faceCasters) {
        if (drawIdx >= replayDraws.size() || replayDraws[drawIdx] == nullptr) {
          pointReplayPlanComplete = false;
          break;
        }
        const auto& draw = *replayDraws[drawIdx];
        if (draw.alphaBlendEnabled && !draw.alphaTestEnabled)
          continue;
        const bool alphaPayloadComplete =
            draw.diffuseTexture && draw.HasUsableUvBinding();
        if (draw.alphaTestEnabled && !alphaPayloadComplete) {
          pointReplayPlanComplete = false;
          break;
        }

        ShadowCasterPipelineKey key = {};
        key.positionFormat = draw.positionFormat;
        key.positionStride = draw.positionStride;
        key.positionOffset = draw.positionOffset;
        key.topology = draw.topology;
        key.pointShadowRadialDepth = true;
        key.blendWeightFormat =
            draw.vertexBlendEnabled && draw.vertexBlendCount > 0u
                ? draw.blendWeightFormat
                : VK_FORMAT_UNDEFINED;
        key.blendWeightOffset =
            key.blendWeightFormat != VK_FORMAT_UNDEFINED
                ? draw.blendWeightOffset
                : 0u;
        key.blendIndexFormat =
            draw.vertexBlendEnabled && draw.vertexBlendIndexed
                ? draw.blendIndexFormat
                : VK_FORMAT_UNDEFINED;
        key.blendIndexOffset =
            key.blendIndexFormat != VK_FORMAT_UNDEFINED
                ? draw.blendIndexOffset
                : 0u;
        key.blendBinding = draw.blendBinding;
        key.blendStride = draw.blendStride;
        key.alphaTestEnabled = draw.alphaTestEnabled;
        if (draw.alphaTestEnabled) {
          key.uvFormat = draw.uvFormat;
          key.uvOffset = draw.uvOffset;
          key.uvStride = draw.uvStride;
          key.uvBinding = draw.uvBinding;
        }
        const ShadowCasterPipeline pipeline = getShadowCasterPipeline(key);
        const ShadowGpuSkinDirectDecision direct =
            EvaluateShadowGpuSkinDirectInput(draw);
        if (pipeline.pipeline == VK_NULL_HANDLE ||
            (draw.gpuSkinInput.irreversible && direct.requested && !direct)) {
          pointReplayPlanComplete = false;
          break;
        }
      }
    }
  }
  if (!pointReplayPlanComplete) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    ++reconciliation.replayValidationRejectedCount;
    ++reconciliation.replayPartialPreventedCount;
    reconciliation.replayValidationLastReason = static_cast<uint32_t>(
        war3::render::War3ShadowReplayRejectReason::IncompleteReplayPlan);
    g_shadowReplayDiagnostics.validationRejectCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_shadowReplayDiagnostics.partialPreventedCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_shadowReplayDiagnostics.lastRejectReason.store(
        reconciliation.replayValidationLastReason,
        std::memory_order_release);
    return;
  }

  // A point update is one command-recording transaction. Build the cost of
  // every scheduled complete face first, then reserve the whole batch in one
  // atomic governor operation. Budget pressure may cancel the optional cube,
  // but it can never leave a prefix of faces recorded or published.
  war3::render::War3GpuWorkloadCost pointWorkloadCost = {};
  uint64_t pointWorkloadFaceCount = 0u;
  for (uint32_t lightIndex = 0u; lightIndex < shadowLightCount; ++lightIndex) {
    const uint8_t updateMask = m_pointShadowCpuPlan.updateMask[lightIndex];
    for (uint32_t face = 0u; face < 6u; ++face) {
      if ((updateMask & uint8_t(1u << face)) == 0u)
        continue;
      ++pointWorkloadFaceCount;
      const auto& faceCasters =
          m_pointShadowCpuPlan.faceCasters[lightIndex * 6u + face];
      for (const uint32_t drawIndex : faceCasters) {
        if (drawIndex >= replayDraws.size() ||
            replayDraws[drawIndex] == nullptr ||
            !AddWar3ShadowWorkloadDraw(pointWorkloadCost,
                                       *replayDraws[drawIndex], 1u)) {
          pointWorkloadCost.valid = false;
          break;
        }
      }
      if (!pointWorkloadCost.valid)
        break;
    }
    if (!pointWorkloadCost.valid)
      break;
  }
  ScopedWar3GpuWorkloadReservation pointWorkloadReservation(
      m_gpuWorkloadGovernor);
  if (!pointWorkloadReservation.reserve(
          war3::render::War3GpuWorkloadConsumer::PointShadow,
          pointWorkloadFaceCount, pointWorkloadCost)) {
    const bool heldLastComplete =
        holdPointShadowLastCompleteAfterBudgetReject(input, lightSnapshot,
                                                     *settings);
    if (!heldLastComplete) {
      invalidatePointShadowPublishedState();
      m_pointShadowCpuPlan.shouldRender = false;
    }
    m_gpuWorkloadGovernor.notePointShadowBudgetFallback(heldLastComplete);
    const auto& governor = m_gpuWorkloadGovernor.diagnostics();
    static uint32_t s_pointWorkloadRejectLogs = 0u;
    const uint32_t logIndex = s_pointWorkloadRejectLogs++;
    if (logIndex < 16u || (logIndex % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK War3GpuWorkload: reject point-shadow frame=%llu "
          "request(draw=%llu vertex=%llu index=%llu faces=%llu) "
          "used(draw=%llu vertex=%llu index=%llu) reason=%u held=%u\n",
          static_cast<unsigned long long>(input.frameSerial),
          static_cast<unsigned long long>(pointWorkloadCost.draws),
          static_cast<unsigned long long>(pointWorkloadCost.vertices),
          static_cast<unsigned long long>(pointWorkloadCost.indices),
          static_cast<unsigned long long>(pointWorkloadFaceCount),
          static_cast<unsigned long long>(governor.used.draws),
          static_cast<unsigned long long>(governor.used.vertices),
          static_cast<unsigned long long>(governor.used.indices),
          governor.lastRejectReason, heldLastComplete ? 1u : 0u);
    }
    return;
  }

  // 不整表清 ready：face budget 下未更新的 face 仍保留上一帧 depth。
  m_pointShadowReadyCount = 0;

  DxvkDescriptorWrite paletteDesc = {};
  paletteDesc.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  paletteDesc.buffer = ensureShadowMatrixBuffer(ctx, input, &replayDraws);
  if (paletteDesc.buffer.buffer == VK_NULL_HANDLE) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return;
  }
  const uint32_t objectBase = m_shadowMatrixObjectBase;

  // Face validity is CPU publication state, not merely command-recording
  // scratch. If any barrier/begin/draw/end operation throws after one face has
  // been marked valid but before the aggregate signature is committed, revoke
  // the whole cube so a later frame cannot reuse a partially recorded set.
  bool pointShadowPublicationCommitted = false;
  [[maybe_unused]] auto pointShadowPublicationRollback = MakeWar3ScopeExit([&]() {
    if (!pointShadowPublicationCommitted)
      invalidatePointShadowPublishedState();
  });

  // 定向阴影被关闭或复用时，点阴影可能成为第一个重放消费者。冻结旧缓冲和
  // VS-S1 palette 由 transfer 写入，共享 GPU 蒙皮输出页由 compute 写入；
  // 这里不能依赖 CSM 已经先建立 vertex/index/storage 读取可见性。
  {
    VkMemoryBarrier2 memBarrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT |
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT |
                               VK_ACCESS_2_SHADER_WRITE_BIT;
    memBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                              VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    memBarrier.dstAccessMask =
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
        VK_ACCESS_2_SHADER_READ_BIT;

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.memoryBarrierCount = 1u;
    depInfo.pMemoryBarriers = &memBarrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
  }

  const uint32_t resolution = pointShadowResolution;
  const uint32_t pointShadowCapacityLayerCount =
      m_pointShadowCapacityLights * 6u;
  if (m_pointShadowFaceLayouts[0].layout() == VK_IMAGE_LAYOUT_UNDEFINED) {
    bool allFacesUndefined = true;
    for (uint32_t layer = 0u; layer < pointShadowCapacityLayerCount; ++layer) {
      allFacesUndefined &= m_pointShadowFaceLayouts[layer].layout() ==
                           VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (!allFacesUndefined) {
      invalidatePointShadowPublishedState();
      m_pointShadowCpuPlan.shouldRender = false;
      return;
    }

    // The descriptor spans the full allocated CubeArray, so establish one
    // coherent sampled layout before any face is updated independently.
    const VkImageSubresourceRange fullCubeRange = {
        VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u,
        pointShadowCapacityLayerCount};
    const auto initialRead = m_pointShadowFaceLayouts[0].plan(
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT);
    const VkImageMemoryBarrier2 initialReadBarrier =
        war3::render::MakeWar3OwnedImageBarrier(
            initialRead, m_pointShadowCube->handle(), fullCubeRange);
    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1u;
    depInfo.pImageMemoryBarriers = &initialReadBarrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
    for (uint32_t layer = 0u; layer < pointShadowCapacityLayerCount; ++layer)
      m_pointShadowFaceLayouts[layer].commit(initialRead);
    m_pointShadowCube->trackLayout(
        fullCubeRange, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  }

  for (uint32_t layer = 0u; layer < pointShadowCapacityLayerCount; ++layer) {
    if (m_pointShadowFaceLayouts[layer].layout() !=
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
      invalidatePointShadowPublishedState();
      m_pointShadowCpuPlan.shouldRender = false;
      return;
    }
  }

  std::array<uint32_t, kMaxPointShadowLights * 6u> pointShadowWriteLayers = {};
  uint32_t pointShadowWriteLayerCount = 0u;
  for (uint32_t lightIndex = 0u; lightIndex < shadowLightCount; ++lightIndex) {
    const uint8_t updateMask = m_pointShadowCpuPlan.updateMask[lightIndex];
    for (uint32_t face = 0u; face < 6u; ++face) {
      if ((updateMask & (1u << face)) != 0u)
        pointShadowWriteLayers[pointShadowWriteLayerCount++] =
            lightIndex * 6u + face;
    }
  }
  if (pointShadowWriteLayerCount == 0u) {
    invalidatePointShadowPublishedState();
    m_pointShadowCpuPlan.shouldRender = false;
    return;
  }

  // The cube can contain preserved history, freshly rendered faces and layers
  // that have never been used. Track every face independently so neither the
  // cube sampling view's preferred layout nor one face's state is used as the
  // oldLayout proof for another face.
  auto transitionPointShadowWriteLayers =
      [&](VkImageLayout newLayout, VkPipelineStageFlags2 dstStages,
          VkAccessFlags2 dstAccess) {
        std::array<war3::render::War3OwnedImageLayoutTransition,
                   kMaxPointShadowLights * 6u> transitions = {};
        std::array<VkImageMemoryBarrier2, kMaxPointShadowLights * 6u> barriers =
            {};
        for (uint32_t i = 0u; i < pointShadowWriteLayerCount; ++i) {
          const uint32_t layer = pointShadowWriteLayers[i];
          const VkImageSubresourceRange range = {
              VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, layer, 1u};
          transitions[i] =
              m_pointShadowFaceLayouts[layer].plan(newLayout, dstStages,
                                                    dstAccess);
          barriers[i] = war3::render::MakeWar3OwnedImageBarrier(
              transitions[i], m_pointShadowCube->handle(), range);
        }

        VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = pointShadowWriteLayerCount;
        depInfo.pImageMemoryBarriers = barriers.data();
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

        for (uint32_t i = 0u; i < pointShadowWriteLayerCount; ++i) {
          const uint32_t layer = pointShadowWriteLayers[i];
          const VkImageSubresourceRange range = {
              VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, layer, 1u};
          war3::render::CommitWar3OwnedImageLayout(
              m_pointShadowFaceLayouts[layer], transitions[i],
              *m_pointShadowCube, range);
        }
      };

  // Vulkan recording has a second transaction boundary in addition to the
  // CPU publication rollback above. Once the cube layers enter attachment
  // layout, every exceptional exit must first close an active dynamic-rendering
  // scope and then return the entire transitioned range to read-only before the
  // original exception is allowed to escape.
  bool pointShadowLayersInAttachmentLayout = false;
  bool pointShadowDynamicRenderingActive = false;
  try {
    transitionPointShadowWriteLayers(
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    pointShadowLayersInAttachmentLayout = true;

  // Hoist the per-draw GPU-skin-direct decision out of the (face x draw)
  // inner loop: it depends only on the draw, so the exact-input probe was
  // re-run up to 24x per caster (6 faces x up to 4 lights). Precompute once
  // per replay draw and reuse; the per-attempt telemetry below is unchanged.
  thread_local std::vector<ShadowGpuSkinDirectDecision>
      pointShadowGpuSkinDecisions;
  pointShadowGpuSkinDecisions.clear();
  pointShadowGpuSkinDecisions.reserve(replayDraws.size());
  for (const auto* replayDraw : replayDraws)
    pointShadowGpuSkinDecisions.push_back(
        replayDraw ? EvaluateShadowGpuSkinDirectInput(*replayDraw)
                   : ShadowGpuSkinDirectDecision{});

  // GPU 阶段仅消费 CPU plan：face updateMask + faceCasters 索引列表。
  for (uint32_t lightIndex = 0; lightIndex < shadowLightCount; ++lightIndex) {
    const auto &shadowData = m_pointShadowData[lightIndex];
    const uint8_t updateMask = m_pointShadowCpuPlan.updateMask[lightIndex];

    for (uint32_t face = 0; face < 6u; ++face) {
      if ((updateMask & (1u << face)) == 0u) {
        // 未更新 face：年龄递增，depth 保留。
        m_pointShadowFaceAge[lightIndex][face] =
            std::min(m_pointShadowFaceAge[lightIndex][face] + 1u, 100000u);
        continue;
      }

      const uint32_t faceLayer = lightIndex * 6u + face;
      if (faceLayer >= m_pointShadowCapacityLights * 6u ||
          !m_pointShadowFaceViews[faceLayer])
        continue;
      const auto &faceCasterIdx =
          m_pointShadowCpuPlan.faceCasters[lightIndex * 6u + face];
      war3::tools::SetGpuFlightBreadcrumb(
          war3::tools::GpuFlightBreadcrumb::PointShadowFace,
          0xFFFFFFFFu, lightIndex, face);
      uint32_t pointFaceDrawCount = 0u;
      uint64_t pointFaceTriangleCount = 0u;

      VkRenderingAttachmentInfo depthAtt = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      depthAtt.imageView = m_pointShadowFaceViews[faceLayer]->handle();
      depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depthAtt.clearValue.depthStencil = {1.0f, 0};

      VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      renderInfo.renderArea = {{0, 0}, {resolution, resolution}};
      renderInfo.layerCount = 1;
      renderInfo.pDepthAttachment = &depthAtt;
      pointWorkloadReservation.commit();
      ctx->cmdBeginRendering(&renderInfo);
      pointShadowDynamicRenderingActive = true;

      VkViewport vp = {0.0f, 0.0f, float(resolution), float(resolution),
                       0.0f, 1.0f};
      ctx->cmdSetViewport(1, &vp);
      VkRect2D sc = {{0, 0}, {resolution, resolution}};
      ctx->cmdSetScissor(1, &sc);

      // 空 face：只 clear，不扫全 caster 列表。
      for (uint32_t drawIdx : faceCasterIdx) {
        if (drawIdx >= replayDraws.size())
          continue;
        const auto &draw = *replayDraws[drawIdx];
        if (draw.positionInfo.buffer == VK_NULL_HANDLE ||
            draw.positionInfo.size == 0)
          continue;
        if (draw.indexed &&
            (draw.indexInfo.buffer == VK_NULL_HANDLE ||
             draw.indexInfo.size == 0))
          continue;

        ShadowCasterPipelineKey key = {};
        key.positionFormat = draw.positionFormat;
        key.positionStride = draw.positionStride;
        key.positionOffset = draw.positionOffset;
        key.topology = draw.topology;
        // The cube receiver compares distance/range, so point faces must bind
        // the dedicated fragment shader that writes the same radial domain.
        key.pointShadowRadialDepth = true;

        if (draw.vertexBlendEnabled && draw.vertexBlendCount > 0) {
          key.blendWeightFormat = draw.blendWeightFormat;
          key.blendWeightOffset = draw.blendWeightOffset;
        } else {
          key.blendWeightFormat = VK_FORMAT_UNDEFINED;
          key.blendWeightOffset = 0;
        }

        if (draw.vertexBlendEnabled && draw.vertexBlendIndexed) {
          key.blendIndexFormat = draw.blendIndexFormat;
          key.blendIndexOffset = draw.blendIndexOffset;
        } else {
          key.blendIndexFormat = VK_FORMAT_UNDEFINED;
          key.blendIndexOffset = 0;
        }

        key.blendBinding = draw.blendBinding;
        key.blendStride = draw.blendStride;

        const bool alphaPayloadComplete =
            draw.diffuseTexture && draw.HasUsableUvBinding();
        if ((draw.alphaBlendEnabled && !draw.alphaTestEnabled) ||
            (draw.alphaTestEnabled && !alphaPayloadComplete)) {
          continue;
        }
        const bool effectiveAlphaTestShadowPoint =
            draw.alphaTestEnabled && alphaPayloadComplete;

        key.alphaTestEnabled = effectiveAlphaTestShadowPoint;
        if (effectiveAlphaTestShadowPoint) {
          key.uvFormat = draw.uvFormat;
          key.uvOffset = draw.uvOffset;
          key.uvStride = draw.uvStride;
          key.uvBinding = draw.uvBinding;
        }

        ShadowCasterPipeline pipeline = getShadowCasterPipeline(key);
        if (pipeline.pipeline == VK_NULL_HANDLE)
          continue;

        const ShadowGpuSkinDirectDecision& gpuSkinDirectDecision =
            pointShadowGpuSkinDecisions[drawIdx];
        const bool gpuSkinDirect =
            static_cast<bool>(gpuSkinDirectDecision);
        if (gpuSkinDirectDecision.requested) {
          ++m_gpuSkinVsShadowDirectAttempts;
          if (!gpuSkinDirectDecision.inputExact)
            ++m_gpuSkinVsShadowDirectInputRejects;
          else if (!gpuSkinDirectDecision.stateExact)
            ++m_gpuSkinVsShadowDirectStateRejects;
        }

        trackCasterPipeline(pipeline);
        ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                             VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

        ShadowCasterPushConstants pc = {};
        pc.blendCount = draw.vertexBlendCount;
        pc.flags = kShadowCasterFlagPointShadowLinearDepth;
        pc.mvp = shadowData.faceViewProj[face];
        pc.pointLightPosRange = shadowData.lightPos;
        // The fragment shader reconstructs exact radial depth from
        // gl_FragCoord.w and the 90-degree face coordinates. Reuse the first
        // padding slot for the point-shadow viewport size without changing the
        // shared CSM push-constant layout.
        pc.padding[0] = resolution;
        const bool s1TerrainCaster =
            draw.category == War3RenderState::StageCategory::Terrain &&
            draw.stage == 1;
        if (s1TerrainCaster &&
            war3::internal::kShadowS1TerrainCasterDepthBiasEnabled) {
          pc.flags |= kShadowCasterFlagStage1Terrain;
          pc.terrainDepthBias =
              war3::internal::kShadowS1TerrainCasterDepthBiasNdc;
        }

        if (draw.vertexBlendEnabled) {
          pc.flags |= kShadowCasterFlagUseBlend;
          if (draw.vertexBlendIndexed)
            pc.flags |= kShadowCasterFlagIndexedBlend;
          pc.paletteOffset = draw.paletteIndex * 256u;
        } else {
          pc.paletteOffset = objectBase + drawIdx;
        }

        if (gpuSkinDirect) {
          pc.flags |= kShadowCasterFlagGpuSkinDirectInput |
                      PackShadowCasterGpuSkinMetadata(
                          draw.gpuSkinInput.desc);
          if (draw.gpuSkinInput.irreversible)
            pc.flags |= kShadowCasterFlagGpuSkinNoFallback;
          pc.blendCount = draw.gpuSkinInput.desc.paletteMatrixCount;
          pc.padding[1] = draw.gpuSkinInput.desc.vertexCount;
        }

        if (effectiveAlphaTestShadowPoint && draw.diffuseTexture) {
          pc.flags |= kShadowCasterFlagAlphaTest;
          if (alphaShadowHashed)
            pc.flags |= kShadowCasterFlagHashAlpha;
          pc.alphaRef = draw.alphaRef;
          pc.samplerIndex = draw.diffuseSamplerIndex;
        }

        std::array<DxvkDescriptorWrite, 5> descriptors = {};
        descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptors[0].buffer = paletteDesc.buffer;
        if (effectiveAlphaTestShadowPoint && draw.diffuseTexture) {
          descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          descriptors[1].descriptor = &draw.textureDescriptor;
          ctx->track(draw.diffuseTexture->image(), DxvkAccess::Read);
        } else {
          descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          descriptors[1].descriptor = nullptr;
        }
        descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptors[2].descriptor = nullptr;
        SetShadowGpuSkinStorageDescriptors(
            descriptors, paletteDesc.buffer, draw, gpuSkinDirect);

        ctx->bindResources(DxvkCmdBuffer::ExecBuffer, pipeline.layout,
                           descriptors.size(), descriptors.data(), sizeof(pc),
                           &pc);

        if (draw.positionStorage.ptr() != nullptr)
          ctx->track(draw.positionStorage);
        if (draw.indexStorage.ptr() != nullptr &&
            draw.indexStorage.ptr() != draw.positionStorage.ptr())
          ctx->track(draw.indexStorage);
        if (draw.blendStorage.ptr() != nullptr)
          ctx->track(draw.blendStorage);
        if (effectiveAlphaTestShadowPoint && draw.uvBinding != 0u &&
            draw.uvStorage.ptr() != nullptr &&
            draw.uvStorage.ptr() != draw.positionStorage.ptr() &&
            draw.uvStorage.ptr() != draw.blendStorage.ptr())
          ctx->track(draw.uvStorage);
        if (gpuSkinDirect) {
          ctx->track(draw.gpuSkinInput.staticSource.buffer());
          if (draw.gpuSkinInput.palette.buffer().ptr() !=
              draw.gpuSkinInput.staticSource.buffer().ptr())
            ctx->track(draw.gpuSkinInput.palette.buffer());
        }

        VkBuffer vbs[2];
        VkDeviceSize offsets[2];
        VkDeviceSize sizes[2];
        VkDeviceSize strides[2];
        uint32_t vbCount = 1;
        vbs[0] = draw.positionInfo.buffer;
        offsets[0] = draw.positionInfo.offset;
        sizes[0] = draw.positionInfo.size;
        strides[0] = draw.positionStride;
        if (draw.blendBinding == 1) {
          vbCount = 2;
          vbs[1] = draw.blendInfo.buffer;
          offsets[1] = draw.blendInfo.offset;
          sizes[1] = draw.blendInfo.size;
          strides[1] = draw.blendStride;
        } else if (effectiveAlphaTestShadowPoint && draw.uvBinding == 1u) {
          vbCount = 2;
          vbs[1] = draw.uvInfo.buffer;
          offsets[1] = draw.uvInfo.offset;
          sizes[1] = draw.uvInfo.size;
          strides[1] = draw.uvStride;
        }
        ctx->cmdBindVertexBuffers(0, vbCount, vbs, offsets, sizes, strides);
        if (effectiveAlphaTestShadowPoint && draw.uvBinding == 2u) {
          const VkBuffer uvBuffer = draw.uvInfo.buffer;
          const VkDeviceSize uvOffset = draw.uvInfo.offset;
          const VkDeviceSize uvSize = draw.uvInfo.size;
          const VkDeviceSize uvStride = draw.uvStride;
          ctx->cmdBindVertexBuffers(2u, 1u, &uvBuffer, &uvOffset, &uvSize,
                                    &uvStride);
        }

        if (draw.indexed) {
          ctx->cmdBindIndexBuffer2(draw.indexInfo.buffer, draw.indexInfo.offset,
                                   draw.indexInfo.size, draw.indexType);
          ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,
                              draw.vertexOffset, 0);
          pointFaceTriangleCount += uint64_t(draw.indexCount / 3u);
        } else {
          ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);
          pointFaceTriangleCount += uint64_t(draw.vertexCount / 3u);
        }
        ++pointFaceDrawCount;

        if (gpuSkinDirect) {
          auto clearedDescriptors = descriptors;
          SetShadowGpuSkinStorageDescriptors(
              clearedDescriptors, paletteDesc.buffer, draw, false);
          auto clearedPc = pc;
          clearedPc.flags &= ~(kShadowCasterFlagGpuSkinDirectInput |
                               kShadowCasterFlagGpuSkinNoFallback |
                               kShadowCasterGpuSkinMetadataMask);
          clearedPc.blendCount = draw.vertexBlendCount;
          clearedPc.padding[1] = 0u;
          // padding[0] 继续保存 point cube face resolution，不能被 direct
          // 元数据复用，否则径向深度会发生系统性偏移。
          ctx->bindResources(DxvkCmdBuffer::ExecBuffer, pipeline.layout,
                             clearedDescriptors.size(),
                             clearedDescriptors.data(), sizeof(clearedPc),
                             &clearedPc);
          ++m_gpuSkinVsShadowDirectDrawsSubmitted;
          ++m_gpuSkinVsShadowDirectBindingsCleared;
          ++m_gpuSkinVsShadowReplayPoint;
        }
      }

      ctx->cmdEndRendering();
      pointShadowDynamicRenderingActive = false;
      war3::tools::SetGpuFlightPointShadowFaceWork(
          lightIndex, face, pointFaceDrawCount, pointFaceTriangleCount);
      m_pointShadowFaceAge[lightIndex][face] = 0;
      m_pointShadowFaceValidMask[lightIndex] |= uint8_t(1u << face);
    }

    // A cube lookup may select any of the six layers, including across a PCF
    // seam. Sampling after only one face was initialized exposed undefined or
    // stale layers and looked like a hard, misplaced shadow wedge. Fail soft
    // (direct light only) until the complete cube is valid.
    m_pointShadowReady[lightIndex] =
        (m_pointShadowFaceValidMask[lightIndex] &
         kPointShadowCompleteFaceMask) == kPointShadowCompleteFaceMask;
    if (m_pointShadowReady[lightIndex]) {
      m_pointShadowReadyCount =
          std::max(m_pointShadowReadyCount, lightIndex + 1u);
    }
  }

  transitionPointShadowWriteLayers(
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
  pointShadowLayersInAttachmentLayout = false;

  ctx->track(m_pointShadowCube, DxvkAccess::Write);
  // The signature describes cube contents, not merely a CPU plan. Commit it
  // only after resource creation and all scheduled face recordings succeeded;
  // otherwise a later frame could treat an old cube as matching new settings.
  m_pointShadowContentSignature = m_pointShadowCpuPlan.contentSignature;
  m_pointShadowPublishedLightGeneration =
      m_pointShadowCpuPlan.lightGeneration;
  m_pointShadowPublishedFrameSerial = input.frameSerial;
  m_pointShadowPublishedMapEpoch = input.mapEpoch;
  m_pointShadowPublishedDeviceEpoch = input.deviceEpoch;
  m_pointShadowPublishedResourceGeneration =
      m_pointShadowResourceGeneration;
  m_pointShadowPublishedPolicyRevision =
      PointShadowPolicySeal(FreezePointShadowSettings(*settings));
  m_pointShadowPublishedLightCount = shadowLightCount;
  m_pointShadowPublishedLightIds.fill(0);
  for (uint32_t lightIndex = 0u; lightIndex < shadowLightCount; ++lightIndex)
    m_pointShadowPublishedLightIds[lightIndex] =
        lightSnapshot.lights[lightIndex].id;
  pointShadowPublicationCommitted = true;
  } catch (...) {
    // These command-list wrappers directly record Vulkan commands and do not
    // allocate. Keep cleanup in the same catch so no exception can leave the
    // command buffer inside dynamic rendering or the cube descriptor pointing
    // at layers that remain in attachment layout.
    if (pointShadowDynamicRenderingActive) {
      ctx->cmdEndRendering();
      pointShadowDynamicRenderingActive = false;
    }
    if (pointShadowLayersInAttachmentLayout) {
      transitionPointShadowWriteLayers(
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT);
      pointShadowLayersInAttachmentLayout = false;
    }
    throw;
  }

  const bool pointShadowPublicationSampleable =
      pointShadowPublishedStateMatchesCurrentPlan();
  if (pointShadowPublicationSampleable) {
    static uint32_t s_pointShadowSuccessLogs = 0u;
    const uint32_t successLog = s_pointShadowSuccessLogs++;
    if (successLog < 16u || (successLog % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK PointShadow: Rendered! lights=%u light0=(%.1f,%.1f,%.1f) "
          "range=%.1f casters=%zu faceBudget=%u temporal=%d worker=%d "
          "cubeConvention=vulkan radialDepth=fragment\n",
          shadowLightCount, m_pointShadowData[0].lightPos.x,
          m_pointShadowData[0].lightPos.y, m_pointShadowData[0].lightPos.z,
          m_pointShadowData[0].lightPos.w, replayDraws.size(),
          m_pointShadowCpuPlan.maxFacesPerFrame,
          settings->shadows.pointShadowTemporalReuse ? 1 : 0,
          War3WorkerPrepareEnabled() ? 1 : 0);
    }
  } else {
    // Incremental face budgets may record useful work without yet completing a
    // six-face cube. Keep this distinct from the success marker so automation
    // cannot promote an un-sampleable publication.
    static uint32_t s_pointShadowIncompleteLogs = 0u;
    const uint32_t incompleteLog = s_pointShadowIncompleteLogs++;
    if (incompleteLog < 16u || (incompleteLog % 240u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK PointShadow: recorded but publication not sampleable "
          "lights=%u ready=%u generation=%llu faceMask0=0x%02x\n",
          shadowLightCount, m_pointShadowReadyCount,
          static_cast<unsigned long long>(
              m_pointShadowCpuPlan.lightGeneration),
          unsigned(m_pointShadowFaceValidMask[0]));
    }
  }
}

void War3ShadowReceiverPass::drawReceiver(const Rc<DxvkCommandList> &ctx,
                                          const Rc<DxvkImageView> &dstView) {
  if (!m_colorCopyView || !m_depthCopyView || !m_shadowMapSampleView ||
      !m_shadowUniformBuffer)
    return;

  ensurePointShadowNeutralResources();
  if (m_pointShadowNeutralCube &&
      m_pointShadowNeutralLayout.layout() !=
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
    const VkImageSubresourceRange range = {
        VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 6u};
    const auto toClear = m_pointShadowNeutralLayout.plan(
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageMemoryBarrier2 clearBarrier =
        war3::render::MakeWar3OwnedImageBarrier(
            toClear, m_pointShadowNeutralCube->handle(), range);
    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1u;
    depInfo.pImageMemoryBarriers = &clearBarrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
    war3::render::CommitWar3OwnedImageLayout(
        m_pointShadowNeutralLayout, toClear, *m_pointShadowNeutralCube, range);

    const VkClearDepthStencilValue neutralDepth = {1.0f, 0u};
    ctx->cmdClearDepthStencilImage(
        DxvkCmdBuffer::ExecBuffer, m_pointShadowNeutralCube->handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &neutralDepth, 1u, &range);

    const auto toRead = m_pointShadowNeutralLayout.plan(
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    const VkImageMemoryBarrier2 readBarrier =
        war3::render::MakeWar3OwnedImageBarrier(
            toRead, m_pointShadowNeutralCube->handle(), range);
    depInfo.pImageMemoryBarriers = &readBarrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);
    war3::render::CommitWar3OwnedImageLayout(
        m_pointShadowNeutralLayout, toRead, *m_pointShadowNeutralCube, range);
    ctx->track(m_pointShadowNeutralCube, DxvkAccess::Write);
  }
  const bool pointShadowPublicationCurrent =
      pointShadowPublishedStateMatchesCurrentPlan();
  const Rc<DxvkImageView> pointShadowSampleView =
      (pointShadowPublicationCurrent && m_pointShadowCubeView)
          ? m_pointShadowCubeView
          : m_pointShadowNeutralCubeView;
  if (!pointShadowSampleView)
    return;

  // All vkCmdUpdateBuffer calls must be recorded outside dynamic rendering.
  // Build both point-light payloads from the immutable snapshot captured by
  // Run(), then serialize offset-0 reuse in both directions.
  if (!m_pointShadowUniformBuffer) {
    DxvkBufferCreateInfo bufInfo = {};
    bufInfo.size = sizeof(PointShadowUniform);
    bufInfo.usage =
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.stages =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    bufInfo.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    bufInfo.debugName = "War3PointShadowUBO";
    m_pointShadowUniformBuffer =
        m_device->createBuffer(bufInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  }

  const LightUniform lightUbo = m_pointLightFrameUniform;
  static bool s_loggedLightCount = false;
  if (!s_loggedLightCount) {
    s_loggedLightCount = true;
    WAR3_RENDER_LOG("DXVK Shadow: Light system check - lUbo.count=%u "
                    "pointLightsEnabled=%d\n",
                    lightUbo.count, m_pointLightsEnabled ? 1 : 0);
    if (lightUbo.count > 0u) {
      WAR3_RENDER_LOG("DXVK Shadow: light[0] pos=(%.1f,%.1f,%.1f,%.1f) "
                      "color=(%.1f,%.1f,%.1f,%.1f)\n",
                      lightUbo.lights[0].pos.x, lightUbo.lights[0].pos.y,
                      lightUbo.lights[0].pos.z, lightUbo.lights[0].pos.w,
                      lightUbo.lights[0].color.x, lightUbo.lights[0].color.y,
                      lightUbo.lights[0].color.z,
                      lightUbo.lights[0].color.w);
    }
  }

  PointShadowUniform pointShadowUbo = {};
  const bool pointShadowActive =
      m_pointShadowEnabled && pointShadowPublicationCurrent &&
      m_pointShadowCubeView;
  pointShadowUbo.lightCount =
      pointShadowActive
          ? std::min<uint32_t>(m_pointShadowReadyCount, kMaxPointShadowLights)
          : 0u;
  pointShadowUbo.debugLightIndex = std::min<uint32_t>(
      m_pointShadowDebugLightIndex, kMaxPointShadowLights - 1u);
  // Manual 16-tap PCF requires the dedicated nearest sampler. Reusing the CSM
  // linear sampler double-filtered comparisons and shifted blocker edges.
  const Rc<DxvkSampler> pointShadowSampler =
      m_shadowSampler;
  if (!pointShadowSampler)
    return;
  pointShadowUbo.samplerIndex =
      pointShadowSampler->getDescriptor().samplerIndex;
  pointShadowUbo.filterParams = m_pointShadowFilterParams;
  for (uint32_t i = 0u; i < pointShadowUbo.lightCount; ++i) {
    const bool ready = m_pointShadowReady[i];
    pointShadowUbo.lights[i].lightPos = m_pointShadowData[i].lightPos;
    pointShadowUbo.lights[i].bias = m_pointShadowBias;
    pointShadowUbo.lights[i].enabled = ready ? 1.0f : 0.0f;
    pointShadowUbo.lights[i].shadowIntensity =
        ready ? std::clamp(m_pointShadowData[i].shadowIntensity, 0.0f, 1.0f)
              : 0.0f;
  }

  const auto lightInfo =
      m_lightBuffer->getSliceInfo(0u, sizeof(LightUniform));
  const auto pointShadowInfo = m_pointShadowUniformBuffer->getSliceInfo(
      0u, sizeof(PointShadowUniform));
  std::array<VkBufferMemoryBarrier2, 2> uniformBarriers = {};
  uniformBarriers[0] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  uniformBarriers[0].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  uniformBarriers[0].srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  uniformBarriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  uniformBarriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  uniformBarriers[0].buffer = lightInfo.buffer;
  uniformBarriers[0].offset = lightInfo.offset;
  uniformBarriers[0].size = sizeof(LightUniform);
  uniformBarriers[1] = uniformBarriers[0];
  uniformBarriers[1].buffer = pointShadowInfo.buffer;
  uniformBarriers[1].offset = pointShadowInfo.offset;
  uniformBarriers[1].size = sizeof(PointShadowUniform);

  VkDependencyInfo uniformDependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  uniformDependency.bufferMemoryBarrierCount =
      static_cast<uint32_t>(uniformBarriers.size());
  uniformDependency.pBufferMemoryBarriers = uniformBarriers.data();
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &uniformDependency);

  ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, lightInfo.buffer,
                       lightInfo.offset, sizeof(lightUbo), &lightUbo);
  ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, pointShadowInfo.buffer,
                       pointShadowInfo.offset, sizeof(pointShadowUbo),
                       &pointShadowUbo);

  for (auto &barrier : uniformBarriers) {
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
  }
  ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &uniformDependency);
  ctx->track(m_lightBuffer, DxvkAccess::Write);
  ctx->track(m_pointShadowUniformBuffer, DxvkAccess::Write);

  Pipeline pipeline = getPipeline(dstView->image()->info().format,
                                  dstView->image()->info().sampleCount);

  VkExtent3D extent = dstView->mipLevelExtent(0u);

  VkRenderingAttachmentInfo attachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = dstView->handle();
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderInfo.renderArea.offset = {0u, 0u};
  renderInfo.renderArea.extent = {extent.width, extent.height};
  renderInfo.layerCount = 1u;
  renderInfo.colorAttachmentCount = 1u;
  renderInfo.pColorAttachments = &attachment;

  ctx->cmdBeginRendering(&renderInfo);

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = float(extent.width);
  viewport.height = float(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor = {};
  scissor.offset = {0, 0};
  scissor.extent = {extent.width, extent.height};

  ctx->cmdSetViewport(1, &viewport);
  ctx->cmdSetScissor(1, &scissor);

  std::array<DxvkDescriptorWrite, 14> descriptors = {};
  descriptors[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[0].descriptor = m_colorCopyView->getDescriptor();

  descriptors[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[1].descriptor = m_depthCopyView->getDescriptor();

  descriptors[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[2].descriptor = m_shadowMapSampleView->getDescriptor();

  descriptors[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[3].descriptor = nullptr;
  descriptors[3].buffer =
      m_shadowUniformBuffer->getSliceInfo(0, sizeof(ShadowReceiverUniform));

  descriptors[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[4].descriptor = nullptr;
  descriptors[4].buffer = lightInfo;

  // [NEW] binding 5: Point Shadow Cube Map
  descriptors[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[5].descriptor = pointShadowSampleView->getDescriptor();

  descriptors[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptors[6].descriptor = nullptr;
  descriptors[6].buffer = pointShadowInfo;

  // [ShadowTAA] binding 7: ShadowCurrent（R8）
  descriptors[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[7].descriptor =
      m_shadowCurrentView ? m_shadowCurrentView->getDescriptor() : nullptr;

  // [ShadowTAA] binding 8: MotionVector（RG16F）
  descriptors[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[8].descriptor =
      m_motionVectorView ? m_motionVectorView->getDescriptor() : nullptr;

  // [ShadowTAA] binding 9/10: History Read/Write（Ping-Pong）
  const uint32_t readIndex = m_shadowHistoryIndex & 1u;
  const uint32_t writeIndex = readIndex ^ 1u;
  descriptors[9].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[9].descriptor =
      m_shadowHistoryView[readIndex]
          ? m_shadowHistoryView[readIndex]->getDescriptor()
          : nullptr;
  descriptors[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  descriptors[10].descriptor =
      m_shadowHistoryStorageView[writeIndex]
          ? m_shadowHistoryStorageView[writeIndex]->getDescriptor()
          : nullptr;

  descriptors[11].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[11].descriptor =
      m_shadowCasterMaskSampleView
          ? m_shadowCasterMaskSampleView->getDescriptor()
          : m_shadowMapSampleView->getDescriptor();

  descriptors[12].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[12].descriptor =
      m_pointRayHiZVisibilityView
          ? m_pointRayHiZVisibilityView->getDescriptor()
          : m_depthCopyView->getDescriptor();
  descriptors[13].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptors[13].descriptor =
      m_pointRayHiZView
          ? m_pointRayHiZView->getDescriptor()
          : (m_depthCopyView2D ? m_depthCopyView2D->getDescriptor() : nullptr);

  ReceiverPushConstants pc = {};
  pc.colorSampler = m_samplerLinear->getDescriptor().samplerIndex;
  pc.rawShadowSampler = m_shadowSampler->getDescriptor().samplerIndex;
  pc.compareShadowSampler =
      m_shadowCompareSamplerActive->getDescriptor().samplerIndex;
  pc.shadowCompareMode = m_shadowCompareMode;

  ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,
                       VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);

  ctx->bindResources(DxvkCmdBuffer::ExecBuffer, pipeline.layout,
                     descriptors.size(), descriptors.data(), sizeof(pc), &pc);

  ctx->cmdDraw(3, 1, 0, 0);

  ctx->cmdEndRendering();

  ctx->track(dstView->image(), DxvkAccess::Write);
  ctx->track(m_colorCopy, DxvkAccess::Read);
  ctx->track(m_depthCopy, DxvkAccess::Read);
  if (m_pointRayHiZVisibilityView)
    ctx->track(m_pointRayHiZVisibilityView->image(), DxvkAccess::Read);
  if (m_pointRayHiZView)
    ctx->track(m_pointRayHiZView->image(), DxvkAccess::Read);
  ctx->track(m_shadowMap, DxvkAccess::Read);
  if (m_shadowCasterMask)
    ctx->track(m_shadowCasterMask, DxvkAccess::Read);
  ctx->track(m_shadowUniformBuffer, DxvkAccess::Write);
  ctx->track(m_lightBuffer, DxvkAccess::Write);
  ctx->track(pointShadowSampleView->image(), DxvkAccess::Read);
  ctx->track(m_pointShadowUniformBuffer, DxvkAccess::Write);
  if (m_shadowCurrent)
    ctx->track(m_shadowCurrent, DxvkAccess::Read);
  if (m_motionVectorImage)
    ctx->track(m_motionVectorImage, DxvkAccess::Read);
  if (m_shadowHistory[readIndex])
    ctx->track(m_shadowHistory[readIndex], DxvkAccess::Read);
  if (m_shadowHistory[writeIndex])
    ctx->track(m_shadowHistory[writeIndex], DxvkAccess::Write);
  ctx->track(m_samplerLinear);
  ctx->track(m_shadowCompareSamplerActive);
  ctx->track(pointShadowSampler);
  ctx->track(m_shadowSampler);
  reconciliation.receiverDrawExecutedThisFrame = 1u;
}

void War3ShadowReceiverPass::Run(const Rc<DxvkCommandList> &ctx,
                                 const War3PipelineInput &input) {
  // [Perf] Add timing scope for Shadow/Outline pass
  auto perfScope = war3::War3PerfMonitor::instance().scope("Shadow/Main", ctx);
  m_gpuWorkloadGovernor.beginFrame(input.frameSerial);
  m_workloadGovernorRejectedThisFrame = false;
  [[maybe_unused]] auto gpuWorkloadDiagnosticsPublish =
      MakeWar3ScopeExit([&]() {
        std::lock_guard<std::mutex> lock(g_shadowDiagnosticsMutex);
        g_gpuWorkloadGovernorDiagnostics =
            m_gpuWorkloadGovernor.diagnostics();
      });
  const uint32_t shadowMainPhaseSampleWeight = War3ShadowPhaseSampleWeight(
      input.frameSerial, 0x5a17c93de0428f61ull);
  War3SampledPhaseRawTiming<kWar3ShadowMainRawPhaseCount>
      shadowMainPhaseTiming(
          shadowMainPhaseSampleWeight, "ShadowMainPhaseSample",
          "FramePipeline/ShadowMainPhaseSample",
          kWar3ShadowMainRawPhaseNames);
  shadowMainPhaseTiming.enter(
      static_cast<size_t>(War3ShadowMainRawPhase::EntryAndValidation));
  ++m_pointShadowRunSerial;
  const War3PointShadowPersistentMode pointShadowPersistentConfiguredMode =
      PointShadowPersistentMode();
  if (pointShadowPersistentConfiguredMode ==
      War3PointShadowPersistentMode::Off) {
    m_pointShadowPersistentLastBeginRejectReason =
        PointShadowPersistentBeginRejectReason::ModeOff;
  } else if (!War3WorkerPrepareEnabled()) {
    m_pointShadowPersistentLastBeginRejectReason =
        PointShadowPersistentBeginRejectReason::WorkerPrepareDisabled;
  } else {
    m_pointShadowPersistentLastBeginRejectReason =
        PointShadowPersistentBeginRejectReason::NoPointShadowWork;
  }
  [[maybe_unused]] auto pointShadowPersistentDiagnosticsPublish =
      MakeWar3ScopeExit([&]() noexcept {
        // Release-default Off never creates a worker and must add no status or
        // perf mutex traffic to the frame hot path. The global zero/default
        // snapshot already reports configured/effective Off.
        if (pointShadowPersistentConfiguredMode ==
                War3PointShadowPersistentMode::Off &&
            !m_pointShadowPersistentWorker) {
          return;
        }
        const auto workerDiagnostics =
            m_pointShadowPersistentWorker
                ? m_pointShadowPersistentWorker->diagnostics()
                : war3::render::War3PointShadowPrepareWorkerDiagnostics{};
        PointShadowPersistentDiagnostics diagnostics = {};
        diagnostics.configuredMode =
            static_cast<uint32_t>(pointShadowPersistentConfiguredMode);
        diagnostics.effectiveMode =
            pointShadowPersistentConfiguredMode !=
                    War3PointShadowPersistentMode::Off &&
                m_pointShadowPersistentWorker && workerDiagnostics.available
            ? static_cast<uint32_t>(pointShadowPersistentConfiguredMode)
            : static_cast<uint32_t>(War3PointShadowPersistentMode::Off);
        diagnostics.lastBeginRejectReason = static_cast<uint32_t>(
            m_pointShadowPersistentLastBeginRejectReason);
        diagnostics.workerCreated =
            m_pointShadowPersistentWorker ? 1u : 0u;
        diagnostics.workerAvailable = workerDiagnostics.available ? 1u : 0u;
        diagnostics.lastFrameSerial = input.frameSerial;
        diagnostics.beginAttempts = m_pointShadowPersistentBeginAttempts;
        diagnostics.beginEligible = m_pointShadowPersistentBeginEligible;
        diagnostics.workerCreateCount =
            m_pointShadowPersistentWorkerCreateCount;
        diagnostics.workerThreadStarts = workerDiagnostics.threadStarts;
        diagnostics.accepted = m_pointShadowPersistentAccepted;
        diagnostics.ready = workerDiagnostics.readyJobs;
        diagnostics.deadlineFallback =
            m_pointShadowPersistentDeadlineFallback;
        diagnostics.rejectedFallback =
            m_pointShadowPersistentRejectedFallback;
        diagnostics.observeMatch = m_pointShadowPersistentObserveMatch;
        diagnostics.mismatch = m_pointShadowPersistentObserveMismatch;
        diagnostics.consumed = m_pointShadowPersistentConsumed;
        diagnostics.failed = workerDiagnostics.failedJobs;
        diagnostics.busy = workerDiagnostics.busyRejections;
        PublishPointShadowPersistentDiagnostics(diagnostics);

        war3::PointShadowPersistentFrameTelemetry telemetry = {};
        telemetry.configuredMode = diagnostics.configuredMode;
        telemetry.effectiveMode = diagnostics.effectiveMode;
        telemetry.lastBeginRejectReason =
            diagnostics.lastBeginRejectReason;
        telemetry.workerCreated = diagnostics.workerCreated;
        telemetry.workerAvailable = diagnostics.workerAvailable;
        telemetry.beginAttempts = diagnostics.beginAttempts;
        telemetry.beginEligible = diagnostics.beginEligible;
        telemetry.workerCreateCount = diagnostics.workerCreateCount;
        telemetry.workerThreadStarts = diagnostics.workerThreadStarts;
        telemetry.accepted = diagnostics.accepted;
        telemetry.ready = diagnostics.ready;
        telemetry.deadlineFallback = diagnostics.deadlineFallback;
        telemetry.rejectedFallback = diagnostics.rejectedFallback;
        telemetry.observeMatch = diagnostics.observeMatch;
        telemetry.mismatch = diagnostics.mismatch;
        telemetry.consumed = diagnostics.consumed;
        telemetry.failed = diagnostics.failed;
        telemetry.busy = diagnostics.busy;
        war3::War3PerfMonitor::instance()
            .notePointShadowPersistentFrame(telemetry);
      });
  // External consumers execute later in the same frame graph. Revoke the old
  // settlement before any fallible work so a caught Shadow exception cannot
  // make Volume pair a prior CSM with current camera/depth.
  m_shadowPublicationSettledFrameSerial = 0u;

  // Never let a previous frame's A1 result survive an early return, resize or
  // light-snapshot change. The receiver only sees views republished later in
  // this Run after the full generation tuple has matched.
  m_pointRayHiZVisibilityView = nullptr;
  m_pointRayHiZView = nullptr;
  m_pointRayHiZLightCount = 0u;
  m_pointRayHiZFrameSerial = 0u;
  m_pointRayHiZResourceGeneration = 0u;
  m_pointRayHiZLightGeneration = 0u;

  // Phase 7.2: 每帧重置对账计数器
  reconciliation = {};
  reconciliation.shadowMapRenderSerial = m_shadowMapRenderSerial;
  if (input.mapEpoch == 0u || input.deviceEpoch == 0u ||
      input.mapEpoch != m_shadowMapEpoch ||
      input.deviceEpoch != m_shadowDeviceEpoch) {
    reconciliation.replayValidationRejectedCount = 1u;
    reconciliation.replayPartialPreventedCount = 1u;
    reconciliation.replayValidationLastReason = static_cast<uint32_t>(
        input.mapEpoch != m_shadowMapEpoch
            ? war3::render::War3ShadowReplayRejectReason::StaleMapEpoch
            : war3::render::War3ShadowReplayRejectReason::StaleDeviceEpoch);
    reconciliation.replayValidationLastExpectedMapEpoch = m_shadowMapEpoch;
    reconciliation.replayValidationLastDrawMapEpoch = input.mapEpoch;
    g_shadowReplayDiagnostics.staleEpochConsumerRejectCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_shadowReplayDiagnostics.partialPreventedCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_shadowReplayDiagnostics.lastRejectReason.store(
        reconciliation.replayValidationLastReason,
        std::memory_order_release);
    g_shadowReplayDiagnostics.lastOffenderMapEpoch.store(
        input.mapEpoch, std::memory_order_release);
    return;
  }
  if (m_epochFirstCandidateFrameSerial == 0u) {
    m_epochFirstCandidateFrameSerial = input.frameSerial;
    g_shadowReplayDiagnostics.candidateFrameSerial.store(
        input.frameSerial, std::memory_order_release);
  }
  const uint64_t shadowMapResourceGenerationAtRunEntry =
      m_shadowMapResourceGeneration;
  const uint64_t shadowTaaResourceGenerationAtRunEntry =
      m_shadowTaaResourceGeneration;
  const uint64_t shadowLifecycleTombstoneSerial =
      war3::render::CurrentShadowCasterTombstoneSerial();
  const uint64_t shadowStagePolicyRevision =
      war3::render::CurrentShadowStagePolicyRevision();
  const bool lifecycleInvalidated =
      shadowLifecycleTombstoneSerial !=
          m_shadowLifecycleTombstoneSerialSeen ||
      shadowStagePolicyRevision != m_shadowStagePolicyRevisionSeen;
  if (lifecycleInvalidated) {
    reconciliation.shadowHistoryInvalidationMask |=
        kShadowTaaInvalidateLifecycle;
    m_shadowLifecycleTombstoneSerialSeen =
        shadowLifecycleTombstoneSerial;
    m_shadowStagePolicyRevisionSeen = shadowStagePolicyRevision;

    // An authoritative removal or policy transition outranks all temporal
    // continuity helpers. The next map must be freshly rendered or explicitly
    // cleared; no adaptive reuse, empty-replay hold or last-good hold survives.
    m_hasCompleteShadowMap = false;
    m_shadowHistoryValid = false;
    m_shadowTaaWasActiveLastFrame = false;
    m_shadowTaaHistoryContractValid = false;
    m_lastShadowMapCasterCount = 0u;
    m_lastDynamicPoseSignature = 0u;
    m_lastShadowMapReplayContentHash = 0u;
    m_lastShadowMapReplayBackingHash = 0u;
    m_lastShadowMapStagePolicyRevision = 0u;
    m_lastShadowMapCsmHash = 0u;
    m_lastShadowMapResourceGeneration = 0u;
    m_shadowAdaptiveFrameIndex = 0u;
    m_transientEmptyReplayHoldFramesRemaining = 0u;
    m_recentSemanticDynamicHoldFramesRemaining = 0u;
    m_semanticIdentityChurnHoldFramesRemaining = 0u;
    m_semanticCoverageDropHoldStreak = 0u;
    m_lastShadowMapSemanticIdentityHash = 0u;
    m_pendingShadowMapSemanticIdentityHash = 0u;
    m_pendingShadowMapSemanticIdentityStableFrames = 0u;
    m_hasLastShadowMapLighting = false;
    invalidatePointShadowPublishedState();
    invalidateVolumeSunShadowPublication();
  }
  const bool shadowTaaRuntimeModuleEnabled =
      war3::runtime::IsWar3RuntimeModuleEnabled(
          war3::runtime::War3RuntimeModule::ShadowTaa);
  const War3ShadowTaaMode shadowTaaRequestedMode =
      ResolveShadowTaaRequestedMode(
          input.settings != nullptr ? &input.settings->shadows : nullptr);
  const uint64_t shadowTaaSettingsRevision =
      input.settings != nullptr
          ? input.settings->shadows.shadowTaaSettingsRevision
          : 0u;
  if (!m_shadowTaaModeInitialized) {
    m_shadowTaaModeInitialized = true;
    m_shadowTaaRequestedModeSeen = shadowTaaRequestedMode;
    m_shadowTaaSettingsRevisionSeen = shadowTaaSettingsRevision;
  } else if (m_shadowTaaRequestedModeSeen != shadowTaaRequestedMode ||
             m_shadowTaaSettingsRevisionSeen != shadowTaaSettingsRevision) {
    // A user mode transition owns exactly one history cut. The next Temporal
    // frame writes current-only and the following valid frame may read it.
    reconciliation.shadowHistoryInvalidationMask |=
        kShadowTaaInvalidateModeSwitch;
    m_shadowHistoryValid = false;
    m_shadowTaaWasActiveLastFrame = false;
    m_shadowTaaHistoryContractValid = false;
    m_shadowTaaRequestedModeSeen = shadowTaaRequestedMode;
    m_shadowTaaSettingsRevisionSeen = shadowTaaSettingsRevision;
  }
  reconciliation.shadowTaaRuntimeModuleEnabled =
      shadowTaaRuntimeModuleEnabled ? 1u : 0u;
  reconciliation.shadowTaaRequestedMode =
      static_cast<uint32_t>(shadowTaaRequestedMode);
  reconciliation.shadowTaaEffectiveMode =
      shadowTaaRuntimeModuleEnabled
          ? static_cast<uint32_t>(shadowTaaRequestedMode)
          : static_cast<uint32_t>(War3ShadowTaaMode::DirectInline);
  if (!shadowTaaRuntimeModuleEnabled) {
    m_shadowHistoryValid = false;
    m_shadowTaaWasActiveLastFrame = false;
    m_shadowTaaHistoryContractValid = false;
  }
  [[maybe_unused]] auto shadowTaaFrameTelemetryPublish =
      MakeWar3ScopeExit([&]() noexcept {
        war3::ShadowTaaFrameTelemetry telemetry = {};
        telemetry.runtimeModuleEnabled =
            reconciliation.shadowTaaRuntimeModuleEnabled;
        telemetry.requestedMode = reconciliation.shadowTaaRequestedMode;
        telemetry.effectiveMode = reconciliation.shadowTaaEffectiveMode;
        telemetry.shaderMode = reconciliation.shadowTaaMode;
        telemetry.blockedSemanticDynamic =
            reconciliation.shadowTaaBlockedSemanticDynamic;
        telemetry.blockedSunMotion =
            reconciliation.shadowTaaBlockedSunMotion;
        telemetry.blockedCsmFallback =
            reconciliation.shadowTaaBlockedCsmFallback;
        telemetry.visibilityExecuted =
            reconciliation.shadowVisibilityExecutedThisFrame;
        telemetry.motionVectorExecuted =
            reconciliation.shadowMotionVectorExecutedThisFrame;
        telemetry.receiverExecuted =
            reconciliation.receiverDrawExecutedThisFrame;
        telemetry.historyWriteExecuted =
            reconciliation.shadowHistoryWriteExecutedThisFrame;
        telemetry.historyAdvanced =
            reconciliation.shadowHistoryAdvancedThisFrame;
        telemetry.historyAdvanceSkippedIncomplete =
            reconciliation.shadowHistoryAdvanceSkippedIncomplete;
        telemetry.historyValidBefore =
            reconciliation.shadowHistoryValidBefore;
        telemetry.historyValidAfter =
            reconciliation.shadowHistoryValidAfter;
        telemetry.historyInvalidationMask =
            reconciliation.shadowHistoryInvalidationMask;
        war3::War3PerfMonitor::instance().noteShadowTaaFrame(telemetry);

        if (reconciliation.shadowHistoryInvalidationMask != 0u) {
          m_shadowTaaLastInvalidationReason =
              reconciliation.shadowHistoryInvalidationMask;
        }
        ShadowTaaDiagnostics diagnostics = {};
        diagnostics.requestedMode = reconciliation.shadowTaaRequestedMode;
        diagnostics.effectiveMode = reconciliation.shadowTaaEffectiveMode;
        diagnostics.shaderMode = reconciliation.shadowTaaMode;
        diagnostics.historyValid = reconciliation.shadowHistoryValidAfter;
        diagnostics.historyReadable =
            reconciliation.shadowTaaMode >= 3u ? 1u : 0u;
        diagnostics.historyGeneration = m_shadowTaaHistoryGeneration;
        diagnostics.lastInvalidationReason =
            m_shadowTaaLastInvalidationReason;
        diagnostics.fixedWallBypassCount =
            m_shadowTaaFixedWallBypassCount;
        diagnostics.settingsRevision = m_shadowTaaSettingsRevisionSeen;
        PublishShadowTaaDiagnostics(diagnostics);
      });
  const auto strengthToMilli = [](float value) -> uint32_t {
    return uint32_t(std::clamp(value, 0.0f, 1.0f) * 1000.0f + 0.5f);
  };
  const auto recordShadowResourceFingerprint =
      [&](uint32_t historyReadIndex, uint32_t historyWriteIndex) {
        reconciliation.shadowMapImagePtr = War3RcObjectId(m_shadowMap);
        reconciliation.shadowMapSampleViewPtr =
            War3RcObjectId(m_shadowMapSampleView);
        reconciliation.shadowCurrentImagePtr =
            War3RcObjectId(m_shadowCurrent);
        reconciliation.shadowCurrentViewPtr =
            War3RcObjectId(m_shadowCurrentView);
        reconciliation.shadowHistoryReadImagePtr =
            historyReadIndex < m_shadowHistory.size()
                ? War3RcObjectId(m_shadowHistory[historyReadIndex])
                : 0u;
        reconciliation.shadowHistoryReadViewPtr =
            historyReadIndex < m_shadowHistoryView.size()
                ? War3RcObjectId(m_shadowHistoryView[historyReadIndex])
                : 0u;
        reconciliation.shadowHistoryWriteImagePtr =
            historyWriteIndex < m_shadowHistory.size()
                ? War3RcObjectId(m_shadowHistory[historyWriteIndex])
                : 0u;
        reconciliation.shadowHistoryWriteViewPtr =
            historyWriteIndex < m_shadowHistoryStorageView.size()
                ? War3RcObjectId(m_shadowHistoryStorageView[historyWriteIndex])
                : 0u;
      };
  const auto setReceiverRunEntryFlags =
      [&](bool inputValid, bool shadowsEnabled, bool outlineEnabled,
          bool hasSunShadow, bool hasPointShadow, bool hasPointLights,
          bool needOutlinePass, bool receiverNeedsShadowMap,
          bool needReceiverPass, bool hasReplayDraws,
          bool shadowMapExecuted, bool debugShadow) {
        uint32_t flags = 0u;
        if (inputValid)
          flags |= ReceiverRunEntryInputValid;
        if (shadowsEnabled)
          flags |= ReceiverRunEntryShadowsEnabled;
        if (outlineEnabled)
          flags |= ReceiverRunEntryOutlineEnabled;
        if (hasSunShadow)
          flags |= ReceiverRunEntryHasSunShadow;
        if (hasPointShadow)
          flags |= ReceiverRunEntryHasPointShadow;
        if (hasPointLights)
          flags |= ReceiverRunEntryHasPointLights;
        if (needOutlinePass)
          flags |= ReceiverRunEntryNeedOutlinePass;
        if (receiverNeedsShadowMap)
          flags |= ReceiverRunEntryNeedsShadowMap;
        if (needReceiverPass)
          flags |= ReceiverRunEntryNeedsReceiverPass;
        if (hasReplayDraws)
          flags |= ReceiverRunEntryHasReplayDraws;
        if (shadowMapExecuted)
          flags |= ReceiverRunEntryShadowMapExecuted;
        if (debugShadow)
          flags |= ReceiverRunEntryDebugShadow;
        reconciliation.receiverRunEntryFlags = flags;
      };
  const auto publishReconciliationStats = [&]() {
    reconciliation.gpuSkinVsShadowDirectAttempts =
        m_gpuSkinVsShadowDirectAttempts;
    reconciliation.gpuSkinVsShadowDirectInputRejects =
        m_gpuSkinVsShadowDirectInputRejects;
    reconciliation.gpuSkinVsShadowDirectStateRejects =
        m_gpuSkinVsShadowDirectStateRejects;
    reconciliation.gpuSkinVsShadowDirectDrawsSubmitted =
        m_gpuSkinVsShadowDirectDrawsSubmitted;
    reconciliation.gpuSkinVsShadowDirectBindingsCleared =
        m_gpuSkinVsShadowDirectBindingsCleared;
    reconciliation.gpuSkinVsShadowReplayDirectional =
        m_gpuSkinVsShadowReplayDirectional;
    reconciliation.gpuSkinVsShadowReplayPoint =
        m_gpuSkinVsShadowReplayPoint;
    reconciliation.gpuSkinVsShadowReplayUnknown =
        m_gpuSkinVsShadowReplayUnknown;
    auto stats = input.scene.shadowStats;
    stats.gpuSkinVsShadowDirectAttempts =
        reconciliation.gpuSkinVsShadowDirectAttempts;
    stats.gpuSkinVsShadowDirectInputRejects =
        reconciliation.gpuSkinVsShadowDirectInputRejects;
    stats.gpuSkinVsShadowDirectStateRejects =
        reconciliation.gpuSkinVsShadowDirectStateRejects;
    stats.gpuSkinVsShadowDirectDrawsSubmitted =
        reconciliation.gpuSkinVsShadowDirectDrawsSubmitted;
    stats.gpuSkinVsShadowDirectBindingsCleared =
        reconciliation.gpuSkinVsShadowDirectBindingsCleared;
    stats.gpuSkinVsShadowReplayDirectional =
        reconciliation.gpuSkinVsShadowReplayDirectional;
    stats.gpuSkinVsShadowReplayPoint =
        reconciliation.gpuSkinVsShadowReplayPoint;
    stats.gpuSkinVsShadowReplayUnknown =
        reconciliation.gpuSkinVsShadowReplayUnknown;
    stats.semanticSceneShadowCastersCount = reconciliation.shadowCastersCount;
    stats.semanticSceneReplayDrawsCount = reconciliation.replayDrawsCount;
    stats.semanticSceneShadowMapDrawnCasters =
        reconciliation.shadowMapDrawnCasters;
    stats.semanticSceneShadowMapCascadeCulledCount =
        reconciliation.cascadeCulledCount;
    stats.semanticSceneTerrainBoundsCullMode =
        reconciliation.terrainBoundsCullMode;
    stats.semanticSceneTerrainBoundsCandidateCount =
        reconciliation.terrainBoundsCandidateCount;
    stats.semanticSceneTerrainBoundsProofAcceptedCount =
        reconciliation.terrainBoundsProofAcceptedCount;
    stats.semanticSceneTerrainBoundsFailVisibleCount =
        reconciliation.terrainBoundsFailVisibleCount;
    stats.semanticSceneTerrainBoundsWouldCullCount =
        reconciliation.terrainBoundsWouldCullCount;
    stats.semanticSceneTerrainBoundsAppliedCullCount =
        reconciliation.terrainBoundsAppliedCullCount;
    stats.semanticSceneTerrainBoundsC0WouldCullCount =
        reconciliation.terrainBoundsC0WouldCullCount;
    stats.semanticSceneTerrainBoundsC1WouldCullCount =
        reconciliation.terrainBoundsC1WouldCullCount;
    stats.semanticSceneTerrainBoundsC2WouldCullCount =
        reconciliation.terrainBoundsC2WouldCullCount;
    stats.semanticSceneTerrainBoundsC3WouldCullCount =
        reconciliation.terrainBoundsC3WouldCullCount;
    stats.semanticSceneObjectBoundsCandidateCount =
        reconciliation.objectBoundsCandidateCount;
    stats.semanticSceneObjectBoundsProofAcceptedCount =
        reconciliation.objectBoundsProofAcceptedCount;
    stats.semanticSceneObjectBoundsFailVisibleCount =
        reconciliation.objectBoundsFailVisibleCount;
    stats.semanticSceneObjectBoundsWouldCullCount =
        reconciliation.objectBoundsWouldCullCount;
    stats.semanticSceneObjectBoundsAppliedCullCount =
        reconciliation.objectBoundsAppliedCullCount;
    stats.semanticSceneUnionCullMode = reconciliation.unionCullMode;
    stats.semanticSceneUnionCullObserveFrameCount =
        reconciliation.unionCullObserveFrameCount;
    stats.semanticSceneUnionCullCandidateCount =
        reconciliation.unionCullCandidateCount;
    stats.semanticSceneUnionCullProofAcceptedCount =
        reconciliation.unionCullProofAcceptedCount;
    stats.semanticSceneUnionCullFailVisibleCount =
        reconciliation.unionCullFailVisibleCount;
    stats.semanticSceneUnionCullDynamicConservativeCount =
        reconciliation.unionCullDynamicConservativeCount;
    stats.semanticSceneUnionCullUnknownOrStaleCount =
        reconciliation.unionCullUnknownOrStaleCount;
    stats.semanticSceneUnionCullC2WouldCullCount =
        reconciliation.unionCullC2WouldCullCount;
    stats.semanticSceneUnionCullC3WouldCullCount =
        reconciliation.unionCullC3WouldCullCount;
    stats.semanticSceneUnionCullBothFarWouldCullCount =
        reconciliation.unionCullBothFarWouldCullCount;
    stats.semanticSceneUnionCullFalseNegativeCount =
        reconciliation.unionCullFalseNegativeCount;
    stats.semanticSceneUnionCullFalsePositiveCount =
        reconciliation.unionCullFalsePositiveCount;
    stats.semanticSceneShadowMapPreparedDrawCount =
        reconciliation.shadowMapPreparedDrawCount;
    stats.semanticSceneShadowMapAlphaTestPreparedCount =
        reconciliation.shadowMapAlphaTestPreparedCount;
    stats.semanticSceneShadowMapAlphaPromotedPreparedCount =
        reconciliation.shadowMapAlphaPromotedPreparedCount;
    stats.semanticSceneShadowMapDynamicPreparedCount =
        reconciliation.shadowMapDynamicPreparedCount;
    stats.semanticSceneShadowMapStaticPreparedCount =
        reconciliation.shadowMapStaticPreparedCount;
    stats.semanticSceneShadowMapOtherPreparedCount =
        reconciliation.shadowMapOtherPreparedCount;
    stats.semanticSceneShadowMapTerrainDoodadPreparedCount =
        reconciliation.shadowMapTerrainDoodadPreparedCount;
    stats.semanticSceneShadowMapTerrainS1PreparedCount =
        reconciliation.shadowMapTerrainS1PreparedCount;
    stats.semanticSceneShadowMapCascade0DrawnCount =
        reconciliation.shadowMapCascade0DrawnCount;
    stats.semanticSceneShadowMapCascade1DrawnCount =
        reconciliation.shadowMapCascade1DrawnCount;
    stats.semanticSceneShadowMapCascade2DrawnCount =
        reconciliation.shadowMapCascade2DrawnCount;
    stats.semanticSceneShadowMapCascade3DrawnCount =
        reconciliation.shadowMapCascade3DrawnCount;
    stats.semanticSceneShadowMapCascade0CulledCount =
        reconciliation.shadowMapCascade0CulledCount;
    stats.semanticSceneShadowMapCascade1CulledCount =
        reconciliation.shadowMapCascade1CulledCount;
    stats.semanticSceneShadowMapCascade2CulledCount =
        reconciliation.shadowMapCascade2CulledCount;
    stats.semanticSceneShadowMapCascade3CulledCount =
        reconciliation.shadowMapCascade3CulledCount;
    stats.semanticSceneShadowMapTerrainDoodadCascade0DrawnCount =
        reconciliation.shadowMapTerrainDoodadCascade0DrawnCount;
    stats.semanticSceneShadowMapTerrainDoodadCascade1DrawnCount =
        reconciliation.shadowMapTerrainDoodadCascade1DrawnCount;
    stats.semanticSceneShadowMapTerrainDoodadCascade2DrawnCount =
        reconciliation.shadowMapTerrainDoodadCascade2DrawnCount;
    stats.semanticSceneShadowMapTerrainDoodadCascade3DrawnCount =
        reconciliation.shadowMapTerrainDoodadCascade3DrawnCount;
    stats.semanticSceneShadowMapTerrainS1Cascade0DrawnCount =
        reconciliation.shadowMapTerrainS1Cascade0DrawnCount;
    stats.semanticSceneShadowMapTerrainS1Cascade1DrawnCount =
        reconciliation.shadowMapTerrainS1Cascade1DrawnCount;
    stats.semanticSceneShadowMapTerrainS1Cascade2DrawnCount =
        reconciliation.shadowMapTerrainS1Cascade2DrawnCount;
    stats.semanticSceneShadowMapTerrainS1Cascade3DrawnCount =
        reconciliation.shadowMapTerrainS1Cascade3DrawnCount;
    stats.semanticSceneShadowMapSkinnedCasterCount =
        reconciliation.skinnedCasterCount;
    stats.semanticSceneShadowMapSkinnedPreparedCount =
        reconciliation.skinnedPreparedCount;
    stats.semanticSceneShadowMapSkinnedInvalidBufferCount =
        reconciliation.skinnedInvalidBufferCount;
    stats.semanticSceneShadowMapSkinnedInvalidPipelineCount =
        reconciliation.skinnedInvalidPipelineCount;
    stats.semanticSceneShadowMapSkinnedDrawnCount =
        reconciliation.skinnedDrawnCount;
    stats.semanticSceneShadowTaaActive = reconciliation.shadowTaaActive;
    stats.semanticSceneReceiverReuseShadowMap =
        reconciliation.receiverReuseShadowMap;
    stats.semanticSceneReceiverInputValid =
        reconciliation.receiverInputValid;
    stats.semanticSceneReceiverInputRejectReason =
        reconciliation.receiverInputRejectReason;
    stats.semanticSceneReceiverNeedPass = reconciliation.receiverNeedPass;
    stats.semanticSceneReceiverNeedShadowMap =
        reconciliation.receiverNeedShadowMap;
    stats.semanticSceneReceiverHasCompleteShadowMap =
        reconciliation.receiverHasCompleteShadowMap;
    stats.semanticSceneReceiverHasUsableDirectionalShadow =
        reconciliation.receiverHasUsableDirectionalShadow;
    stats.semanticSceneReceiverActiveStrengthMilli =
        reconciliation.receiverActiveStrengthMilli;
    stats.semanticSceneReceiverUboStrengthMilli =
        reconciliation.receiverUboStrengthMilli;
    stats.semanticSceneReceiverDebugMode = reconciliation.receiverDebugMode;
    stats.semanticSceneReceiverCsmCascadeCount =
        reconciliation.receiverCsmCascadeCount;
    stats.semanticSceneReceiverRunEntryFlags =
        reconciliation.receiverRunEntryFlags;
    stats.semanticSceneReceiverRunEarlyReturnReason =
        reconciliation.receiverRunEarlyReturnReason;
    stats.semanticSceneShadowMapExecutedThisFrame =
        reconciliation.shadowMapExecutedThisFrame;
    stats.semanticSceneReceiverSettingsShadowsEnabled =
        reconciliation.receiverSettingsShadowsEnabled;
    stats.semanticSceneReceiverSettingsOutlineEnabled =
        reconciliation.receiverSettingsOutlineEnabled;
    stats.semanticSceneReceiverSettingsRawStrengthMilli =
        reconciliation.receiverSettingsRawStrengthMilli;
    stats.semanticSceneReceiverComputedShadowStrengthMilli =
        reconciliation.receiverComputedShadowStrengthMilli;
    stats.semanticSceneReceiverHasSunShadow =
        reconciliation.receiverHasSunShadow;
    stats.semanticSceneReceiverHasPointShadow =
        reconciliation.receiverHasPointShadow;
    stats.semanticSceneReceiverNeedOutlinePass =
        reconciliation.receiverNeedOutlinePass;
    stats.semanticSceneReceiverZeroStrengthFrameCount =
        reconciliation.receiverZeroStrengthFrameCount;
    stats.semanticSceneReceiverDrawnWithZeroStrengthCount =
        reconciliation.receiverDrawnWithZeroStrengthCount;
    stats.semanticSceneReceiverNoCompleteShadowMapCount =
        reconciliation.receiverNoCompleteShadowMapCount;
    stats.semanticSceneReceiverNoShadowMapImageCount =
        reconciliation.receiverNoShadowMapImageCount;
    stats.semanticSceneReceiverNoShadowMapSampleViewCount =
        reconciliation.receiverNoShadowMapSampleViewCount;
    stats.semanticSceneReceiverNoCandidateCsmCount =
        reconciliation.receiverNoCandidateCsmCount;
    stats.semanticSceneReceiverCsmFallbackToLastGoodCount =
        reconciliation.receiverCsmFallbackToLastGoodCount;
    stats.semanticSceneReceiverHoldInvalidCsmCount =
        reconciliation.receiverHoldInvalidCsmCount;
    stats.semanticSceneReceiverHoldEmptyReplayCount =
        reconciliation.receiverHoldEmptyReplayCount;
    stats.semanticSceneReceiverHoldIdentityChurnCount =
        reconciliation.receiverHoldIdentityChurnCount;
    stats.semanticSceneReceiverReuseInvalidatedAfterEnsureCount =
        reconciliation.receiverReuseInvalidatedAfterEnsureCount;
    stats.semanticSceneShadowMapRenderSkippedNoResourcesCount =
        reconciliation.shadowMapRenderSkippedNoResourcesCount;
    stats.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount =
        reconciliation.shadowMapRenderSkippedNoMatrixBufferCount;
    stats.semanticSceneReceiverViewportX = reconciliation.receiverViewportX;
    stats.semanticSceneReceiverViewportY = reconciliation.receiverViewportY;
    stats.semanticSceneReceiverViewportWidth =
        reconciliation.receiverViewportWidth;
    stats.semanticSceneReceiverViewportHeight =
        reconciliation.receiverViewportHeight;
    stats.semanticSceneShadowMatrixSceneKey =
        reconciliation.shadowMatrixSceneKey;
    stats.semanticSceneShadowMatrixUploadSerial =
        reconciliation.shadowMatrixUploadSerial;
    stats.semanticSceneShadowMatrixBufferObjectPtr =
        reconciliation.shadowMatrixBufferObjectPtr;
    stats.semanticSceneShadowMatrixBufferOffset =
        reconciliation.shadowMatrixBufferOffset;
    stats.semanticSceneShadowMatrixBufferSize =
        reconciliation.shadowMatrixBufferSize;
    stats.semanticSceneShadowMatrixBufferGpuAddress =
        reconciliation.shadowMatrixBufferGpuAddress;
    stats.semanticSceneReceiverCameraHash =
        reconciliation.receiverCameraHash;
    stats.semanticSceneReceiverSunDirectionHash =
        reconciliation.receiverSunDirectionHash;
    stats.semanticSceneReceiverCsmHash =
        reconciliation.receiverCsmHash;
    stats.semanticSceneReceiverCameraDeltaNano =
        reconciliation.receiverCameraDeltaNano;
    stats.semanticSceneReceiverSunDeltaNano =
        reconciliation.receiverSunDeltaNano;
    stats.semanticSceneReceiverCsmDeltaNano =
        reconciliation.receiverCsmDeltaNano;
    stats.semanticSceneReceiverSnappedCenterDeltaTexelsNano =
        reconciliation.receiverSnappedCenterDeltaTexelsNano;
    stats.semanticSceneReceiverTexelSizeDeltaNano =
        reconciliation.receiverTexelSizeDeltaNano;
    stats.semanticSceneReplayBackingHash =
        reconciliation.replayBackingHash;
    stats.semanticSceneStage13ReplayContentHash =
        reconciliation.stage13ReplayContentHash;
    stats.semanticSceneStage13ReplayBackingHash =
        reconciliation.stage13ReplayBackingHash;
    stats.semanticSceneStage13ReplayDrawCount =
        reconciliation.stage13ReplayDrawCount;
    stats.semanticSceneShadowMapRenderSerial =
        reconciliation.shadowMapRenderSerial;
    stats.semanticSceneShadowMapImagePtr = reconciliation.shadowMapImagePtr;
    stats.semanticSceneShadowMapSampleViewPtr =
        reconciliation.shadowMapSampleViewPtr;
    stats.semanticSceneShadowCurrentImagePtr =
        reconciliation.shadowCurrentImagePtr;
    stats.semanticSceneShadowCurrentViewPtr =
        reconciliation.shadowCurrentViewPtr;
    stats.semanticSceneShadowHistoryReadImagePtr =
        reconciliation.shadowHistoryReadImagePtr;
    stats.semanticSceneShadowHistoryReadViewPtr =
        reconciliation.shadowHistoryReadViewPtr;
    stats.semanticSceneShadowHistoryWriteImagePtr =
        reconciliation.shadowHistoryWriteImagePtr;
    stats.semanticSceneShadowHistoryWriteViewPtr =
        reconciliation.shadowHistoryWriteViewPtr;
    stats.semanticSceneShadowVisibilityExecutedThisFrame =
        reconciliation.shadowVisibilityExecutedThisFrame;
    stats.semanticSceneReceiverDrawExecutedThisFrame =
        reconciliation.receiverDrawExecutedThisFrame;
    stats.semanticSceneShadowTaaMode = reconciliation.shadowTaaMode;
    stats.semanticSceneShadowHistoryValidBefore =
        reconciliation.shadowHistoryValidBefore;
    stats.semanticSceneShadowHistoryValidAfter =
        reconciliation.shadowHistoryValidAfter;
    stats.semanticSceneShadowHistoryReadIndex =
        reconciliation.shadowHistoryReadIndex;
    stats.semanticSceneShadowHistoryWriteIndex =
        reconciliation.shadowHistoryWriteIndex;
    stats.semanticSceneShadowHistoryAdvancedThisFrame =
        reconciliation.shadowHistoryAdvancedThisFrame;
    stats.semanticSceneShadowHistoryAdvanceSkippedIncomplete =
        reconciliation.shadowHistoryAdvanceSkippedIncomplete;
    stats.semanticSceneShadowHistoryInvalidationMask =
        reconciliation.shadowHistoryInvalidationMask;
    stats.semanticSceneShadowReceiverSampleSource =
        reconciliation.shadowReceiverSampleSource;
    dxvk::war3::render::NoteShadowSceneTerminalStats(stats);
    war3::War3PerfMonitor::instance().noteShadowBudgetFrame(stats);
  };

  static bool s_first = true;
  if (s_first) {
    s_first = false;
    WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: FIRST_CALL (BeforeUi)\n");
  }

  VkExtent3D receiverColorExtent = {};
  VkExtent3D receiverDepthExtent = {};
  const ReceiverInputRejectReason receiverInputRejectReason =
      ValidateMainWorldReceiverInput(input, &receiverColorExtent,
                                     &receiverDepthExtent);
  reconciliation.receiverInputRejectReason =
      static_cast<uint32_t>(receiverInputRejectReason);
  reconciliation.receiverInputValid =
      receiverInputRejectReason == ReceiverInputRejectReason::None ? 1u : 0u;
  if (receiverInputRejectReason != ReceiverInputRejectReason::None) {
    reconciliation.receiverRunEarlyReturnReason =
        static_cast<uint32_t>(ReceiverRunEarlyReturnReason::InvalidInput);
    setReceiverRunEntryFlags(false, false, false, false, false, false, false,
                             false, false, false, false, false);
    static uint32_t s_rejectLogs[16] = {};
    const uint32_t reasonIndex =
        std::min<uint32_t>(static_cast<uint32_t>(receiverInputRejectReason),
                           15u);
    if (s_rejectLogs[reasonIndex]++ < 8u) {
      const auto& vp = input.scene.worldCamera.viewport;
      WAR3_RENDER_LOG(
          "DXVK War3ShadowReceiverPass: skip invalid receiver input "
          "reason=%s frame=%llu ring=%u cameraFrame=%llu cameraRing=%u "
          "color=%ux%u depth=%ux%u "
          "vp=%ux%u@(%u,%u) z=(%.3f,%.3f)\n",
          ReceiverInputRejectReasonName(receiverInputRejectReason),
          static_cast<unsigned long long>(input.frameSerial),
          static_cast<unsigned>(input.frameIndex),
          static_cast<unsigned long long>(
              input.scene.worldCamera.frameSerial),
          static_cast<unsigned>(input.scene.worldCamera.frameIndex),
          static_cast<unsigned>(receiverColorExtent.width),
          static_cast<unsigned>(receiverColorExtent.height),
          static_cast<unsigned>(receiverDepthExtent.width),
          static_cast<unsigned>(receiverDepthExtent.height),
          static_cast<unsigned>(vp.Width), static_cast<unsigned>(vp.Height),
          static_cast<unsigned>(vp.X), static_cast<unsigned>(vp.Y),
          static_cast<double>(vp.MinZ), static_cast<double>(vp.MaxZ));
    }
    if (War3RenderState::HasOutlineHandles() && !war3dbg::RenderLogEnabled()) {
      static uint32_t s_outlineSkipLogs = 0;
      if (s_outlineSkipLogs++ < 3) {
        war3dbg::Print("DXVK_Outline: skip (invalid receiver input) reason=%s "
                       "handles=%u\n",
                       ReceiverInputRejectReasonName(receiverInputRejectReason),
                       War3RenderState::GetOutlineHandleCount());
      }
    }
    publishReconciliationStats();
    return;
  }
  {
    const auto& vp = input.scene.worldCamera.viewport;
    reconciliation.receiverViewportX = vp.X;
    reconciliation.receiverViewportY = vp.Y;
    reconciliation.receiverViewportWidth = vp.Width;
    reconciliation.receiverViewportHeight = vp.Height;
    static uint32_t s_validLogs = 0;
    if (s_validLogs++ < 3u) {
      WAR3_RENDER_LOG(
          "DXVK War3ShadowReceiverPass: receiver input valid frame=%llu "
          "ring=%u cameraFrame=%llu cameraRing=%u color=%ux%u depth=%ux%u "
          "vp=%ux%u@(%u,%u) "
          "z=(%.3f,%.3f)\n",
          static_cast<unsigned long long>(input.frameSerial),
          static_cast<unsigned>(input.frameIndex),
          static_cast<unsigned long long>(
              input.scene.worldCamera.frameSerial),
          static_cast<unsigned>(input.scene.worldCamera.frameIndex),
          static_cast<unsigned>(receiverColorExtent.width),
          static_cast<unsigned>(receiverColorExtent.height),
          static_cast<unsigned>(receiverDepthExtent.width),
          static_cast<unsigned>(receiverDepthExtent.height),
          static_cast<unsigned>(vp.Width), static_cast<unsigned>(vp.Height),
          static_cast<unsigned>(vp.X), static_cast<unsigned>(vp.Y),
          static_cast<double>(vp.MinZ), static_cast<double>(vp.MaxZ));
    }
  }

  War3RenderSettings defaultSettings = {};
  const War3RenderSettings *settings =
      input.settings ? input.settings.get() : &defaultSettings;
  const bool linearShadowFilter =
      settings->shadows.filterMode == War3ShadowFilterMode::Linear;
  if (!linearShadowFilter) {
    m_shadowCompareSamplerActive = m_shadowCompareSampler;
    m_shadowCompareMode = 0u;
  } else if (m_shadowCompareLinearSupported) {
    m_shadowCompareSamplerActive = m_shadowCompareSamplerLinear;
    m_shadowCompareMode = 1u;
  } else {
    // The comparison sampler remains nearest; the shader compares the four
    // raw D32 texels before bilinear interpolation.
    m_shadowCompareSamplerActive = m_shadowCompareSampler;
    m_shadowCompareMode = 2u;
  }
  const float casterBias = std::max(settings->shadows.casterDepthBias, 0.0f);
  const float casterSlope = std::max(settings->shadows.casterSlopeBias, 0.0f);
  const float casterClamp = std::max(settings->shadows.casterBiasClamp, 0.0f);
  if (casterBias != m_shadowCasterBiasConstant ||
      casterSlope != m_shadowCasterBiasSlope ||
      casterClamp != m_shadowCasterBiasClamp) {
    // 参数变化时重建 caster 管线，确保偏移生效
    m_shadowCasterBiasConstant = casterBias;
    m_shadowCasterBiasSlope = casterSlope;
    m_shadowCasterBiasClamp = casterClamp;

    // The old cache entries may already be referenced by submitted command
    // buffers. Their tracked owners defer vkDestroyPipeline until the last
    // command list completes; clearing this map only prevents future binds.
    m_shadowCasterPipelines.clear();
  }
  const bool shadowsEnabled = settings->shadows.enabled;
  const bool outlineEnabled = settings->occludedOutline.enabled;
  reconciliation.receiverSettingsShadowsEnabled = shadowsEnabled ? 1u : 0u;
  reconciliation.receiverSettingsOutlineEnabled = outlineEnabled ? 1u : 0u;
  reconciliation.receiverSettingsRawStrengthMilli =
      strengthToMilli(settings->shadows.strength);
  setReceiverRunEntryFlags(true, shadowsEnabled, outlineEnabled, false, false,
                           false, false, false, false, false, false, false);
  if (!shadowsEnabled && !outlineEnabled) {
    reconciliation.receiverRunEarlyReturnReason =
        static_cast<uint32_t>(
            ReceiverRunEarlyReturnReason::ShadowsAndOutlineDisabled);
    if (War3RenderState::HasOutlineHandles()) {
      static bool s_loggedOutlineDisabled = false;
      if (!s_loggedOutlineDisabled) {
        s_loggedOutlineDisabled = true;
        WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: outline disabled in "
                        "settings (handles=%u)\n",
                        War3RenderState::GetOutlineHandleCount());
      }
      if (!war3dbg::RenderLogEnabled()) {
        static uint32_t s_outlineDisabledLogs = 0;
        if (s_outlineDisabledLogs++ < 3) {
          war3dbg::Print("DXVK_Outline: disabled in settings (handles=%u)\n",
                         War3RenderState::GetOutlineHandleCount());
        }
      }
    }
    publishReconciliationStats();
    return;
  }

  const bool hasListeners = war3shader::internal::HasAnyRenderListeners();
  if (hasListeners) {
    war3shader::internal::DispatchRenderEvent(
        war3shader::RenderEventID::SHADOW_PASS_BEGIN);
  }

  shadowMainPhaseTiming.enter(
      static_cast<size_t>(War3ShadowMainRawPhase::LightingAndPolicy));
  // ========== 日夜循环逻辑 ==========
  const auto now = std::chrono::steady_clock::now();
  if (!m_timeInitialized) {
    m_timeInitialized = true;
    m_timeStart = now;
    m_timeSmoothingInitialized = false;
    m_time01LastUpdate = now;
    m_time01LastRawSample = now;
    m_time01Speed = 1.0f / std::max(1.0f, m_dayLengthSeconds);

    // 推断 worldUp（保持原有逻辑）
    const Matrix4 invView = inverse(input.scene.worldCamera.view);
    Vector4 camUpWorld = invView * Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    float len =
        std::sqrt(camUpWorld.x * camUpWorld.x + camUpWorld.y * camUpWorld.y +
                  camUpWorld.z * camUpWorld.z);
    if (len > 1e-6f) {
      camUpWorld.x /= len;
      camUpWorld.y /= len;
      camUpWorld.z /= len;
    }

    const Vector4 yUp(0.0f, 1.0f, 0.0f, 0.0f);
    const Vector4 zUp(0.0f, 0.0f, 1.0f, 0.0f);
    float yScore = std::abs(camUpWorld.x * yUp.x + camUpWorld.y * yUp.y +
                            camUpWorld.z * yUp.z);
    float zScore = std::abs(camUpWorld.x * zUp.x + camUpWorld.y * zUp.y +
                            camUpWorld.z * zUp.z);

    m_cachedWorldUp = (yScore >= zScore) ? yUp : zUp;

    WAR3_RENDER_LOG("DXVK War3Shadow: DayNight system started. worldUp=%s\n",
                    (yScore >= zScore) ? "Y" : "Z");
  }

  // 1. 计算当前时间（0..1）
  auto wrap01 = [](float v) {
    v = std::fmod(v, 1.0f);
    if (v < 0.0f)
      v += 1.0f;
    return v;
  };
  auto diffSigned01 = [&](float a, float b) {
    // 返回 a-b 的最短差值，范围约 [-0.5, 0.5)
    float d = a - b;
    d -= std::floor(d + 0.5f);
    return d;
  };
  auto diffForward01 = [&](float a, float b) {
    // 返回从 b 走到 a 的“正向”差值，范围 [0, 1)
    float d = a - b;
    d -= std::floor(d);
    return d;
  };

  float rawTime01 = 0.0f;
  const float realGameTime = War3RenderState::GetGameTime();
  const War3DayNightSettings &dayNightSettings = settings->dayNight;
  War3RenderSettings mutableSettings = *settings;

  static bool s_hasValidGameTime = false;
  const bool hasRealGameTime =
      realGameTime >= 0.0f && realGameTime <= 24.0f;
  const War3LightingClockMode clockMode = dayNightSettings.clockMode;
  if (clockMode != War3LightingClockMode::GameTime) {
    rawTime01 = wrap01(dayNightSettings.renderTimeHours / 24.0f);
    m_timeSmoothingInitialized = false;
  } else if (hasRealGameTime) {
    // Game Time 0=Midnight, 6=Sunrise, 18=Sunset, 24=Midnight.
    if (!s_hasValidGameTime) {
      s_hasValidGameTime = true;
      WAR3_RENDER_LOG("DXVK War3Shadow: Switched to Real Game Time! t=%f\n",
                      realGameTime);
    }
    rawTime01 = wrap01(realGameTime / 24.0f);
  } else {
    // Follow-game mode retains the historical startup fallback until the
    // native TIME_OF_DAY source becomes readable.
    static int s_failLog = 0;
    if (s_failLog++ < 10) {
      WAR3_RENDER_LOG("DXVK War3Shadow: Fallback used. RealGameTime=%.4f "
                      "(Valid Range: 0-24)\n",
                      realGameTime);
    }
    const float elapsed =
        std::chrono::duration<float>(now - m_timeStart).count();
    rawTime01 = wrap01(elapsed / m_dayLengthSeconds + m_startTime01);
  }

  const War3ShadowSettings &shadowSettings = settings->shadows;
  if (shadowSettings.lockSun) {
    rawTime01 = std::clamp(shadowSettings.lockSunTime, 0.0f, 1.0f);
    // 锁太阳时强制重置平滑状态，避免解锁后“追赶”造成突跳
    m_timeSmoothingInitialized = false;
  }

  // 说明：TIME_OF_DAY 通常是 100ms 台阶更新；这里把它平滑成逐帧连续值，
  // 以消除日夜切换/太阳移动时的高频跳变（尤其在 CSM 与阴影接收器上很明显）。
  float time01 = rawTime01;
  if (clockMode == War3LightingClockMode::GameTime && hasRealGameTime &&
      !shadowSettings.lockSun) {
    if (!m_timeSmoothingInitialized) {
      m_timeSmoothingInitialized = true;
      m_time01Smoothed = rawTime01;
      m_time01LastRaw = rawTime01;
      m_time01Speed = 1.0f / std::max(1.0f, m_dayLengthSeconds);
      m_time01LastUpdate = now;
      m_time01LastRawSample = now;
    } else {
      const float dt =
          std::chrono::duration<float>(now - m_time01LastUpdate).count();
      m_time01LastUpdate = now;
      if (dt > 0.0f && dt < 0.25f) {
        m_time01Smoothed = wrap01(m_time01Smoothed + m_time01Speed * dt);
      }

      const float rawDelta = diffForward01(rawTime01, m_time01LastRaw);
      if (rawDelta > 1e-5f) {
        const float rawDt =
            std::chrono::duration<float>(now - m_time01LastRawSample).count();
        m_time01LastRawSample = now;
        m_time01LastRaw = rawTime01;

        // 时间被脚本/触发器重置时可能会出现大跳变；此时直接对齐，避免“极速追赶”
        if constexpr (dxvk::war3::internal::kShadowTimeAdaptiveSpeedEnabled) {
          if (rawDt > 0.0f && rawDt < 2.0f && rawDelta < 0.25f) {
            const float newSpeed = rawDelta / rawDt;
            // 合理上限：允许加速（比如加速昼夜），但避免异常值把系统带飞
            const float maxSpeed = 1.0f / 5.0f; // 5 秒跑完一天已经非常夸张
            m_time01Speed = std::clamp(newSpeed, 0.0f, maxSpeed);
          } else {
            m_time01Smoothed = rawTime01;
            m_time01Speed = 1.0f / std::max(1.0f, m_dayLengthSeconds);
          }
        } else {
          if (rawDelta >= 0.25f || rawDt >= 2.0f)
            m_time01Smoothed = rawTime01;
          m_time01Speed = 1.0f / std::max(1.0f, m_dayLengthSeconds);
        }
      }

      const float err = diffSigned01(rawTime01, m_time01Smoothed);
      const float absErr = std::abs(err);
      if (absErr > 0.25f) {
        // 误差过大（例如瞬移时间/重置时间），直接对齐
        m_time01Smoothed = rawTime01;
      } else {
        // 小误差回正：限制每帧回正幅度，避免可见跳动
        constexpr float kPull = 0.05f;
        constexpr float kMaxPullPerFrame = 0.02f;
        const float pull =
            std::clamp(err * kPull, -kMaxPullPerFrame, kMaxPullPerFrame);
        m_time01Smoothed = wrap01(m_time01Smoothed + pull);
      }
    }
    time01 = m_time01Smoothed;
  }

  mutableSettings.dayNight.renderTimeHours = wrap01(time01) * 24.0f;

  // 2. 计算太阳的“真实”轨迹 (Real Trajectory)
  // 假设：X=东, -X=西, Y=北, -Y=南, Z=上
  // 方位角(Azimuth): 从东(0) -> 南(PI/2) -> 西(PI)
  // 让太阳稍微偏南一点，保证阴影更有立体感
  // Time 0.25(Sunrise) -> Azimuth 0 (East)
  // Time 0.50(Noon)    -> Azimuth PI/2 (South)
  // Time 0.75(Sunset)  -> Azimuth PI (West)

  // 真实高度角 (Real Altitude): 正弦波模拟
  // T=0.25(Sunrise) -> Alt=0, T=0.5(Noon) -> Alt=90
  // sin argument: (time01 - 0.25) * 2 * PI -> 0 ~ PI (for 0.25~0.75 range)
  // (time01 - 0.25) * 2PI 涵盖整个 0..1 周期 (对应 0..2PI)
  float sunAnglePhase = (time01 - 0.25f) * 2.0f * 3.14159265f;
  float realAltitudeRad = std::sin(sunAnglePhase) * (3.14159265f / 2.0f);

  // 3. 计算受控的“投影”高度角（全时段平滑变化）
  float minAltRad = m_minSunAltitudeDeg * (3.14159265f / 180.0f);
  float maxAltRad = m_maxSunAltitudeDeg * (3.14159265f / 180.0f);
  const float halfPi = 3.14159265f / 2.0f;

  auto smoothstep01 = [](float v) {
    v = std::min(1.0f, std::max(0.0f, v));
    return v * v * (3.0f - 2.0f * v);
  };

  auto calcLinearBell01 = [&](float phase01) {
    float p = std::min(1.0f, std::max(0.0f, phase01));
    float tri = 1.0f - std::abs(p * 2.0f - 1.0f);
    return std::min(1.0f, std::max(0.0f, tri));
  };

  float lengthScale = std::clamp(shadowSettings.shadowLengthScale, 0.1f, 2.0f);
  float maxLenScale =
      std::clamp(shadowSettings.shadowMaxLengthScale, 0.1f, 2.0f);

  auto calcShadowAltitude01 = [&](float altitude01) {
    float t = smoothstep01(altitude01);
    float minAltForLength = minAltRad;
    if (maxLenScale > 0.0f) {
      float baseTan = std::tan(std::max(0.001f, minAltRad));
      float scaledMinAlt = std::atan(baseTan * (lengthScale / maxLenScale));
      minAltForLength = std::min(scaledMinAlt, maxAltRad - 0.01f);
    }
    float baseAlt = minAltForLength + (maxAltRad - minAltForLength) * t;
    float tanBase = std::tan(std::max(0.001f, baseAlt));
    return std::atan(tanBase / lengthScale);
  };

  float dayPhase = (time01 - 0.25f) / 0.5f;
  float dayAlt01 = 0.0f;
  if (shadowSettings.altitudeMode == War3ShadowAltitudeMode::TimeLinear) {
    dayAlt01 = calcLinearBell01(dayPhase);
  } else {
    dayAlt01 = std::clamp(realAltitudeRad / halfPi, 0.0f, 1.0f);
  }
  float shadowAltRad = calcShadowAltitude01(dayAlt01);

  // 2026-07-21 优化：进程环境变量在启动后不可变，每帧两次 getenv+string
  // 分配是纯浪费。与其他 flag 一致改为静态缓存（语义完全等价）。
  static const int s_debugOverride =
      EnvIntOverride("DXVK_WAR3_SHADOW_DEBUG", 0, 9);
  if (s_debugOverride >= 0) {
    mutableSettings.shadows.debugMode =
        static_cast<War3ShadowDebugMode>(s_debugOverride);
    static int s_lastDebugOverrideLog = -1;
    if (s_lastDebugOverrideLog != s_debugOverride) {
      s_lastDebugOverrideLog = s_debugOverride;
      WAR3_RENDER_LOG(
          "DXVK War3Shadow: runtime DXVK_WAR3_SHADOW_DEBUG=%d\n",
          s_debugOverride);
    }
  }
  static const int s_pointDebugOverride = EnvIntOverride(
      "DXVK_WAR3_POINT_SHADOW_DEBUG_LIGHT", 0,
      int(kMaxPointShadowLights - 1u));
  if (s_pointDebugOverride >= 0) {
    mutableSettings.shadows.pointShadowDebugLightIndex =
        static_cast<uint32_t>(s_pointDebugOverride);
  }

  // 5. 计算光源方向向量 (Z-Up)
  // 修正：War3 中 +X=East, +Y=North.
  // 想要太阳从东(X) -> 南(-Y) -> 西(-X)
  // Angle 0 = East (+X).
  // Angle -90 (PI/2) = South (-Y).
  // Angle -180 (PI) = West (-X).
  // 所以我们让 azimuth 从 0 变到 -PI

  // [Fix] Optional time quantization for diagnosis. Default stays smooth:
  // quantized sun steps can look like periodic shadow pops when visual QA is
  // focused on flicker.
  const float kSunQuantStep = 0.0002f;
  float quantizedTime01 = time01;
  if constexpr (dxvk::war3::internal::kShadowSunTimeQuantizationEnabled)
    quantizedTime01 = std::round(time01 / kSunQuantStep) * kSunQuantStep;

  float rawDayPhase = (quantizedTime01 - 0.25f) / 0.5f;
  float sunYaw = -rawDayPhase * 3.14159265f;

  // 球坐标转笛卡尔 (Z-Up)
  // X = cos(Alt) * cos(Yaw)
  // Y = cos(Alt) * sin(Yaw)
  // Z = sin(Alt)
  float cosAlt = std::cos(shadowAltRad);
  float sinAlt = std::sin(shadowAltRad);
  float cosYaw = std::cos(sunYaw);
  float sinYaw = std::sin(sunYaw);

  Vector4 sunPos;
  sunPos.x = cosAlt * cosYaw;
  sunPos.y = cosAlt * sinYaw;
  sunPos.z = sinAlt;

  // LightDir = -SunPos
  mutableSettings.sun.direction =
      Vector4(-sunPos.x, -sunPos.y, -sunPos.z, 0.0f);

  // 6. 光源逻辑 (Sun vs Moon)
  Vector4 finalLightDir;
  Vector4 finalLightColor;
  float finalShadowStrength = 0.0f;

  // 关键视觉参数
  // 地平线颜色：改为【暖灰/米色】，消除过于鲜艳的红色，提供更自然的晨昏过渡
  // 原值(0.5, 0.35, 0.35)太红 -> 新值(0.7, 0.65, 0.6)更亮且低饱和
  const Vector4 kColorHorizon = Vector4(0.7f, 0.65f, 0.60f, 1.0f);
  // 月光颜色：更偏冷蓝以增强夜间氛围
  const Vector4 kColorMoon = Vector4(0.4f, 0.55f, 0.95f, 1.0f);

  // 混合过渡带宽度 (度)：加大到 24 度，让变化更从容
  const float kTransitionDeg = 24.0f;
  const float kTransitionRad = kTransitionDeg * (3.14159265f / 180.0f);

  // 阴影衰减地平线 (Decoupled Fade Horizon)
  const float kFadeHorizonDeg = 10.0f;
  const float kFadeHorizonRad = kFadeHorizonDeg * (3.14159265f / 180.0f);

  // ======== 白天 (Sun) 结果 ========
  Vector4 dayLightDir = mutableSettings.sun.direction;
  float dayAlt = std::max(0.0f, realAltitudeRad);
  Vector4 dayColor;
  if (dayAlt > kTransitionRad) {
    float range = (3.14159265f / 2.0f) - kTransitionRad;
    float tPrime = std::max(0.0f, (dayAlt - kTransitionRad) / range);
    float kelvin = 2500.0f + tPrime * 4000.0f;
    dayColor = kelvinToRgb(kelvin);
  } else {
    float linearBlend =
        (kTransitionRad > 0.0f) ? (dayAlt / kTransitionRad) : 0.0f;
    float blend = linearBlend * linearBlend * (3.0f - 2.0f * linearBlend);
    Vector4 sunLow = kelvinToRgb(2500.0f);
    dayColor = kColorHorizon + (sunLow - kColorHorizon) * blend;
  }
  float dayFadeRatio = (kFadeHorizonRad > 0.0f)
                           ? std::min(1.0f, dayAlt / kFadeHorizonRad)
                           : 0.0f;

  // 【修复】太阳光强度应该与阴影强度绑定
  // 原代码使用 minLightFactor (0.85) 作为最低值，导致太阳落下后依然很亮
  // 修改为：太阳升起时 100%，太阳落下时接近 0%（只保留月光水平）
  const float nightLightLevel = 0.15f; // 夜间光照水平（月光）
  const float dayLightFactor =
      nightLightLevel + (1.0f - nightLightLevel) * dayFadeRatio;

  Vector4 dayLightColor = dayColor * dayLightFactor;
  float dayShadowStrength = dayFadeRatio * 0.6075f;

  // ======== 黑夜 (Moon) 结果 ========
  float moonRealAlt = std::max(0.0f, -realAltitudeRad);
  float nightPhase = 0.0f;
  if (time01 < 0.25f) {
    nightPhase = (time01 + 0.25f) / 0.5f;
  } else {
    nightPhase = (time01 - 0.75f) / 0.5f;
  }
  float moonAlt01 = 0.0f;
  if (shadowSettings.altitudeMode == War3ShadowAltitudeMode::TimeLinear) {
    moonAlt01 = calcLinearBell01(nightPhase);
  } else {
    moonAlt01 = std::clamp(moonRealAlt / halfPi, 0.0f, 1.0f);
  }
  float moonShadowAlt = calcShadowAltitude01(moonAlt01);
  float moonYaw = sunYaw + 3.14159265f;
  float cAlt = std::cos(moonShadowAlt);
  float sAlt = std::sin(moonShadowAlt);
  float cYaw = std::cos(moonYaw);
  float sYaw = std::sin(moonYaw);
  Vector4 moonPos;
  moonPos.x = cAlt * cYaw;
  moonPos.y = cAlt * sYaw;
  moonPos.z = sAlt;
  Vector4 nightLightDir = Vector4(-moonPos.x, -moonPos.y, -moonPos.z, 0.0f);

  Vector4 nightColor;
  if (moonRealAlt < kTransitionRad) {
    float linearBlend =
        (kTransitionRad > 0.0f) ? (moonRealAlt / kTransitionRad) : 0.0f;
    float blend = linearBlend * linearBlend * (3.0f - 2.0f * linearBlend);
    nightColor = kColorHorizon + (kColorMoon - kColorHorizon) * blend;
  } else {
    nightColor = kColorMoon;
  }
  float moonFadeRatio = (kFadeHorizonRad > 0.0f)
                            ? std::min(1.0f, moonRealAlt / kFadeHorizonRad)
                            : 0.0f;

  // 【修复】月光强度使用夜间基础水平，月亮落下后保持最低亮度
  // 复用上面定义的 nightLightLevel
  const float nightLightFactor =
      nightLightLevel + (1.0f - nightLightLevel) * moonFadeRatio;
  Vector4 nightLightColor = nightColor * nightLightFactor;
  const float nightShadowScale =
      std::clamp(shadowSettings.nightShadowScale, 0.0f, 1.0f);
  float nightShadowStrength = moonFadeRatio * 0.5508f * nightShadowScale;

  // ======== 主光源选择（解决“日夜切换阴影方向瞬移”） ========
  // 说明：
  // -
  // 日落/日出时，太阳与月亮方向接近相反，直接对方向做线性混合会出现“向量相互抵消”，
  //   进而产生极端角速度：看起来像太阳/月亮在一秒内快速乱跑。
  // - 我们渲染端只有一套方向光阴影（CSM），因此在过渡带内应当“选一个主光源”，
  //   并依赖地平线附近的阴影强度淡出使切换不可见。
  const bool useDayLight = realAltitudeRad >= 0.0f;
  finalLightDir = useDayLight ? dayLightDir : nightLightDir;
  finalLightColor = useDayLight ? dayLightColor : nightLightColor;
  finalShadowStrength = useDayLight ? dayShadowStrength : nightShadowStrength;

  if (dayNightSettings.timeColorGradingEnabled &&
      dayNightSettings.customColorTemperatureProfile) {
    const auto validKelvin = [](float value, float fallback) {
      return std::isfinite(value)
          ? std::clamp(value, 1000.0f, 20000.0f)
          : fallback;
    };
    const float keyKelvin[4] = {
        validKelvin(dayNightSettings.midnightKelvin, 9000.0f),
        validKelvin(dayNightSettings.dawnKelvin, 2500.0f),
        validKelvin(dayNightSettings.noonKelvin, 6500.0f),
        validKelvin(dayNightSettings.duskKelvin, 2500.0f),
    };
    const float hours = wrap01(time01) * 24.0f;
    const uint32_t segment =
        std::min(3u, static_cast<uint32_t>(hours / 6.0f));
    const uint32_t next = (segment + 1u) & 3u;
    const float segmentT = smoothstep01(
        (hours - static_cast<float>(segment) * 6.0f) / 6.0f);
    const float kelvin = keyKelvin[segment] +
        (keyKelvin[next] - keyKelvin[segment]) * segmentT;
    const float brightness =
        useDayLight ? dayLightFactor : nightLightFactor;
    finalLightColor = kelvinToRgb(kelvin) * brightness;
  }

  // Manual ownership is final. When a cycle is disabled the pass neither
  // rewrites the corresponding global sun value nor substitutes a local CSM
  // value, so a JASS setting remains stable on every subsequent frame.
  if (!dayNightSettings.celestialMotionEnabled) {
    finalLightDir = settings->sun.direction;
    finalShadowStrength = settings->shadows.strength;
  }
  if (!dayNightSettings.timeColorGradingEnabled)
    finalLightColor = settings->sun.color;

  // [Event System] Update Phase
  bool isRising = std::cos(time01 * (2.0f * 3.14159265f)) >= 0.0f;
  UpdatePhase(realAltitudeRad, isRising, kTransitionRad);

  // Shadow-specific day-night strength remains local to this pass.
  mutableSettings.sun.direction = finalLightDir;
  mutableSettings.sun.color = finalLightColor;
  mutableSettings.shadows.strength = finalShadowStrength;

  // Later passes in this queued command consume a private derived-lighting
  // object. Never cast away const on the authored settings snapshot: it may be
  // shared by async readers and is intentionally immutable.
  if (input.lighting != nullptr) {
    input.lighting->renderTimeHours =
        mutableSettings.dayNight.renderTimeHours;
    input.lighting->sunDirection = finalLightDir;
    input.lighting->sunColor = finalLightColor;
  }

  // Feed only resolved day/night fields back for the next render-owner frame.
  // A newer JASS/UI edit increments pendingRevision and rejects this stale CS
  // feedback instead of being overwritten by an older queued command.
  if (input.settingsMailbox != nullptr) {
    const auto mailbox = input.settingsMailbox;
    std::lock_guard<std::mutex> lock(mailbox->mutex);
    if (mailbox->pendingRevision == input.settingsRevision) {
      mailbox->pending.dayNight.renderTimeHours =
          mutableSettings.dayNight.renderTimeHours;
      if (dayNightSettings.celestialMotionEnabled)
        mailbox->pending.sun.direction = finalLightDir;
      if (dayNightSettings.timeColorGradingEnabled)
        mailbox->pending.sun.color = finalLightColor;
      ++mailbox->pendingRevision;
    }
  }

  // 调试输出 (每 60 帧或满足条件时)
  /*         static int s_logTimer = 0;
          if (s_logTimer++ > 120) {
              s_logTimer = 0;
              WAR3_RENDER_LOG("DXVK War3Shadow: T=%.2f RealAlt=%.1f°
     Clamped=%.1f° Str=%.2f Color=(%.2f,%.2f,%.2f)\n", time01, realAltitudeRad *
     180.0f / 3.14159265f, shadowAltRad * 180.0f / 3.14159265f,
                  mutableSettings.shadows.strength,
                  lightColor.x, lightColor.y, lightColor.z);
          } */

  static uint32_t s_logCount = 0;
  if (s_logCount < 10) {
    s_logCount++;
    WAR3_RENDER_LOG("DXVK War3Shadow: t=%.3f shadowAlt=%.2f str=%.2f "
                    "dir=(%.2f,%.2f,%.2f)\n",
                    static_cast<double>(time01),
                    static_cast<double>(shadowAltRad * 180.0f / 3.14159265f),
                    static_cast<double>(mutableSettings.shadows.strength),
                    static_cast<double>(mutableSettings.sun.direction.x),
                    static_cast<double>(mutableSettings.sun.direction.y),
                    static_cast<double>(mutableSettings.sun.direction.z));
  }

  m_csmConfig = mutableSettings.shadows.csm;
  // The ordinary surface CSM remains bit-for-bit unchanged while volumetrics
  // are disabled. When the volume consumer is active, reserve a fixed
  // toward-sun slice in C2/C3 so an upstream tree/building is not clipped out
  // merely because the receiver frustum is farther from the camera. This is
  // deliberately bounded: the previous symmetric +3000 expansion destroyed
  // useful normalized-depth precision and made surface shadows shimmer.
  if (!std::isfinite(m_csmConfig.farCasterDepthExtension))
    m_csmConfig.farCasterDepthExtension = 0.0f;
  m_csmConfig.farCasterDepthExtension =
      std::clamp(m_csmConfig.farCasterDepthExtension, 0.0f, 384.0f);
  if (mutableSettings.postFx.enabled &&
      mutableSettings.postFx.volumetricLight.enabled) {
    m_csmConfig.farCasterDepthExtension =
        (std::max)(m_csmConfig.farCasterDepthExtension, 384.0f);
  }
  // Diagnostic override for separating upstream-caster Z clipping from
  // capture/publication loss. The normal path remains byte-for-byte unchanged
  // when the variable is absent. Values above the production 384-unit volume
  // allowance are intentionally permitted only through this explicit gate.
  static const float s_farCasterDepthExtensionOverride =
      EnvFloatDefault("DXVK_WAR3_SHADOW_FAR_CASTER_DEPTH_EXTENSION", -1.0f);
  if (s_farCasterDepthExtensionOverride >= 0.0f) {
    m_csmConfig.farCasterDepthExtension =
        std::clamp(s_farCasterDepthExtensionOverride, 0.0f, 4096.0f);
  }
  if (dayNightSettings.celestialMotionEnabled && !shadowSettings.lockSun &&
      !shadowSettings.stableSnapWhenSunMoving) {
    m_csmConfig.stableSnap = 0.0f;
  }
  // Settings can also arrive through runtime/UI paths, so keep a consumer-side
  // finite gate in addition to rejecting NaN/Inf environment values. A NaN
  // filter radius would otherwise poison every cube lookup coordinate.
  const auto finitePointShadowSetting = [](float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
  };
  m_pointShadowBias = std::max(
      0.0f, finitePointShadowSetting(
          mutableSettings.shadows.pointShadowBias, 0.05f));
  const float pointPcfNear = std::clamp(
      finitePointShadowSetting(
          mutableSettings.shadows.pointShadowPcfRadiusNear, 0.65f),
      0.0f, 4.0f);
  const float pointPcfFar = std::clamp(
      finitePointShadowSetting(
          mutableSettings.shadows.pointShadowPcfRadiusFar, 1.15f),
      pointPcfNear, 6.0f);
  m_pointShadowFilterParams = Vector4(
      pointPcfNear, pointPcfFar,
      std::clamp(finitePointShadowSetting(
                     mutableSettings.shadows.pointShadowTexelBiasScale,
                     0.50f),
                 0.0f, 1.0f),
      std::clamp(finitePointShadowSetting(
                     mutableSettings.shadows.pointShadowRangeFadeStart,
                     0.78f),
                 0.50f, 0.98f));
  m_pointShadowDebugLightIndex =
      std::min<uint32_t>(mutableSettings.shadows.pointShadowDebugLightIndex,
                         kMaxPointShadowLights - 1u);

  // 如果阴影强度太低，跳过阴影渲染
  // [Opt P0-4] Hard Gate: Skip entire receiver if no shadows needed
  const bool debugShadow =
      mutableSettings.shadows.debugMode != War3ShadowDebugMode::None;
  bool hasSunShadow =
      (mutableSettings.shadows.strength > 0.001f) || debugShadow;
  // 点光总开关关闭时只读 atomic 计数，不构建快照，确保零额外 CPU 影响 CSM。
  m_pointLightsEnabled = settings->shadows.pointLightsEnabled;
  m_hasPointLights =
      m_pointLightsEnabled && War3LightManager::Instance().HasActiveLights();
  bool hasPointShadow =
      m_pointLightsEnabled && mutableSettings.shadows.pointShadowEnabled &&
      mutableSettings.shadows.pointShadowMaxLights > 0 && m_hasPointLights;
  m_pointShadowEnabled = hasPointShadow;
  {
    static uint32_t s_pointShadowStateLog = 0;
    if (s_pointShadowStateLog < 12 &&
        (m_pointLightsEnabled || mutableSettings.shadows.pointShadowEnabled)) {
      s_pointShadowStateLog++;
      WAR3_RENDER_LOG(
          "DXVK PointShadow: state lightsEnabled=%d activeLights=%u "
          "shadowEnabled=%d hasPointShadow=%d ready=%u bias=%.4f "
          "temporal=%d period=%u\n",
          m_pointLightsEnabled ? 1 : 0,
          War3LightManager::Instance().GetLightCount(),
          mutableSettings.shadows.pointShadowEnabled ? 1 : 0,
          hasPointShadow ? 1 : 0, m_pointShadowReadyCount,
          static_cast<double>(m_pointShadowBias),
          mutableSettings.shadows.pointShadowTemporalReuse ? 1 : 0,
          mutableSettings.shadows.pointShadowUpdatePeriod);
    }
  }
  if (!m_pointShadowEnabled) {
    invalidatePointShadowPublishedState();
  }

  const bool needOutlinePass =
      outlineEnabled && War3RenderState::HasOutlineHandles();
  const float activeShadowStrengthForGates =
      shadowsEnabled ? mutableSettings.shadows.strength : 0.0f;
  reconciliation.receiverComputedShadowStrengthMilli =
      strengthToMilli(mutableSettings.shadows.strength);
  reconciliation.receiverActiveStrengthMilli =
      strengthToMilli(activeShadowStrengthForGates);
  reconciliation.receiverDebugMode =
      uint32_t(static_cast<int>(mutableSettings.shadows.debugMode));
  reconciliation.receiverHasSunShadow = hasSunShadow ? 1u : 0u;
  reconciliation.receiverHasPointShadow = hasPointShadow ? 1u : 0u;
  reconciliation.receiverNeedOutlinePass = needOutlinePass ? 1u : 0u;
  setReceiverRunEntryFlags(true, shadowsEnabled, outlineEnabled, hasSunShadow,
                           hasPointShadow, m_hasPointLights, needOutlinePass,
                           false, false, false, false, debugShadow);
  if (!hasSunShadow && !hasPointShadow && !m_hasPointLights &&
      !needOutlinePass) {
    reconciliation.receiverRunEarlyReturnReason =
        static_cast<uint32_t>(ReceiverRunEarlyReturnReason::NoWorkNeeded);
    if (hasListeners) {
      war3shader::internal::DispatchRenderEvent(
          war3shader::RenderEventID::SHADOW_PASS_END);
    }
    publishReconciliationStats();
    return;
  }
  {
    static bool s_loggedParams = false;
    if (!s_loggedParams) {
      s_loggedParams = true;
      const auto &vp = input.scene.worldCamera.viewport;
      WAR3_RENDER_LOG(
          "DXVK War3ShadowReceiverPass: params cascades=%u res=%u snap=%.0f "
          "maxDist=%.1f lambda=%.2f sunDir=(%.3f,%.3f,%.3f) vp=%ux%u@(%u,%u) "
          "z=(%.3f,%.3f)\n",
          static_cast<unsigned>(std::min<uint32_t>(
              std::max<uint32_t>(m_csmConfig.cascadeCount, 1u), 4u)),
          static_cast<unsigned>(
              std::max<uint32_t>(m_csmConfig.shadowResolution, 1u)),
          static_cast<double>(m_csmConfig.stableSnap),
          static_cast<double>(m_csmConfig.maxDistance),
          static_cast<double>(m_csmConfig.splitLambda),
          static_cast<double>(mutableSettings.sun.direction.x),
          static_cast<double>(mutableSettings.sun.direction.y),
          static_cast<double>(mutableSettings.sun.direction.z),
          static_cast<unsigned>(vp.Width), static_cast<unsigned>(vp.Height),
          static_cast<unsigned>(vp.X), static_cast<unsigned>(vp.Y),
          static_cast<double>(vp.MinZ), static_cast<double>(vp.MaxZ));
    }
  }

  shadowMainPhaseTiming.enter(
      static_cast<size_t>(War3ShadowMainRawPhase::ReplayAndCsmPrepare));
  const auto& replayDraws = BuildShadowReplayDraws(input.scene, input.frameSerial);
  // replayDraws 是 thread_local 缓存的 const 引用，其地址被 Worker_Prepare 消费。
  // 在任何正常/异常退出前排空 future，保证 worker 不会读到被下一帧重建的
  // cache。guard 刻意紧跟其后创建。
  [[maybe_unused]] auto pointShadowPrepareWaitGuard =
      MakeWar3ScopeExit([this]() noexcept { waitPointShadowCpuPrepare(); });
  waitPointShadowCpuPrepare();
  resetPointShadowCpuPlanPreservingCapacity();
  const size_t replayCasterCount = replayDraws.size();
  // Adaptive reuse is only correct when the exact replay descriptor and all
  // backing identities match the map that was actually rendered. This hash is
  // therefore always computed; the optional continuity report merely exposes
  // the already-required contract.
  const War3ReplayContinuityHashes replayHashes =
      War3BuildReplayContinuityHashes(replayDraws);
  if (War3CsmContinuityTraceEnabled()) {
    reconciliation.replayBackingHash = replayHashes.backingHash;
    reconciliation.stage13ReplayContentHash =
        replayHashes.stage13ContentHash;
    reconciliation.stage13ReplayBackingHash =
        replayHashes.stage13BackingHash;
    reconciliation.stage13ReplayDrawCount =
        replayHashes.stage13DrawCount;
  }
  const uint64_t replayGeometryWork =
      EstimateShadowReplayGeometryWork(replayDraws);
  const uint32_t requestedShadowResolution = m_csmConfig.shadowResolution;
  // Resolve and allocate before computing cascade matrices so texel snapping
  // always uses the same resolution as the image rendered this frame.
  ensureShadowResources(m_csmConfig.cascadeCount,
                        requestedShadowResolution);
  War3CsmConfig effectiveCsmConfig = m_csmConfig;
  if (m_csmEffectiveResolution != 0u)
    effectiveCsmConfig.shadowResolution = m_csmEffectiveResolution;
  war3::War3PerfMonitor::instance().noteShadowReceiverFrame(
      static_cast<uint32_t>(std::min<size_t>(
          replayCasterCount, std::numeric_limits<uint32_t>::max())),
      replayGeometryWork, requestedShadowResolution,
      effectiveCsmConfig.shadowResolution);

  // 计算本帧级联数据（需要外部捕获世界相机矩阵后才会有效）
  War3WorldCameraState effectiveWorldCamera = input.scene.worldCamera;
  War3CsmData newCsm = m_csm.Compute(
      effectiveWorldCamera, mutableSettings.sun.direction,
      effectiveCsmConfig);
  if (War3CsmContinuityTraceEnabled()) {
    reconciliation.receiverCameraHash =
        War3ContinuityHashCamera(effectiveWorldCamera);
    reconciliation.receiverSunDirectionHash =
        War3ContinuityHashVector(bit::fnv1a_init(),
                                 mutableSettings.sun.direction);
    reconciliation.receiverCsmHash = War3ContinuityHashCsm(newCsm);
    const float cameraDelta =
        m_hasLastGoodReceiverCamera
            ? MaxMatrixAbsDelta(effectiveWorldCamera.viewProj,
                                m_lastGoodReceiverCamera.viewProj)
            : 0.0f;
    const float csmDelta =
        m_csmData.cascadeCount != 0u
            ? MaxCsmAbsDelta(newCsm, m_csmData)
            : 0.0f;
    const float sunDelta =
        m_shadowTaaHasPreviousSunDirection
            ? ShadowTaaSunDirectionDelta(
                  mutableSettings.sun.direction,
                  m_shadowTaaPreviousSunDirection)
            : 0.0f;
    const float snappedCenterDeltaTexels =
        m_csmData.cascadeCount != 0u
            ? MaxCsmSnappedCenterDeltaTexels(newCsm, m_csmData)
            : 0.0f;
    const float texelSizeDelta =
        m_csmData.cascadeCount != 0u
            ? MaxCsmTexelSizeDelta(newCsm, m_csmData)
            : 0.0f;
    reconciliation.receiverCameraDeltaNano =
        War3ContinuityDeltaNano(cameraDelta);
    reconciliation.receiverSunDeltaNano =
        War3ContinuityDeltaNano(sunDelta);
    reconciliation.receiverCsmDeltaNano =
        War3ContinuityDeltaNano(csmDelta);
    reconciliation.receiverSnappedCenterDeltaTexelsNano =
        War3ContinuityDeltaNano(snappedCenterDeltaTexels);
    reconciliation.receiverTexelSizeDeltaNano =
        War3ContinuityDeltaNano(texelSizeDelta);
  }
  const bool hasCandidateCsm = newCsm.cascadeCount != 0;
  if (!hasCandidateCsm)
    reconciliation.receiverNoCandidateCsmCount = 1u;
  if (hasCandidateCsm) {
    m_lastGoodReceiverCamera = effectiveWorldCamera;
    m_hasLastGoodReceiverCamera = true;
  }
  const bool lastGoodReceiverCameraFresh =
      m_hasLastGoodReceiverCamera && War3WorldCameraIsFreshForFrame(
          m_lastGoodReceiverCamera, input.frameSerial);
  const bool csmFallbackToLastGood =
      !hasCandidateCsm && m_csmData.cascadeCount != 0 &&
      lastGoodReceiverCameraFresh;
  if (csmFallbackToLastGood)
    reconciliation.receiverCsmFallbackToLastGoodCount = 1u;
  if (csmFallbackToLastGood)
    effectiveWorldCamera = m_lastGoodReceiverCamera;

  if (!hasCandidateCsm) {
    // 容错：偶发相机矩阵被 overlay/正交污染时，CSM 计算可能失败。
    // 若上一帧已有有效 CSM，则复用以避免“阴影整帧消失→下一帧恢复”的闪烁。
    if (csmFallbackToLastGood) {
      static bool s_loggedFallback = false;
      if (!s_loggedFallback) {
        s_loggedFallback = true;
        WAR3_RENDER_LOG("DXVK War3ShadowReceiverPass: CSM compute failed, "
                        "fallback to last-good CSM/camera\n");
      }
    } else {
      // Do not leave an arbitrarily old CSM publication visible to the later
      // volumetric pass after the receiver has rejected its camera source.
      m_hasCompleteShadowMap = false;
      static bool s_warned = false;
      if (!s_warned) {
        s_warned = true;
        const auto &p = input.scene.worldCamera.proj;
        WAR3_RENDER_LOG(
            "DXVK War3ShadowReceiverPass: CSM compute failed, skip (proj "
            "m22=%.6f m23=%.6f m32=%.6f m33=%.6f)\n",
            static_cast<double>(p[2][2]), static_cast<double>(p[2][3]),
            static_cast<double>(p[3][2]), static_cast<double>(p[3][3]));
      }
      if (hasListeners) {
        war3shader::internal::DispatchRenderEvent(
            war3shader::RenderEventID::SHADOW_PASS_END);
      }
      reconciliation.receiverRunEarlyReturnReason =
          static_cast<uint32_t>(ReceiverRunEarlyReturnReason::CsmComputeFailed);
      publishReconciliationStats();
      return;
    }
  }

  if (effectiveWorldCamera.valid) {
    const Matrix4 invView = inverse(effectiveWorldCamera.view);
    m_pointLightCameraPos =
        Vector4(invView[3].x, invView[3].y, invView[3].z, 1.0f);
  }
  const Vector4 pointLightCameraPosForFrame = m_pointLightCameraPos;
  War3PointLightFrameSnapshot pointLightSnapshot = {};
  m_pointLightFrameUniform = {};
  m_pointRayEligibleLightCount = 0u;
  if (m_pointLightsEnabled && War3LightManager::Instance().HasActiveLights()) {
    pointLightSnapshot = War3LightManager::Instance().GetFrameSnapshot(
        input.frameSerial, pointLightCameraPosForFrame);
  }
  // Freeze direct-light ordering and point-shadow ordering to the exact same
  // manager generation for the full pass. Manager writes become visible on the
  // following frame instead of moving a light between cube recording and draw.
  m_hasPointLights = m_pointLightsEnabled && pointLightSnapshot.hasAny;
  if (m_hasPointLights) {
    m_pointLightFrameUniform.count =
        std::min<uint32_t>(pointLightSnapshot.count, 16u);
    m_pointRayEligibleLightCount = std::min<uint32_t>(
        pointLightSnapshot.shadowCount, m_pointLightFrameUniform.count);
    for (uint32_t i = 0u; i < m_pointLightFrameUniform.count; ++i) {
      const War3PointLight &source = pointLightSnapshot.lights[i];
      m_pointLightFrameUniform.lights[i].pos = source.position;
      m_pointLightFrameUniform.lights[i].color = source.color;

      // source.position.w is the authored range, not a homogeneous component.
      // Construct w=1 explicitly before applying the exact view matrix that is
      // uploaded to the receiver below. params.yzw were previously unused, so
      // this removes one uniform mat4 transform per pixel/light without growing
      // LightUniform or changing the canonical light/shadow ordering.
      const Vector4 viewPosition = effectiveWorldCamera.view * Vector4(
          source.position.x, source.position.y, source.position.z, 1.0f);
      m_pointLightFrameUniform.lights[i].params =
          Vector4(source.params.x, viewPosition.x, viewPosition.y,
                  viewPosition.z);
    }
  }
  hasPointShadow =
      m_pointLightsEnabled && mutableSettings.shadows.pointShadowEnabled &&
      mutableSettings.shadows.pointShadowMaxLights > 0u && m_hasPointLights;
  m_pointShadowEnabled = hasPointShadow;
  if (!m_pointShadowEnabled) {
    invalidatePointShadowPublishedState();
  }

  const auto debugModeEnum = mutableSettings.shadows.debugMode;
  const float activeShadowStrength =
      shadowsEnabled ? mutableSettings.shadows.strength : 0.0f;
  const bool debugNeedsDirectionalShadowMap =
      debugModeEnum == War3ShadowDebugMode::ShadowFactor ||
      debugModeEnum == War3ShadowDebugMode::ShadowHistory ||
      debugModeEnum == War3ShadowDebugMode::ShadowCurrent ||
      debugModeEnum == War3ShadowDebugMode::ShadowCurrentOverlay ||
      debugModeEnum == War3ShadowDebugMode::ShadowCsmDiagnosis;
  const bool receiverNeedsShadowMap =
      shadowsEnabled && (debugNeedsDirectionalShadowMap ||
                         (debugModeEnum == War3ShadowDebugMode::None &&
                          activeShadowStrength > 1e-3f));
  const bool needOutlineDepth = settings->occludedOutline.enabled &&
                                settings->occludedOutline.useScreenSpace &&
                                War3RenderState::HasOutlineHandles();
  const bool needReceiverPass =
      (shadowsEnabled && ((debugModeEnum != War3ShadowDebugMode::None) ||
                          (activeShadowStrength > 1e-3f))) ||
      m_hasPointLights;
  reconciliation.receiverNeedShadowMap = receiverNeedsShadowMap ? 1u : 0u;
  reconciliation.receiverNeedPass = needReceiverPass ? 1u : 0u;
  reconciliation.receiverActiveStrengthMilli =
      strengthToMilli(activeShadowStrength);
  reconciliation.receiverDebugMode =
      uint32_t(static_cast<int>(debugModeEnum));
  setReceiverRunEntryFlags(true, shadowsEnabled, outlineEnabled, hasSunShadow,
                           hasPointShadow, m_hasPointLights, needOutlinePass,
                           receiverNeedsShadowMap, needReceiverPass,
                           replayCasterCount != 0u, false, debugShadow);

  // Phase 4：shadow map 优先由 instances + fallbacks 双通道重放生成。
  // shadowCasters 仍保留为兼容容器，但不再作为 directional shadow 的主输入。
  // 即便本帧 caster 为空，也会清屏 shadow map（全亮）以避免复用旧 shadow
  // map 导致拖影。

  // Phase 7.2: 对账计数器
  reconciliation.shadowCastersCount =
      static_cast<uint32_t>(input.scene.shadowCasters.size());
  reconciliation.replayDrawsCount = static_cast<uint32_t>(replayCasterCount);

  const auto& st = input.scene.shadowStats;
  const bool semanticDynamicCastersActive =
      st.semanticSceneSubmittedSkinned != 0u ||
      st.dynamicSkinnedOutputCount != 0u ||
      st.capturedUnitVertexBlend != 0u;
  const bool semanticReceiverStabilityActive =
      SemanticReceiverStabilityModeEnabled() && semanticDynamicCastersActive &&
      debugModeEnum == War3ShadowDebugMode::None;
  const uint64_t semanticIdentityHash =
      st.semanticSceneDirectLastSubmittedIdentityHash;
  const bool semanticIdentityKnown =
      semanticDynamicCastersActive && semanticIdentityHash != 0u;
  if (semanticIdentityKnown) {
    if (semanticIdentityHash == m_pendingShadowMapSemanticIdentityHash) {
      m_pendingShadowMapSemanticIdentityStableFrames =
          std::min<uint32_t>(m_pendingShadowMapSemanticIdentityStableFrames + 1u,
                             0x7fffffffu);
    } else {
      m_pendingShadowMapSemanticIdentityHash = semanticIdentityHash;
      m_pendingShadowMapSemanticIdentityStableFrames = 1u;
    }
  } else {
    m_pendingShadowMapSemanticIdentityHash = 0u;
    m_pendingShadowMapSemanticIdentityStableFrames = 0u;
  }
  const bool semanticIdentityChanged =
      semanticIdentityKnown && m_lastShadowMapSemanticIdentityHash != 0u &&
      semanticIdentityHash != m_lastShadowMapSemanticIdentityHash;
  const uint32_t requiredStableIdentityFrames =
      SemanticReceiverStableIdentityFramesBeforeRedraw();
  const bool semanticIdentityStableForRedraw =
      !semanticIdentityChanged ||
      !SemanticReceiverHoldUntilStableIdentityEnabled() ||
      m_pendingShadowMapSemanticIdentityStableFrames >=
          requiredStableIdentityFrames;
  if (semanticDynamicCastersActive && replayCasterCount != 0u) {
    m_recentSemanticDynamicHoldFramesRemaining =
        dxvk::war3::internal::kShadowSemanticDynamicEmptyReplayHoldFrames;
  } else if (m_recentSemanticDynamicHoldFramesRemaining != 0u) {
    m_recentSemanticDynamicHoldFramesRemaining--;
  }
  const bool canHoldCompleteShadowMap =
      receiverNeedsShadowMap && m_hasCompleteShadowMap && m_shadowMap &&
      m_shadowMapSampleView;
  const bool holdForInvalidCsm =
      dxvk::war3::internal::kShadowHoldLastGoodMapOnInvalidCsm &&
      canHoldCompleteShadowMap && csmFallbackToLastGood;
  const bool holdForTransientEmptyReplay =
      dxvk::war3::internal::kShadowHoldLastGoodMapOnTransientEmptyReplay &&
      canHoldCompleteShadowMap && replayCasterCount == 0u &&
      m_lastShadowMapCasterCount != 0u &&
      m_transientEmptyReplayHoldFramesRemaining != 0u;
  const bool holdForSemanticDynamicEmptyReplay =
      dxvk::war3::internal::kShadowHoldLastGoodMapOnTransientEmptyReplay &&
      canHoldCompleteShadowMap && replayCasterCount == 0u &&
      m_lastShadowMapCasterCount != 0u &&
      m_recentSemanticDynamicHoldFramesRemaining != 0u;
  const bool holdForSemanticIdentityChurn =
      dxvk::war3::internal::kShadowHoldLastGoodMapOnSemanticIdentityChurn &&
      canHoldCompleteShadowMap && replayCasterCount != 0u &&
      semanticIdentityChanged &&
      (!SemanticReceiverHoldUntilStableIdentityEnabled()
           ? m_semanticIdentityChurnHoldFramesRemaining != 0u
           : !semanticIdentityStableForRedraw);
  const bool semanticSelectionUnderCapPressure =
      st.semanticSceneDirectScanCapHitCount != 0u ||
      st.semanticSceneDirectRecordCapHitCount != 0u;
  const uint32_t coverageDropMaxHoldFrames =
      SemanticReceiverCoverageDropMaxHoldFrames();
  const bool semanticCoverageDropCandidate =
      SemanticReceiverHoldOnCoverageDropEnabled() &&
      dxvk::war3::internal::kShadowHoldLastGoodMapOnSemanticIdentityChurn &&
      canHoldCompleteShadowMap && replayCasterCount != 0u &&
      m_lastShadowMapCasterCount != 0u && semanticDynamicCastersActive &&
      semanticSelectionUnderCapPressure &&
      uint32_t(replayCasterCount) + SemanticReceiverCoverageDropTolerance() <
          m_lastShadowMapCasterCount;
  const bool holdForSemanticCoverageDrop =
      semanticCoverageDropCandidate &&
      (coverageDropMaxHoldFrames == 0u ||
       m_semanticCoverageDropHoldStreak < coverageDropMaxHoldFrames);
  if (holdForInvalidCsm)
    reconciliation.receiverHoldInvalidCsmCount = 1u;
  if (holdForTransientEmptyReplay || holdForSemanticDynamicEmptyReplay)
    reconciliation.receiverHoldEmptyReplayCount = 1u;
  if (holdForSemanticIdentityChurn || holdForSemanticCoverageDrop)
    reconciliation.receiverHoldIdentityChurnCount = 1u;

  // 诊断：若 caster
  // 数量在相邻帧间频繁变化，通常意味着“阶段/批次识别不稳定”或“插入点时机漂移”，
  // 这会表现为阴影时有时无/抽搐。默认仅在变化时打点少量日志，避免刷屏。
  /*         {
              static uint32_t s_lastCasterCount = 0;
              static uint32_t s_lastPaletteCount = 0;
              static uint32_t s_loggedChanges = 0;

              const uint32_t casterCount =
     static_cast<uint32_t>(input.scene.shadowCasters.size()); const uint32_t
     paletteCount = static_cast<uint32_t>(input.scene.shadowPalettes.size());

              if ((casterCount != s_lastCasterCount || paletteCount !=
     s_lastPaletteCount) && s_loggedChanges < 24) { s_loggedChanges++; const
     auto& vp = input.scene.worldCamera.viewport; WAR3_RENDER_LOG( "DXVK
     War3ShadowTelemetry: casters %u->%u palettes %u->%u cam=%d
     vp=%ux%u@(%u,%u)\n", static_cast<unsigned>(s_lastCasterCount),
                      static_cast<unsigned>(casterCount),
                      static_cast<unsigned>(s_lastPaletteCount),
                      static_cast<unsigned>(paletteCount),
                      input.scene.worldCamera.valid ? 1 : 0,
                      static_cast<unsigned>(vp.Width),
                      static_cast<unsigned>(vp.Height),
                      static_cast<unsigned>(vp.X),
                      static_cast<unsigned>(vp.Y));
              }

              s_lastCasterCount = casterCount;
              s_lastPaletteCount = paletteCount;
          } */

  bool reuseLastShadowMap = false;
  // Publication settlement must describe work that was actually resolved for
  // this frame. A non-throwing renderShadowMap() failure intentionally leaves
  // the last complete map available for recovery, but it must not relabel that
  // old map as a current-frame result unless reuse was explicitly selected.
  bool directionalMapResolvedForFrame = false;
  if (receiverNeedsShadowMap && ShadowAdaptiveMapUpdateRuntimeEnabled() &&
      m_hasCompleteShadowMap && hasCandidateCsm &&
      m_csmData.cascadeCount != 0 &&
      replayCasterCount >=
          dxvk::war3::internal::kShadowAdaptiveMapUpdateMinCasters) {
    const uint32_t period = ComputeAdaptiveShadowMapPeriod(replayCasterCount);
    const bool cadenceAllowsReuse =
        period > 1 && ((m_shadowAdaptiveFrameIndex + 1u) % period) != 0u;
    const bool dynamicPoseStable =
        input.scene.shadowStats.dynamicPoseSignature ==
        m_lastDynamicPoseSignature;
    const auto& st = input.scene.shadowStats;
    const bool hasDynamicSkinnedCasters =
        st.dynamicSkinnedOutputCount != 0u ||
        st.semanticSceneSubmittedSkinned != 0u ||
        st.capturedUnitVertexBlend != 0u;
    const bool dynamicContentStable =
        !hasDynamicSkinnedCasters ||
        (st.dynamicPoseSignature != 0u && dynamicPoseStable);
    const bool noSemanticOrCaptureInstability =
        st.semanticBridgeMiss == 0 &&
        st.skippedVertexShader == 0 &&
        st.skippedVertexBlend == 0 &&
        st.skippedFreezeBudget == 0 &&
        st.skippedPriorityBudget == 0 &&
        st.skippedMissingPerDrawUpload == 0;
    const bool exactReplayContractStable =
        replayCasterCount == m_lastShadowMapCasterCount &&
        replayHashes.contentHash != 0u &&
        replayHashes.contentHash == m_lastShadowMapReplayContentHash &&
        replayHashes.backingHash != 0u &&
        replayHashes.backingHash == m_lastShadowMapReplayBackingHash;
    const bool stagePolicyStable =
        shadowStagePolicyRevision ==
        m_lastShadowMapStagePolicyRevision;
    const uint64_t candidateCsmHash = War3ContinuityHashCsm(newCsm);
    const bool exactCsmContractStable =
        candidateCsmHash != 0u &&
        candidateCsmHash == m_lastShadowMapCsmHash;
    const bool shadowMapGenerationStable =
        m_shadowMapResourceGeneration ==
        m_lastShadowMapResourceGeneration;

    reuseLastShadowMap =
        cadenceAllowsReuse &&
        exactReplayContractStable &&
        stagePolicyStable &&
        exactCsmContractStable &&
        shadowMapGenerationStable &&
        noSemanticOrCaptureInstability &&
        dynamicContentStable;
  }

  if (holdForInvalidCsm || holdForTransientEmptyReplay ||
      holdForSemanticDynamicEmptyReplay || holdForSemanticIdentityChurn ||
      holdForSemanticCoverageDrop) {
    reuseLastShadowMap = true;
    if (holdForTransientEmptyReplay &&
        m_transientEmptyReplayHoldFramesRemaining != 0u) {
      m_transientEmptyReplayHoldFramesRemaining--;
    }
    if (holdForSemanticIdentityChurn &&
        !SemanticReceiverHoldUntilStableIdentityEnabled() &&
        m_semanticIdentityChurnHoldFramesRemaining != 0u) {
      m_semanticIdentityChurnHoldFramesRemaining--;
    }
    if (holdForSemanticCoverageDrop)
      m_semanticCoverageDropHoldStreak =
          std::min<uint32_t>(m_semanticCoverageDropHoldStreak + 1u,
                             0x7fffffffu);

    static uint32_t s_loggedHold = 0;
    if (s_loggedHold < 16u || (s_loggedHold % 240u) == 0u) {
      s_loggedHold++;
      WAR3_RENDER_LOG(
          "DXVK War3ShadowReceiverPass: hold last-good shadow map reason=%s "
          "replay=%u lastReplay=%u csmCandidate=%d semanticDynamic=%d "
          "holdFrames=%u semanticHoldFrames=%u identityHoldFrames=%u "
          "identityHash=%llu lastIdentityHash=%llu\n",
          holdForInvalidCsm
              ? "invalid-csm"
              : (holdForSemanticCoverageDrop
                     ? "semantic-coverage-drop"
                     : (holdForSemanticIdentityChurn
                            ? "semantic-identity-churn"
                            : (holdForSemanticDynamicEmptyReplay
                                   ? "semantic-empty-replay"
                                   : "empty-replay"))),
          static_cast<unsigned>(replayCasterCount),
          static_cast<unsigned>(m_lastShadowMapCasterCount),
          hasCandidateCsm ? 1 : 0, semanticDynamicCastersActive ? 1 : 0,
          static_cast<unsigned>(m_transientEmptyReplayHoldFramesRemaining),
          static_cast<unsigned>(
              m_recentSemanticDynamicHoldFramesRemaining),
          static_cast<unsigned>(
              m_semanticIdentityChurnHoldFramesRemaining),
          static_cast<unsigned long long>(semanticIdentityHash),
          static_cast<unsigned long long>(
              m_lastShadowMapSemanticIdentityHash));
    }
  } else if (!semanticCoverageDropCandidate) {
    m_semanticCoverageDropHoldStreak = 0u;
  }

  shadowMainPhaseTiming.enter(
      static_cast<size_t>(War3ShadowMainRawPhase::ShadowMapAndVolume));
  // m_csmData names the matrices paired with the current complete depth map.
  // A fallible candidate render may temporarily replace it below, so retain
  // the full value bundle for the bounded same-epoch last-good path.
  const War3CsmData csmDataBeforeShadowCandidate = m_csmData;
  if (!reuseLastShadowMap && hasCandidateCsm) {
    m_csmData = newCsm;
  }

  const bool allowPointShadowPrepare =
      (!semanticReceiverStabilityActive ||
       !SemanticReceiverDisablePointLightsEnabled()) &&
      m_pointLightsEnabled && mutableSettings.shadows.pointShadowEnabled &&
      mutableSettings.shadows.pointShadowMaxLights > 0u && m_hasPointLights;
  const bool allowPointShadowPersistentPrepare =
      allowPointShadowPrepare && pointLightSnapshot.shadowCount > 0u;
  // Persistent admission is a point-shadow concern, not a directional CSM
  // concern. Start it before the CSM-only branch so point-only frames get the
  // same exact same-frame proposal while CSM frames retain useful overlap.
  // The release-default Off route deliberately stays at its historical spot
  // inside receiverNeedsShadowMap below, preserving std::async behaviour.
  if (allowPointShadowPersistentPrepare &&
      pointShadowPersistentConfiguredMode !=
          War3PointShadowPersistentMode::Off) {
    beginPointShadowCpuPrepare(input, pointLightSnapshot, &replayDraws);
  }

  if (receiverNeedsShadowMap) {
    // When the frame chooses to reuse the last complete map, keep the old
    // image dimensions alive. Recreating here clears m_hasCompleteShadowMap and
    // turns a stability hold into a forced redraw, which presents as rhythmic
    // whole-scene shadow pulsing under semantic identity churn.
    const bool shadowMapResourcesUsable =
        m_hasCompleteShadowMap && m_shadowMap && m_shadowMapSampleView;
    if (!reuseLastShadowMap || !shadowMapResourcesUsable) {
      ensureShadowResources(m_csmData.cascadeCount, m_csmConfig.shadowResolution);
    }
    if (reuseLastShadowMap &&
        (!m_hasCompleteShadowMap || !m_shadowMap || !m_shadowMapSampleView)) {
      reconciliation.receiverReuseInvalidatedAfterEnsureCount = 1u;
      static uint32_t s_loggedInvalidReuse = 0;
      if (s_loggedInvalidReuse++ < 16u ||
          (s_loggedInvalidReuse % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3ShadowReceiverPass: cancel shadow map reuse after "
            "resource ensure invalidated completeness (replay=%u "
            "layers=%u res=%u)\n",
            static_cast<unsigned>(replayCasterCount),
            static_cast<unsigned>(m_csmData.cascadeCount),
            static_cast<unsigned>(m_csmConfig.shadowResolution));
      }
      reuseLastShadowMap = false;
    }
    // A2 legacy route: keep release-default Off and its std::async launch
    // exactly inside the original CSM recording/reuse window.
    if (allowPointShadowPrepare &&
        pointShadowPersistentConfiguredMode ==
            War3PointShadowPersistentMode::Off) {
      beginPointShadowCpuPrepare(input, pointLightSnapshot,
                                 &replayDraws);
    }

    if (reuseLastShadowMap) {
      war3::War3PerfMonitor::instance().noteShadowMapFallback(true, false);
      reconciliation.receiverReuseShadowMap = 1u;
      directionalMapResolvedForFrame = true;
    } else {
      auto perfScope = war3::War3PerfMonitor::instance().scope("ShadowMap", ctx);
      const bool renderedShadowMap = renderShadowMap(ctx, input, &replayDraws);
      if (renderedShadowMap) {
        directionalMapResolvedForFrame = true;
        m_semanticCoverageDropHoldStreak = 0u;
        m_hasCompleteShadowMap = true;
        if (m_epochFirstCompleteLatencyFrames == 0u &&
            input.frameSerial >= m_epochFirstCandidateFrameSerial) {
          m_epochFirstCompleteLatencyFrames =
              input.frameSerial - m_epochFirstCandidateFrameSerial + 1u;
          g_shadowReplayDiagnostics.firstCompleteLatencyFrames.store(
              m_epochFirstCompleteLatencyFrames,
              std::memory_order_release);
        }
        m_replayValidationHoldFramesRemaining = 8u;
        m_lastShadowMapCasterCount = static_cast<uint32_t>(replayCasterCount);
        m_lastDynamicPoseSignature =
            input.scene.shadowStats.dynamicPoseSignature;
        m_lastShadowMapReplayContentHash = replayHashes.contentHash;
        m_lastShadowMapReplayBackingHash = replayHashes.backingHash;
        m_lastShadowMapStagePolicyRevision =
            shadowStagePolicyRevision;
        m_lastShadowMapCsmHash = War3ContinuityHashCsm(m_csmData);
        m_lastShadowMapResourceGeneration =
            m_shadowMapResourceGeneration;
        m_lastShadowMapSunDir = m_csmData.lightDirection;
        m_lastShadowMapStrength = activeShadowStrength;
        m_hasLastShadowMapLighting = true;
        if (semanticIdentityHash != 0u) {
          m_lastShadowMapSemanticIdentityHash = semanticIdentityHash;
          m_semanticIdentityChurnHoldFramesRemaining =
              dxvk::war3::internal::kShadowSemanticIdentityChurnHoldFrames;
        } else if (!semanticDynamicCastersActive) {
          m_lastShadowMapSemanticIdentityHash = 0u;
          m_pendingShadowMapSemanticIdentityHash = 0u;
          m_pendingShadowMapSemanticIdentityStableFrames = 0u;
          m_semanticIdentityChurnHoldFramesRemaining = 0u;
        }
        m_transientEmptyReplayHoldFramesRemaining =
            replayCasterCount != 0u
                ? dxvk::war3::internal::kShadowTransientEmptyReplayHoldFrames
                : 0u;
      } else if ((m_replayValidationFailedThisFrame ||
                  m_workloadGovernorRejectedThisFrame) &&
                 m_hasCompleteShadowMap &&
                 m_replayValidationHoldFramesRemaining != 0u) {
        // The candidate was rejected before clear/draw. Preserve only the
        // complete map produced by this same receiver epoch, with a strict
        // bounded hold; a new epoch starts with m_hasCompleteShadowMap=false.
        --m_replayValidationHoldFramesRemaining;
        m_csmData = csmDataBeforeShadowCandidate;
        reconciliation.receiverReuseShadowMap = 1u;
        directionalMapResolvedForFrame = true;
      } else {
        // The same-epoch recovery window is a real upper bound. The old code
        // stopped decrementing at zero but left completeness set forever,
        // allowing a loading-camera CSM to cover the screen indefinitely.
        if ((m_replayValidationFailedThisFrame ||
             m_workloadGovernorRejectedThisFrame) &&
            m_hasCompleteShadowMap &&
            m_replayValidationHoldFramesRemaining == 0u) {
          m_hasCompleteShadowMap = false;
        }
        if (!m_hasCompleteShadowMap) {
          m_lastShadowMapCasterCount = 0u;
          m_lastDynamicPoseSignature = 0u;
          m_lastShadowMapReplayContentHash = 0u;
          m_lastShadowMapReplayBackingHash = 0u;
          m_lastShadowMapStagePolicyRevision = 0u;
          m_lastShadowMapCsmHash = 0u;
          m_lastShadowMapResourceGeneration = 0u;
          m_lastShadowMapSemanticIdentityHash = 0u;
          m_pendingShadowMapSemanticIdentityHash = 0u;
          m_pendingShadowMapSemanticIdentityStableFrames = 0u;
          m_semanticIdentityChurnHoldFramesRemaining = 0u;
          m_transientEmptyReplayHoldFramesRemaining = 0u;
          m_hasLastShadowMapLighting = false;
        }
      }
    }
  }
  m_shadowAdaptiveFrameIndex++;
  reconciliation.receiverHasCompleteShadowMap =
      (m_hasCompleteShadowMap && m_shadowMap && m_shadowMapSampleView) ? 1u
                                                                       : 0u;
  reconciliation.receiverCsmCascadeCount = m_csmData.cascadeCount;
  recordShadowResourceFingerprint(m_shadowHistoryIndex & 1u,
                                  (m_shadowHistoryIndex & 1u) ^ 1u);

  // 长期线：体积光开启时额外渲染「体积太阳 ortho」阴影。
  // 与相机 CSM 资源分离；体积关或设置关闭时立即失效、不分配。
  // 不依赖表面 CSM 本帧是否绘制：只要有合法 sun/csm 方向与 replay casters。
  {
    const auto& volSettings = mutableSettings.postFx.volumetricLight;
    if (!reuseLastShadowMap && hasCandidateCsm && m_csmData.cascadeCount == 0u)
      m_csmData = newCsm;
    const bool wantVolumeSun =
        mutableSettings.postFx.enabled && volSettings.enabled &&
        volSettings.volumeSunShadowEnabled && hasCandidateCsm &&
        m_csmData.cascadeCount > 0u && !replayDraws.empty();
    if (wantVolumeSun) {
      war3::tools::SetGpuFlightBreadcrumb(
          war3::tools::GpuFlightBreadcrumb::VolumeSunShadow);
      auto volScope =
          war3::War3PerfMonitor::instance().scope("VolumeSunShadow", ctx);
      if (!renderVolumeSunShadow(ctx, input, &replayDraws))
        invalidateVolumeSunShadowPublication();
    } else {
      invalidateVolumeSunShadowPublication();
    }
  }
  shadowMainPhaseTiming.enter(
      static_cast<size_t>(War3ShadowMainRawPhase::PointAndCopies));
  if (receiverNeedsShadowMap) {
    if (!m_hasCompleteShadowMap)
      reconciliation.receiverNoCompleteShadowMapCount = 1u;
    if (!m_shadowMap)
      reconciliation.receiverNoShadowMapImageCount = 1u;
    if (!m_shadowMapSampleView)
      reconciliation.receiverNoShadowMapSampleViewCount = 1u;
  }
  if (needReceiverPass && (!m_shadowMapSampleView || !m_shadowCasterMaskSampleView)) {
    // Point-light-only and point-shadow debug paths do not need a freshly
    // rendered directional shadow map, but the receiver shader still binds the
    // CSM slots. Create a bindable placeholder so drawReceiver is not silently
    // skipped by the descriptor guard.
    ensureShadowResources(
        std::max<uint32_t>(m_csmData.cascadeCount, 1u),
        m_csmConfig.shadowResolution);
  }

  float receiverShadowStrength = activeShadowStrength;
  Vector4 receiverSunDirSource =
      m_csmData.cascadeCount > 0u ? m_csmData.lightDirection
                                 : mutableSettings.sun.direction;
  if (semanticReceiverStabilityActive &&
      SemanticReceiverFreezeLastGoodLightingEnabled() && reuseLastShadowMap &&
      m_hasLastShadowMapLighting) {
    receiverShadowStrength = m_lastShadowMapStrength;
    receiverSunDirSource = m_lastShadowMapSunDir;
  }
  if (semanticReceiverStabilityActive) {
    const float strengthClamp = SemanticReceiverStableStrengthClamp();
    if (strengthClamp >= 0.0f)
      receiverShadowStrength =
          std::min(receiverShadowStrength, std::clamp(strengthClamp, 0.0f, 1.0f));
  }

  // [NEW] Point Light Cube Shadow（如果启用）
  const bool semanticReceiverPointLightsAllowed =
      !semanticReceiverStabilityActive ||
      !SemanticReceiverDisablePointLightsEnabled();

  if (semanticReceiverPointLightsAllowed && m_pointLightsEnabled &&
      mutableSettings.shadows.pointShadowEnabled &&
      mutableSettings.shadows.pointShadowMaxLights > 0 && m_hasPointLights) {
    renderPointShadow(ctx, input, pointLightSnapshot, &replayDraws);
  }

  VkExtent3D extent = input.colorView->mipLevelExtent(0u);
  VkExtent3D depthExtent = input.depthView->mipLevelExtent(0u);
  const bool needCopyColor = needReceiverPass;
  const bool needCopyDepth = needReceiverPass || needOutlineDepth;

  if (needCopyColor) {
    ensureCopyResources(extent, input.colorView->image()->info().format);
  }
  if (needCopyDepth) {
    ensureDepthCopyResources(depthExtent,
                             input.depthView->image()->info().format);
  }

  if (needCopyColor || needCopyDepth) {
    war3::tools::SetGpuFlightBreadcrumb(
        war3::tools::GpuFlightBreadcrumb::ShadowCopy);
    auto perfScope = war3::War3PerfMonitor::instance().scope("ShadowCopy", ctx);
    if (needCopyColor) {
      copyColor(ctx, input.colorView);
    }
    if (needCopyDepth) {
      copyDepth(ctx, input.depthView);
    }
  }

  shadowMainPhaseTiming.enter(
      static_cast<size_t>(War3ShadowMainRawPhase::ReceiverPrepare));
  if (replayCasterCount > 0) {
    dxvk::war3::tools::MarkInGameRenderReady("War3Shadow/Replay",
                                             input.frameSerial);
  }

  static uint32_t s_logTimer = 0;
  if (war3dbg::RenderLogEnabled() &&
      (s_logTimer++ % 300) == 0 && replayCasterCount > 0) {
    const auto &st = input.scene.shadowStats;
    WAR3_RENDER_LOG("DXVK War3Shadow: Run frame=%u casters=%u instances=%u "
                    "fallbacks=%u liveGeom=%u pool=%llu/%llu "
                    "captured=%u/%u/%u/%u unit=%u unitVB=%u "
                    "semScene=%u/%u/%u "
                    "forceFreeze=%u/%u dynPose=%u dynSkin=%u "
                    "rej(noId/mode/dyn/alpha/miss/create)=%u/%u/%u/%u/%u/%u "
                    "skip(cap/vs/vb)=%u/%u/%u cascades=%u res=%u\n",
                    s_logTimer,
                    static_cast<unsigned>(replayCasterCount),
                    static_cast<unsigned>(input.scene.shadowInstances.size()),
                    static_cast<unsigned>(input.scene.shadowFallbacks.size()),
                    static_cast<unsigned>(
                        input.scene.shadowPersistentPool.liveGeometryCount),
                    static_cast<unsigned long long>(
                        input.scene.shadowPersistentPool.bytesUsed),
                    static_cast<unsigned long long>(
                        input.scene.shadowPersistentPool.bytesCap),
                    static_cast<unsigned>(st.capturedIndexed),
                    static_cast<unsigned>(st.capturedNonIndexed),
                    static_cast<unsigned>(st.capturedTerrain),
                    static_cast<unsigned>(st.capturedWorldObject),
                    static_cast<unsigned>(st.capturedUnitObject),
                    static_cast<unsigned>(st.capturedUnitVertexBlend),
                    static_cast<unsigned>(st.semanticSceneSubmitted),
                    static_cast<unsigned>(st.semanticSceneSubmittedUnit),
                    static_cast<unsigned>(st.semanticSceneSubmittedSkinned),
                    static_cast<unsigned>(st.forcedFallbackWorldFreezeCount),
                    static_cast<unsigned>(st.forcedFallbackUnitFreezeCount),
                    static_cast<unsigned>(st.dynamicPoseCount),
                    static_cast<unsigned>(st.dynamicSkinnedOutputCount),
                    static_cast<unsigned>(st.persistentRejectNoIdentity),
                    static_cast<unsigned>(st.persistentRejectUnsupportedMode),
                    static_cast<unsigned>(st.persistentRejectDynamicSource),
                    static_cast<unsigned>(st.persistentRejectAlphaBlend),
                    static_cast<unsigned>(st.persistentRejectMissingStorage),
                    static_cast<unsigned>(st.persistentRejectCreateOrBudget),
                    static_cast<unsigned>(st.skippedCasterCap),
                    static_cast<unsigned>(st.skippedVertexShader),
                    static_cast<unsigned>(st.skippedVertexBlend),
                    static_cast<unsigned>(m_csmData.cascadeCount),
                    static_cast<unsigned>(m_shadowMapResolution));

    // Log Light Matrices
    for (uint32_t i = 0; i < std::min(m_csmData.cascadeCount, 4u); i++) {
      auto &m = m_csmData.cascades[i].lightViewProj;
      WAR3_RENDER_LOG("  Cascade[%u] Proj[3]: %.2f %.2f %.2f %.2f Split=%.2f\n",
                      i, m[3].x, m[3].y, m[3].z, m[3].w,
                      m_csmData.cascades[i].splitFar);
    }

    const War3ShadowSettings &ss = settings->shadows;
    WAR3_RENDER_LOG(
        "  Budget: fallback=%llu/%llu arena=%llu skipFreeze=%u skipPriority=%u "
        "skipCap=%u dropAlpha=%u\n",
        static_cast<unsigned long long>(st.fallbackBudgetUsedBytes),
        static_cast<unsigned long long>(st.fallbackBudgetBytes),
        static_cast<unsigned long long>(st.fallbackArenaBytes),
        static_cast<unsigned>(st.skippedFreezeBudget),
        static_cast<unsigned>(st.skippedPriorityBudget),
        static_cast<unsigned>(st.skippedCasterCap),
        static_cast<unsigned>(st.degradedAlphaBudget));
    WAR3_RENDER_LOG("  Settings: Enabled=%d Strength=%.2f Bias=%.4f PCF=%.4f\n",
                    ss.enabled, activeShadowStrength, ss.receiverBias,
                    ss.pcfRadius);
  }

  if (needReceiverPass || needOutlineDepth) {
    War3ShadowTaaMode shadowTaaEffectiveMode =
        static_cast<War3ShadowTaaMode>(
            reconciliation.shadowTaaEffectiveMode);
    if (!receiverNeedsShadowMap)
      shadowTaaEffectiveMode = War3ShadowTaaMode::DirectInline;

    const bool temporalRequested =
        shadowTaaEffectiveMode == War3ShadowTaaMode::Temporal;
    const uint64_t currentDynamicPoseSignature =
        input.scene.shadowStats.dynamicPoseSignature;
    const bool shadowTaaDynamicPoseChanged =
        semanticDynamicCastersActive &&
        (!m_shadowTaaHistoryContractValid ||
         currentDynamicPoseSignature == 0u ||
         currentDynamicPoseSignature !=
             m_shadowTaaHistoryDynamicPoseSignature);
    const bool shadowTaaBlockedForSemanticDynamic =
        temporalRequested &&
        ShadowTaaDisableForSemanticDynamicEnabled() &&
        shadowTaaDynamicPoseChanged;
    const bool sunDirectionMoved =
        m_shadowTaaHasPreviousSunDirection &&
        ShadowTaaSunDirectionDelta(
            receiverSunDirSource, m_shadowTaaPreviousSunDirection) > 1.0e-5f;
    const bool shadowTaaBlockedForSunMotion =
        temporalRequested && ShadowTaaDisableOnSunMotionEnabled() &&
        sunDirectionMoved;
    const bool shadowTaaBlockedForCsmFallback =
        temporalRequested && csmFallbackToLastGood;
    if (shadowTaaBlockedForSemanticDynamic ||
        shadowTaaBlockedForSunMotion ||
        shadowTaaBlockedForCsmFallback) {
      if (shadowTaaBlockedForSemanticDynamic) {
        reconciliation.shadowHistoryInvalidationMask |=
            kShadowTaaInvalidateDynamicPose;
      }
      if (shadowTaaBlockedForSunMotion) {
        reconciliation.shadowHistoryInvalidationMask |=
            kShadowTaaInvalidateSun;
      }
      if (shadowTaaBlockedForCsmFallback) {
        reconciliation.shadowHistoryInvalidationMask |=
            kShadowTaaInvalidateCsmFallback;
      }
      // Preserve the Temporal execution mode but invalidate its history
      // contract. The current frame then becomes TemporalCurrentOnly and, if
      // all four passes complete, publishes a fresh history for the next
      // frame. Switching to Prepass here would suppress HistoryWrite and cost
      // an additional recovery frame.
      m_shadowHistoryValid = false;
      m_shadowTaaWasActiveLastFrame = false;
    }

    reconciliation.shadowTaaEffectiveMode =
        static_cast<uint32_t>(shadowTaaEffectiveMode);
    reconciliation.shadowTaaBlockedSemanticDynamic =
        shadowTaaBlockedForSemanticDynamic ? 1u : 0u;
    reconciliation.shadowTaaBlockedSunMotion =
        shadowTaaBlockedForSunMotion ? 1u : 0u;
    reconciliation.shadowTaaBlockedCsmFallback =
        shadowTaaBlockedForCsmFallback ? 1u : 0u;

    const bool shadowTaaActive =
        shadowTaaEffectiveMode != War3ShadowTaaMode::DirectInline &&
        receiverNeedsShadowMap;
    const bool shadowTaaTemporalActive =
        shadowTaaEffectiveMode == War3ShadowTaaMode::Temporal &&
        receiverNeedsShadowMap;
    reconciliation.shadowTaaActive = shadowTaaActive ? 1u : 0u;
    if (!shadowTaaTemporalActive) {
      m_shadowHistoryValid = false;
      m_shadowTaaWasActiveLastFrame = false;
      m_shadowTaaHistoryContractValid = false;
    }
    const bool debugMotionVector =
        (debugModeEnum == War3ShadowDebugMode::MotionVector);
    const bool debugShadowHistory =
        (debugModeEnum == War3ShadowDebugMode::ShadowHistory);
    const bool debugShadowCurrent =
        (debugModeEnum == War3ShadowDebugMode::ShadowCurrent);
    const bool debugShadowCurrentOverlay =
        (debugModeEnum == War3ShadowDebugMode::ShadowCurrentOverlay);
    const bool debugShadowCsmDiagnosis =
        (debugModeEnum == War3ShadowDebugMode::ShadowCsmDiagnosis);

    const bool allowShadowTaaAuxiliaryPasses =
        reconciliation.shadowTaaRuntimeModuleEnabled != 0u;
    const bool needMotionVectors =
        allowShadowTaaAuxiliaryPasses &&
        (shadowTaaTemporalActive || debugMotionVector);
    const bool needShadowVisibility =
        allowShadowTaaAuxiliaryPasses &&
        (shadowTaaActive || debugShadowCurrent ||
         debugShadowCurrentOverlay || debugShadowCsmDiagnosis);

    // 先确保资源存在：避免 Resize/重建导致 ubo 中的 hasHistory/prev 状态不同步
    if (needMotionVectors) {
      war3::tools::SetGpuFlightBreadcrumb(
          war3::tools::GpuFlightBreadcrumb::ShadowMotionVectors);
      ensureMotionVectorResources(extent);
    }
    if (allowShadowTaaAuxiliaryPasses &&
        (shadowTaaActive || debugShadowHistory || debugShadowCurrent ||
         debugShadowCurrentOverlay || debugShadowCsmDiagnosis)) {
      ensureShadowTaaResources(extent);
    }
    if (m_shadowMapResourceGeneration !=
        shadowMapResourceGenerationAtRunEntry) {
      reconciliation.shadowHistoryInvalidationMask |=
          kShadowTaaInvalidateShadowMapResource;
    }
    if (m_shadowTaaResourceGeneration !=
        shadowTaaResourceGenerationAtRunEntry) {
      reconciliation.shadowHistoryInvalidationMask |=
          kShadowTaaInvalidateTaaResource;
    }

    if (shadowTaaTemporalActive && m_shadowHistoryValid) {
      uint32_t historyContractInvalidation = 0u;
      if (!m_shadowTaaHistoryContractValid) {
        historyContractInvalidation |= kShadowTaaInvalidateTaaResource;
      } else {
        const auto& historyVp = effectiveWorldCamera.viewport;
        const float projectionDelta =
            MaxMatrixAbsDelta(effectiveWorldCamera.proj,
                              m_shadowTaaHistoryProjection);
        const float cameraCutFarDistance =
            m_csmData.cascadeCount != 0u
                ? std::max(
                      m_csmData.cascades[m_csmData.cascadeCount - 1u].splitFar,
                      1.0f)
                : 5000.0f;
        if (ShadowTaaIsCameraCut(effectiveWorldCamera.view,
                                 m_shadowTaaHistoryView,
                                 cameraCutFarDistance)) {
          historyContractInvalidation |= kShadowTaaInvalidateCameraCut;
        }
        if (projectionDelta > 1.0e-5f)
          historyContractInvalidation |= kShadowTaaInvalidateProjection;
        if (historyVp.X != m_shadowTaaHistoryViewportX ||
            historyVp.Y != m_shadowTaaHistoryViewportY ||
            historyVp.Width != m_shadowTaaHistoryViewportWidth ||
            historyVp.Height != m_shadowTaaHistoryViewportHeight ||
            std::abs(historyVp.MinZ - m_shadowTaaHistoryViewportMinZ) >
                1.0e-6f ||
            std::abs(historyVp.MaxZ - m_shadowTaaHistoryViewportMaxZ) >
                1.0e-6f) {
          historyContractInvalidation |= kShadowTaaInvalidateViewport;
        }
        // Normal sun/CSM/caster evolution is a shading change on the same
        // receiver surface. Let depth rejection, variance rectification and
        // reactive current weighting resolve it per pixel instead of globally
        // discarding useful history every animated frame. The two explicit
        // environment policies below retain conservative diagnostic fallbacks.
        if (ShadowTaaDisableOnSunMotionEnabled() &&
            ShadowTaaSunDirectionDelta(
                receiverSunDirSource,
                m_shadowTaaHistorySunDirection) > 1.0e-5f) {
          historyContractInvalidation |= kShadowTaaInvalidateSun;
        }
        if (ShadowTaaDisableForSemanticDynamicEnabled() &&
            semanticDynamicCastersActive &&
            (currentDynamicPoseSignature == 0u ||
             currentDynamicPoseSignature !=
                 m_shadowTaaHistoryDynamicPoseSignature)) {
          historyContractInvalidation |= kShadowTaaInvalidateDynamicPose;
        }
        if (shadowLifecycleTombstoneSerial !=
                m_shadowTaaHistoryLifecycleSerial ||
            shadowStagePolicyRevision !=
                m_shadowTaaHistoryStagePolicyRevision) {
          historyContractInvalidation |= kShadowTaaInvalidateLifecycle;
        }
        if (m_shadowMapResourceGeneration !=
            m_shadowTaaHistoryMapResourceGeneration) {
          historyContractInvalidation |=
              kShadowTaaInvalidateShadowMapResource;
        }
        if (m_shadowTaaResourceGeneration !=
            m_shadowTaaHistoryResourceGeneration) {
          historyContractInvalidation |= kShadowTaaInvalidateTaaResource;
        }
      }
      reconciliation.shadowHistoryInvalidationMask |=
          historyContractInvalidation;
      if (historyContractInvalidation != 0u) {
        m_shadowHistoryValid = false;
        m_shadowTaaWasActiveLastFrame = false;
      }
    }
    const bool shadowHistoryValidBefore = m_shadowHistoryValid;
    const uint32_t historyReadIndex = m_shadowHistoryIndex & 1u;
    const uint32_t historyWriteIndex = historyReadIndex ^ 1u;
    reconciliation.shadowHistoryValidBefore =
        shadowHistoryValidBefore ? 1u : 0u;
    reconciliation.shadowHistoryReadIndex = historyReadIndex;
    reconciliation.shadowHistoryWriteIndex = historyWriteIndex;
    recordShadowResourceFingerprint(historyReadIndex, historyWriteIndex);
    if (!allowShadowTaaAuxiliaryPasses) {
      reconciliation.shadowCurrentImagePtr = 0u;
      reconciliation.shadowCurrentViewPtr = 0u;
      reconciliation.shadowHistoryReadImagePtr = 0u;
      reconciliation.shadowHistoryReadViewPtr = 0u;
      reconciliation.shadowHistoryWriteImagePtr = 0u;
      reconciliation.shadowHistoryWriteViewPtr = 0u;
      reconciliation.shadowHistoryReadIndex = 0u;
      reconciliation.shadowHistoryWriteIndex = 0u;
    }

    // Update receiver uniforms (device-local UBO via cmdUpdateBuffer)
    ShadowReceiverUniform ubo = {};
    ubo.view = effectiveWorldCamera.view;
    ubo.invViewProj = effectiveWorldCamera.invViewProj;

    // Clamp the fallback cascade index: on a point-lights-only frame the
    // receiver pass still runs (needReceiverPass via m_hasPointLights) while
    // m_csmData.cascadeCount can be 0. The unsigned expression cascadeCount-1
    // would then wrap to UINT32_MAX and read far out of bounds of the 4-entry
    // cascades array before being uploaded to the GPU UBO.
    const uint32_t lastCascadeIndex =
        m_csmData.cascadeCount > 0u ? m_csmData.cascadeCount - 1u : 0u;
    for (uint32_t i = 0; i < 4; i++) {
      ubo.lightViewProj[i] =
          (i < m_csmData.cascadeCount)
              ? m_csmData.cascades[i].lightViewProj
              : m_csmData.cascades[lastCascadeIndex].lightViewProj;
    }

    float split0 = m_csmData.cascades[0].splitFar;
    float split1 =
        (m_csmData.cascadeCount > 1) ? m_csmData.cascades[1].splitFar : split0;
    float split2 =
        (m_csmData.cascadeCount > 2) ? m_csmData.cascades[2].splitFar : split1;
    float split3 =
        (m_csmData.cascadeCount > 3) ? m_csmData.cascades[3].splitFar : split2;
    ubo.splitFar = Vector4(split0, split1, split2, split3);

    const bool receiverHasUsableDirectionalShadow =
        receiverNeedsShadowMap && m_hasCompleteShadowMap && m_shadowMap &&
        m_shadowMapSampleView;
    // Newly allocated/new-epoch depth images have no meaningful contents
    // before the first complete candidate. Sampling them with normal strength
    // turns the entire map black during fail-closed admission.
    const float uboShadowStrength = receiverHasUsableDirectionalShadow
        ? receiverShadowStrength
        : 0.0f;
    reconciliation.receiverHasUsableDirectionalShadow =
        receiverHasUsableDirectionalShadow ? 1u : 0u;
    reconciliation.receiverUboStrengthMilli =
        strengthToMilli(uboShadowStrength);
    if (needReceiverPass && receiverNeedsShadowMap &&
        uboShadowStrength <= 1e-4f) {
      reconciliation.receiverZeroStrengthFrameCount = 1u;
      reconciliation.receiverDrawnWithZeroStrengthCount = 1u;
    }
    float pcfRadius = settings->shadows.pcfRadius;
    if (semanticReceiverStabilityActive) {
      const float pcfOverride = SemanticReceiverStablePcfRadiusOverride();
      if (pcfOverride >= 0.0f)
        pcfRadius = pcfOverride;
    }
    const float invShadowRes =
        1.0f / float(std::max<uint32_t>(m_shadowMapResolution, 1u));
    ubo.params = Vector4(uboShadowStrength, pcfRadius, invShadowRes,
                         float(m_csmData.cascadeCount));

    const float receiverBias = settings->shadows.receiverBias;
    const float cascadeBlendRange = settings->shadows.cascadeBlendRange;
    const float debugModeF =
        float(static_cast<int>(mutableSettings.shadows.debugMode));
    const float pointLightsEnabled =
        (semanticReceiverPointLightsAllowed && m_pointLightsEnabled &&
         m_hasPointLights)
            ? 1.0f
            : 0.0f;
    ubo.params2 = Vector4(receiverBias, cascadeBlendRange, debugModeF,
                          pointLightsEnabled);

    const auto &vp = effectiveWorldCamera.viewport;
    const float invViewportW = 1.0f / float(std::max<DWORD>(vp.Width, 1u));
    const float invViewportH = 1.0f / float(std::max<DWORD>(vp.Height, 1u));
    const float pcssEnable =
        (!semanticReceiverStabilityActive && settings->shadows.pcssEnabled)
            ? 1.0f
            : 0.0f;
    const float pcssSearchRadius =
        std::max(settings->shadows.pcssSearchRadius, 0.0f);
    ubo.params3 =
        Vector4(invViewportW, invViewportH, pcssEnable, pcssSearchRadius);

    const float pcssMinRadius = std::max(settings->shadows.pcssMinRadius, 0.0f);
    const float pcssMaxRadius =
        std::max(settings->shadows.pcssMaxRadius, pcssMinRadius);
    const float pcssDepthScale =
        std::max(settings->shadows.pcssDepthScale, 0.0f);
    const float cascadeBiasScale =
        std::clamp(settings->shadows.cascadeBiasScale, 0.0f, 1.0f);
    ubo.params4 =
        Vector4(pcssMinRadius, pcssMaxRadius, pcssDepthScale, cascadeBiasScale);

    Vector4 sunDir = receiverSunDirSource;
    const float sunLen2 =
        sunDir.x * sunDir.x + sunDir.y * sunDir.y + sunDir.z * sunDir.z;
    if (sunLen2 > 1e-6f) {
      const float invLen = 1.0f / std::sqrt(sunLen2);
      sunDir.x *= invLen;
      sunDir.y *= invLen;
      sunDir.z *= invLen;
    }
    ubo.sunDir = sunDir;

    const float normalBiasScale =
        semanticReceiverStabilityActive
            ? 0.0f
            : std::max(settings->shadows.normalBiasScale, 0.0f);
    const float receiverMode =
        semanticReceiverStabilityActive
            ? float(static_cast<uint32_t>(War3ShadowReceiverMode::Legacy))
            : float(static_cast<uint32_t>(settings->shadows.receiverMode));
    const float rimIntensity =
        semanticReceiverStabilityActive
            ? 0.0f
            : std::max(settings->shadows.rimIntensity, 0.0f);
    const float rimPower = std::max(settings->shadows.rimPower, 0.1f);
    ubo.params5 =
        Vector4(normalBiasScale, rimIntensity, rimPower, receiverMode);
    const float pcfKernel =
        float(static_cast<uint32_t>(settings->shadows.pcfKernel));
    const float pcfRotateMode =
        (!semanticReceiverStabilityActive && settings->shadows.pcfRotate)
            ? float(static_cast<uint32_t>(settings->shadows.pcfRotateMode))
            : 0.0f;
    const float pcssSearchKernel =
        float(static_cast<uint32_t>(settings->shadows.pcssSearchKernel));
    const float pcfCascadeRadiusScale =
        std::clamp(settings->shadows.pcfCascadeRadiusScale, 0.0f, 1.0f);
    ubo.params6 = Vector4(pcfKernel, pcfRotateMode, pcssSearchKernel,
                          pcfCascadeRadiusScale);

    if (semanticReceiverStabilityActive) {
      static uint32_t s_loggedSemanticReceiverStability = 0;
      if (s_loggedSemanticReceiverStability++ < 8u ||
          (s_loggedSemanticReceiverStability % 240u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3ShadowReceiverPass: semantic receiver stability active "
            "reuse=%d strength=%.3f->%.3f normalBias=0 pcfRotate=0 pcss=0 "
            "lastLighting=%d\n",
            reuseLastShadowMap ? 1 : 0,
            static_cast<double>(activeShadowStrength),
            static_cast<double>(uboShadowStrength),
            m_hasLastShadowMapLighting ? 1 : 0);
      }
    }

    ubo.viewport =
        Vector4(float(vp.X), float(vp.Y), float(vp.Width), float(vp.Height));
    const bool s1TerrainCasterMaskForReceiver =
        ShadowS1TerrainCasterMaskRuntimeEnabled() &&
        m_shadowCasterMaskSampleView && m_hasCompleteShadowMap;
    ubo.viewportZ =
        Vector4(vp.MinZ, vp.MaxZ,
                s1TerrainCasterMaskForReceiver ? 1.0f : 0.0f,
                war3::internal::kShadowS1TerrainCasterMaskDepthEpsilon);
    float receiverClearRaw = 0.0f;
    const bool receiverClearKnown =
        war3::render::InferWar3FarClearRaw(effectiveWorldCamera,
                                           receiverClearRaw);
    ubo.depthContract = Vector4(
        receiverClearRaw, receiverClearKnown ? 1.0f : 0.0f,
        war3::render::War3RawDepthQuantum(m_cachedDepthFormat), 0.0f);

    // Shadow TAA / Motion Vector：TemporalHistory 必须重投影到实际生成当前
    // history image 的相机。若本帧只做调试 motion vector，则回退到普通上一帧。
    const Matrix4 currentViewProj = effectiveWorldCamera.viewProj;

    // Shadow TAA v2 execution contract:
    //   0 DirectInline
    //   1 PrepassCurrentOnly (never reads or writes history)
    //   2 TemporalCurrentOnly (writes a fresh complete history)
    //   3 TemporalHistory (reads and writes history)
    const bool shadowHistoryReadable =
        shadowTaaTemporalActive && m_shadowHistoryValid &&
        m_shadowTaaWasActiveLastFrame && m_hasPrevFrameData;
    ubo.prevViewProj = shadowHistoryReadable
        ? m_shadowTaaHistoryViewProj
        : (m_hasPrevFrameData ? m_prevViewProj : currentViewProj);
    float taaMode = 0.0f;
    if (shadowTaaEffectiveMode == War3ShadowTaaMode::PrepassCurrentOnly) {
      taaMode = 1.0f;
    } else if (shadowTaaTemporalActive) {
      taaMode = shadowHistoryReadable ? 3.0f : 2.0f;
    }
    const float taaBlend =
        std::clamp(settings->shadows.shadowTaaBlendFactor, 0.0f, 1.0f);
    const float taaClamp =
        settings->shadows.shadowTaaNeighborClamp ? 1.0f : 0.0f;
    const float taaHasHistory = shadowHistoryReadable ? 1.0f : 0.0f;
    ubo.taaParams = Vector4(taaMode, taaBlend, taaClamp, taaHasHistory);
    ubo.proj = effectiveWorldCamera.proj;
    const uint32_t pointRayRequestedLights = std::clamp<uint32_t>(
        settings->shadows.pointRayShadowMaxLights, 1u, 2u);
    const uint32_t pointRayMaxLights = std::min<uint32_t>(
        pointRayRequestedLights, m_pointRayEligibleLightCount);
    const float pointRayStrength =
        std::clamp(settings->shadows.pointRayShadowStrength, 0.0f, 1.0f);
    const bool pointRayActive = pointLightsEnabled > 0.5f &&
        settings->shadows.pointRayShadowEnabled && pointRayMaxLights > 0u &&
        pointRayStrength > 1.0e-4f;
    const float pointRayMaxDistance =
        std::clamp(settings->shadows.pointRayShadowMaxDistance, 32.0f,
                   2400.0f);
    const float pointRayThickness =
        std::clamp(settings->shadows.pointRayShadowThickness, 1.0f, 160.0f);
    const uint32_t pointRaySteps =
        std::clamp<uint32_t>(settings->shadows.pointRayShadowSteps, 4u, 32u);
    const float pointRayStartOffset =
        std::clamp(settings->shadows.pointRayShadowStartOffset, 1.0f, 96.0f);

    if (pointRayActive && settings->shadows.pointRayShadowHiZEnabled &&
        m_depthCopyView && !m_hybridRayTracingUnavailable) {
      if (!m_hybridRayTracing) {
        try {
          m_hybridRayTracing =
              std::make_unique<war3::render::War3HybridRayTracing>(m_device);
        } catch (const DxvkError& e) {
          m_hybridRayTracingUnavailable = true;
          static bool s_loggedHybridPipelineFailure = false;
          if (!std::exchange(s_loggedHybridPipelineFailure, true)) {
            WAR3_RENDER_LOG(
                "DXVK War3HybridRay: pipeline creation failed; A0 remains "
                "active (%s)\n",
                e.message().c_str());
          }
        } catch (...) {
          m_hybridRayTracingUnavailable = true;
          static bool s_loggedUnknownHybridPipelineFailure = false;
          if (!std::exchange(s_loggedUnknownHybridPipelineFailure, true)) {
            WAR3_RENDER_LOG(
                "DXVK War3HybridRay: unexpected pipeline creation failure; "
                "A0 remains active\n");
          }
        }
      }

      if (m_hybridRayTracing) {
        auto hybridPerfScope = war3::War3PerfMonitor::instance().scope(
            "Shadow/PointRayHiZ", ctx);
        war3::render::War3HybridRayResult hybridResult = {};
        try {
          hybridResult = m_hybridRayTracing->Run(
                ctx, m_depthCopyView, depthExtent, effectiveWorldCamera,
                settings->shadows, pointLightSnapshot, pointRayMaxLights,
                input.frameSerial);
        } catch (const DxvkError& e) {
          m_hybridRayTracingUnavailable = true;
          static uint32_t s_hybridRuntimeFailureLogs = 0u;
          if (s_hybridRuntimeFailureLogs++ < 8u ||
              (s_hybridRuntimeFailureLogs % 240u) == 0u) {
            WAR3_RENDER_LOG(
                "DXVK War3HybridRay: runtime recording failed; A1 disabled "
                "(%s)\n",
                e.message().c_str());
          }
        } catch (...) {
          m_hybridRayTracingUnavailable = true;
          static bool s_loggedUnknownHybridRuntimeFailure = false;
          if (!std::exchange(s_loggedUnknownHybridRuntimeFailure, true)) {
            WAR3_RENDER_LOG(
                "DXVK War3HybridRay: unexpected runtime recording failure; "
                "A1 disabled\n");
          }
        }
        const bool exactResult =
            hybridResult.valid() &&
            hybridResult.producedFrameSerial == input.frameSerial &&
            hybridResult.resourceGeneration != 0u &&
            hybridResult.lightGeneration == pointLightSnapshot.generation &&
            hybridResult.lightLayerCount == pointRayMaxLights;
        if (exactResult) {
          m_pointRayHiZVisibilityView = hybridResult.visibilityView;
          m_pointRayHiZView = hybridResult.hizView;
          m_pointRayHiZLightCount = hybridResult.lightLayerCount;
          m_pointRayHiZFrameSerial = hybridResult.producedFrameSerial;
          m_pointRayHiZResourceGeneration =
              hybridResult.resourceGeneration;
          m_pointRayHiZLightGeneration = hybridResult.lightGeneration;
        }
      }
    }

    const bool pointRayHiZActive =
        m_pointRayHiZVisibilityView && m_pointRayHiZView &&
        m_pointRayHiZLightCount == pointRayMaxLights &&
        m_pointRayHiZFrameSerial == input.frameSerial &&
        m_pointRayHiZResourceGeneration != 0u &&
        m_pointRayHiZLightGeneration == pointLightSnapshot.generation;
    ubo.pointRayParams =
        Vector4(pointRayActive ? (pointRayHiZActive ? 2.0f : 1.0f) : 0.0f,
                pointRayStrength,
                pointRayMaxDistance, pointRayThickness);
    ubo.pointRayParams2 =
        Vector4(float(pointRaySteps), pointRayStartOffset,
                float(pointRayMaxLights),
                pointRayHiZActive ? float(m_pointRayHiZLightCount) : 0.0f);
    reconciliation.shadowTaaMode = uint32_t(taaMode + 0.5f);
    // Compatibility telemetry is execution evidence, not a requested-setting
    // echo. A non-zero value proves the receiver shader entered a TAA mode.
    reconciliation.shadowTaaActive =
        reconciliation.shadowTaaMode != 0u ? 1u : 0u;
    reconciliation.shadowReceiverSampleSource =
        receiverHasUsableDirectionalShadow
            ? (shadowTaaActive
                   ? (reconciliation.shadowTaaMode >= 3u ? 3u : 2u)
                   : 1u)
            : 0u;

    auto uboInfo =
        m_shadowUniformBuffer->getSliceInfo(0u, sizeof(ShadowReceiverUniform));
    VkBufferMemoryBarrier2 bufBarrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bufBarrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    bufBarrier.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    bufBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    bufBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bufBarrier.buffer = uboInfo.buffer;
    bufBarrier.offset = uboInfo.offset;
    bufBarrier.size = sizeof(ShadowReceiverUniform);

    VkDependencyInfo depInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.bufferMemoryBarrierCount = 1;
    depInfo.pBufferMemoryBarriers = &bufBarrier;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

    ctx->cmdUpdateBuffer(DxvkCmdBuffer::ExecBuffer, uboInfo.buffer,
                         uboInfo.offset, sizeof(ShadowReceiverUniform), &ubo);

    bufBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    bufBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bufBarrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    bufBarrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo);

    ctx->track(m_shadowUniformBuffer, DxvkAccess::Write);

    shadowMainPhaseTiming.enter(
        static_cast<size_t>(War3ShadowMainRawPhase::ReceiverPasses));
    // 先生成 Motion Vector / 当前帧阴影可见性（供 receiver 采样）
    if (needMotionVectors) {
      auto perfScope =
          war3::War3PerfMonitor::instance().scope("Shadow/MotionVector", ctx);
      renderMotionVectors(ctx, input);
    }

    if (needShadowVisibility) {
      war3::tools::SetGpuFlightBreadcrumb(
          war3::tools::GpuFlightBreadcrumb::ShadowVisibility);
      auto perfScope =
          war3::War3PerfMonitor::instance().scope("Shadow/Visibility", ctx);
      renderShadowVisibility(ctx, input);
    }

    // History 读写同步：
    // - 读：上一帧 storage 写入 -> 本帧 fragment 采样
    // - 写：上一帧 fragment 采样/写入 -> 本帧 fragment storage 写入
    if (allowShadowTaaAuxiliaryPasses &&
        (shadowTaaTemporalActive || debugShadowHistory) && m_shadowHistory[0] &&
        m_shadowHistory[1]) {
      std::array<VkImageMemoryBarrier2, 2> imgBarriers = {};
      std::array<war3::render::War3OwnedImageLayoutTransition, 2>
          layoutTransitions = {};
      std::array<uint32_t, 2> layoutIndices = {};
      uint32_t barrierCount = 0;

      if (shadowHistoryReadable || debugShadowHistory) {
        const uint32_t slot = barrierCount++;
        layoutIndices[slot] = historyReadIndex;
        layoutTransitions[slot] =
            m_shadowHistoryLayouts[historyReadIndex].plan(
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
        imgBarriers[slot] = war3::render::MakeWar3OwnedImageBarrier(
            layoutTransitions[slot],
            m_shadowHistory[historyReadIndex]->handle(),
            {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u});
      }

      if (shadowTaaTemporalActive) {
        const uint32_t slot = barrierCount++;
        layoutIndices[slot] = historyWriteIndex;
        layoutTransitions[slot] =
            m_shadowHistoryLayouts[historyWriteIndex].plan(
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_WRITE_BIT);
        imgBarriers[slot] = war3::render::MakeWar3OwnedImageBarrier(
            layoutTransitions[slot],
            m_shadowHistory[historyWriteIndex]->handle(),
            {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u});
      }

      if (barrierCount > 0) {
        VkDependencyInfo depInfo2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo2.imageMemoryBarrierCount = barrierCount;
        depInfo2.pImageMemoryBarriers = imgBarriers.data();
        ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo2);
        for (uint32_t i = 0u; i < barrierCount; ++i) {
          const uint32_t index = layoutIndices[i];
          const auto subresources = imgBarriers[i].subresourceRange;
          war3::render::CommitWar3OwnedImageLayout(
              m_shadowHistoryLayouts[index], layoutTransitions[i],
              *m_shadowHistory[index], subresources);
        }
      }
    }

    if (needReceiverPass) {
      war3::tools::SetGpuFlightBreadcrumb(
          war3::tools::GpuFlightBreadcrumb::ShadowReceiverDraw);
      auto perfScope =
          war3::War3PerfMonitor::instance().scope("ShadowReceiver", ctx);
      const uint32_t receiverDrawBefore =
          reconciliation.receiverDrawExecutedThisFrame;
      drawReceiver(ctx, input.colorView);
      if (receiverDrawBefore == 0u &&
          reconciliation.receiverDrawExecutedThisFrame != 0u &&
          reconciliation.shadowTaaMode >= 2u) {
        ++m_shadowTaaFixedWallBypassCount;
      }
      if (pointRayActive && !pointRayHiZActive &&
          receiverDrawBefore == 0u &&
          reconciliation.receiverDrawExecutedThisFrame != 0u) {
        // This marker is emitted only after the fullscreen receiver draw was
        // actually recorded. It is therefore execution evidence for the A0
        // fallback, not a settings echo that can pass on an early-return path.
        static uint32_t s_pointRayA0SubmitLogs = 0u;
        const uint32_t submitLog = s_pointRayA0SubmitLogs++;
        if (submitLog < 16u || (submitLog % 240u) == 0u) {
          WAR3_RENDER_LOG(
              "DXVK War3HybridRay: A0 receiver submitted frame=%llu "
              "lights=%u steps=%u\n",
              static_cast<unsigned long long>(input.frameSerial),
              pointRayMaxLights, pointRaySteps);
        }
      }
    }

    // Receiver writes History only for TemporalCurrentOnly/TemporalHistory
    // (taaMode 2/3). PrepassCurrentOnly is deliberately history-free.
    // 只有全屏 receiver draw 确实被记录且目标资源存在时，才能做同步并推进
    // ping-pong。旧代码只检查 shadowTaaActive：一旦 drawReceiver 因瞬时资源/
    // pipeline 条件早退，就会把完全没写过的 image 标成有效历史，下一帧采样时
    // 可表现为瞬时黑块或缺失区域。
    const bool shadowHistoryWriteExecuted =
        shadowTaaTemporalActive &&
        reconciliation.receiverDrawExecutedThisFrame != 0u &&
        m_shadowHistory[historyWriteIndex] != nullptr &&
        m_shadowHistoryStorageView[historyWriteIndex] != nullptr;
    reconciliation.shadowHistoryWriteExecutedThisFrame =
        shadowHistoryWriteExecuted ? 1u : 0u;
    const bool shadowHistoryWriteComplete =
        shadowTaaTemporalActive &&
        reconciliation.shadowVisibilityExecutedThisFrame != 0u &&
        reconciliation.shadowMotionVectorExecutedThisFrame != 0u &&
        reconciliation.receiverDrawExecutedThisFrame != 0u &&
        shadowHistoryWriteExecuted;
    if (shadowHistoryWriteComplete) {
      const VkImageSubresourceRange subresources = {
          VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
      const auto transition =
          m_shadowHistoryLayouts[historyWriteIndex].plan(
              VK_IMAGE_LAYOUT_GENERAL,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_READ_BIT);
      VkImageMemoryBarrier2 barrier =
          war3::render::MakeWar3OwnedImageBarrier(
              transition, m_shadowHistory[historyWriteIndex]->handle(),
              subresources);

      VkDependencyInfo depInfo2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      depInfo2.imageMemoryBarrierCount = 1;
      depInfo2.pImageMemoryBarriers = &barrier;
      ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer, &depInfo2);
      war3::render::CommitWar3OwnedImageLayout(
          m_shadowHistoryLayouts[historyWriteIndex], transition,
          *m_shadowHistory[historyWriteIndex], subresources);

      m_shadowHistoryIndex = historyWriteIndex;
      m_shadowHistoryValid = true;
      m_shadowTaaHistoryContractValid = true;
      m_shadowTaaHistoryView = effectiveWorldCamera.view;
      m_shadowTaaHistoryViewProj = effectiveWorldCamera.viewProj;
      m_shadowTaaHistoryProjection = effectiveWorldCamera.proj;
      m_shadowTaaHistorySunDirection = receiverSunDirSource;
      m_shadowTaaHistoryViewportX = effectiveWorldCamera.viewport.X;
      m_shadowTaaHistoryViewportY = effectiveWorldCamera.viewport.Y;
      m_shadowTaaHistoryViewportWidth =
          effectiveWorldCamera.viewport.Width;
      m_shadowTaaHistoryViewportHeight =
          effectiveWorldCamera.viewport.Height;
      m_shadowTaaHistoryViewportMinZ =
          effectiveWorldCamera.viewport.MinZ;
      m_shadowTaaHistoryViewportMaxZ =
          effectiveWorldCamera.viewport.MaxZ;
      m_shadowTaaHistoryCsmHash =
          War3ContinuityHashCsm(m_csmData);
      m_shadowTaaHistoryReplayContentHash = replayHashes.contentHash;
      m_shadowTaaHistoryReplayBackingHash = replayHashes.backingHash;
      m_shadowTaaHistoryDynamicPoseSignature =
          currentDynamicPoseSignature;
      m_shadowTaaHistoryLifecycleSerial =
          shadowLifecycleTombstoneSerial;
      m_shadowTaaHistoryStagePolicyRevision =
          shadowStagePolicyRevision;
      m_shadowTaaHistoryMapResourceGeneration =
          m_shadowMapResourceGeneration;
      m_shadowTaaHistoryResourceGeneration =
          m_shadowTaaResourceGeneration;
      ++m_shadowTaaHistoryGeneration;
      reconciliation.shadowHistoryAdvancedThisFrame = 1u;
    } else if (shadowTaaTemporalActive) {
      reconciliation.shadowHistoryAdvanceSkippedIncomplete = 1u;
    }
    reconciliation.shadowHistoryValidAfter =
        m_shadowHistoryValid ? 1u : 0u;

    // “上一帧 TAA 连续”要求上一帧确实完成 history write，不能只看设置开关。
    // 失败帧保留最后一张已完成 history，但下一帧强制 current-only 覆盖后再恢复混合。
    m_shadowTaaWasActiveLastFrame = shadowHistoryWriteComplete;
  } else {
    // 本帧未执行 receiver/ubo 更新，视为 TAA 断档
    m_shadowTaaWasActiveLastFrame = false;
  }

  shadowMainPhaseTiming.enter(
      static_cast<size_t>(War3ShadowMainRawPhase::OutlineAndPublish));
  // 单位被遮挡描边
  // 此时场景已完全渲染，深度缓冲完整，可以正确判断遮挡
  if (settings->occludedOutline.enabled) {
    war3::tools::SetGpuFlightBreadcrumb(
        war3::tools::GpuFlightBreadcrumb::ShadowOutline);
    auto perfScope = war3::War3PerfMonitor::instance().scope("Outline", ctx);
    renderUnitOutline(ctx, input);
  }

  if (hasListeners) {
    war3shader::internal::DispatchRenderEvent(
        war3shader::RenderEventID::SHADOW_PASS_END);
  }

  setReceiverRunEntryFlags(true, shadowsEnabled, outlineEnabled, hasSunShadow,
                           hasPointShadow, m_hasPointLights, needOutlinePass,
                           receiverNeedsShadowMap, needReceiverPass,
                           replayCasterCount != 0u,
                           reconciliation.shadowMapExecutedThisFrame != 0u,
                           debugShadow);
  publishReconciliationStats();

  // 保存上一帧相机矩阵（用于 Motion Vector / ShadowTAA 重投影）
  m_prevViewMatrix = effectiveWorldCamera.view;
  m_prevProjMatrix = effectiveWorldCamera.proj;
  m_prevViewProj = effectiveWorldCamera.viewProj;
  m_hasPrevFrameData = true;
  m_shadowTaaPreviousSunDirection = receiverSunDirSource;
  m_shadowTaaHasPreviousSunDirection = true;
  if (input.frameSerial != 0u && receiverNeedsShadowMap &&
      directionalMapResolvedForFrame &&
      m_hasCompleteShadowMap && m_shadowMapSampleView &&
      m_csmData.cascadeCount != 0u &&
      War3WorldCameraIsFreshForFrame(effectiveWorldCamera,
                                     input.frameSerial)) {
    m_shadowPublicationSettledFrameSerial = input.frameSerial;
  }
}

// ----------------------------------------------------------------------------
// War3ShadowDataPool Implementation

/*
    void War3ShadowDataPool::init(DxvkDevice* device) {
        if (m_device != nullptr)
            return; // Already inited

        m_device = device;
        m_frameIndex = 0;
        m_cursor = 0;

        DxvkBufferCreateInfo info = { };
        info.size = kBufferSize;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
VK_BUFFER_USAGE_INDEX_BUFFER_BIT; info.stages =
VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        info.access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
VK_ACCESS_INDEX_READ_BIT; info.debugName = "War3ShadowStagingPool";

        // Host visible and coherent heavily reduces mapping overhead
        VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        for (size_t i = 0; i < kRingSize; i++) {
            m_buffers[i] = m_device->createBuffer(info, flags);
            if (m_buffers[i] != nullptr)
                m_mapPtrs[i] = m_buffers[i]->mapPtr(0);
        }
    }

    void War3ShadowDataPool::newFrame() {
        if (m_device == nullptr) return;

        m_frameIndex = (m_frameIndex + 1) % kRingSize;
        m_cursor = 0; // Reset cursor for new frame
    }

    DxvkResourceBufferInfo War3ShadowDataPool::upload(const void* data,
VkDeviceSize size, VkBufferUsageFlags usage) { DxvkResourceBufferInfo result = {
};

        if (m_device == nullptr || data == nullptr || size == 0)
            return result;

        size_t align = 256; // Standard alignment
        VkDeviceSize alignedCursor = (m_cursor + align - 1) & ~(align - 1);

        if (alignedCursor + size > kBufferSize) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                WAR3_RENDER_LOG("DXVK War3ShadowDataPool: EXHAUSTED (size >
8MB)\n");
            }
            return result;
        }

        void* dst = static_cast<uint8_t*>(m_mapPtrs[m_frameIndex]) +
alignedCursor; std::memcpy(dst, data, size);

        // Get info directly from buffer slice
        DxvkResourceBufferInfo baseInfo =
m_buffers[m_frameIndex]->getSliceInfo(0, kBufferSize); result.buffer =
baseInfo.buffer; result.offset = baseInfo.offset + alignedCursor; result.size =
size;

        m_cursor = alignedCursor + size;
        return result;
    }

    Rc<DxvkBuffer> War3ShadowDataPool::getCurrentBuffer() {
        if (m_device == nullptr) return nullptr;
        return m_buffers[m_frameIndex];
    }

} // namespace dxvk */

// Tanner Helland's algorithm (simplified)
Vector4 War3ShadowReceiverPass::kelvinToRgb(float k) {
  float temp = k / 100.0f;
  float r, g, b;

  // Red
  if (temp <= 66.0f) {
    r = 255.0f;
  } else {
    r = temp - 60.0f;
    r = 329.698727446f * std::pow(r, -0.1332047592f);
    r = std::min(255.0f, std::max(0.0f, r));
  }

  // Green
  if (temp <= 66.0f) {
    g = temp;
    g = 99.4708025861f * std::log(g) - 161.1195681661f;
    g = std::min(255.0f, std::max(0.0f, g));
  } else {
    g = temp - 60.0f;
    g = 288.1221695283f * std::pow(g, -0.0755148492f);
    g = std::min(255.0f, std::max(0.0f, g));
  }

  // Blue
  if (temp >= 66.0f) {
    b = 255.0f;
  } else {
    if (temp <= 19.0f) {
      b = 0.0f;
    } else {
      b = temp - 10.0f;
      b = 138.5177312231f * std::log(b) - 305.0447927307f;
      b = std::min(255.0f, std::max(0.0f, b));
    }
  }

  return Vector4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

void War3ShadowReceiverPass::UpdatePhase(float altitudeRad, bool isRising,
                                         float transRad) {
  War3DayNightPhase nextPhase = m_currentPhase;
  float progress = 0.0f;

  if (altitudeRad > transRad) {
    nextPhase = War3DayNightPhase::Day;
    // Progress: 0 at Transition, 1 at Zenith (PI/2)
    float range = (3.14159265f / 2.0f) - transRad;
    progress = (std::max)(0.0f, (altitudeRad - transRad) / range);
  } else if (altitudeRad < -transRad) {
    nextPhase = War3DayNightPhase::Night;
    // Progress: 0 at Transition, 1 at Nadir (-PI/2)
    float range = (3.14159265f / 2.0f) - transRad;
    progress = (std::max)(0.0f, (-altitudeRad - transRad) / range);
  } else {
    // Transition Zone
    if (isRising) {
      nextPhase = War3DayNightPhase::Dawn;
      // Progress: 0 at -Trans, 1 at +Trans
      progress = (altitudeRad + transRad) / (2.0f * transRad);
    } else {
      nextPhase = War3DayNightPhase::Dusk;
      // Progress: 0 at +Trans, 1 at -Trans
      progress = (transRad - altitudeRad) / (2.0f * transRad);
    }
  }

  // State change check
  if (nextPhase != m_currentPhase) {
    // Can fire "OnEnter" event here if needed
    m_currentPhase = nextPhase;
  }

  // Fire continuous event
  if (m_eventCb) {
    m_eventCb(m_currentPhase, (std::min)(1.0f, (std::max)(0.0f, progress)));
  }
}

} // namespace dxvk
