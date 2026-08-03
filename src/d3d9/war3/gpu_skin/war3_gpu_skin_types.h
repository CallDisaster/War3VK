#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dxvk::war3::gpu_skin {

enum class GpuSkinMode : uint8_t {
  Disabled = 0,
  Observe = 1,
  Dual = 2,
  Shadow = 3,
  Main = 4,
  Bypass = 5,

// 为 native bridge owner 保留的临时拼写兼容别名。
  Override = Main,
};

// 选择蒙皮计算的执行位置。Compute 是默认产品路径；VertexShader 保留同帧
// compute 输出作为 VS-A 兜底；VertexShaderInputOnly 只允许已证明的 opaque、
// format2、单 UV candidate 省略 compute output/job，但仍完整执行原生 CPU kernel。
// VertexShaderBypass 是独立的 VS-B1 实验路线：只有 input lease、Main 与 Shadow
// 的不可逆消费者都在原生 kernel 前闭合时，才允许沿用 P4 跳过 CPU kernel。
// 未显式选择时任何枚举都不自行授予 P4 绕过权限。
enum class GpuSkinExecutionRoute : uint8_t {
  Compute = 0u,
  VertexShader = 1u,
  VertexShaderInputOnly = 2u,
  VertexShaderBypass = 3u,
  Count = 4u,
};

inline constexpr std::array<const char*, 4u>
    kGpuSkinExecutionRouteNames = {{
        "compute", "vertex_shader", "vertex_shader_input_only",
        "vertex_shader_bypass",
    }};

constexpr bool GpuSkinExecutionRouteValid(
    GpuSkinExecutionRoute route) noexcept {
  return static_cast<uint8_t>(route) <
      static_cast<uint8_t>(GpuSkinExecutionRoute::Count);
}

static_assert(static_cast<uint8_t>(GpuSkinExecutionRoute::Compute) == 0u);
static_assert(static_cast<uint8_t>(GpuSkinExecutionRoute::VertexShader) == 1u);
static_assert(static_cast<uint8_t>(
    GpuSkinExecutionRoute::VertexShaderInputOnly) == 2u);
static_assert(static_cast<uint8_t>(
    GpuSkinExecutionRoute::VertexShaderBypass) == 3u);
static_assert(kGpuSkinExecutionRouteNames.size() ==
    static_cast<size_t>(GpuSkinExecutionRoute::Count));
static_assert(GpuSkinExecutionRouteValid(GpuSkinExecutionRoute::Compute));
static_assert(GpuSkinExecutionRouteValid(
    GpuSkinExecutionRoute::VertexShader));
static_assert(GpuSkinExecutionRouteValid(
    GpuSkinExecutionRoute::VertexShaderInputOnly));
static_assert(GpuSkinExecutionRouteValid(
    GpuSkinExecutionRoute::VertexShaderBypass));

// 仅报告 outside-poison D3D9 sidecar 的进程生命周期策略。
// 数值按位构造：O0 是旧 normal-return 比较，O1 是成功 Lock/Unlock 证明；
// 两个位都不授予运行时 authority。
enum class GpuSkinOutsidePoisonSidecarPolicy : uint8_t {
  None = 0u,
  O0 = 1u,
  O1 = 2u,
  Both = 3u,
  Count = 4u,
};

inline constexpr std::array<const char*, 4u>
    kGpuSkinOutsidePoisonSidecarPolicyNames = {{
        "none", "o0", "o1", "both",
    }};

constexpr bool GpuSkinOutsidePoisonO0Enabled(
    GpuSkinOutsidePoisonSidecarPolicy policy) noexcept {
  return (static_cast<uint8_t>(policy) & 1u) != 0u;
}

constexpr bool GpuSkinOutsidePoisonO1Enabled(
    GpuSkinOutsidePoisonSidecarPolicy policy) noexcept {
  return (static_cast<uint8_t>(policy) & 2u) != 0u;
}

constexpr bool GpuSkinOutsidePoisonSidecarPolicyValid(
    GpuSkinOutsidePoisonSidecarPolicy policy) noexcept {
  return static_cast<uint8_t>(policy) <
      static_cast<uint8_t>(GpuSkinOutsidePoisonSidecarPolicy::Count);
}

static_assert(!GpuSkinOutsidePoisonO0Enabled(
    GpuSkinOutsidePoisonSidecarPolicy::None));
static_assert(!GpuSkinOutsidePoisonO1Enabled(
    GpuSkinOutsidePoisonSidecarPolicy::None));
static_assert(GpuSkinOutsidePoisonO0Enabled(
    GpuSkinOutsidePoisonSidecarPolicy::O0));
static_assert(GpuSkinOutsidePoisonO1Enabled(
    GpuSkinOutsidePoisonSidecarPolicy::O1));
static_assert(GpuSkinOutsidePoisonO0Enabled(
    GpuSkinOutsidePoisonSidecarPolicy::Both));
static_assert(GpuSkinOutsidePoisonO1Enabled(
    GpuSkinOutsidePoisonSidecarPolicy::Both));

// Production Bypass 让占多数的小 geoset 继续走原生 SSE 路径。native admission
// bridge 与 manager 共享该策略，避免 fail-closed CPU 路由在两层之间发生漂移。
inline constexpr uint32_t kProductionGpuMinVertices = 449u;

enum class GpuSkinConsumerBits : uint32_t {
  None = 0u,
  Main = 1u << 0,
  Shadow = 1u << 1,
  Outline = 1u << 2,
  Parity = 1u << 3,
};

static_assert(static_cast<uint32_t>(GpuSkinConsumerBits::Main) == 1u);
static_assert(static_cast<uint32_t>(GpuSkinConsumerBits::Shadow) == 2u);
static_assert(static_cast<uint32_t>(GpuSkinConsumerBits::Outline) == 4u);
static_assert(static_cast<uint32_t>(GpuSkinConsumerBits::Parity) == 8u);

constexpr GpuSkinConsumerBits operator|(
    GpuSkinConsumerBits lhs, GpuSkinConsumerBits rhs) {
  return static_cast<GpuSkinConsumerBits>(
      static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

enum class GpuSkinFallbackReason : uint16_t {
  None = 0,
  Disabled,
  InvalidEpoch,
  UnsupportedLayout,
  StaticResourceMiss,
  StaticResourcePending,
  StaticResourceInvalid,
  StaticBudgetExhausted,
  UploadBudgetExhausted,
  OutputBudgetExhausted,
  OutputLeaseUnretired,
  MissQueueFull,
  InvalidPalette,
  InvalidJob,
  DeviceLost,
};

enum class GpuSkinStaticResourceState : uint8_t {
  PendingUpload = 0,
  UploadSubmitted = 1,
  Ready = 2,
  Invalid = 3,
};

enum class GpuSkinBatchState : uint8_t {
  Pending = 0,
  Claimed = 1,
  Submitting = 2,
  Submitted = 3,
  Retired = 4,
};

// 由集成层所有者一次性解析。未知模式保持禁用；未知执行路线保持 Compute，
// 并另行标记为无效。
struct GpuSkinRuntimeConfig {
  GpuSkinMode mode = GpuSkinMode::Disabled;
  // DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE 只控制显式 VS 实验路线。
  // Compute 是失效关闭的默认值，也适用于所有未知的显式值；
  // explicit/invalid 字段保留诊断上的区别。
  GpuSkinExecutionRoute executionRoute = GpuSkinExecutionRoute::Compute;
  uint32_t diffSamplePeriod = 0;
  bool fullDiagnostics = false;
  uint32_t diagnosticPeriodFrames = 0;
  bool executionRouteExplicit = false;
  bool executionRouteInvalid = false;
  GpuSkinOutsidePoisonSidecarPolicy outsidePoisonSidecarPolicy =
      GpuSkinOutsidePoisonSidecarPolicy::None;
  bool outsidePoisonSidecarPolicyExplicit = false;
  bool outsidePoisonSidecarPolicyInvalid = false;

  static GpuSkinMode parseMode(const char* value);
  static GpuSkinExecutionRoute parseExecutionRoute(
      const char* value, bool& invalid);
  static GpuSkinRuntimeConfig fromEnvironment();
};

static_assert(GpuSkinRuntimeConfig{}.executionRoute ==
    GpuSkinExecutionRoute::Compute);
static_assert(!GpuSkinRuntimeConfig{}.executionRouteExplicit);
static_assert(!GpuSkinRuntimeConfig{}.executionRouteInvalid);

bool ShouldSampleGpuSkinDiff(uint64_t sequence, uint32_t period);

struct GpuSkinStaticResourceKey {
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uintptr_t geosetData = 0;
  uint64_t contentHash = 0;
  uint64_t immutableModelGeneration = 0;
  uint32_t layoutGeneration = 0;
  uint32_t reserved = 0;

  bool operator==(const GpuSkinStaticResourceKey& other) const {
    return mapEpoch == other.mapEpoch && deviceEpoch == other.deviceEpoch &&
           geosetData == other.geosetData && contentHash == other.contentHash &&
           immutableModelGeneration == other.immutableModelGeneration &&
           layoutGeneration == other.layoutGeneration &&
           reserved == other.reserved;
  }
};

struct GpuSkinStaticResourceKeyHash {
  size_t operator()(const GpuSkinStaticResourceKey& key) const;
};

// Compute 线协议 ABI。上传切片按 std430 对齐，但临时 CPU vector 必须兼容 32 位
// 运行时只保证 8 字节对齐的 allocator。曾经对该 staging 值过度对齐，导致
// std::vector 向优化器作出错误的 16 字节承诺，并在 Dual 模式触发 movaps 崩溃。
struct GpuSkinJob {
  uint64_t resourceKeyHash = 0;
  uint64_t frameTag = 0;
  uint32_t inputVertexOffset = 0;
  uint32_t paletteOffset = 0;
  uint32_t outputOffset = 0;
  uint32_t vertexCount = 0;
  uint32_t outputStride = 0;
  uint32_t outputFormat = 0;
  uint32_t layoutGeneration = 0;
  uint32_t flags = 0;
  uint32_t token = 0;
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;
  uint32_t reserved2 = 0;
};

// 固定功能 VS-A 的局部推送契约。全零初始化表示未激活；
// 只有精确的 WVS1 魔数才能允许着色器侧读取。
// 两个偏移分别以完整绑定的静态图集和保留的调色板页为基准，单位为字节。
inline constexpr uint32_t kGpuSkinVsDrawActiveMagic = 0x31535657u;
// WVS2 表示 native CPU kernel 已被不可逆跳过；着色器私有门若意外失败，
// 必须裁掉该 primitive，绝不能回读带毒的原生动态 VB。
inline constexpr uint32_t kGpuSkinVsDrawBypassActiveMagic = 0x32535657u;

struct GpuSkinVsDrawParams {
  uint32_t activationMagic = 0u;
  uint32_t inputVertexOffset = 0u;
  uint32_t paletteOffset = 0u;
  uint32_t vertexCount = 0u;
  uint32_t paletteMatrixCount = 0u;
  uint32_t sourceUvLayerCount = 0u;
  uint32_t outputFormat = 0u;
  uint32_t layoutGeneration = 0u;
};

constexpr bool GpuSkinVsDrawParamsActive(
    const GpuSkinVsDrawParams& params) noexcept {
  return params.activationMagic == kGpuSkinVsDrawActiveMagic ||
      params.activationMagic == kGpuSkinVsDrawBypassActiveMagic;
}

static_assert(!GpuSkinVsDrawParamsActive(GpuSkinVsDrawParams{}));

// 描述由消费者栅栏租约持有的不可变输入字节。缓冲区所有权和物理 page
// 身份由运行时 GpuSkinInputLease 持有；本结构只保存可做 exact 校验的值语义。
// 所有范围均以各自完整后备缓冲区为基准。
struct GpuSkinInputLeaseDesc {
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  uint64_t frameTag = 0u;
  uint64_t token = 0u;
  uint64_t dispatchEpoch = 0u;
  uint64_t uploadEpoch = 0u;
  uint32_t staticByteOffset = 0u;
  uint32_t staticByteLength = 0u;
  uint32_t paletteByteOffset = 0u;
  uint32_t paletteByteLength = 0u;
  uint32_t vertexCount = 0u;
  uint32_t paletteMatrixCount = 0u;
  uint32_t sourceUvLayerCount = 0u;
  uint32_t outputFormat = 0u;
  uint32_t layoutGeneration = 0u;
  uint32_t consumerBits = 0u;
};

static_assert(sizeof(GpuSkinJob) == 64);
static_assert(sizeof(GpuSkinVsDrawParams) == 32u);
static_assert(alignof(GpuSkinVsDrawParams) == alignof(uint32_t));
static_assert(offsetof(GpuSkinVsDrawParams, activationMagic) == 0u);
static_assert(offsetof(GpuSkinVsDrawParams, inputVertexOffset) == 4u);
static_assert(offsetof(GpuSkinVsDrawParams, paletteOffset) == 8u);
static_assert(offsetof(GpuSkinVsDrawParams, vertexCount) == 12u);
static_assert(offsetof(GpuSkinVsDrawParams, paletteMatrixCount) == 16u);
static_assert(offsetof(GpuSkinVsDrawParams, sourceUvLayerCount) == 20u);
static_assert(offsetof(GpuSkinVsDrawParams, outputFormat) == 24u);
static_assert(offsetof(GpuSkinVsDrawParams, layoutGeneration) == 28u);
static_assert(sizeof(GpuSkinInputLeaseDesc) == 88u);
static_assert(offsetof(GpuSkinInputLeaseDesc, staticByteOffset) == 48u);
static_assert(offsetof(GpuSkinInputLeaseDesc, consumerBits) == 84u);
static_assert(std::is_standard_layout_v<GpuSkinJob>);
static_assert(std::is_trivially_copyable_v<GpuSkinJob>);
static_assert(std::is_standard_layout_v<GpuSkinVsDrawParams>);
static_assert(std::is_trivially_copyable_v<GpuSkinVsDrawParams>);
static_assert(std::is_standard_layout_v<GpuSkinInputLeaseDesc>);
static_assert(std::is_trivially_copyable_v<GpuSkinInputLeaseDesc>);
static_assert(std::is_trivially_copyable_v<GpuSkinStaticResourceKey>);
static_assert(std::is_trivially_copyable_v<GpuSkinRuntimeConfig>);

struct FlushRequest {
  uint64_t frameTag = 0;
  uint64_t flushToken = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint32_t requestedJobCount = 0;
  uint32_t reserved = 0;
};

struct NativeDispatchIdentity {
  uint64_t renderThreadId = 0;
  uint64_t frameTag = 0;
  uint64_t uploadToken = 0;
  uintptr_t geosetData = 0;
  uintptr_t palettePtr = 0;
  uint32_t paletteGroupCount = 0;
  uint32_t vertexCount = 0;
  uint32_t outputFormat = 0;
  uint32_t outputStride = 0;
};

struct NativeUploadArgs {
  uintptr_t geosetData = 0;
  uintptr_t sourceVertexData = 0;
  uintptr_t palettePtr = 0;
  uintptr_t destinationVertexRing = 0;
  uint32_t paletteGroupCount = 0;
  uint32_t vertexCount = 0;
  uint32_t outputFormat = 0;
  uint32_t outputStride = 0;
  uint32_t baseVertex = 0;
  uint32_t flags = 0;
};

struct DipSignature {
  uint64_t renderThreadId = 0;
  uint64_t uploadToken = 0;
  uint64_t dispatchEpoch = 0;
  uint64_t uploadEpoch = 0;
  uintptr_t expectedStream0 = 0;
  uint32_t dipOrdinal = 0;
  uint32_t consumerBits = 0;
  uint32_t primitiveType = 0;
  uint32_t baseVertexIndex = 0;
  uint32_t minVertexIndex = 0;
  uint32_t numVertices = 0;
  uint32_t startIndex = 0;
  uint32_t primitiveCount = 0;
  uint32_t vertexStride = 0;
};

// plan 将每个已知消费者分类为“未请求”或“由该 prepared output lease 支撑”。
// signature 把分类绑定到一次精确 native DIP；mask 使用 GpuSkinConsumerBits。
struct GpuSkinConsumerPlan {
  uint32_t known = 0;
  uint32_t notRequested = 0;
  uint32_t leaseBacked = 0;
  uint32_t reserved = 0;
  DipSignature signature;
};

// Resolve 只记录可用性；消费者只有经过 CommitConsumer 或 FailConsumer 才进入终态。
struct GpuSkinConsumerLedger {
  uint32_t resolved = 0;
  uint32_t consumed = 0;
  uint32_t cpuFallback = 0;
  uint32_t suppressFuse = 0;
};

enum class GpuSkinConsumerFailure : uint8_t {
  CpuFallback = 0,
  SuppressAndFuse = 1,
};

enum class GpuSkinConsumerWindowState : uint8_t {
  Open = 0,
  RetireDeferred = 1,
  Closed = 2,
};

struct OutputLeaseDesc {
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint64_t frameTag = 0;
  uint64_t token = 0;
  uint64_t dispatchEpoch = 0;
  uint64_t uploadEpoch = 0;
  uint32_t byteOffset = 0;
  uint32_t byteLength = 0;
  uint32_t vertexStride = 0;
  uint32_t vertexCount = 0;
  uint32_t dipOrdinal = 0;
  uint32_t consumerBits = 0;
};

// D3D9 owner 调度 CPU/GPU 字节 parity 所需的元数据。manager 负责识别源字节，
// 但不执行 D3D9 readback。
struct GpuSkinParityMetadata {
  uint64_t cpuSourceSignature = 0;
  uint64_t paletteSignature = 0;
  uint64_t sampleSequence = 0;
  uint32_t diffSamplePeriod = 0;
  uint32_t expectedCpuByteCount = 0;
  uint32_t expectedVertexCount = 0;
  uint32_t expectedVertexStride = 0;
  uint32_t sampleRequested = 0;
};

struct GpuSkinJobFallback {
  uint64_t frameTag = 0;
  uint32_t token = 0;
  GpuSkinFallbackReason reason = GpuSkinFallbackReason::None;
  uint16_t reserved = 0;
};

struct GpuSkinDiagnostics {
  uint64_t storageBufferOffsetAlignment = 0;
  uint64_t staticCacheHits = 0;
  uint64_t staticCacheMisses = 0;
  uint64_t staticUploadsPrepared = 0;
  uint64_t staticUploadBytes = 0;
  uint64_t staticUploadRetirementsQueued = 0;
  uint64_t staticUploadRetirementsReclaimed = 0;
  uint64_t staticUploadsCompleted = 0;
  uint64_t staticUploadCompletionsRejected = 0;
  uint64_t paletteUploadBytes = 0;
  uint64_t jobUploadBytes = 0;
  uint64_t outputLeaseBytes = 0;
  uint64_t outputLeaseRetired = 0;
  uint64_t outputLeasePending = 0;
  uint64_t uploadPagesAllocated = 0;
  uint64_t uploadPagesReclaimed = 0;
  uint64_t outputPagesAllocated = 0;
  uint64_t retirementFenceQueries = 0;
  uint64_t retirementFenceCacheHits = 0;
  uint64_t fallbackCount = 0;
  uint64_t fallbackByReason[16] = {};
};

static_assert(std::is_trivially_copyable_v<FlushRequest>);
static_assert(std::is_trivially_copyable_v<NativeDispatchIdentity>);
static_assert(std::is_trivially_copyable_v<NativeUploadArgs>);
static_assert(std::is_trivially_copyable_v<DipSignature>);
static_assert(std::is_trivially_copyable_v<GpuSkinConsumerPlan>);
static_assert(std::is_trivially_copyable_v<GpuSkinConsumerLedger>);
static_assert(std::is_trivially_copyable_v<OutputLeaseDesc>);
static_assert(std::is_trivially_copyable_v<GpuSkinParityMetadata>);
static_assert(std::is_trivially_copyable_v<GpuSkinJobFallback>);
static_assert(std::is_trivially_copyable_v<GpuSkinDiagnostics>);
static_assert(static_cast<size_t>(GpuSkinFallbackReason::DeviceLost) < 16u);

}  // namespace dxvk::war3::gpu_skin
